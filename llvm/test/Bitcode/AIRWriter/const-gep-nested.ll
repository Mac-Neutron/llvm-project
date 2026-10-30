; A constant GEP whose base is not of its source type: LLVM 23's i8 GEP over
; the [2 x i8] global @sample_pos_2 gets a CE_CAST of the global to i8
; addrspace(2)* nested inside it, written before the GEP. Its nuw and
; inrange, which LLVM 15 cannot hold, are dropped and counted per use.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/m.ll --explain --emit-ll %t/m -o %t/m.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t/m.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t/m.bc -o %t/m.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t/m.dis.ll
; RUN: tail -n +2 %t/m.dis.ll > %t/m.dis.tail
; RUN: tail -n +2 %t/m.post.ll > %t/m.post.tail
; RUN: diff %t/m.dis.tail %t/m.post.tail

; STATS: drop 2 constant getelementptr flag(s) on %v (in @k)
; STATS: cast @sample_pos_2 to i8 addrspace(2)* (constant)
; STATS-NOT: {{^}}cast 
; STATS: stats inst-casts=0 const-casts=1 use-typed=0 attrs-dropped=0 flags-dropped=2

; BC-LABEL: <FUNCTION_BLOCK
; BC: <CONSTANTS_BLOCK
; BC: <CE_CAST {{.*}}op0=11 
; BC: <CE_INBOUNDS_GEP
; BC: </CONSTANTS_BLOCK>
; BC-NOT: <INST_CAST

; DIS: %v = load i8, ptr addrspace(2) getelementptr inbounds (i8, ptr addrspace(2) @sample_pos_2, i64 1), align 1

;--- m.ll
target triple = "air64-apple-macosx14.0.0"

@sample_pos_2 = internal addrspace(2) constant [2 x i8] c"\01\02"

define i8 @k() {
  %v = load i8, ptr addrspace(2) getelementptr inbounds nuw inrange(-1, 1) (i8, ptr addrspace(2) @sample_pos_2, i64 1), align 1
  ret i8 %v
}
