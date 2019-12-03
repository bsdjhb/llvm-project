// REQUIRES: riscv
// RUN: llvm-mc -filetype=obj -triple=riscv64-none-linux %s -o %t.o
// RUN: not ld.lld %t.o -o /dev/null 2>&1 | FileCheck %s

// Check that we don't permit conditional branches to undefined weak
// symbols in non-PIC code.
	
 .weak target

 .text
 .global _start
_start:
 beq a0, a1, target

// CHECK: error: {{.*}}: conditional branch to undefined weak symbol 'target'
