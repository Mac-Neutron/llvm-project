; P13: an llvm.* declaration the writer's list does not hold is an error
; naming it, also without pointer parameters: llvm.exp10, which LLVM 15
; lacks (llvm.scmp, which LLVM 23's InstCombine makes, is lowered instead:
; scmp-ucmp.ll). Every entry on the list is written: the
; corpus's (Task 7: the C++ AIR's min, max, funnel-shift and saturating
; calls; LLVM 23's passes add fabs, copysign and is.fpclass), each with
; LLVM 15's signature.
; RUN: split-file %s %t
; RUN: not llvm-air-write %t/exp10.ll -o %t/exp10.bc 2>&1 | FileCheck %s --check-prefix=EXP10
; RUN: llvm-air-write %t/listed.ll -o %t/listed.bc 2>&1 | FileCheck %s --check-prefix=LISTED

; EXP10: error: intrinsic @llvm.exp10.f32: not on the writer's list of LLVM 15 intrinsics (in @k)
; EXP10-NOT: stats

; LISTED: stats inst-casts=0

;--- exp10.ll
target triple = "air64-apple-macosx14.0.0"

declare float @llvm.exp10.f32(float)

define float @k(float %a) {
  %c = call float @llvm.exp10.f32(float %a)
  ret float %c
}
;--- listed.ll
target triple = "air64-apple-macosx14.0.0"

declare i64 @llvm.umax.i64(i64, i64)
declare i32 @llvm.usub.sat.i32(i32, i32)
declare i32 @llvm.umax.i32(i32, i32)
declare i32 @llvm.umin.i32(i32, i32)
declare i64 @llvm.umin.i64(i64, i64)
declare i32 @llvm.smax.i32(i32, i32)
declare i64 @llvm.smax.i64(i64, i64)
declare i32 @llvm.smin.i32(i32, i32)
declare i64 @llvm.smin.i64(i64, i64)
declare i32 @llvm.fshl.i32(i32, i32, i32)
declare float @llvm.fabs.f32(float)
declare <4 x float> @llvm.fabs.v4f32(<4 x float>)
declare <4 x i1> @llvm.is.fpclass.v4f32(<4 x float>, i32 immarg)
declare i1 @llvm.is.fpclass.f32(float, i32 immarg)
declare float @llvm.copysign.f32(float, float)
declare half @llvm.copysign.f16(half, half)

define i32 @k(i64 %a, i64 %b, i32 %c, i32 %d) {
  %m = call i64 @llvm.umax.i64(i64 %a, i64 %b)
  %t = trunc i64 %m to i32
  %s = call i32 @llvm.usub.sat.i32(i32 %t, i32 %c)
  %u1 = call i32 @llvm.umax.i32(i32 %s, i32 %d)
  %u2 = call i32 @llvm.umin.i32(i32 %u1, i32 %c)
  %s1 = call i32 @llvm.smax.i32(i32 %u2, i32 %d)
  %s2 = call i32 @llvm.smin.i32(i32 %s1, i32 %c)
  %f = call i32 @llvm.fshl.i32(i32 %s2, i32 %d, i32 3)
  %l1 = call i64 @llvm.umin.i64(i64 %a, i64 %b)
  %l2 = call i64 @llvm.smax.i64(i64 %l1, i64 %b)
  %l3 = call i64 @llvm.smin.i64(i64 %l2, i64 %a)
  %lt = trunc i64 %l3 to i32
  %r = add i32 %f, %lt
  ret i32 %r
}

define <4 x i1> @fp(float %x, <4 x float> %v) {
  %a = call float @llvm.fabs.f32(float %x)
  %va = call <4 x float> @llvm.fabs.v4f32(<4 x float> %v)
  %w = insertelement <4 x float> %va, float %a, i32 0
  %c = call <4 x i1> @llvm.is.fpclass.v4f32(<4 x float> %w, i32 516)
  %n = call i1 @llvm.is.fpclass.f32(float %a, i32 3)
  %c2 = insertelement <4 x i1> %c, i1 %n, i32 1
  ret <4 x i1> %c2
}

define half @sign(float %x, float %y, half %h, half %g) {
  %s = call float @llvm.copysign.f32(float %x, float %y)
  %t = fptrunc float %s to half
  %r = call half @llvm.copysign.f16(half %t, half %g)
  ret half %r
}
