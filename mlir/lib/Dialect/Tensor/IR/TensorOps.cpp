//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Arith/Utils/Utils.h"
#include "mlir/Dialect/Complex/IR/Complex.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Utils/ReshapeOpsUtils.h"
#include "mlir/Dialect/Utils/StaticValueUtils.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributeInterfaces.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Interfaces/DestinationStyleOpInterface.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/StringRef.h"
#include <algorithm>

using namespace mlir;
using namespace mlir::tensor;

/// Materialize a single constant operation from a given attribute value with
/// the desired resultant type.
Operation *TensorDialect::materializeConstant(OpBuilder &builder,
                                              Attribute value, Type type,
                                              Location loc) {
  if (arith::ConstantOp::isBuildableWith(value, type))
    return builder.create<arith::ConstantOp>(loc, value, type);
  if (complex::ConstantOp::isBuildableWith(value, type))
    return builder.create<complex::ConstantOp>(loc, type,
                                               value.cast<ArrayAttr>());
  return nullptr;
}

SmallVector<OpFoldResult> tensor::getMixedSizes(OpBuilder &builder,
                                                Location loc, Value value) {
  auto tensorType = value.getType().cast<RankedTensorType>();
  SmallVector<OpFoldResult> result;
  for (int64_t i = 0; i < tensorType.getRank(); ++i) {
    if (tensorType.isDynamicDim(i)) {
      Value size = builder.create<tensor::DimOp>(loc, value, i);
      result.push_back(size);
    } else {
      result.push_back(builder.getIndexAttr(tensorType.getDimSize(i)));
    }
  }
  return result;
}

FailureOr<Value> tensor::getOrCreateDestination(OpBuilder &b, Location loc,
                                                OpResult opResult) {
  auto tensorType = opResult.getType().dyn_cast<TensorType>();
  assert(tensorType && "expected tensor type");

  // If the op has a destination, it implements DestinationStyleOpInterface and
  // we can query the destination operand from that interface.
  auto destOp = opResult.getDefiningOp<DestinationStyleOpInterface>();
  if (destOp)
    return destOp.getTiedOpOperand(opResult)->get();

  // Otherwise, create a new destination tensor with the same shape.
  OpBuilder::InsertionGuard g(b);
  b.setInsertionPoint(opResult.getDefiningOp());

  // Compute sizes.
  SmallVector<OpFoldResult> mixedSizes;
  if (!tensorType.hasStaticShape()) {
    // Dynamic shape: Query ReifyRankedShapedTypeOpInterface.
    ReifiedRankedShapedTypeDims reifiedShapes;
    ReifyRankedShapedTypeOpInterface reifyShapedTypeInterface =
        dyn_cast<ReifyRankedShapedTypeOpInterface>(opResult.getDefiningOp());
    if (!reifyShapedTypeInterface)
      return failure();
    if (failed(reifyShapedTypeInterface.reifyResultShapes(b, reifiedShapes)))
      return failure();
    mixedSizes = getAsOpFoldResult(reifiedShapes[opResult.getResultNumber()]);
  } else {
    // Static shape: Take static sizes directly.
    for (int64_t sz : tensorType.getShape())
      mixedSizes.push_back(b.getIndexAttr(sz));
  }

  // Create empty tensor.
  Value emptyTensor =
      b.create<tensor::EmptyOp>(loc, mixedSizes, tensorType.getElementType());
  return emptyTensor;
}

LogicalResult tensor::getOrCreateDestinations(OpBuilder &b, Location loc,
                                              Operation *op,
                                              SmallVector<Value> &result) {
  for (OpResult opResult : op->getResults()) {
    if (opResult.getType().isa<TensorType>()) {
      FailureOr<Value> destination = getOrCreateDestination(b, loc, opResult);
      if (failed(destination))
        return failure();
      result.push_back(*destination);
    }
  }
  return success();
}

//===----------------------------------------------------------------------===//
// CastOp
//===----------------------------------------------------------------------===//

void CastOp::getAsmResultNames(function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "cast");
}

/// Returns true if `target` is a ranked tensor type that preserves static
/// information available in the `source` ranked tensor type.
bool mlir::tensor::preservesStaticInformation(Type source, Type target) {
  auto sourceType = source.dyn_cast<RankedTensorType>();
  auto targetType = target.dyn_cast<RankedTensorType>();

  // Requires RankedTensorType.
  if (!sourceType || !targetType)
    return false;

  // Requires same elemental type.
  if (sourceType.getElementType() != targetType.getElementType())
    return false;

  // Requires same rank.
  if (sourceType.getRank() != targetType.getRank())
    return false;

  // If cast is towards more static sizes along any dimension, don't fold.
  for (auto t : llvm::zip(sourceType.getShape(), targetType.getShape())) {
    if (!ShapedType::isDynamic(std::get<0>(t)) &&
        ShapedType::isDynamic(std::get<1>(t)))
      return false;
  }

  return true;
}

/// Determines whether tensor::CastOp casts to a more dynamic version of the
/// source tensor. This is useful to fold a tensor.cast into a consuming op and
/// implement canonicalization patterns for ops in different dialects that may
/// consume the results of tensor.cast operations. Such foldable tensor.cast
/// operations are typically inserted as `slice` ops and are canonicalized,
/// to preserve the type compatibility of their uses.
///
/// Returns true when all conditions are met:
/// 1. source and result are ranked tensors with same element type and rank.
/// 2. the tensor type has more static information than the result
///
/// Example:
/// ```mlir
///   %1 = tensor.cast %0 : tensor<8x16xf32> to tensor<?x?xf32>
///   %2 = consumer %1 ... : tensor<?x?xf32> ...
/// ```
///
/// folds into:
///
/// ```mlir
///   %2 = consumer %0 ... : tensor<8x16xf32> ...
/// ```
bool mlir::tensor::canFoldIntoConsumerOp(CastOp castOp) {
  if (!castOp)
    return false;

  // Can fold if the source of cast has at least as much static information as
  // its results.
  return preservesStaticInformation(castOp.getType(),
                                    castOp.getSource().getType());
}

/// Determines whether the tensor::CastOp casts to a more static version of the
/// source tensor. This is useful to fold into a producing op and implement
/// canonicaliation patterns with the `tensor.cast` op as the root, but producer
/// being from different dialects. Returns true when all conditions are met:
/// 1. source and result and ranked tensors with same element type and rank.
/// 2. the result type has more static information than the source.
///
/// Example:
/// ```mlir
///   %1 = producer ... : tensor<?x?xf32>
///   %2 = tensor.cast %1 : tensor<?x?xf32> to tensor<8x16xf32>
/// ```
///
/// can be canonicalized to :
///
/// ```mlir
///   %2 = producer ... : tensor<8x16xf32>
/// ```
/// Not all ops might be canonicalizable this way, but for those that can be,
/// this method provides a check that it is worth doing the canonicalization.
bool mlir::tensor::canFoldIntoProducerOp(CastOp castOp) {
  if (!castOp)
    return false;
  return preservesStaticInformation(castOp.getSource().getType(),
                                    castOp.getType());
}

/// Performs folding of any operand of `op` if it comes from a tensor::CastOp
/// that can be folded.
LogicalResult mlir::tensor::foldTensorCast(Operation *op) {
  bool folded = false;
  for (OpOperand &operand : op->getOpOperands()) {
    auto castOp = operand.get().getDefiningOp<tensor::CastOp>();
    if (castOp && tensor::canFoldIntoConsumerOp(castOp)) {
      operand.set(castOp.getOperand());
      folded = true;
    }
  }
  return success(folded);
}

bool CastOp::areCastCompatible(TypeRange inputs, TypeRange outputs) {
  if (inputs.size() != 1 || outputs.size() != 1)
    return false;
  Type a = inputs.front(), b = outputs.front();
  auto aT = a.dyn_cast<TensorType>();
  auto bT = b.dyn_cast<TensorType>();
  if (!aT || !bT)
    return false;

  if (aT.getElementType() != bT.getElementType())
    return false;

  return succeeded(verifyCompatibleShape(aT, bT));
}

/// Compute a TensorType that has the joined shape knowledge of the two
/// given TensorTypes. The element types need to match.
static TensorType joinShapes(TensorType one, TensorType two) {
  assert(one.getElementType() == two.getElementType());

  if (!one.hasRank())
    return two;
  if (!two.hasRank())
    return one;

  int64_t rank = one.getRank();
  if (rank != two.getRank())
    return {};

  SmallVector<int64_t, 4> join;
  join.reserve(rank);
  for (int64_t i = 0; i < rank; ++i) {
    if (one.isDynamicDim(i)) {
      join.push_back(two.getDimSize(i));
      continue;
    }
    if (two.isDynamicDim(i)) {
      join.push_back(one.getDimSize(i));
      continue;
    }
    if (one.getDimSize(i) != two.getDimSize(i))
      return {};
    join.push_back(one.getDimSize(i));
  }
  return RankedTensorType::get(join, one.getElementType());
}

namespace {

/// Replaces chains of two tensor.cast operations by a single tensor.cast
/// operation if doing so does not remove runtime constraints.
struct ChainedTensorCast : public OpRewritePattern<CastOp> {
  using OpRewritePattern<CastOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(CastOp tensorCast,
                                PatternRewriter &rewriter) const final {
    auto tensorCastOperand = tensorCast.getOperand().getDefiningOp<CastOp>();

    if (!tensorCastOperand)
      return failure();

    auto sourceType =
        tensorCastOperand.getOperand().getType().cast<TensorType>();
    auto intermediateType = tensorCastOperand.getType().cast<TensorType>();
    auto resultType = tensorCast.getType().cast<TensorType>();

    // We can remove the intermediate cast if joining all three produces the
    // same result as just joining the source and result shapes.
    auto firstJoin =
        joinShapes(joinShapes(sourceType, intermediateType), resultType);

    // The join might not exist if the cast sequence would fail at runtime.
    if (!firstJoin)
      return failure();

    // The newJoin always exists if the above join exists, it might just contain
    // less information. If so, we cannot drop the intermediate cast, as doing
    // so would remove runtime checks.
    auto newJoin = joinShapes(sourceType, resultType);
    if (firstJoin != newJoin)
      return failure();

    rewriter.replaceOpWithNewOp<CastOp>(tensorCast, resultType,
                                        tensorCastOperand.getOperand());
    return success();
  }
};

/// Fold tensor.cast into tesor.extract_slice producer.
/// Example:
/// ```
///  %0 = tensor.extract_slice %arg0[%o, 0] [%s, 512] [1, 1] :
///    tensor<128x512xf32> to tensor<?x512xf32>
///  %1 = tensor.cast %0 : tensor<?x512xf32> to tensor<16x512xf32>
/// ```
/// ->
/// ```
/// %1 = tensor.extract_slice %arg0[%o, 0] [16, 512] [1, 1] :
///   tensor<128x512xf32> to tensor<16x512xf32>
/// ```
struct TensorCastExtractSlice : public OpRewritePattern<CastOp> {
  using OpRewritePattern<CastOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(CastOp tensorCast,
                                PatternRewriter &rewriter) const final {
    auto extractOperand =
        tensorCast.getOperand().getDefiningOp<ExtractSliceOp>();

    if (!extractOperand || !canFoldIntoProducerOp(tensorCast) ||
        tensorCast.getType().getShape() == tensorCast.getSource()
                                               .getType()
                                               .cast<RankedTensorType>()
                                               .getShape())
      return failure();

    SmallVector<OpFoldResult, 4> sizes = extractOperand.getMixedSizes();
    auto dimMask = computeRankReductionMask(
        extractFromI64ArrayAttr(extractOperand.getStaticSizes()),
        extractOperand.getType().getShape());
    size_t dimIndex = 0;
    for (size_t i = 0, e = sizes.size(); i < e; i++) {
      if (dimMask && dimMask->count(i))
        continue;
      int64_t dim = tensorCast.getType().getShape()[dimIndex++];
      if (ShapedType::isDynamic(dim))
        continue;
      sizes[i] = rewriter.getIndexAttr(dim);
    }

    rewriter.replaceOpWithNewOp<ExtractSliceOp>(
        tensorCast, tensorCast.getType().cast<RankedTensorType>(),
        extractOperand.getSource(), extractOperand.getMixedOffsets(), sizes,
        extractOperand.getMixedStrides());
    return success();
  }
};

} // namespace

void CastOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                         MLIRContext *context) {
  results.add<ChainedTensorCast, TensorCastExtractSlice>(context);
}

//===----------------------------------------------------------------------===//
// DimOp
//===----------------------------------------------------------------------===//

void DimOp::getAsmResultNames(function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "dim");
}

void DimOp::build(OpBuilder &builder, OperationState &result, Value source,
                  int64_t index) {
  auto loc = result.location;
  Value indexValue = builder.create<arith::ConstantIndexOp>(loc, index);
  build(builder, result, source, indexValue);
}

Optional<int64_t> DimOp::getConstantIndex() {
  if (auto constantOp = getIndex().getDefiningOp<arith::ConstantOp>())
    return constantOp.getValue().cast<IntegerAttr>().getInt();
  return {};
}

Speculation::Speculatability DimOp::getSpeculatability() {
  auto constantIndex = getConstantIndex();
  if (!constantIndex)
    return Speculation::NotSpeculatable;

  auto rankedSourceType = dyn_cast<RankedTensorType>(getSource().getType());
  if (!rankedSourceType)
    return Speculation::NotSpeculatable;

  // The verifier rejects operations that violate this assertion.
  assert(constantIndex < rankedSourceType.getRank());
  return Speculation::Speculatable;
}

LogicalResult DimOp::verify() {
  // Assume unknown index to be in range.
  Optional<int64_t> index = getConstantIndex();
  if (!index)
    return success();

  // Check that constant index is not knowingly out of range.
  auto type = getSource().getType();
  if (auto tensorType = type.dyn_cast<RankedTensorType>()) {
    if (*index >= tensorType.getRank())
      return emitOpError("index is out of range");
  } else if (type.isa<UnrankedTensorType>()) {
    // Assume index to be in range.
  } else {
    llvm_unreachable("expected operand with tensor type");
  }
  return success();
}

OpFoldResult DimOp::fold(ArrayRef<Attribute> operands) {
  // All forms of folding require a known index.
  auto index = operands[1].dyn_cast_or_null<IntegerAttr>();
  if (!index)
    return {};

  // Folding for unranked types (UnrankedTensorType) is not supported.
  auto tensorType = getSource().getType().dyn_cast<RankedTensorType>();
  if (!tensorType)
    return {};

  // Fold if the shape extent along the given index is known.
  if (!tensorType.isDynamicDim(index.getInt())) {
    Builder builder(getContext());
    return builder.getIndexAttr(tensorType.getShape()[index.getInt()]);
  }

  Operation *definingOp = getSource().getDefiningOp();

  // Fold dim to the operand of tensor.generate.
  if (auto fromElements = dyn_cast_or_null<tensor::GenerateOp>(definingOp)) {
    auto resultType =
        fromElements.getResult().getType().cast<RankedTensorType>();
    // The case where the type encodes the size of the dimension is handled
    // above.
    assert(ShapedType::isDynamic(resultType.getShape()[index.getInt()]));

    // Find the operand of the fromElements that corresponds to this index.
    auto dynExtents = fromElements.getDynamicExtents().begin();
    for (auto dim : resultType.getShape().take_front(index.getInt()))
      if (ShapedType::isDynamic(dim))
        dynExtents++;

    return Value{*dynExtents};
  }

  // The size at the given index is now known to be a dynamic size.
  unsigned unsignedIndex = index.getValue().getZExtValue();

  if (auto sliceOp = dyn_cast_or_null<tensor::ExtractSliceOp>(definingOp)) {
    // Fold only for non-rank reduced ops. For the rank-reduced version, rely on
    // `resolve-shaped-type-result-dims` pass.
    if (sliceOp.getType().getRank() == sliceOp.getSourceType().getRank() &&
        sliceOp.isDynamicSize(unsignedIndex)) {
      return {sliceOp.getDynamicSize(unsignedIndex)};
    }
  }

  // dim(cast) -> dim
  if (succeeded(foldTensorCast(*this)))
    return getResult();

  return {};
}

namespace {
/// Fold dim of a cast into the dim of the source of the tensor cast.
struct DimOfCastOp : public OpRewritePattern<DimOp> {
  using OpRewritePattern<DimOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(DimOp dimOp,
                                PatternRewriter &rewriter) const override {
    auto castOp = dimOp.getSource().getDefiningOp<CastOp>();
    if (!castOp)
      return failure();
    Value newSource = castOp.getOperand();
    rewriter.replaceOpWithNewOp<DimOp>(dimOp, newSource, dimOp.getIndex());
    return success();
  }
};
} // namespace

void DimOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                        MLIRContext *context) {
  results.add<DimOfCastOp>(context);
}

//===----------------------------------------------------------------------===//
// EmptyOp
//===----------------------------------------------------------------------===//

void EmptyOp::build(OpBuilder &builder, OperationState &result,
                    ArrayRef<int64_t> staticShape, Type elementType,
                    Attribute encoding) {
  assert(all_of(staticShape,
                [](int64_t sz) { return !ShapedType::isDynamic(sz); }) &&
         "expected only static sizes");
  build(builder, result, staticShape, elementType, ValueRange{}, encoding);
}

void EmptyOp::build(OpBuilder &builder, OperationState &result,
                    ArrayRef<int64_t> staticShape, Type elementType,
                    ValueRange dynamicSizes, Attribute encoding) {
  auto tensorType = RankedTensorType::get(staticShape, elementType, encoding);
  build(builder, result, tensorType, dynamicSizes);
}

void EmptyOp::build(OpBuilder &builder, OperationState &result,
                    ArrayRef<OpFoldResult> sizes, Type elementType,
                    Attribute encoding) {
  SmallVector<int64_t> staticShape;
  SmallVector<Value> dynamicSizes;
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticShape,
                             ShapedType::kDynamic);
  build(builder, result, staticShape, elementType, dynamicSizes, encoding);
}

LogicalResult EmptyOp::verify() {
  if (getType().getNumDynamicDims() !=
      static_cast<int64_t>(getDynamicSizes().size()))
    return emitOpError("incorrect number of dynamic sizes, has ")
           << getDynamicSizes().size() << ", expected "
           << getType().getNumDynamicDims();
  return success();
}

LogicalResult
EmptyOp::reifyResultShapes(OpBuilder &builder,
                           ReifiedRankedShapedTypeDims &reifiedReturnShapes) {
  reifiedReturnShapes.resize(1, SmallVector<Value>(getType().getRank()));
  unsigned ctr = 0;
  for (int64_t i = 0; i < getType().getRank(); ++i) {
    if (getType().isDynamicDim(i)) {
      reifiedReturnShapes[0][i] = getDynamicSizes()[ctr++];
    } else {
      reifiedReturnShapes[0][i] =
          builder.create<arith::ConstantIndexOp>(getLoc(), i);
    }
  }
  return success();
}

Value EmptyOp::getDynamicSize(unsigned idx) {
  assert(getType().isDynamicDim(idx) && "expected dynamic dim");
  unsigned ctr = 0;
  for (int64_t i = 0; i < static_cast<int64_t>(idx); ++i)
    if (getType().isDynamicDim(i))
      ++ctr;
  return getDynamicSizes()[ctr];
}

SmallVector<OpFoldResult> EmptyOp::getMixedSizes() {
  SmallVector<OpFoldResult> result;
  unsigned ctr = 0;
  OpBuilder b(getContext());
  for (int64_t i = 0; i < getType().getRank(); ++i) {
    if (getType().isDynamicDim(i)) {
      result.push_back(getDynamicSizes()[ctr++]);
    } else {
      result.push_back(b.getIndexAttr(getType().getShape()[i]));
    }
  }
  return result;
}

namespace {
/// Change the type of the result of a `tensor.empty` by making the result
/// type statically sized along dimensions that in the original operation were
/// defined as dynamic, but the size was defined using a `constant` op. For
/// example
///
///  %c5 = arith.constant 5: index
///  %0 = tensor.empty(%arg0, %c5) : tensor<?x?xf32>
///
///  to
///
///  %0 = tensor.empty(%arg0) : tensor<?x5xf32>
struct ReplaceEmptyTensorStaticShapeDims : OpRewritePattern<EmptyOp> {
  using OpRewritePattern<EmptyOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(EmptyOp op,
                                PatternRewriter &rewriter) const override {
    SmallVector<int64_t> staticShape(op.getType().getShape().begin(),
                                     op.getType().getShape().end());
    SmallVector<Value> dynamicSizes;

    // Compute new static and dynamic sizes.
    unsigned ctr = 0;
    bool changedType = false;
    for (int64_t i = 0; i < op.getType().getRank(); ++i) {
      if (op.getType().isDynamicDim(i)) {
        Value dynamicSize = op.getDynamicSizes()[ctr++];
        Optional<int64_t> cst = getConstantIntValue(dynamicSize);
        if (cst.has_value()) {
          staticShape[i] = *cst;
          changedType = true;
        } else {
          dynamicSizes.push_back(dynamicSize);
        }
      }
    }

    // Stop here if no dynamic size was promoted to static.
    if (!changedType)
      return failure();

    auto tensorType = RankedTensorType::get(
        staticShape, op.getType().getElementType(), op.getType().getEncoding());
    auto newOp =
        rewriter.create<EmptyOp>(op.getLoc(), tensorType, dynamicSizes);
    rewriter.replaceOpWithNewOp<tensor::CastOp>(op, op.getType(), newOp);
    return success();
  }
};

/// `tensor.empty` does not define any tensor contents, so a slice of a
/// `tensor.empty` can be canonicalized to a smaller `tensor.empty`.
struct FoldEmptyTensorWithExtractSliceOp
    : public OpRewritePattern<ExtractSliceOp> {
  using OpRewritePattern<ExtractSliceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ExtractSliceOp sliceOp,
                                PatternRewriter &rewriter) const override {
    if (!sliceOp.getSource().getDefiningOp<EmptyOp>())
      return failure();

    // ExtractSliceOp may be rank-reducing; its dynamic sizes must be
    // preserved as well as its result type.
    auto tensorType = RankedTensorType::get(sliceOp.getType().getShape(),
                                            sliceOp.getType().getElementType(),
                                            sliceOp.getType().getEncoding());
    rewriter.replaceOpWithNewOp<EmptyOp>(sliceOp, tensorType,
                                         sliceOp.getSizes());
    return success();
  }
};

template <typename ReshapeOp>
struct FoldEmptyTensorWithReshapeOp : public OpRewritePattern<ReshapeOp> {
  using OpRewritePattern<ReshapeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ReshapeOp reshapeOp,
                                PatternRewriter &rewriter) const override {
    if (!reshapeOp.getSrc().template getDefiningOp<EmptyOp>())
      return failure();
    Location loc = reshapeOp.getLoc();
    ReifiedRankedShapedTypeDims resultShapes;
    ReifyRankedShapedTypeOpInterface reifyShapedTypeInterface =
        cast<ReifyRankedShapedTypeOpInterface>(reshapeOp.getOperation());
    if (failed(reifyShapedTypeInterface.reifyResultShapes(rewriter,
                                                          resultShapes)) ||
        !llvm::hasSingleElement(resultShapes))
      return failure();
    // TODO: Do not drop tensor type encoding.
    Value emptyTensor =
        rewriter.create<EmptyOp>(loc, getAsOpFoldResult(resultShapes[0]),
                                 reshapeOp.getResultType().getElementType());
    if (emptyTensor.getType() != reshapeOp.getResultType()) {
      rewriter.replaceOpWithNewOp<tensor::CastOp>(
          reshapeOp, reshapeOp.getResultType(), emptyTensor);
    } else {
      rewriter.replaceOp(reshapeOp, emptyTensor);
    }
    return success();
  }
};

struct FoldEmptyTensorWithDimOp : public OpRewritePattern<DimOp> {
  using OpRewritePattern<DimOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::DimOp dimOp,
                                PatternRewriter &rewriter) const override {
    Optional<int64_t> maybeConstantIndex = dimOp.getConstantIndex();
    auto emptyTensorOp = dimOp.getSource().getDefiningOp<EmptyOp>();
    if (!emptyTensorOp || !maybeConstantIndex)
      return failure();
    if (!emptyTensorOp.getType().isDynamicDim(*maybeConstantIndex))
      return failure();
    rewriter.replaceOp(dimOp,
                       emptyTensorOp.getDynamicSize(*maybeConstantIndex));
    return success();
  }
};

/// Canonicalize
///
/// ```mlir
///   %0 = tensor.empty(%d0, %d1) : tensor<?x?xf32>
///   %1 = tensor.cast %0 : tensor<?x?xf32> to tensor<4x?xf32>
/// ```
///
/// into
///
/// ```mlir
///   %0 = tensor.empty(%d1) : tensor<4x?xf32>
/// ```
///
/// This assumes the input program is correct in terms of its shape. So it is
/// safe to assume that `%d0` is in fact 4.
struct FoldEmptyTensorWithCastOp : public OpRewritePattern<CastOp> {
  using OpRewritePattern<CastOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(CastOp castOp,
                                PatternRewriter &rewriter) const override {
    if (!canFoldIntoProducerOp(castOp))
      return failure();
    auto producer = castOp.getSource().getDefiningOp<EmptyOp>();
    if (!producer)
      return failure();

    auto resultType = castOp->getResult(0).getType().cast<RankedTensorType>();
    ArrayRef<int64_t> resultShape = resultType.getShape();
    SmallVector<OpFoldResult> currMixedSizes = producer.getMixedSizes();
    SmallVector<OpFoldResult> newMixedSizes;
    newMixedSizes.reserve(currMixedSizes.size());
    assert(resultShape.size() == currMixedSizes.size() &&
           "mismatch in result shape and sizes of empty op");
    for (auto it : llvm::zip(resultShape, currMixedSizes)) {
      int64_t newDim = std::get<0>(it);
      OpFoldResult currDim = std::get<1>(it);
      // Case 1: The empty tensor dim is static. Check that the tensor cast
      // result dim matches.
      if (auto attr = currDim.dyn_cast<Attribute>()) {
        if (ShapedType::isDynamic(newDim) ||
            newDim != attr.cast<IntegerAttr>().getInt()) {
          // Something is off, the cast result shape cannot be more dynamic
          // than the empty tensor result shape (enforced by
          // `canFoldIntoProducer`). Abort for now.
          return rewriter.notifyMatchFailure(
              producer, "mismatch in static value of shape of empty tensor "
                        "result and cast result");
        }
        newMixedSizes.push_back(attr);
        continue;
      }

      // Case 2 : The tensor cast shape is static, but empty tensor result
      // shape is dynamic.
      if (!ShapedType::isDynamic(newDim)) {
        newMixedSizes.push_back(rewriter.getIndexAttr(newDim));
        continue;
      }

      // Case 3 : The tensor cast shape is dynamic and empty tensor result
      // shape is dynamic. Use the dynamic value from the empty tensor op.
      newMixedSizes.push_back(currDim);
    }

    // TODO: Do not drop tensor encoding.
    rewriter.replaceOpWithNewOp<EmptyOp>(castOp, newMixedSizes,
                                         resultType.getElementType());
    return success();
  }
};

} // namespace

void EmptyOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                          MLIRContext *context) {
  results.add<FoldEmptyTensorWithCastOp, FoldEmptyTensorWithDimOp,
              FoldEmptyTensorWithExtractSliceOp,
              FoldEmptyTensorWithReshapeOp<tensor::ExpandShapeOp>,
              FoldEmptyTensorWithReshapeOp<tensor::CollapseShapeOp>,
              ReplaceEmptyTensorStaticShapeDims>(context);
}

//===----------------------------------------------------------------------===//
// ExtractOp
//===----------------------------------------------------------------------===//

namespace {

/// Canonicalizes the pattern of the form
///
/// %val = tensor.cast %source : : tensor<?xi32> to tensor<2xi32>
/// %extracted_element = tensor.extract %val[%c0] : tensor<2xi32>
///
/// to
///
/// %extracted_element = tensor.extract %source[%c0] : tensor<?xi32>
struct ExtractFromTensorCast : public OpRewritePattern<tensor::ExtractOp> {
  using OpRewritePattern<tensor::ExtractOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::ExtractOp extract,
                                PatternRewriter &rewriter) const final {
    auto tensorCast = extract.getTensor().getDefiningOp<tensor::CastOp>();
    if (!tensorCast)
      return failure();
    if (!tensorCast.getSource().getType().isa<RankedTensorType>())
      return failure();
    rewriter.replaceOpWithNewOp<tensor::ExtractOp>(
        extract, tensorCast.getSource(), extract.getIndices());
    return success();
  }
};

} // namespace

void ExtractOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "extracted");
}

LogicalResult ExtractOp::verify() {
  // Verify the # indices match if we have a ranked type.
  auto tensorType = getTensor().getType().cast<RankedTensorType>();
  if (tensorType.getRank() != static_cast<int64_t>(getIndices().size()))
    return emitOpError("incorrect number of indices for extract_element");
  return success();
}

OpFoldResult ExtractOp::fold(ArrayRef<Attribute> operands) {
  // If this is a splat elements attribute, simply return the value. All of
  // the elements of a splat attribute are the same.
  if (Attribute tensor = operands.front())
    if (auto splatTensor = tensor.dyn_cast<SplatElementsAttr>())
      return splatTensor.getSplatValue<Attribute>();

  // Collect the constant indices into the tensor.
  SmallVector<uint64_t, 8> indices;
  for (Attribute indice : llvm::drop_begin(operands, 1)) {
    if (!indice || !indice.isa<IntegerAttr>())
      return {};
    indices.push_back(indice.cast<IntegerAttr>().getInt());
  }

  // Fold extract(from_elements(...)).
  if (auto fromElementsOp = getTensor().getDefiningOp<FromElementsOp>()) {
    auto tensorType = fromElementsOp.getType().cast<RankedTensorType>();
    auto rank = tensorType.getRank();
    assert(static_cast<int64_t>(indices.size()) == tensorType.getRank() &&
           "rank mismatch");
    int flatIndex = 0;
    int stride = 1;
    for (int i = rank - 1; i >= 0; --i) {
      if (i < rank - 1)
        stride *= tensorType.getDimSize(i);
      flatIndex += indices[i] * stride;
    }
    // Prevent out of bounds accesses. This can happen in invalid code that
    // will never execute.
    if (static_cast<int>(fromElementsOp.getElements().size()) <= flatIndex ||
        flatIndex < 0)
      return {};
    return fromElementsOp.getElements()[flatIndex];
  }

  // If this is an elements attribute, query the value at the given indices.
  if (Attribute tensor = operands.front()) {
    auto elementsAttr = tensor.dyn_cast<ElementsAttr>();
    if (elementsAttr && elementsAttr.isValidIndex(indices))
      return elementsAttr.getValues<Attribute>()[indices];
  }

  return {};
}

void ExtractOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                            MLIRContext *context) {
  results.add<ExtractFromTensorCast>(context);
}

//===----------------------------------------------------------------------===//
// FromElementsOp
//===----------------------------------------------------------------------===//

void FromElementsOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "from_elements");
}

void FromElementsOp::build(OpBuilder &builder, OperationState &result,
                           Type resultType, ValueRange elements) {
  result.addOperands(elements);
  result.addTypes(resultType);
}

void FromElementsOp::build(OpBuilder &builder, OperationState &result,
                           ValueRange elements) {
  assert(!elements.empty() && "expected at least one element");
  Type resultType = RankedTensorType::get(
      {static_cast<int64_t>(elements.size())}, elements.front().getType());
  build(builder, result, resultType, elements);
}

OpFoldResult FromElementsOp::fold(ArrayRef<Attribute> operands) {
  if (!llvm::is_contained(operands, nullptr))
    return DenseElementsAttr::get(getType(), operands);
  return {};
}

namespace {

// Pushes the index_casts that occur before extractions to after the extract.
// This minimizes type conversion in some cases and enables the extract
// canonicalizer. This changes:
//
// %cast = arith.index_cast %tensor : tensor<1xi32> to tensor<1xindex>
// %extract = tensor.extract %cast[%index] : tensor<1xindex>
//
// to the following:
//
// %extract = tensor.extract %tensor[%index] : tensor<1xindex>
// %cast = arith.index_cast %extract : i32 to index
//
// to just %element.
//
// Consider expanding this to a template and handle all tensor cast
// operations.
struct ExtractElementFromIndexCast
    : public OpRewritePattern<tensor::ExtractOp> {
  using OpRewritePattern<tensor::ExtractOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::ExtractOp extract,
                                PatternRewriter &rewriter) const final {
    Location loc = extract.getLoc();
    auto indexCast = extract.getTensor().getDefiningOp<arith::IndexCastOp>();
    if (!indexCast)
      return failure();

    Type elementTy = getElementTypeOrSelf(indexCast.getIn());

    auto newExtract = rewriter.create<tensor::ExtractOp>(
        loc, elementTy, indexCast.getIn(), extract.getIndices());

    rewriter.replaceOpWithNewOp<arith::IndexCastOp>(extract, extract.getType(),
                                                    newExtract);

    return success();
  }
};

} // namespace

void FromElementsOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                 MLIRContext *context) {
  results.add<ExtractElementFromIndexCast>(context);
}

//===----------------------------------------------------------------------===//
// GatherOp
//===----------------------------------------------------------------------===//

void GatherOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "gather");
}

/// Return the inferred result type for a gatherOp where:
///   - sourceType is the type of the source tensor gathered from
///   - indicesType is the type of the indices used to gather
///   - gatherDims are the dims along which the gather occurs.
/// Return a full rank or ranked-reduced variant of the type depending on
/// the value of rankReduced.
///
/// The leading dimensions of the index tensor give the result tensor its
/// leading dimensions.
/// The trailing dimensions of the result tensor are obtained from the source
/// tensor by setting the dimensions specified in gather_dims to `1` (if
/// rankedReduced is false), or skipping them (otherwise).
RankedTensorType GatherOp::inferResultType(RankedTensorType sourceType,
                                           RankedTensorType indicesType,
                                           ArrayRef<int64_t> gatherDims,
                                           bool rankReduced) {
  SmallVector<int64_t> resultShape(indicesType.getShape().drop_back());
  resultShape.reserve(resultShape.size() + sourceType.getRank());
  for (int64_t idx : llvm::seq<int64_t>(0, sourceType.getRank())) {
    if (std::binary_search(gatherDims.begin(), gatherDims.end(), idx)) {
      if (!rankReduced)
        resultShape.push_back(1);
      continue;
    }
    resultShape.push_back(sourceType.getDimSize(idx));
  }
  return RankedTensorType::Builder(sourceType).setShape(resultShape);
}

static LogicalResult
verifyGatherOrScatterDims(Operation *op, ArrayRef<int64_t> dims, int64_t rank,
                          StringRef gatherOrScatter, StringRef sourceOrDest) {
  if (dims.empty())
    return op->emitOpError(gatherOrScatter) << "_dims must be non-empty";

  int64_t numGatherDims = dims.size();
  if (numGatherDims > rank)
    return op->emitOpError(gatherOrScatter)
           << "_dims overflow " << sourceOrDest << " rank";
  for (int64_t val : dims) {
    if (val < 0)
      return op->emitOpError(gatherOrScatter)
             << "_dims value must be non-negative";
    if (val >= rank)
      return op->emitOpError(gatherOrScatter)
             << "_dims value must be smaller than " << sourceOrDest << " rank";
  }
  for (int64_t i = 1; i < numGatherDims; ++i) {
    if (dims[i - 1] >= dims[i])
      return op->emitOpError(gatherOrScatter)
             << "_dims values must be strictly increasing";
  }
  return success();
}

LogicalResult GatherOp::verify() {
  int64_t sourceRank = getSourceType().getRank();
  ArrayRef<int64_t> gatherDims = getGatherDims();
  if (failed(verifyGatherOrScatterDims(getOperation(), gatherDims, sourceRank,
                                       "gather", "source")))
    return failure();

  RankedTensorType expectedResultType = GatherOp::inferResultType(
      getSourceType(), getIndicesType(), gatherDims, /*rankReduced=*/false);
  RankedTensorType expectedRankReducedResultType = GatherOp::inferResultType(
      getSourceType(), getIndicesType(), gatherDims, /*rankReduced=*/true);
  if (getResultType() != expectedResultType &&
      getResultType() != expectedRankReducedResultType) {
    return emitOpError("result type "
                       "mismatch: "
                       "expected ")
           << expectedResultType << " or its rank-reduced variant "
           << expectedRankReducedResultType << " (got: " << getResultType()
           << ")";
  }

  return success();
}

//===----------------------------------------------------------------------===//
// InsertOp
//===----------------------------------------------------------------------===//

void InsertOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "inserted");
}

LogicalResult InsertOp::verify() {
  // Verify the # indices match if we have a ranked type.
  auto destType = getDest().getType().cast<RankedTensorType>();
  if (destType.getRank() != static_cast<int64_t>(getIndices().size()))
    return emitOpError("incorrect number of indices");
  return success();
}

OpFoldResult InsertOp::fold(ArrayRef<Attribute> operands) {
  Attribute scalar = operands[0];
  Attribute dest = operands[1];
  if (scalar && dest)
    if (auto splatDest = dest.dyn_cast<SplatElementsAttr>())
      if (scalar == splatDest.getSplatValue<Attribute>())
        return dest;
  return {};
}

//===----------------------------------------------------------------------===//
// GenerateOp
//===----------------------------------------------------------------------===//

void GenerateOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "generated");
}

LogicalResult GenerateOp::reifyResultShapes(
    OpBuilder &builder, ReifiedRankedShapedTypeDims &reifiedReturnShapes) {
  reifiedReturnShapes.resize(1, SmallVector<Value>(getType().getRank()));
  int idx = 0;
  for (auto dim : llvm::seq<int64_t>(0, getType().getRank())) {
    if (getType().isDynamicDim(dim)) {
      reifiedReturnShapes[0][dim] = getOperand(idx++);
    } else {
      reifiedReturnShapes[0][dim] = builder.create<arith::ConstantIndexOp>(
          getLoc(), getType().getDimSize(dim));
    }
  }
  return success();
}

LogicalResult GenerateOp::verify() {
  // Ensure that the tensor type has as many dynamic dimensions as are
  // specified by the operands.
  RankedTensorType resultTy = getType().cast<RankedTensorType>();
  if (getNumOperands() != resultTy.getNumDynamicDims())
    return emitError("must have as many index operands as dynamic extents "
                     "in the result type");

  return success();
}

LogicalResult GenerateOp::verifyRegions() {
  RankedTensorType resultTy = getType().cast<RankedTensorType>();
  // Ensure that region arguments span the index space.
  if (!llvm::all_of(getBody().getArgumentTypes(),
                    [](Type ty) { return ty.isIndex(); }))
    return emitError("all body arguments must be index");
  if (getBody().getNumArguments() != resultTy.getRank())
    return emitError("must have one body argument per input dimension");

  // Ensure that the region yields an element of the right type.
  auto yieldOp = cast<YieldOp>(getBody().getBlocks().front().getTerminator());

  if (yieldOp.getValue().getType() != resultTy.getElementType())
    return emitOpError(
        "body must be terminated with a `yield` operation of the tensor "
        "element type");

  return success();
}

void GenerateOp::build(
    OpBuilder &b, OperationState &result, Type resultTy,
    ValueRange dynamicExtents,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuilder) {
  build(b, result, resultTy, dynamicExtents);

  // Build and populate body.
  OpBuilder::InsertionGuard guard(b);
  Region *bodyRegion = result.regions.front().get();
  auto rank = resultTy.cast<RankedTensorType>().getRank();
  SmallVector<Type, 2> argumentTypes(rank, b.getIndexType());
  SmallVector<Location, 2> argumentLocs(rank, result.location);
  Block *bodyBlock =
      b.createBlock(bodyRegion, bodyRegion->end(), argumentTypes, argumentLocs);
  bodyBuilder(b, result.location, bodyBlock->getArguments());
}

namespace {

/// Canonicalizes tensor.generate operations with a constant
/// operand into the equivalent operation with the operand expressed in the
/// result type, instead. We also insert a type cast to make sure that the
/// resulting IR is still well-typed.
struct StaticTensorGenerate : public OpRewritePattern<GenerateOp> {
  using OpRewritePattern<GenerateOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(GenerateOp tensorFromElements,
                                PatternRewriter &rewriter) const final {
    auto resultType =
        tensorFromElements.getResult().getType().cast<RankedTensorType>();

    if (resultType.hasStaticShape())
      return failure();

    SmallVector<Value, 4> newOperands;
    SmallVector<int64_t, 4> newShape;
    auto operandsIt = tensorFromElements.getDynamicExtents().begin();

    for (int64_t dim : resultType.getShape()) {
      if (!ShapedType::isDynamic(dim)) {
        newShape.push_back(dim);
        continue;
      }
      APInt index;
      if (!matchPattern(*operandsIt, m_ConstantInt(&index))) {
        newShape.push_back(ShapedType::kDynamic);
        newOperands.push_back(*operandsIt++);
        continue;
      }
      newShape.push_back(index.getSExtValue());
      operandsIt++;
    }

    if (newOperands.size() == tensorFromElements.getDynamicExtents().size())
      return failure();

    auto loc = tensorFromElements.getLoc();
    auto newOp = rewriter.create<GenerateOp>(
        loc, RankedTensorType::get(newShape, resultType.getElementType()),
        newOperands);
    rewriter.inlineRegionBefore(tensorFromElements.getBody(), newOp.getBody(),
                                newOp.getBody().begin());
    rewriter.replaceOpWithNewOp<tensor::CastOp>(tensorFromElements, resultType,
                                                newOp);
    return success();
  }
};

/// Canonicalizes the pattern of the form
///
/// %tensor = tensor.generate %x {
///   ^bb0(%arg0: index):
///   <computation>
///   yield %1 : index
/// } : tensor<?xindex>
/// %extracted_element = tensor.extract %tensor[%c0] : tensor<?xi32>
///
/// to just <computation> with %arg0 replaced by %c0. We only do this if the
/// tensor.generate operation has no side-effects.
struct ExtractFromTensorGenerate : public OpRewritePattern<tensor::ExtractOp> {
  using OpRewritePattern<tensor::ExtractOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::ExtractOp extract,
                                PatternRewriter &rewriter) const final {
    auto tensorFromElements = extract.getTensor().getDefiningOp<GenerateOp>();
    if (!tensorFromElements || !wouldOpBeTriviallyDead(tensorFromElements))
      return failure();

    BlockAndValueMapping mapping;
    Block *body = &tensorFromElements.getBody().front();
    mapping.map(body->getArguments(), extract.getIndices());
    for (auto &op : body->without_terminator())
      rewriter.clone(op, mapping);

    auto yield = cast<YieldOp>(body->getTerminator());

    rewriter.replaceOp(extract, mapping.lookupOrDefault(yield.getValue()));
    return success();
  }
};

} // namespace

void GenerateOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                             MLIRContext *context) {
  // TODO: Move extract pattern to tensor::ExtractOp.
  results.add<ExtractFromTensorGenerate, StaticTensorGenerate>(context);
}

//===----------------------------------------------------------------------===//
// RankOp
//===----------------------------------------------------------------------===//

void RankOp::getAsmResultNames(function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "rank");
}

OpFoldResult RankOp::fold(ArrayRef<Attribute> operands) {
  // Constant fold rank when the rank of the operand is known.
  auto type = getOperand().getType();
  auto shapedType = type.dyn_cast<ShapedType>();
  if (shapedType && shapedType.hasRank())
    return IntegerAttr::get(IndexType::get(getContext()), shapedType.getRank());
  return IntegerAttr();
}

//===----------------------------------------------------------------------===//
// ReshapeOp
//===----------------------------------------------------------------------===//

void ReshapeOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "reshape");
}

static int64_t getNumElements(ShapedType type) {
  int64_t numElements = 1;
  for (auto dim : type.getShape())
    numElements *= dim;
  return numElements;
}

LogicalResult ReshapeOp::verify() {
  TensorType operandType = getSource().getType().cast<TensorType>();
  TensorType resultType = getResult().getType().cast<TensorType>();

  if (operandType.getElementType() != resultType.getElementType())
    return emitOpError("element types of source and destination tensor "
                       "types should be the same");

  int64_t shapeSize =
      getShape().getType().cast<RankedTensorType>().getDimSize(0);
  auto resultRankedType = resultType.dyn_cast<RankedTensorType>();
  auto operandRankedType = operandType.dyn_cast<RankedTensorType>();

  if (resultRankedType) {
    if (operandRankedType && resultRankedType.hasStaticShape() &&
        operandRankedType.hasStaticShape()) {
      if (getNumElements(operandRankedType) != getNumElements(resultRankedType))
        return emitOpError("source and destination tensor should have the "
                           "same number of elements");
    }
    if (ShapedType::isDynamic(shapeSize))
      return emitOpError("cannot use shape operand with dynamic length to "
                         "reshape to statically-ranked tensor type");
    if (shapeSize != resultRankedType.getRank())
      return emitOpError(
          "length of shape operand differs from the result's tensor rank");
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Reassociative reshape ops
//===----------------------------------------------------------------------===//

void CollapseShapeOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "collapsed");
}

void ExpandShapeOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "expanded");
}

SmallVector<AffineMap, 4> CollapseShapeOp::getReassociationMaps() {
  return getSymbolLessAffineMaps(getReassociationExprs());
}
SmallVector<ReassociationExprs, 4> CollapseShapeOp::getReassociationExprs() {
  return convertReassociationIndicesToExprs(getContext(),
                                            getReassociationIndices());
}

SmallVector<AffineMap, 4> ExpandShapeOp::getReassociationMaps() {
  return getSymbolLessAffineMaps(getReassociationExprs());
}
SmallVector<ReassociationExprs, 4> ExpandShapeOp::getReassociationExprs() {
  return convertReassociationIndicesToExprs(getContext(),
                                            getReassociationIndices());
}

/// Compute the RankedTensorType obtained by applying `reassociation` to
/// `type`.
static RankedTensorType
computeTensorReshapeCollapsedType(RankedTensorType type,
                                  ArrayRef<AffineMap> reassociation) {
  auto shape = type.getShape();
  SmallVector<int64_t, 4> newShape;
  newShape.reserve(reassociation.size());

  // Use the fact that reassociation is valid to simplify the logic: only use
  // each map's rank.
  assert(isReassociationValid(reassociation) && "invalid reassociation");
  unsigned currentDim = 0;
  for (AffineMap m : reassociation) {
    unsigned dim = m.getNumResults();
    auto band = shape.slice(currentDim, dim);
    int64_t size = 1;
    if (llvm::is_contained(band, ShapedType::kDynamic))
      size = ShapedType::kDynamic;
    else
      for (unsigned d = 0; d < dim; ++d)
        size *= shape[currentDim + d];
    newShape.push_back(size);
    currentDim += dim;
  }

  return RankedTensorType::get(newShape, type.getElementType());
}

void CollapseShapeOp::build(OpBuilder &b, OperationState &result, Value src,
                            ArrayRef<ReassociationIndices> reassociation,
                            ArrayRef<NamedAttribute> attrs) {
  auto resultType = computeTensorReshapeCollapsedType(
      src.getType().cast<RankedTensorType>(),
      getSymbolLessAffineMaps(
          convertReassociationIndicesToExprs(b.getContext(), reassociation)));
  build(b, result, resultType, src, attrs);
  result.addAttribute(getReassociationAttrStrName(),
                      getReassociationIndicesAttribute(b, reassociation));
}

// Checks if types are the same, but ignoring encoding on ranked tensors.
static bool isSameTypesWithoutEncoding(Type tp1, Type tp2) {
  if (auto rtp1 = tp1.dyn_cast<RankedTensorType>()) {
    if (auto rtp2 = tp2.dyn_cast<RankedTensorType>())
      return rtp1.getShape() == rtp2.getShape() &&
             rtp1.getElementType() == rtp2.getElementType();
    return false;
  }
  // Default implementation.
  return tp1 == tp2;
}

template <typename TensorReshapeOp, bool isExpansion = std::is_same<
                                        TensorReshapeOp, ExpandShapeOp>::value>
static LogicalResult verifyTensorReshapeOp(TensorReshapeOp op,
                                           RankedTensorType expandedType,
                                           RankedTensorType collapsedType) {
  if (failed(
          verifyReshapeLikeTypes(op, expandedType, collapsedType, isExpansion)))
    return failure();

  auto maps = op.getReassociationMaps();
  RankedTensorType expectedType =
      computeTensorReshapeCollapsedType(expandedType, maps);
  if (!isSameTypesWithoutEncoding(collapsedType, expectedType))
    return op.emitOpError("expected collapsed type to be ")
           << expectedType << ", but got " << collapsedType;
  return success();
}

LogicalResult ExpandShapeOp::verify() {
  return verifyTensorReshapeOp(*this, getResultType(), getSrcType());
}

LogicalResult CollapseShapeOp::verify() {
  return verifyTensorReshapeOp(*this, getSrcType(), getResultType());
}

namespace {
/// Reshape of a splat constant can be replaced with a constant of the result
/// type.
template <typename TensorReshapeOp>
struct FoldReshapeWithConstant : OpRewritePattern<TensorReshapeOp> {
  using OpRewritePattern<TensorReshapeOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(TensorReshapeOp reshapeOp,
                                PatternRewriter &rewriter) const override {
    DenseElementsAttr attr;
    if (!matchPattern(reshapeOp.getSrc(), m_Constant(&attr)))
      return failure();
    if (!attr || !attr.isSplat())
      return failure();
    DenseElementsAttr newAttr = DenseElementsAttr::getFromRawBuffer(
        reshapeOp.getResultType(), attr.getRawData());
    rewriter.replaceOpWithNewOp<arith::ConstantOp>(reshapeOp, newAttr);
    return success();
  }
};

/// Reshape of a FromElements can be replaced with a FromElements of the
/// result type
template <typename TensorReshapeOp>
struct FoldReshapeWithFromElements : OpRewritePattern<TensorReshapeOp> {
  using OpRewritePattern<TensorReshapeOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(TensorReshapeOp reshapeOp,
                                PatternRewriter &rewriter) const override {
    auto fromElements =
        reshapeOp.getSrc().template getDefiningOp<FromElementsOp>();
    if (!fromElements)
      return failure();

    auto shapedTy = reshapeOp.getType().template cast<ShapedType>();

    if (!shapedTy.hasStaticShape())
      return failure();

    rewriter.replaceOpWithNewOp<FromElementsOp>(reshapeOp, reshapeOp.getType(),
                                                fromElements.getElements());
    return success();
  }
};

// Fold CastOp into CollapseShapeOp when adding static information.
struct FoldCollapseOfCastOp : public OpRewritePattern<CollapseShapeOp> {
  using OpRewritePattern<CollapseShapeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(CollapseShapeOp collapseShapeOp,
                                PatternRewriter &rewriter) const override {
    auto castOp = collapseShapeOp.getSrc().getDefiningOp<tensor::CastOp>();
    if (!tensor::canFoldIntoConsumerOp(castOp))
      return failure();

    RankedTensorType srcType =
        castOp.getSource().getType().cast<RankedTensorType>();
    RankedTensorType newResultType = computeTensorReshapeCollapsedType(
        srcType, collapseShapeOp.getReassociationMaps());

    if (newResultType == collapseShapeOp.getResultType()) {
      rewriter.updateRootInPlace(collapseShapeOp, [&]() {
        collapseShapeOp.getSrcMutable().assign(castOp.getSource());
      });
    } else {
      auto newOp = rewriter.create<CollapseShapeOp>(
          collapseShapeOp.getLoc(), newResultType, castOp.getSource(),
          collapseShapeOp.getReassociation());
      rewriter.replaceOpWithNewOp<tensor::CastOp>(
          collapseShapeOp, collapseShapeOp.getResultType(), newOp);
    }
    return success();
  }
};

} // namespace

void ExpandShapeOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                MLIRContext *context) {
  results.add<ComposeReassociativeReshapeOps<ExpandShapeOp>,
              ComposeExpandOfCollapseOp<ExpandShapeOp, CollapseShapeOp>,
              FoldReshapeWithConstant<ExpandShapeOp>,
              FoldReshapeWithFromElements<ExpandShapeOp>>(context);
}

void CollapseShapeOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                  MLIRContext *context) {
  results
      .add<ComposeReassociativeReshapeOps<CollapseShapeOp>,
           ComposeCollapseOfExpandOp<CollapseShapeOp, ExpandShapeOp>,
           FoldReshapeWithConstant<CollapseShapeOp>,
           FoldReshapeWithFromElements<CollapseShapeOp>, FoldCollapseOfCastOp>(
          context);
}

OpFoldResult ExpandShapeOp::fold(ArrayRef<Attribute> operands) {
  return foldReshapeOp<ExpandShapeOp, CollapseShapeOp>(*this, operands);
}
OpFoldResult CollapseShapeOp::fold(ArrayRef<Attribute> operands) {
  return foldReshapeOp<CollapseShapeOp, ExpandShapeOp>(*this, operands);
}

//===----------------------------------------------------------------------===//
// ExtractSliceOp
//===----------------------------------------------------------------------===//

void ExtractSliceOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "extracted_slice");
}

/// An extract_slice result type can be inferred, when it is not
/// rank-reduced, from the source type and the static representation of
/// offsets, sizes and strides. Special sentinels encode the dynamic case.
RankedTensorType ExtractSliceOp::inferResultType(
    ShapedType sourceShapedTensorType, ArrayRef<int64_t> staticOffsets,
    ArrayRef<int64_t> staticSizes, ArrayRef<int64_t> staticStrides) {
  // An extract_slice op may specify only a leading subset of offset/sizes/
  // strides in which case we complete with offset=0, sizes from memref type
  // and strides=1.
  assert(static_cast<int64_t>(staticSizes.size()) ==
             sourceShapedTensorType.getRank() &&
         "unexpected staticSizes not equal to rank of source");
  return RankedTensorType::get(staticSizes,
                               sourceShapedTensorType.getElementType());
}

RankedTensorType ExtractSliceOp::inferResultType(
    ShapedType sourceShapedTensorType, ArrayRef<OpFoldResult> offsets,
    ArrayRef<OpFoldResult> sizes, ArrayRef<OpFoldResult> strides) {
  SmallVector<int64_t> staticOffsets, staticSizes, staticStrides;
  SmallVector<Value> dynamicOffsets, dynamicSizes, dynamicStrides;
  dispatchIndexOpFoldResults(offsets, dynamicOffsets, staticOffsets,
                             ShapedType::kDynamic);
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticSizes,
                             ShapedType::kDynamic);
  dispatchIndexOpFoldResults(strides, dynamicStrides, staticStrides,
                             ShapedType::kDynamic);
  return ExtractSliceOp::inferResultType(sourceShapedTensorType, staticOffsets,
                                         staticSizes, staticStrides);
}

/// If the rank is reduced (i.e. the desiredResultRank is smaller than the
/// number of sizes), drop as many size 1 as needed to produce an inferred
/// type with the desired rank.
///
/// Note that there may be multiple ways to compute this rank-reduced type:
///   e.g. 1x6x1 can rank-reduce to either 1x6 or 6x1 2-D tensors.
///
/// To disambiguate, this function always drops the first 1 sizes occurrences.
RankedTensorType ExtractSliceOp::inferCanonicalRankReducedResultType(
    unsigned desiredResultRank, RankedTensorType sourceRankedTensorType,
    ArrayRef<int64_t> offsets, ArrayRef<int64_t> sizes,
    ArrayRef<int64_t> strides) {
  // Type inferred in the absence of rank-reducing behavior.
  auto inferredType =
      inferResultType(sourceRankedTensorType, offsets, sizes, strides)
          .cast<RankedTensorType>();
  int rankDiff = inferredType.getRank() - desiredResultRank;
  if (rankDiff > 0) {
    auto shape = inferredType.getShape();
    llvm::SmallBitVector dimsToProject =
        getPositionsOfShapeOne(rankDiff, shape);
    SmallVector<int64_t> projectedShape;
    // Best effort rank-reducing: drop 1s in order.
    for (unsigned pos = 0, e = shape.size(); pos < e; ++pos)
      if (!dimsToProject.test(pos))
        projectedShape.push_back(shape[pos]);
    inferredType =
        RankedTensorType::get(projectedShape, inferredType.getElementType());
  }
  return inferredType;
}

RankedTensorType ExtractSliceOp::inferCanonicalRankReducedResultType(
    unsigned desiredResultRank, RankedTensorType sourceRankedTensorType,
    ArrayRef<OpFoldResult> offsets, ArrayRef<OpFoldResult> sizes,
    ArrayRef<OpFoldResult> strides) {
  SmallVector<int64_t> staticOffsets, staticSizes, staticStrides;
  SmallVector<Value> dynamicOffsets, dynamicSizes, dynamicStrides;
  dispatchIndexOpFoldResults(offsets, dynamicOffsets, staticOffsets,
                             ShapedType::kDynamic);
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticSizes,
                             ShapedType::kDynamic);
  dispatchIndexOpFoldResults(strides, dynamicStrides, staticStrides,
                             ShapedType::kDynamic);
  return ExtractSliceOp::inferCanonicalRankReducedResultType(
      desiredResultRank, sourceRankedTensorType, staticOffsets, staticSizes,
      staticStrides);
}

/// Build an ExtractSliceOp with mixed static and dynamic entries and custom
/// result type. If the type passed is nullptr, it is inferred.
void ExtractSliceOp::build(OpBuilder &b, OperationState &result,
                           RankedTensorType resultType, Value source,
                           ArrayRef<OpFoldResult> offsets,
                           ArrayRef<OpFoldResult> sizes,
                           ArrayRef<OpFoldResult> strides,
                           ArrayRef<NamedAttribute> attrs) {
  SmallVector<int64_t> staticOffsets, staticSizes, staticStrides;
  SmallVector<Value> dynamicOffsets, dynamicSizes, dynamicStrides;
  dispatchIndexOpFoldResults(offsets, dynamicOffsets, staticOffsets,
                             ShapedType::kDynamic);
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticSizes,
                             ShapedType::kDynamic);
  dispatchIndexOpFoldResults(strides, dynamicStrides, staticStrides,
                             ShapedType::kDynamic);
  auto sourceRankedTensorType = source.getType().cast<RankedTensorType>();
  // Structuring implementation this way avoids duplication between builders.
  if (!resultType) {
    resultType =
        ExtractSliceOp::inferResultType(sourceRankedTensorType, staticOffsets,
                                        staticSizes, staticStrides)
            .cast<RankedTensorType>();
  }
  build(b, result, resultType, source, dynamicOffsets, dynamicSizes,
        dynamicStrides, b.getI64ArrayAttr(staticOffsets),
        b.getI64ArrayAttr(staticSizes), b.getI64ArrayAttr(staticStrides));
  result.addAttributes(attrs);
}

/// Build an ExtractSliceOp with mixed static and dynamic entries and inferred
/// result type.
void ExtractSliceOp::build(OpBuilder &b, OperationState &result, Value source,
                           ArrayRef<OpFoldResult> offsets,
                           ArrayRef<OpFoldResult> sizes,
                           ArrayRef<OpFoldResult> strides,
                           ArrayRef<NamedAttribute> attrs) {
  build(b, result, RankedTensorType(), source, offsets, sizes, strides, attrs);
}

/// Build an ExtractSliceOp with mixed static and dynamic entries packed into
/// a Range vector.
void ExtractSliceOp::build(OpBuilder &b, OperationState &result, Value source,
                           ArrayRef<Range> ranges,
                           ArrayRef<NamedAttribute> attrs) {
  auto [offsets, sizes, strides] = getOffsetsSizesAndStrides(ranges);
  build(b, result, RankedTensorType(), source, offsets, sizes, strides, attrs);
}

/// Build an ExtractSliceOp with dynamic entries and custom result type. If
/// the type passed is nullptr, it is inferred.
void ExtractSliceOp::build(OpBuilder &b, OperationState &result,
                           RankedTensorType resultType, Value source,
                           ValueRange offsets, ValueRange sizes,
                           ValueRange strides, ArrayRef<NamedAttribute> attrs) {
  SmallVector<OpFoldResult> offsetValues = llvm::to_vector<4>(
      llvm::map_range(offsets, [](Value v) -> OpFoldResult { return v; }));
  SmallVector<OpFoldResult> sizeValues = llvm::to_vector<4>(
      llvm::map_range(sizes, [](Value v) -> OpFoldResult { return v; }));
  SmallVector<OpFoldResult> strideValues = llvm::to_vector<4>(
      llvm::map_range(strides, [](Value v) -> OpFoldResult { return v; }));
  build(b, result, resultType, source, offsetValues, sizeValues, strideValues);
}

/// Build an ExtractSliceOp with dynamic entries and inferred result type.
void ExtractSliceOp::build(OpBuilder &b, OperationState &result, Value source,
                           ValueRange offsets, ValueRange sizes,
                           ValueRange strides, ArrayRef<NamedAttribute> attrs) {
  build(b, result, RankedTensorType(), source, offsets, sizes, strides, attrs);
}

template <typename OpTy>
static LogicalResult produceSliceErrorMsg(SliceVerificationResult result,
                                          OpTy op, Type expectedType) {
  auto memrefType = expectedType.cast<ShapedType>();
  switch (result) {
  case SliceVerificationResult::Success:
    return success();
  case SliceVerificationResult::RankTooLarge:
    return op.emitError("expected rank to be smaller or equal to ")
           << "the other rank. ";
  case SliceVerificationResult::SizeMismatch:
    return op.emitError("expected type to be ")
           << expectedType << " or a rank-reduced version. (size mismatch) ";
  case SliceVerificationResult::ElemTypeMismatch:
    return op.emitError("expected element type to be ")
           << memrefType.getElementType();
  default:
    llvm_unreachable("unexpected extract_slice op verification result");
  }
}

/// Verifier for ExtractSliceOp.
LogicalResult ExtractSliceOp::verify() {
  // Verify result type against inferred type.
  RankedTensorType expectedType = ExtractSliceOp::inferResultType(
      getSourceType(), getMixedOffsets(), getMixedSizes(), getMixedStrides());
  SliceVerificationResult result = isRankReducedType(expectedType, getType());
  return produceSliceErrorMsg(result, *this, expectedType);
}

llvm::SmallBitVector ExtractSliceOp::getDroppedDims() {
  ArrayRef<int64_t> resultShape = getType().getShape();
  SmallVector<OpFoldResult> mixedSizes = getMixedSizes();
  llvm::SmallBitVector droppedDims(mixedSizes.size());
  unsigned shapePos = 0;
  for (const auto &size : enumerate(mixedSizes)) {
    Optional<int64_t> sizeVal = getConstantIntValue(size.value());
    // If the size is not 1, or if the current matched dimension of the result
    // is the same static shape as the size value (which is 1), then the
    // dimension is preserved.
    if (!sizeVal || *sizeVal != 1 ||
        (shapePos < resultShape.size() && resultShape[shapePos] == 1)) {
      shapePos++;
      continue;
    }
    droppedDims.set(size.index());
  }
  return droppedDims;
}

LogicalResult ExtractSliceOp::reifyResultShapes(
    OpBuilder &builder, ReifiedRankedShapedTypeDims &reifiedReturnShapes) {
  reifiedReturnShapes.resize(1);
  reifiedReturnShapes[0].reserve(getType().getRank());
  SmallVector<OpFoldResult> mixedSizes = getMixedSizes();
  llvm::SmallBitVector droppedDims = getDroppedDims();
  Location loc = getLoc();
  for (const auto &size : enumerate(mixedSizes)) {
    if (droppedDims.test(size.index()))
      continue;
    if (auto attr = size.value().dyn_cast<Attribute>()) {
      reifiedReturnShapes[0].push_back(builder.create<arith::ConstantIndexOp>(
          loc, attr.cast<IntegerAttr>().getInt()));
      continue;
    }
    reifiedReturnShapes[0].push_back(size.value().get<Value>());
  }
  return success();
}

namespace {
/// Pattern to rewrite an extract_slice op with tensor::Cast arguments.
/// This essentially pushes memref_cast past its consuming slice when
/// `canFoldIntoConsumerOp` is true.
///
/// Example:
/// ```
///   %0 = tensor.cast %V : tensor<16x16xf32> to tensor<?x?xf32>
///   %1 = tensor.extract_slice %0[0, 0][3, 4][1, 1] : tensor<?x?xf32> to
///   tensor<3x4xf32>
/// ```
/// is rewritten into:
/// ```
///   %0 = tensor.extract_slice %V[0, 0][3, 4][1, 1] : tensor<16x16xf32> to
///   tensor<3x4xf32> %1 = tensor.cast %0: tensor<3x4xf32> to tensor<3x4xf32>
/// ```
class ExtractSliceOpCastFolder final : public OpRewritePattern<ExtractSliceOp> {
public:
  using OpRewritePattern<ExtractSliceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ExtractSliceOp sliceOp,
                                PatternRewriter &rewriter) const override {
    // Any constant operand, just return to let the constant folder kick in.
    if (llvm::any_of(sliceOp.getOperands(), [](Value operand) {
          return matchPattern(operand, matchConstantIndex());
        }))
      return failure();

    auto castOp = sliceOp.getSource().getDefiningOp<tensor::CastOp>();
    if (!castOp)
      return failure();

    if (!canFoldIntoConsumerOp(castOp))
      return failure();

    /// Deduce the type of the result to use for the canonicalized operation.
    RankedTensorType resultType =
        ExtractSliceOp::inferCanonicalRankReducedResultType(
            sliceOp.getType().getRank(), sliceOp.getSourceType(),
            sliceOp.getMixedOffsets(), sliceOp.getMixedSizes(),
            sliceOp.getMixedStrides());
    Value newSlice = rewriter.create<ExtractSliceOp>(
        sliceOp.getLoc(), resultType, castOp.getSource(), sliceOp.getOffsets(),
        sliceOp.getSizes(), sliceOp.getStrides(), sliceOp.getStaticOffsets(),
        sliceOp.getStaticSizes(), sliceOp.getStaticStrides());
    rewriter.replaceOpWithNewOp<tensor::CastOp>(sliceOp, sliceOp.getType(),
                                                newSlice);
    return success();
  }
};

/// Slice elements from `values` into `outValues`. `counts` represents the
/// numbers of elements to stride in the original values for each dimension.
/// The output values can be used to construct a DenseElementsAttr.
template <typename IterTy, typename ElemTy>
static void sliceElements(IterTy values, ArrayRef<int64_t> counts,
                          ArrayRef<int64_t> offsets, ArrayRef<int64_t> sizes,
                          ArrayRef<int64_t> strides,
                          llvm::SmallVectorImpl<ElemTy> *outValues) {
  assert(offsets.size() == sizes.size());
  assert(offsets.size() == strides.size());
  if (offsets.empty())
    return;

  int64_t offset = offsets.front();
  int64_t size = sizes.front();
  int64_t stride = strides.front();
  if (offsets.size() == 1) {
    for (int64_t i = 0; i < size; ++i, offset += stride)
      outValues->push_back(*(values + offset));

    return;
  }

  for (int64_t i = 0; i < size; ++i, offset += stride) {
    auto begin = values + offset * counts.front();
    sliceElements<IterTy, ElemTy>(begin, counts.drop_front(),
                                  offsets.drop_front(), sizes.drop_front(),
                                  strides.drop_front(), outValues);
  }
}

/// Fold arith.constant and tensor.extract_slice into arith.constant. The
/// folded operation might introduce more constant data; Users can control
/// their heuristics by the control function.
class ConstantOpExtractSliceFolder final
    : public OpRewritePattern<ExtractSliceOp> {
public:
  using OpRewritePattern<ExtractSliceOp>::OpRewritePattern;

  ConstantOpExtractSliceFolder(MLIRContext *context,
                               ControlConstantExtractSliceFusionFn controlFn)
      : OpRewritePattern<ExtractSliceOp>(context),
        controlFn(std::move(controlFn)) {}

  LogicalResult matchAndRewrite(ExtractSliceOp op,
                                PatternRewriter &rewriter) const override {
    DenseElementsAttr attr;
    if (!matchPattern(op.getSource(), m_Constant(&attr)))
      return failure();

    // A constant splat is handled by fold().
    if (attr.isSplat())
      return failure();

    // Dynamic result shape is not supported.
    auto sourceType = op.getSource().getType().cast<ShapedType>();
    auto resultType = op.getResult().getType().cast<ShapedType>();
    if (!sourceType.hasStaticShape() || !resultType.hasStaticShape())
      return failure();

    // Customized control over the folding.
    if (!controlFn(op))
      return failure();

    int64_t count = sourceType.getNumElements();
    if (count == 0)
      return failure();

    // Check if there are any dynamic parts, which are not supported.
    auto offsets = extractFromI64ArrayAttr(op.getStaticOffsets());
    if (llvm::is_contained(offsets, ShapedType::kDynamic))
      return failure();
    auto sizes = extractFromI64ArrayAttr(op.getStaticSizes());
    if (llvm::is_contained(sizes, ShapedType::kDynamic))
      return failure();
    auto strides = extractFromI64ArrayAttr(op.getStaticStrides());
    if (llvm::is_contained(strides, ShapedType::kDynamic))
      return failure();

    // Compute the stride for each dimension.
    SmallVector<int64_t> counts;
    ArrayRef<int64_t> shape = sourceType.getShape();
    counts.reserve(shape.size());
    for (int64_t v : shape) {
      count = count / v;
      counts.push_back(count);
    }

    // New attribute constructed by the sliced values.
    DenseElementsAttr newAttr;

    if (auto elems = attr.dyn_cast<DenseIntElementsAttr>()) {
      SmallVector<APInt> outValues;
      outValues.reserve(sourceType.getNumElements());
      sliceElements<DenseElementsAttr::IntElementIterator, APInt>(
          elems.begin(), counts, offsets, sizes, strides, &outValues);
      newAttr = DenseElementsAttr::get(resultType, outValues);
    } else if (auto elems = attr.dyn_cast<DenseFPElementsAttr>()) {
      SmallVector<APFloat> outValues;
      outValues.reserve(sourceType.getNumElements());
      sliceElements<DenseElementsAttr::FloatElementIterator, APFloat>(
          elems.begin(), counts, offsets, sizes, strides, &outValues);
      newAttr = DenseElementsAttr::get(resultType, outValues);
    }

    if (newAttr) {
      rewriter.replaceOpWithNewOp<arith::ConstantOp>(op, resultType, newAttr);
      return success();
    }

    return failure();
  }

private:
  /// This additionally controls whether the fold happens or not. Users can
  /// impose their heuristics in the function.
  ControlConstantExtractSliceFusionFn controlFn;
};

} // namespace

void mlir::tensor::populateFoldConstantExtractSlicePatterns(
    RewritePatternSet &patterns,
    const ControlConstantExtractSliceFusionFn &controlFn) {
  patterns.add<ConstantOpExtractSliceFolder>(patterns.getContext(), controlFn);
}

/// Return the canonical type of the result of an extract_slice op.
struct SliceReturnTypeCanonicalizer {
  RankedTensorType operator()(ExtractSliceOp op,
                              ArrayRef<OpFoldResult> mixedOffsets,
                              ArrayRef<OpFoldResult> mixedSizes,
                              ArrayRef<OpFoldResult> mixedStrides) {
    return ExtractSliceOp::inferCanonicalRankReducedResultType(
        op.getType().getRank(), op.getSourceType(), mixedOffsets, mixedSizes,
        mixedStrides);
  }
};

/// A canonicalizer wrapper to replace ExtractSliceOps.
struct SliceCanonicalizer {
  void operator()(PatternRewriter &rewriter, ExtractSliceOp op,
                  ExtractSliceOp newOp) {
    Value replacement = newOp.getResult();
    if (replacement.getType() != op.getType())
      replacement = rewriter.create<tensor::CastOp>(op.getLoc(), op.getType(),
                                                    replacement);
    rewriter.replaceOp(op, replacement);
  }
};

void ExtractSliceOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                 MLIRContext *context) {
  results.add<
      OpWithOffsetSizesAndStridesConstantArgumentFolder<
          ExtractSliceOp, SliceReturnTypeCanonicalizer, SliceCanonicalizer>,
      ExtractSliceOpCastFolder>(context);
}

//
static LogicalResult
foldIdentityOffsetSizeAndStrideOpInterface(OffsetSizeAndStrideOpInterface op,
                                           ShapedType shapedType) {
  OpBuilder b(op.getContext());
  for (OpFoldResult ofr : op.getMixedOffsets())
    if (getConstantIntValue(ofr) != static_cast<int64_t>(0))
      return failure();
  // Rank-reducing noops only need to inspect the leading dimensions:
  // llvm::zip is appropriate.
  auto shape = shapedType.getShape();
  for (auto it : llvm::zip(op.getMixedSizes(), shape))
    if (getConstantIntValue(std::get<0>(it)) != std::get<1>(it))
      return failure();
  for (OpFoldResult ofr : op.getMixedStrides())
    if (getConstantIntValue(ofr) != static_cast<int64_t>(1))
      return failure();
  return success();
}

/// If we have an ExtractSliceOp consuming an InsertSliceOp with the same
/// slice, we can return the InsertSliceOp's source directly.
// TODO: This only checks the immediate producer; extend to go up the
// insert/extract chain if the slices are disjoint.
static Value foldExtractAfterInsertSlice(ExtractSliceOp extractOp) {
  auto insertOp = extractOp.getSource().getDefiningOp<InsertSliceOp>();

  auto isSame = [](OpFoldResult a, OpFoldResult b) { return a == b; };
  if (insertOp && insertOp.getSource().getType() == extractOp.getType() &&
      insertOp.isSameAs(extractOp, isSame))
    return insertOp.getSource();

  return {};
}

OpFoldResult ExtractSliceOp::fold(ArrayRef<Attribute> operands) {
  if (auto splat = operands[0].dyn_cast_or_null<SplatElementsAttr>()) {
    auto resultType = getResult().getType().cast<ShapedType>();
    if (resultType.hasStaticShape())
      return splat.resizeSplat(resultType);
  }
  if (getSourceType() == getType() &&
      succeeded(foldIdentityOffsetSizeAndStrideOpInterface(*this, getType())))
    return this->getSource();
  if (Value slice = foldExtractAfterInsertSlice(*this))
    return slice;

  return OpFoldResult();
}

Value mlir::tensor::createCanonicalRankReducingExtractSliceOp(
    OpBuilder &b, Location loc, Value tensor, RankedTensorType targetType) {
  auto rankedTensorType = tensor.getType().cast<RankedTensorType>();
  unsigned rank = rankedTensorType.getRank();
  SmallVector<OpFoldResult> offsets(rank, b.getIndexAttr(0));
  SmallVector<OpFoldResult> sizes = getMixedSizes(b, loc, tensor);
  SmallVector<OpFoldResult> strides(rank, b.getIndexAttr(1));
  return b.createOrFold<tensor::ExtractSliceOp>(loc, targetType, tensor,
                                                offsets, sizes, strides);
}

//===----------------------------------------------------------------------===//
// InsertSliceOp
//===----------------------------------------------------------------------===//

void InsertSliceOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "inserted_slice");
}

// Build a InsertSliceOp with mixed static and dynamic entries.
void InsertSliceOp::build(OpBuilder &b, OperationState &result, Value source,
                          Value dest, ArrayRef<OpFoldResult> offsets,
                          ArrayRef<OpFoldResult> sizes,
                          ArrayRef<OpFoldResult> strides,
                          ArrayRef<NamedAttribute> attrs) {
  SmallVector<int64_t> staticOffsets, staticSizes, staticStrides;
  SmallVector<Value> dynamicOffsets, dynamicSizes, dynamicStrides;
  dispatchIndexOpFoldResults(offsets, dynamicOffsets, staticOffsets,
                             ShapedType::kDynamic);
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticSizes,
                             ShapedType::kDynamic);
  dispatchIndexOpFoldResults(strides, dynamicStrides, staticStrides,
                             ShapedType::kDynamic);
  build(b, result, dest.getType(), source, dest, dynamicOffsets, dynamicSizes,
        dynamicStrides, b.getI64ArrayAttr(staticOffsets),
        b.getI64ArrayAttr(staticSizes), b.getI64ArrayAttr(staticStrides));
  result.addAttributes(attrs);
}

/// Build an InsertSliceOp with mixed static and dynamic entries packed into a
/// Range vector.
void InsertSliceOp::build(OpBuilder &b, OperationState &result, Value source,
                          Value dest, ArrayRef<Range> ranges,
                          ArrayRef<NamedAttribute> attrs) {
  auto [offsets, sizes, strides] = getOffsetsSizesAndStrides(ranges);
  build(b, result, source, dest, offsets, sizes, strides, attrs);
}

// Build a InsertSliceOp with dynamic entries.
void InsertSliceOp::build(OpBuilder &b, OperationState &result, Value source,
                          Value dest, ValueRange offsets, ValueRange sizes,
                          ValueRange strides, ArrayRef<NamedAttribute> attrs) {
  SmallVector<OpFoldResult> offsetValues = llvm::to_vector<4>(
      llvm::map_range(offsets, [](Value v) -> OpFoldResult { return v; }));
  SmallVector<OpFoldResult> sizeValues = llvm::to_vector<4>(
      llvm::map_range(sizes, [](Value v) -> OpFoldResult { return v; }));
  SmallVector<OpFoldResult> strideValues = llvm::to_vector<4>(
      llvm::map_range(strides, [](Value v) -> OpFoldResult { return v; }));
  build(b, result, source, dest, offsetValues, sizeValues, strideValues);
}

/// Rank-reducing type verification for both InsertSliceOp and
/// ParallelInsertSliceOp.
static SliceVerificationResult
verifyInsertSliceOp(ShapedType srcType, ShapedType dstType,
                    ArrayAttr staticOffsets, ArrayAttr staticSizes,
                    ArrayAttr staticStrides,
                    ShapedType *expectedType = nullptr) {
  // insert_slice is the inverse of extract_slice, use the same type
  // inference.
  RankedTensorType expected = ExtractSliceOp::inferResultType(
      dstType, extractFromI64ArrayAttr(staticOffsets),
      extractFromI64ArrayAttr(staticSizes),
      extractFromI64ArrayAttr(staticStrides));
  if (expectedType)
    *expectedType = expected;
  return isRankReducedType(expected, srcType);
}

/// Verifier for InsertSliceOp.
LogicalResult InsertSliceOp::verify() {
  ShapedType expectedType;
  SliceVerificationResult result =
      verifyInsertSliceOp(getSourceType(), getType(), getStaticOffsets(),
                          getStaticSizes(), getStaticStrides(), &expectedType);
  return produceSliceErrorMsg(result, *this, expectedType);
}

/// If we have two consecutive InsertSliceOp writing to the same slice, we
/// can mutate the second InsertSliceOp's destination to the first one's.
///
/// Example:
///
/// ```mlir
///   %0 = tensor.insert_slice %slice0 into %input[0, 0] [64, 64] [1, 1]
///   %1 = tensor.insert_slice %slice1 into %0[0, 0] [64, 64] [1, 1]
/// ```
///
/// folds into:
///
/// ```mlir
///   %1 = tensor.insert_slice %slice1 into %input[0, 0] [64, 64] [1, 1]
/// ```
///
/// This pattern works with both InsertSliceOp and ParallelInsertSliceOp.
static LogicalResult foldInsertAfterInsertSlice(InsertSliceOp insertOp) {
  auto prevInsertOp = insertOp.getDest().getDefiningOp<InsertSliceOp>();

  auto isSame = [](OpFoldResult a, OpFoldResult b) { return a == b; };
  if (!prevInsertOp ||
      prevInsertOp.getSource().getType() != insertOp.getSource().getType() ||
      !prevInsertOp.isSameAs(insertOp, isSame))
    return failure();

  insertOp.getDestMutable().assign(prevInsertOp.getDest());
  return success();
}

/// Folds round-trip extract/insert slice op pairs.
/// Example:
/// ```mlir
/// %0 = tensor.extract_slice %val[0, 0, 0, 0] [1, 1, 2, 4] [1, 1, 1, 1]
/// %1 = tensor.insert_slice %0 into %val[0, 0, 0, 0] [1, 1, 2, 4] [1, 1, 1, 1]
/// ```
/// can be folded into %val.
static Value foldInsertAfterExtractSlice(InsertSliceOp insertOp) {
  auto extractOp = insertOp.getSource().getDefiningOp<ExtractSliceOp>();

  auto isSame = [](OpFoldResult a, OpFoldResult b) { return a == b; };
  if (!extractOp || extractOp.getSource() != insertOp.getDest() ||
      !extractOp.isSameAs(insertOp, isSame))
    return nullptr;

  return extractOp.getSource();
}

OpFoldResult InsertSliceOp::fold(ArrayRef<Attribute>) {
  if (getSourceType().hasStaticShape() && getType().hasStaticShape() &&
      getSourceType() == getType() &&
      succeeded(foldIdentityOffsetSizeAndStrideOpInterface(*this, getType())))
    return this->getSource();
  if (succeeded(foldInsertAfterInsertSlice(*this)))
    return getResult();
  if (auto result = foldInsertAfterExtractSlice(*this))
    return result;
  return OpFoldResult();
}

LogicalResult InsertSliceOp::reifyResultShapes(
    OpBuilder &builder, ReifiedRankedShapedTypeDims &reifiedReturnShapes) {
  reifiedReturnShapes.resize(1, SmallVector<Value>(getType().getRank()));
  for (auto dim : llvm::seq<int64_t>(0, getType().getRank())) {
    reifiedReturnShapes[0][dim] =
        builder.createOrFold<tensor::DimOp>(getLoc(), getDest(), dim);
  }
  return success();
}

namespace {
/// Pattern to rewrite a insert_slice op with constant arguments.
///
/// This pattern works with both InsertSliceOp and ParallelInsertSliceOp.
template <typename InsertOpTy>
class InsertSliceOpConstantArgumentFolder final
    : public OpRewritePattern<InsertOpTy> {
public:
  using OpRewritePattern<InsertOpTy>::OpRewritePattern;

  LogicalResult matchAndRewrite(InsertOpTy insertSliceOp,
                                PatternRewriter &rewriter) const override {
    // No constant operand, just return.
    if (llvm::none_of(insertSliceOp.getOperands(), [](Value operand) {
          return matchPattern(operand, matchConstantIndex());
        }))
      return failure();

    // At least one of offsets/sizes/strides is a new constant.
    // Form the new list of operands and constant attributes from the
    // existing.
    SmallVector<OpFoldResult> mixedOffsets(insertSliceOp.getMixedOffsets());
    SmallVector<OpFoldResult> mixedSizes(insertSliceOp.getMixedSizes());
    SmallVector<OpFoldResult> mixedStrides(insertSliceOp.getMixedStrides());
    canonicalizeSubViewPart(mixedOffsets, ShapedType::isDynamic);
    canonicalizeSubViewPart(mixedSizes, ShapedType::isDynamic);
    canonicalizeSubViewPart(mixedStrides, ShapedType::isDynamic);

    // Create the new op in canonical form.
    auto sourceType = ExtractSliceOp::inferCanonicalRankReducedResultType(
        insertSliceOp.getSourceType().getRank(), insertSliceOp.getDestType(),
        mixedOffsets, mixedSizes, mixedStrides);
    Value toInsert = insertSliceOp.getSource();
    if (sourceType != insertSliceOp.getSourceType()) {
      OpBuilder::InsertionGuard g(rewriter);
      // The only difference between InsertSliceOp and ParallelInsertSliceOp
      // is the the insertion point is just before the ParallelCombiningOp in
      // the parallel case.
      if (std::is_same<InsertOpTy, ParallelInsertSliceOp>::value)
        rewriter.setInsertionPoint(insertSliceOp->getParentOp());
      toInsert = rewriter.create<tensor::CastOp>(insertSliceOp.getLoc(),
                                                 sourceType, toInsert);
    }
    rewriter.replaceOpWithNewOp<InsertOpTy>(
        insertSliceOp, toInsert, insertSliceOp.getDest(), mixedOffsets,
        mixedSizes, mixedStrides);
    return success();
  }
};

/// Fold tensor_casts with insert_slice operations. If the source or
/// destination tensor is a tensor_cast that removes static type information,
/// the cast is folded into the insert_slice operation. E.g.:
///
/// ```mlir
///   %1 = tensor.cast %0 : tensor<8x16xf32> to tensor<?x?xf32>
///   %2 = tensor.insert_slice %1 into ... : tensor<?x?xf32> into ...
/// ```
///
/// folds into:
///
/// ```mlir
///   %2 = tensor.insert_slice %0 into ... : tensor<8x16xf32> into ...
/// ```
///
/// Note: When folding a cast on the destination tensor, the result of the
/// insert_slice operation is casted to ensure that the type of the result did
/// not change.
///
/// This pattern works with both InsertSliceOp and ParallelInsertSliceOp.
template <typename InsertOpTy>
struct InsertSliceOpCastFolder final : public OpRewritePattern<InsertOpTy> {
  using OpRewritePattern<InsertOpTy>::OpRewritePattern;

  LogicalResult matchAndRewrite(InsertOpTy insertSliceOp,
                                PatternRewriter &rewriter) const override {
    if (llvm::any_of(insertSliceOp.getOperands(), [](Value operand) {
          return matchPattern(operand, matchConstantIndex());
        }))
      return failure();

    auto getSourceOfCastOp = [](Value v) -> Optional<Value> {
      auto castOp = v.getDefiningOp<tensor::CastOp>();
      if (!castOp || !canFoldIntoConsumerOp(castOp))
        return llvm::None;
      return castOp.getSource();
    };
    Optional<Value> sourceCastSource =
        getSourceOfCastOp(insertSliceOp.getSource());
    Optional<Value> destCastSource = getSourceOfCastOp(insertSliceOp.getDest());
    if (!sourceCastSource && !destCastSource)
      return failure();

    auto src =
        (sourceCastSource ? *sourceCastSource : insertSliceOp.getSource());
    auto dst = (destCastSource ? *destCastSource : insertSliceOp.getDest());
    auto srcType = src.getType().template cast<ShapedType>();
    auto dstType = dst.getType().template cast<ShapedType>();
    if (verifyInsertSliceOp(srcType, dstType, insertSliceOp.getStaticOffsets(),
                            insertSliceOp.getStaticSizes(),
                            insertSliceOp.getStaticStrides()) !=
        SliceVerificationResult::Success)
      return failure();

    Operation *replacement = rewriter.create<InsertOpTy>(
        insertSliceOp.getLoc(), src, dst, insertSliceOp.getMixedOffsets(),
        insertSliceOp.getMixedSizes(), insertSliceOp.getMixedStrides());

    // In the parallel case there is no result and so nothing to cast.
    bool isParallelInsert =
        std::is_same<InsertOpTy, ParallelInsertSliceOp>::value;
    if (!isParallelInsert && dst.getType() != insertSliceOp.getDestType()) {
      replacement = rewriter.create<tensor::CastOp>(insertSliceOp.getLoc(),
                                                    insertSliceOp.getDestType(),
                                                    replacement->getResult(0));
    }
    rewriter.replaceOp(insertSliceOp, replacement->getResults());
    return success();
  }
};

/// If additional static type information can be deduced from a insert_slice's
/// size operands, insert an explicit cast of the op's source operand. This
/// enables other canonicalization patterns that are matching for tensor_cast
/// ops such as `ForOpTensorCastFolder` in SCF.
///
/// Example:
///
/// ```mlir
///   %r = tensor.insert_slice %0 into %1[...] [64, 64] [1, 1]
///       : tensor<?x?xf32> into ...
/// ```
///
/// folds into:
///
/// ```mlir
///   %tmp = tensor.cast %0 : tensor<?x?xf32> to tensor<64x64xf32>
///   %r = tensor.insert_slice %tmp into %1[...] [64, 64] [1, 1]
///       : tensor<64x64xf32> into ...
/// ```
///
/// This patterns works with both InsertSliceOp and ParallelInsertSliceOp.
template <typename InsertOpTy>
struct InsertSliceOpSourceCastInserter final
    : public OpRewritePattern<InsertOpTy> {
  using OpRewritePattern<InsertOpTy>::OpRewritePattern;

  LogicalResult matchAndRewrite(InsertOpTy insertSliceOp,
                                PatternRewriter &rewriter) const override {
    RankedTensorType srcType = insertSliceOp.getSourceType();
    if (srcType.getRank() != insertSliceOp.getDestType().getRank())
      return failure();
    SmallVector<int64_t> newSrcShape(srcType.getShape().begin(),
                                     srcType.getShape().end());
    for (int64_t i = 0; i < srcType.getRank(); ++i) {
      if (Optional<int64_t> constInt =
              getConstantIntValue(insertSliceOp.getMixedSizes()[i]))
        newSrcShape[i] = *constInt;
    }

    RankedTensorType newSrcType =
        RankedTensorType::get(newSrcShape, srcType.getElementType());
    if (srcType == newSrcType ||
        !preservesStaticInformation(srcType, newSrcType) ||
        !tensor::CastOp::areCastCompatible(srcType, newSrcType))
      return failure();

    // newSrcType is:
    //   1) Different from srcType.
    //   2) "More static" than srcType.
    //   3) Cast-compatible with srcType.
    // Insert the cast.
    OpBuilder::InsertionGuard g(rewriter);
    // The only difference between InsertSliceOp and ParallelInsertSliceOp is
    // the the insertion point is just before the ParallelCombiningOp in the
    // parallel case.
    if (std::is_same<InsertOpTy, ParallelInsertSliceOp>::value)
      rewriter.setInsertionPoint(insertSliceOp->getParentOp());
    Value cast = rewriter.create<tensor::CastOp>(
        insertSliceOp.getLoc(), newSrcType, insertSliceOp.getSource());
    rewriter.replaceOpWithNewOp<InsertOpTy>(
        insertSliceOp, cast, insertSliceOp.getDest(),
        insertSliceOp.getMixedOffsets(), insertSliceOp.getMixedSizes(),
        insertSliceOp.getMixedStrides());
    cast.getDefiningOp()->getParentOfType<ModuleOp>().dump();
    return success();
  }
};
} // namespace

void InsertSliceOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                MLIRContext *context) {
  results.add<InsertSliceOpConstantArgumentFolder<InsertSliceOp>,
              InsertSliceOpCastFolder<InsertSliceOp>,
              InsertSliceOpSourceCastInserter<InsertSliceOp>>(context);
}

Value mlir::tensor::createCanonicalRankReducingInsertSliceOp(OpBuilder &b,
                                                             Location loc,
                                                             Value tensor,
                                                             Value dest) {
  auto rankedTensorType = dest.getType().cast<RankedTensorType>();
  unsigned rank = rankedTensorType.getRank();
  SmallVector<OpFoldResult> offsets(rank, b.getIndexAttr(0));
  SmallVector<OpFoldResult> sizes = getMixedSizes(b, loc, dest);
  SmallVector<OpFoldResult> strides(rank, b.getIndexAttr(1));
  return b.createOrFold<tensor::InsertSliceOp>(loc, tensor, dest, offsets,
                                               sizes, strides);
}

//===----------------------------------------------------------------------===//
// PadOp
//===----------------------------------------------------------------------===//

void PadOp::getAsmResultNames(function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "padded");
}

// TODO: Replace custom<InferType> directive with AllTypesMatch as soon as it
// supports optional types.
void printInferType(OpAsmPrinter &printer, Operation *op, Value optOperand,
                    Type typeToInfer, Type typeToInferFrom) {}

ParseResult parseInferType(OpAsmParser &parser,
                           Optional<OpAsmParser::UnresolvedOperand> optOperand,
                           Type &typeToInfer, Type typeToInferFrom) {
  if (optOperand)
    typeToInfer = typeToInferFrom;
  return success();
}

LogicalResult PadOp::verify() {
  auto sourceType = getSource().getType().cast<RankedTensorType>();
  auto resultType = getResult().getType().cast<RankedTensorType>();
  auto expectedType = PadOp::inferResultType(
      sourceType, extractFromI64ArrayAttr(getStaticLow()),
      extractFromI64ArrayAttr(getStaticHigh()));
  for (int i = 0, e = sourceType.getRank(); i < e; ++i) {
    if (resultType.getDimSize(i) == expectedType.getDimSize(i))
      continue;
    if (expectedType.isDynamicDim(i))
      continue;
    return emitError("specified type ")
           << resultType << " does not match the inferred type "
           << expectedType;
  }

  return success();
}

LogicalResult PadOp::verifyRegions() {
  auto &region = getRegion();
  unsigned rank = getResult().getType().cast<RankedTensorType>().getRank();
  Block &block = region.front();
  if (block.getNumArguments() != rank)
    return emitError("expected the block to have ") << rank << " arguments";

  // Note: the number and type of yield values are checked in the YieldOp.
  for (const auto &en : llvm::enumerate(block.getArgumentTypes())) {
    if (!en.value().isIndex())
      return emitOpError("expected block argument ")
             << (en.index() + 1) << " to be an index";
  }

  // Ensure that the region yields an element of the right type.
  auto yieldOp = llvm::cast<YieldOp>(block.getTerminator());
  if (yieldOp.getValue().getType() !=
      getType().cast<ShapedType>().getElementType())
    return emitOpError("expected yield type to match shape element type");

  return success();
}

RankedTensorType PadOp::inferResultType(RankedTensorType sourceType,
                                        ArrayRef<int64_t> staticLow,
                                        ArrayRef<int64_t> staticHigh,
                                        ArrayRef<int64_t> resultShape) {
  unsigned rank = sourceType.getRank();
  assert(staticLow.size() == rank && "unexpected staticLow size mismatch");
  assert(staticHigh.size() == rank && "unexpected staticHigh size mismatch");
  assert((resultShape.empty() || resultShape.size() == rank) &&
         "unexpected resultShape size mismatch");

  SmallVector<int64_t, 4> inferredShape;
  for (auto i : llvm::seq<unsigned>(0, rank)) {
    if (sourceType.isDynamicDim(i) ||
        staticLow[i] == ShapedType::kDynamic ||
        staticHigh[i] == ShapedType::kDynamic) {
      inferredShape.push_back(resultShape.empty() ? ShapedType::kDynamic
                                                  : resultShape[i]);
    } else {
      int64_t size = sourceType.getDimSize(i) + staticLow[i] + staticHigh[i];
      assert((resultShape.empty() || size == resultShape[i] ||
              resultShape[i] == ShapedType::kDynamic) &&
             "mismatch between inferred shape and result shape");
      inferredShape.push_back(size);
    }
  }

  return RankedTensorType::get(inferredShape, sourceType.getElementType());
}

void PadOp::build(OpBuilder &b, OperationState &result, Value source,
                  ArrayRef<int64_t> staticLow, ArrayRef<int64_t> staticHigh,
                  ValueRange low, ValueRange high, bool nofold,
                  ArrayRef<NamedAttribute> attrs) {
  auto sourceType = source.getType().cast<RankedTensorType>();
  auto resultType = inferResultType(sourceType, staticLow, staticHigh);
  build(b, result, resultType, source, low, high, b.getI64ArrayAttr(staticLow),
        b.getI64ArrayAttr(staticHigh), nofold ? b.getUnitAttr() : UnitAttr());
  result.addAttributes(attrs);
}

void PadOp::build(OpBuilder &b, OperationState &result, Value source,
                  ValueRange low, ValueRange high, bool nofold,
                  ArrayRef<NamedAttribute> attrs) {
  auto sourceType = source.getType().cast<RankedTensorType>();
  unsigned rank = sourceType.getRank();
  SmallVector<int64_t, 4> staticVector(rank, ShapedType::kDynamic);
  build(b, result, source, staticVector, staticVector, low, high, nofold,
        attrs);
}

void PadOp::build(OpBuilder &b, OperationState &result, Type resultType,
                  Value source, ArrayRef<OpFoldResult> low,
                  ArrayRef<OpFoldResult> high, bool nofold,
                  ArrayRef<NamedAttribute> attrs) {
  auto sourceType = source.getType().cast<RankedTensorType>();
  SmallVector<Value, 4> dynamicLow, dynamicHigh;
  SmallVector<int64_t, 4> staticLow, staticHigh;
  // staticLow and staticHigh have full information of the padding config.
  // This will grow staticLow and staticHigh with 1 value. If the config is
  // dynamic (ie not a constant), dynamicLow and dynamicHigh will grow with 1
  // value as well.
  dispatchIndexOpFoldResults(low, dynamicLow, staticLow,
                             ShapedType::kDynamic);
  dispatchIndexOpFoldResults(high, dynamicHigh, staticHigh,
                             ShapedType::kDynamic);
  if (!resultType) {
    resultType = PadOp::inferResultType(sourceType, staticLow, staticHigh);
  }
  assert(resultType.isa<RankedTensorType>());
  build(b, result, resultType, source, dynamicLow, dynamicHigh,
        b.getI64ArrayAttr(staticLow), b.getI64ArrayAttr(staticHigh),
        nofold ? b.getUnitAttr() : UnitAttr());
  result.addAttributes(attrs);
}

void PadOp::build(OpBuilder &b, OperationState &result, Type resultType,
                  Value source, ArrayRef<OpFoldResult> low,
                  ArrayRef<OpFoldResult> high, Value constantPadValue,
                  bool nofold, ArrayRef<NamedAttribute> attrs) {
  build(b, result, resultType, source, low, high, nofold, attrs);

  // Add a region and a block to yield the pad value.
  Region *region = result.regions[0].get();
  int sourceRank = source.getType().cast<RankedTensorType>().getRank();
  SmallVector<Type> blockArgTypes(sourceRank, b.getIndexType());
  SmallVector<Location> blockArgLocs(sourceRank, result.location);

  // `builder.createBlock` changes the insertion point within the block. Create
  // a guard to reset the insertion point of the builder after it is destroyed.
  OpBuilder::InsertionGuard guard(b);
  b.createBlock(region, region->end(), blockArgTypes, blockArgLocs);
  b.create<tensor::YieldOp>(result.location, constantPadValue);
}

llvm::SmallBitVector PadOp::getPaddedDims() {
  llvm::SmallBitVector paddedDims(getSourceType().getRank());
  auto extractPaddedDims = [&](ArrayRef<OpFoldResult> paddingWidths) {
    for (const auto &en : enumerate(paddingWidths))
      if (getConstantIntValue(en.value()) != static_cast<int64_t>(0))
        paddedDims.set(en.index());
  };
  extractPaddedDims(getMixedLowPad());
  extractPaddedDims(getMixedHighPad());
  return paddedDims;
}

namespace {
// Folds tensor.pad when padding is static zeros and the attribute
// doesn't request otherwise.
struct FoldStaticZeroPadding : public OpRewritePattern<PadOp> {
  using OpRewritePattern<PadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(PadOp padTensorOp,
                                PatternRewriter &rewriter) const override {
    if (!padTensorOp.hasZeroLowPad() || !padTensorOp.hasZeroHighPad())
      return failure();
    if (padTensorOp.getNofold())
      return failure();
    rewriter.replaceOpWithNewOp<tensor::CastOp>(
        padTensorOp, padTensorOp.getResult().getType(),
        padTensorOp.getSource());
    return success();
  }
};

// Fold CastOp into PadOp when adding static information.
struct FoldSourceTensorCast : public OpRewritePattern<PadOp> {
  using OpRewritePattern<PadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(PadOp padTensorOp,
                                PatternRewriter &rewriter) const override {
    auto castOp = padTensorOp.getSource().getDefiningOp<tensor::CastOp>();
    if (!tensor::canFoldIntoConsumerOp(castOp))
      return failure();

    auto newResultType = PadOp::inferResultType(
        castOp.getSource().getType().cast<RankedTensorType>(),
        extractFromI64ArrayAttr(padTensorOp.getStaticLow()),
        extractFromI64ArrayAttr(padTensorOp.getStaticHigh()),
        padTensorOp.getResultType().getShape());

    if (newResultType == padTensorOp.getResultType()) {
      rewriter.updateRootInPlace(padTensorOp, [&]() {
        padTensorOp.getSourceMutable().assign(castOp.getSource());
      });
    } else {
      auto newOp = rewriter.create<PadOp>(
          padTensorOp->getLoc(), newResultType, padTensorOp.getSource(),
          padTensorOp.getLow(), padTensorOp.getHigh(),
          padTensorOp.getStaticLow(), padTensorOp.getStaticHigh(),
          padTensorOp.getNofold());
      BlockAndValueMapping mapper;
      padTensorOp.getRegion().cloneInto(&newOp.getRegion(), mapper);

      rewriter.replaceOpWithNewOp<tensor::CastOp>(
          padTensorOp, padTensorOp.getResultType(), newOp);
    }
    return success();
  }
};

// Fold CastOp using the result of PadOp back into the latter if it adds
// static information.
struct FoldTargetTensorCast : public OpRewritePattern<PadOp> {
  using OpRewritePattern<PadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(PadOp padTensorOp,
                                PatternRewriter &rewriter) const override {
    if (!padTensorOp.getResult().hasOneUse())
      return failure();
    auto tensorCastOp =
        dyn_cast<tensor::CastOp>(*padTensorOp->getUsers().begin());
    if (!tensorCastOp)
      return failure();
    if (!tensor::preservesStaticInformation(padTensorOp.getResult().getType(),
                                            tensorCastOp.getDest().getType()))
      return failure();

    auto replacementOp = rewriter.create<PadOp>(
        padTensorOp.getLoc(), tensorCastOp.getDest().getType(),
        padTensorOp.getSource(), padTensorOp.getLow(), padTensorOp.getHigh(),
        padTensorOp.getStaticLow(), padTensorOp.getStaticHigh(),
        padTensorOp.getNofold());
    replacementOp.getRegion().takeBody(padTensorOp.getRegion());

    rewriter.replaceOp(padTensorOp, replacementOp.getResult());
    rewriter.replaceOp(tensorCastOp, replacementOp.getResult());
    return success();
  }
};

/// Fold chains of tensor::ExtractSliceOp, tensor::PadOp pairs that pad
/// different dimensions. The pattern applies if the following preconditions
/// hold:
///   1) the tensor::ExtractSliceOps are not rank-reducing,
///   2) the tensor::ExtractSliceOps have only unit-strides,
///   3) the tensor::PadOps perform only high-padding,
///   4) the tensor::PadOps have the same constant padding value,
///   5) the tensor::PadOps do not have common padding dimensions,
///   6) one tensor::ExtractSliceOp, tensor::PadOp pair has zero-padding and
///      zero-offset for every dimension.
///   7) the tensor::ExtractSliceOp sizes match the source tensor sizes for
///   the
///      padded source dimensions.
///
/// Example:
///
/// ```mlir
///   %0 = tensor.extract_slice %input[16, 0] [%sz0, 64] [1, 1]
///       : tensor<64x64xf32> to tensor<?x64xf32>
///   %1 = tensor.pad %0 low[0, 0] high[%pw0, 0] { ...
///     } : tensor<?x64xf32> to tensor<8x64xf32>
///   %2 = tensor.extract_slice %1[0, 4] [8, %sz1] [1, 1]
///        : tensor<8x64xf32> to tensor<8x?xf32>
///   %res = tensor.pad %2 nofold low[0, 0] high[0, %pw1] { ...
///     } : tensor<8x?xf32> to tensor<8x4xf32>
/// ```
///
/// folds into:
///
/// ```mlir
///   %0 = tensor.extract_slice %input[16, 4] [%sz0, %sz1] [1, 1]
///        : tensor<64x64xf32> to tensor<?x?xf32>
///   %res = tensor.pad %0 nofold low[0, 0] high[%pw0, %pw1] { ...
///     } : tensor<?x?xf32> to tensor<8x4xf32>
/// ```
struct FoldOrthogonalPaddings : public OpRewritePattern<PadOp> {
  using OpRewritePattern<PadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(PadOp padOp,
                                PatternRewriter &rewriter) const override {
    auto innerSliceOp = padOp.getSource().getDefiningOp<ExtractSliceOp>();
    if (!innerSliceOp)
      return failure();
    auto outerPadOp = innerSliceOp.getSource().getDefiningOp<PadOp>();
    if (!outerPadOp || outerPadOp.getNofold())
      return failure();
    auto outerSliceOp = outerPadOp.getSource().getDefiningOp<ExtractSliceOp>();
    if (!outerSliceOp)
      return failure();

    // 1) Fail if the chain is rank-reducing.
    int64_t rank = padOp.getSourceType().getRank();
    if (outerSliceOp.getSourceType().getRank() != rank) {
      return rewriter.notifyMatchFailure(padOp,
                                         "cannot fold rank-reducing chain");
    }

    // 2) Fail if the tensor::ExtractSliceOps have non-unit strides.
    if (!innerSliceOp.hasUnitStride() || !outerSliceOp.hasUnitStride()) {
      return rewriter.notifyMatchFailure(
          padOp, "cannot fold non-unit stride ExtractSliceOps");
    }

    // 3) Fail if the tensor::PadOps have non-zero low padding.
    if (!padOp.hasZeroLowPad() || !outerPadOp.hasZeroLowPad()) {
      return rewriter.notifyMatchFailure(padOp,
                                         "cannot fold PadOps with low padding");
    }

    // 4) Fail if the tensor::PadOps padding values do not match.
    Attribute innerAttr, outerAttr;
    Value innerValue = padOp.getConstantPaddingValue();
    Value outerValue = outerPadOp.getConstantPaddingValue();
    if (!innerValue || !outerValue ||
        !matchPattern(innerValue, m_Constant(&innerAttr)) ||
        !matchPattern(outerValue, m_Constant(&outerAttr)) ||
        innerAttr != outerAttr) {
      return rewriter.notifyMatchFailure(
          padOp, "cannot fold PadOps with different padding values");
    }

    // 5) Fail if a dimension is padded by both tensor::PadOps.
    llvm::SmallBitVector innerDims = padOp.getPaddedDims();
    llvm::SmallBitVector outerDims = outerPadOp.getPaddedDims();
    if (innerDims.anyCommon(outerDims)) {
      return rewriter.notifyMatchFailure(
          padOp, "cannot fold PadOps with common padding dimensions");
    }

    // 6) Combine the offsets of the two tensor::ExtractSliceOps. Find the
    // zero-offset and zero-padding tensor::ExtractSliceOp, tensor::PadOp pair
    // for every dimension, and use the offset the other pair. Fail if no
    // zero-offset and zero-padding tensor::ExtractSliceOp, tensor::PadOp pair
    // exists.
    SmallVector<OpFoldResult> newOffsets(rank, rewriter.getIndexAttr(0));
    for (auto &en : enumerate(newOffsets)) {
      OpFoldResult innerOffset = innerSliceOp.getMixedOffsets()[en.index()];
      OpFoldResult outerOffset = outerSliceOp.getMixedOffsets()[en.index()];
      if (!innerDims.test(en.index()) &&
          (getConstantIntValue(innerOffset) == static_cast<int64_t>(0))) {
        en.value() = outerOffset;
        continue;
      }
      if (!outerDims.test(en.index()) &&
          (getConstantIntValue(outerOffset) == static_cast<int64_t>(0))) {
        en.value() = innerOffset;
        continue;
      }
      return rewriter.notifyMatchFailure(
          padOp, "cannot find zero-offset and zero-padding pair");
    }

    // 7) Combine the sizes of the two tensor::ExtractSliceOps. Take the size
    // of the outer tensor::ExtractSliceOp for the dimensions padded by the
    // outer tensor::PadOp and fail if the size of the inner
    // tensor::ExtractSliceOp does not match the size of the padded dimension.
    // Otherwise, take the size of the inner tensor::ExtractSliceOp.
    SmallVector<OpFoldResult> newSizes = innerSliceOp.getMixedSizes();
    for (auto &en : enumerate(newSizes)) {
      if (!outerDims.test(en.index()))
        continue;
      OpFoldResult sliceSize = innerSliceOp.getMixedSizes()[en.index()];
      int64_t sourceSize = innerSliceOp.getSourceType().getShape()[en.index()];
      assert(!ShapedType::isDynamic(sourceSize) &&
             "expected padded dimension to have a static size");
      if (getConstantIntValue(sliceSize) != sourceSize) {
        return rewriter.notifyMatchFailure(
            padOp, "cannot fold since the inner ExtractSliceOp size does not "
                   "match the size of the outer padding");
      }
      en.value() = outerSliceOp.getMixedSizes()[en.index()];
    }

    // Combine the high paddings of the two tensor::PadOps.
    SmallVector<OpFoldResult> newHighPad(rank, rewriter.getIndexAttr(0));
    for (auto &en : enumerate(newHighPad)) {
      if (innerDims.test(en.index()))
        newHighPad[en.index()] = padOp.getMixedHighPad()[en.index()];
      if (outerDims.test(en.index()))
        newHighPad[en.index()] = outerPadOp.getMixedHighPad()[en.index()];
    }

    // Create a new tensor::ExtractSliceOp, tensor::PadOp pair that performs
    // the two paddings in one step.
    auto newSliceOp = rewriter.create<ExtractSliceOp>(
        padOp.getLoc(), outerSliceOp.getSource(), newOffsets, newSizes,
        innerSliceOp.getMixedStrides());
    auto newPadOp = rewriter.create<PadOp>(
        padOp.getLoc(), padOp.getResultType(), newSliceOp.getResult(),
        padOp.getMixedLowPad(), newHighPad, padOp.getNofold());
    rewriter.inlineRegionBefore(padOp.getRegion(), newPadOp.getRegion(),
                                newPadOp.getRegion().begin());
    rewriter.replaceOp(padOp, newPadOp.getResult());
    return success();
  }
};

} // namespace

void PadOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                        MLIRContext *context) {
  results.add<FoldStaticZeroPadding, FoldSourceTensorCast, FoldTargetTensorCast,
              FoldOrthogonalPaddings>(context);
}

/// Return the padding value of the PadOp if it constant. In this context,
/// "constant" means an actual constant or "defined outside of the block".
///
/// Values are considered constant in three cases:
///  - A ConstantLike value.
///  - A basic block argument from a different block.
///  - A value defined outside of the block.
///
/// If the padding value is not constant, an empty Value is returned.
Value PadOp::getConstantPaddingValue() {
  auto yieldOp = dyn_cast<YieldOp>(getRegion().front().getTerminator());
  if (!yieldOp)
    return {};
  Value padValue = yieldOp.getValue();
  // Check if yield value is a constant.
  if (matchPattern(padValue, m_Constant()))
    return padValue;
  // Check if yield value is defined inside the PadOp block.
  if (padValue.getParentBlock() == &getRegion().front())
    return {};
  // Else: Yield value defined outside of the PadOp block.
  return padValue;
}

OpFoldResult PadOp::fold(ArrayRef<Attribute>) {
  if (getResultType().hasStaticShape() && getResultType() == getSourceType() &&
      !getNofold())
    return getSource();
  return {};
}

//===----------------------------------------------------------------------===//
// ParallelInsertSliceOp
//===----------------------------------------------------------------------===//

OpResult ParallelInsertSliceOp::getTiedOpResult() {
  ParallelCombiningOpInterface parallelCombiningParent =
      getParallelCombiningParent();
  for (const auto &it :
       llvm::enumerate(parallelCombiningParent.getYieldingOps())) {
    Operation &nextOp = it.value();
    if (&nextOp == getOperation())
      return parallelCombiningParent.getParentResult(it.index());
  }
  llvm_unreachable("ParallelInsertSliceOp no tied OpResult found");
}

// Build a ParallelInsertSliceOp with mixed static and dynamic entries.
void ParallelInsertSliceOp::build(OpBuilder &b, OperationState &result,
                                  Value source, Value dest,
                                  ArrayRef<OpFoldResult> offsets,
                                  ArrayRef<OpFoldResult> sizes,
                                  ArrayRef<OpFoldResult> strides,
                                  ArrayRef<NamedAttribute> attrs) {
  SmallVector<int64_t> staticOffsets, staticSizes, staticStrides;
  SmallVector<Value> dynamicOffsets, dynamicSizes, dynamicStrides;
  dispatchIndexOpFoldResults(offsets, dynamicOffsets, staticOffsets,
                             ShapedType::kDynamic);
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticSizes,
                             ShapedType::kDynamic);
  dispatchIndexOpFoldResults(strides, dynamicStrides, staticStrides,
                             ShapedType::kDynamic);
  build(b, result, {}, source, dest, dynamicOffsets, dynamicSizes,
        dynamicStrides, b.getI64ArrayAttr(staticOffsets),
        b.getI64ArrayAttr(staticSizes), b.getI64ArrayAttr(staticStrides));
  result.addAttributes(attrs);
}

/// Build an ParallelInsertSliceOp with mixed static and dynamic entries
/// packed into a Range vector.
void ParallelInsertSliceOp::build(OpBuilder &b, OperationState &result,
                                  Value source, Value dest,
                                  ArrayRef<Range> ranges,
                                  ArrayRef<NamedAttribute> attrs) {
  auto [offsets, sizes, strides] = getOffsetsSizesAndStrides(ranges);
  build(b, result, source, dest, offsets, sizes, strides, attrs);
}

// Build a ParallelInsertSliceOp with dynamic entries.
void ParallelInsertSliceOp::build(OpBuilder &b, OperationState &result,
                                  Value source, Value dest, ValueRange offsets,
                                  ValueRange sizes, ValueRange strides,
                                  ArrayRef<NamedAttribute> attrs) {
  SmallVector<OpFoldResult> offsetValues = llvm::to_vector<4>(
      llvm::map_range(offsets, [](Value v) -> OpFoldResult { return v; }));
  SmallVector<OpFoldResult> sizeValues = llvm::to_vector<4>(
      llvm::map_range(sizes, [](Value v) -> OpFoldResult { return v; }));
  SmallVector<OpFoldResult> strideValues = llvm::to_vector<4>(
      llvm::map_range(strides, [](Value v) -> OpFoldResult { return v; }));
  build(b, result, source, dest, offsetValues, sizeValues, strideValues);
}

LogicalResult ParallelInsertSliceOp::verify() {
  if (!isa<ParallelCombiningOpInterface>(getOperation()->getParentOp()))
    return this->emitError("expected ParallelCombiningOpInterface parent, got:")
           << *(getOperation()->getParentOp());

  ShapedType expectedType;
  SliceVerificationResult result =
      verifyInsertSliceOp(getSourceType(), getDestType(), getStaticOffsets(),
                          getStaticSizes(), getStaticStrides(), &expectedType);
  return produceSliceErrorMsg(result, *this, expectedType);
}

void ParallelInsertSliceOp::getCanonicalizationPatterns(
    RewritePatternSet &results, MLIRContext *context) {
  results.add<InsertSliceOpConstantArgumentFolder<ParallelInsertSliceOp>,
              InsertSliceOpCastFolder<ParallelInsertSliceOp>,
              InsertSliceOpSourceCastInserter<ParallelInsertSliceOp>>(context);
}

//===----------------------------------------------------------------------===//
// ScatterOp
//===----------------------------------------------------------------------===//

void ScatterOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "scatter");
}

LogicalResult ScatterOp::verify() {
  int64_t destRank = getDestType().getRank();
  ArrayRef<int64_t> scatterDims = getScatterDims();
  if (failed(verifyGatherOrScatterDims(getOperation(), scatterDims, destRank,
                                       "scatter", "dest")))
    return failure();

  if (!getUnique())
    return emitOpError("requires 'unique' attribute to be set");
  // TODO: we could also check statically that there are fewer leading index
  // tensor dims than the dest dims. If this is not the case, the unique
  // attribute cannot be true.

  // Use the GatherOp::inferResultType on the `dest` type and verify the
  // expected type matches the source type.
  RankedTensorType expectedSourceType = GatherOp::inferResultType(
      getDestType(), getIndicesType(), scatterDims, /*rankReduced=*/false);
  RankedTensorType expectedRankReducedSourceType = GatherOp::inferResultType(
      getDestType(), getIndicesType(), scatterDims, /*rankReduced=*/true);
  if (getSourceType() != expectedSourceType &&
      getSourceType() != expectedRankReducedSourceType) {
    return emitOpError("source type "
                       "mismatch: "
                       "expected ")
           << expectedSourceType << " or its rank-reduced variant "
           << expectedRankReducedSourceType << " (got: " << getSourceType()
           << ")";
  }

  return success();
}

//===----------------------------------------------------------------------===//
// SplatOp
//===----------------------------------------------------------------------===//

void SplatOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "splat");
}

OpFoldResult SplatOp::fold(ArrayRef<Attribute> operands) {
  auto constOperand = operands.front();
  if (!constOperand.isa_and_nonnull<IntegerAttr, FloatAttr>())
    return {};

  // SplatElementsAttr::get treats single value for second arg as being a
  // splat.
  return SplatElementsAttr::get(getType(), {constOperand});
}

//===----------------------------------------------------------------------===//
// TableGen'd op method definitions
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "mlir/Dialect/Tensor/IR/TensorOps.cpp.inc"
