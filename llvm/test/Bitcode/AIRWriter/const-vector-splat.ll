; Ruling P1 (F-D5-1): a vector-typed ConstantFP or ConstantInt (a splat) is
; written as LLVM 15's CST_CODE_DATA, the element repeated, FP elements by
; their bits; a +0.0 splat has isNullValue() and stays CST_CODE_NULL. A splat
; of integers wider than 64 bits is an error.
; RUN: split-file %s %t
; RUN: llvm-as %t/fp.ll -o %t/fp.in.bc
; RUN: llvm-air-write %t/fp.in.bc -o %t/fp.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t/fp.bc | FileCheck %s --check-prefix=FP-BC
; RUN: llvm-dis %t/fp.bc -o - | FileCheck %s --check-prefix=FP-DIS
; RUN: llvm-as -use-constant-int-for-fixed-length-splat %t/int.ll -o %t/int.in.bc
; RUN: llvm-air-write -use-constant-int-for-fixed-length-splat %t/int.in.bc -o %t/int.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t/int.bc | FileCheck %s --check-prefix=INT-BC
; RUN: llvm-dis %t/int.bc -o - | FileCheck %s --check-prefix=INT-DIS
; RUN: llvm-as -use-constant-int-for-fixed-length-splat %t/wide.ll -o %t/wide.in.bc
; RUN: not llvm-air-write -use-constant-int-for-fixed-length-splat %t/wide.in.bc -o %t/wide.bc 2>&1 | FileCheck %s --check-prefix=WIDE

; STATS: stats inst-casts=0 const-casts=0 use-typed=0 attrs-dropped=0 flags-dropped=0

; FP-BC-LABEL: <CONSTANTS_BLOCK
; FP-BC-NOT: <FLOAT
; FP-BC: <DATA op0=1065353216 op1=1065353216 op2=1065353216/>
; FP-BC-NOT: <FLOAT
; FP-BC: <DATA op0=15360 op1=15360 op2=15360 op3=15360/>
; FP-BC-NOT: <FLOAT
; FP-BC: <DATA op0=-9223372036854775808 op1=-9223372036854775808/>
; FP-BC-NOT: <FLOAT
; FP-BC: <NULL/>
; FP-BC-NOT: <FLOAT
; FP-BC: </CONSTANTS_BLOCK>

; FP-DIS-LABEL: define void @k(
; FP-DIS-NEXT: store <3 x float> splat (float 1.000000e+00), ptr addrspace(1) %p0, align 16
; FP-DIS-NEXT: store <4 x half> splat (half 1.000000e+00), ptr addrspace(1) %p1, align 8
; FP-DIS-NEXT: store <2 x double> splat (double -0.000000e+00), ptr addrspace(1) %p2, align 16
; FP-DIS-NEXT: store <4 x float> zeroinitializer, ptr addrspace(1) %p3, align 16
; FP-DIS-NEXT: ret void

; INT-BC-LABEL: <CONSTANTS_BLOCK
; INT-BC-NOT: <INTEGER
; INT-BC: <DATA op0=7 op1=7 op2=7 op3=7/>
; INT-BC-NOT: <INTEGER
; INT-BC: </CONSTANTS_BLOCK>

; INT-DIS-LABEL: define void @k(
; INT-DIS-NEXT: store <4 x i32> splat (i32 7), ptr addrspace(1) %p, align 16
; INT-DIS-NEXT: ret void

; WIDE: error: constant: a vector splat of <2 x i128> has no LLVM 15 encoding
; WIDE-NOT: stats

;--- fp.ll
target triple = "air64-apple-macosx14.0.0"

define void @k(ptr addrspace(1) %p0, ptr addrspace(1) %p1, ptr addrspace(1) %p2, ptr addrspace(1) %p3) {
  store <3 x float> splat (float 1.0), ptr addrspace(1) %p0, align 16
  store <4 x half> splat (half 0xH3C00), ptr addrspace(1) %p1, align 8
  store <2 x double> splat (double -0.0), ptr addrspace(1) %p2, align 16
  store <4 x float> splat (float 0.0), ptr addrspace(1) %p3, align 16
  ret void
}
;--- int.ll
target triple = "air64-apple-macosx14.0.0"

define void @k(ptr addrspace(1) %p) {
  store <4 x i32> splat (i32 7), ptr addrspace(1) %p, align 16
  ret void
}
;--- wide.ll
target triple = "air64-apple-macosx14.0.0"

define void @k(ptr addrspace(1) %p) {
  store <2 x i128> splat (i128 7), ptr addrspace(1) %p, align 16
  ret void
}
