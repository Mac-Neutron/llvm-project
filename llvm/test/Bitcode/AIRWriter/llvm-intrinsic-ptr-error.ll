; P13: an llvm.* declaration the writer's list does not hold is an error
; naming it, here one with pointer parameters (llvm.memcpy); LLVM 15 would
; read it as a plain function, so only Metal would decide.
; RUN: not llvm-air-write %s -o %t.bc 2>&1 | FileCheck %s

; CHECK: error: intrinsic @llvm.memcpy.p0.p0.i64: not on the writer's list of LLVM 15 intrinsics (in @k)
; CHECK-NOT: stats

target triple = "air64-apple-macosx14.0.0"

declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

define void @k(ptr %d, ptr %s) {
  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 4, i1 false)
  ret void
}
