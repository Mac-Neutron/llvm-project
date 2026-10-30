; A pointer argument with no typed type (no f line) is an error naming the
; argument and its function.
; RUN: not llvm-air-write %s -o %t.bc 2>&1 | FileCheck %s

; CHECK: error: argument %tg: pointer with no typed type (in @k)
; CHECK-NOT: stats

target triple = "air64-apple-macosx14.0.0"

define void @k(i32 %n, ptr addrspace(3) %tg) {
  store i32 %n, ptr addrspace(3) %tg, align 4
  ret void
}
