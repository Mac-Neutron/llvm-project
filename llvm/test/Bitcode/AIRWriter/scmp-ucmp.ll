; llvm.scmp and llvm.ucmp (LLVM 23's InstCombine makes them; LLVM 15 has
; neither) are written as icmp and select (controller ruling D4): a < b gives
; -1, a > b gives 1, else 0, in the call's return type, which may be wider or
; narrower than the operands'; vectors too. The declaration goes, the result
; keeps the call's name, and the module left behind is what LLVM 23 reads.
; RUN: llvm-air-write %s --explain --emit-ll %t -o %t.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-dis %t.bc -o %t.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t.dis.ll
; RUN: tail -n +2 %t.dis.ll > %t.dis.tail
; RUN: tail -n +2 %t.post.ll > %t.post.tail
; RUN: diff %t.dis.tail %t.post.tail

; STATS: lower @llvm.scmp.i32.i32 to icmp and select (in @s)
; STATS-NEXT: lower @llvm.ucmp.i8.i32 to icmp and select (in @u)
; STATS-NEXT: lower @llvm.scmp.v4i32.v4i32 to icmp and select (in @v)
; STATS: stats inst-casts=0

; DIS-LABEL: define i32 @s(
; DIS-NEXT: %scmp.lt = icmp slt i32 %a, %b
; DIS-NEXT: %scmp.gt = icmp sgt i32 %a, %b
; DIS-NEXT: %scmp.sel = select i1 %scmp.gt, i32 1, i32 0
; DIS-NEXT: %r = select i1 %scmp.lt, i32 -1, i32 %scmp.sel
; DIS-NEXT: ret i32 %r
; DIS-LABEL: define i8 @u(
; DIS-NEXT: %ucmp.lt = icmp ult i32 %a, %b
; DIS-NEXT: %ucmp.gt = icmp ugt i32 %a, %b
; DIS-NEXT: %ucmp.sel = select i1 %ucmp.gt, i8 1, i8 0
; DIS-NEXT: %1 = select i1 %ucmp.lt, i8 -1, i8 %ucmp.sel
; DIS-NEXT: ret i8 %1
; DIS-LABEL: define <4 x i32> @v(
; DIS-NEXT: %scmp.lt = icmp slt <4 x i32> %a, %b
; DIS-NEXT: %scmp.gt = icmp sgt <4 x i32> %a, %b
; DIS-NEXT: %scmp.sel = select <4 x i1> %scmp.gt, <4 x i32> splat (i32 1), <4 x i32> zeroinitializer
; DIS-NEXT: %r = select <4 x i1> %scmp.lt, <4 x i32> splat (i32 -1), <4 x i32> %scmp.sel
; DIS-NOT: declare

target triple = "air64-apple-macosx14.0.0"

declare i32 @llvm.scmp.i32.i32(i32, i32)
declare i8 @llvm.ucmp.i8.i32(i32, i32)
declare <4 x i32> @llvm.scmp.v4i32.v4i32(<4 x i32>, <4 x i32>)

define i32 @s(i32 %a, i32 %b) {
  %r = call i32 @llvm.scmp.i32.i32(i32 %a, i32 %b)
  ret i32 %r
}

define i8 @u(i32 %a, i32 %b) {
  %1 = call i8 @llvm.ucmp.i8.i32(i32 %a, i32 %b)
  ret i8 %1
}

define <4 x i32> @v(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.scmp.v4i32.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}
