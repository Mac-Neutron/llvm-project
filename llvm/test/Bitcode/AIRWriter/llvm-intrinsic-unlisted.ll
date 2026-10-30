; P13: an llvm.* declaration the writer's list does not hold is an error
; naming it, also without pointer parameters: LLVM 23's InstCombine makes
; llvm.scmp, which LLVM 15 lacks. The list's own entries (llvm.umax.i64 and
; llvm.usub.sat.i32) are written.
; RUN: split-file %s %t
; RUN: not llvm-air-write %t/scmp.ll -o %t/scmp.bc 2>&1 | FileCheck %s --check-prefix=SCMP
; RUN: llvm-air-write %t/listed.ll -o %t/listed.bc 2>&1 | FileCheck %s --check-prefix=LISTED

; SCMP: error: intrinsic @llvm.scmp.i32.i32: not on the writer's list of LLVM 15 intrinsics (in @k)
; SCMP-NOT: stats

; LISTED: stats inst-casts=0

;--- scmp.ll
target triple = "air64-apple-macosx14.0.0"

declare i32 @llvm.scmp.i32.i32(i32, i32)

define i32 @k(i32 %a, i32 %b) {
  %c = call i32 @llvm.scmp.i32.i32(i32 %a, i32 %b)
  ret i32 %c
}
;--- listed.ll
target triple = "air64-apple-macosx14.0.0"

declare i64 @llvm.umax.i64(i64, i64)
declare i32 @llvm.usub.sat.i32(i32, i32)

define i32 @k(i64 %a, i64 %b, i32 %c, i32 %d) {
  %m = call i64 @llvm.umax.i64(i64 %a, i64 %b)
  %t = trunc i64 %m to i32
  %s = call i32 @llvm.usub.sat.i32(i32 %t, i32 %c)
  ret i32 %s
}
