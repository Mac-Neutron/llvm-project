; A table type that does not erase to its value's type is an error, not a
; write.
; RUN: not llvm-air-write %s --types %S/Inputs/bad-erasure.types -o %t.bc 2>&1 | FileCheck %s

; CHECK: error: table: @k does not erase to void (ptr addrspace(1))
; CHECK-NOT: stats

target triple = "air64-apple-macosx14.0.0"

define void @k(ptr addrspace(1) %out) {
  ret void
}
