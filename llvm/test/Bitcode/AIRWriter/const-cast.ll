; A global whose typed type is not what its use needs is a constant cast
; (CE_CAST, bitcast), never an instruction: @g is [4 x float] addrspace(2)*,
; the parameter i8 addrspace(2)*.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/m.ll --types %t/m.types --explain --emit-ll %t/m -o %t/m.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t/m.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t/m.bc -o %t/m.dis.ll
; RUN: tail -n +2 %t/m.dis.ll > %t/m.dis.tail
; RUN: tail -n +2 %t/m.post.ll > %t/m.post.tail
; RUN: diff %t/m.dis.tail %t/m.post.tail

; STATS: cast @g to i8 addrspace(2)* (constant)
; STATS-NOT: {{^}}cast 
; STATS: stats inst-casts=0 const-casts=1 use-typed=0

; BC-LABEL: <FUNCTION_BLOCK
; BC: <CONSTANTS_BLOCK
; BC: <CE_CAST {{.*}}op0=11 
; BC-NOT: <INST_CAST

;--- m.ll
target triple = "air64-apple-macosx14.0.0"

@g = internal addrspace(2) constant [4 x float] zeroinitializer

declare void @air.take(ptr addrspace(2))

define void @k() {
  call void @air.take(ptr addrspace(2) @g)
  ret void
}
;--- m.types
f @air.take void (i8 addrspace(2)*)
