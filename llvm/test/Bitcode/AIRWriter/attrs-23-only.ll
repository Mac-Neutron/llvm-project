; LLVM 23-only attribute kinds (codes 85 and up, codes-15-23.txt) are dropped
; only if they merely state a guarantee, so that dropping one loses a fact and
; never changes what the program does. The drop list, with why:
;   nofpclass(...)          the value is not of these FP classes, else poison
;   range(...)              the value is in the range, else poison
;   writable                the caller says the memory may be written
;   dead_on_unwind          the memory is dead if the call unwinds
;   initializes(...)        the callee writes these bytes before reading them
;   dead_on_return          the memory is dead when the call returns
;   nocreateundeforpoison   the call makes no undef or poison of its own
;                           (LLVM 23 puts it on intrinsics, e.g. llvm.umax)
; Each dropped entry of each attribute group is counted. Any other 23-only
; kind (here sanitize_realtime) is an error naming it.
; An intrinsic declaration's attributes are the reader's: LLVM 15's and 23's
; readers both reset them from their own intrinsic tables
; (UpgradeIntrinsicFunction). So the writer writes the lowered group, counts
; its drops, and leaves the declaration with its LLVM 23 table attributes,
; which is what LLVM 23 reads back (P9's self row).
; RUN: split-file %s %t
; RUN: llvm-air-write %t/drop.ll --types %t/drop.types --emit-ll %t/drop -o %t/drop.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t/drop.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t/drop.bc -o - | FileCheck %s --check-prefix=DIS
; RUN: FileCheck %s --check-prefix=DIS < %t/drop.post.ll
; RUN: not llvm-air-write %t/error.ll -o %t/error.bc 2>&1 | FileCheck %s --check-prefix=ERROR
; RUN: llvm-air-write %t/intrinsic.ll --emit-ll %t/intrinsic -o %t/intrinsic.bc 2>&1 | FileCheck %s --check-prefix=ISTATS
; RUN: llvm-bcanalyzer -dump %t/intrinsic.bc | FileCheck %s --check-prefix=IBC
; RUN: FileCheck %s --check-prefix=IPOST < %t/intrinsic.post.ll
; RUN: llvm-dis %t/intrinsic.bc -o %t/intrinsic.dis.ll
; RUN: tail -n +2 %t/intrinsic.dis.ll > %t/intrinsic.dis.tail
; RUN: tail -n +2 %t/intrinsic.post.ll > %t/intrinsic.post.tail
; RUN: diff %t/intrinsic.dis.tail %t/intrinsic.post.tail

; STATS: stats inst-casts=0 const-casts=0 use-typed=0 attrs-dropped=7 flags-dropped=0

; BC-LABEL: <PARAMATTR_GROUP_BLOCK_ID
; BC-NEXT: <ENTRY op0=1 op1=4294967295 op2=0 op3=18/>
; BC-NEXT: </PARAMATTR_GROUP_BLOCK_ID>

; DIS: declare i32 @f(float, ptr addrspace(1)) [[A:#[0-9]+]]
; DIS: attributes [[A]] = { nounwind }

; ERROR: error: attribute sanitize_realtime: no LLVM 15 encoding, and not on the guarantee-only drop list (in @g)
; ERROR-NOT: stats

; ISTATS: stats inst-casts=0 const-casts=0 use-typed=0 attrs-dropped=1 flags-dropped=0
; nocallback nofree nosync nounwind speculatable willreturn, then memory(none)
; as readnone; no nocreateundeforpoison
; IBC-LABEL: <PARAMATTR_GROUP_BLOCK_ID
; IBC-NEXT: <ENTRY op0=1 op1=4294967295 op2=0 op3=71 op4=0 op5=62 op6=0 op7=63 op8=0 op9=18 op10=0 op11=53 op12=0 op13=61 op14=0 op15=20/>
; IBC-NEXT: </PARAMATTR_GROUP_BLOCK_ID>
; IPOST: attributes #0 = { nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none) }

;--- drop.ll
target triple = "air64-apple-macosx14.0.0"

declare range(i32 0, 10) i32 @f(float nofpclass(nan), ptr addrspace(1) writable dead_on_unwind initializes((0, 4)) dead_on_return) #0

attributes #0 = { nocreateundeforpoison nounwind }

;--- drop.types
f @f i32 (float, i32 addrspace(1)*)

;--- error.ll
target triple = "air64-apple-macosx14.0.0"

declare void @g() sanitize_realtime

;--- intrinsic.ll
target triple = "air64-apple-macosx14.0.0"

declare i64 @llvm.umax.i64(i64, i64)

define i64 @k(i64 %a, i64 %b) {
  %m = call i64 @llvm.umax.i64(i64 %a, i64 %b)
  ret i64 %m
}
