; A call argument whose typed type is not the callee's typed parameter is cast
; to the parameter's type before the call.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/m.ll --types %t/m.types --explain --emit-ll %t/m -o %t/m.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t/m.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t/m.bc -o %t/m.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t/m.dis.ll
; RUN: tail -n +2 %t/m.dis.ll > %t/m.dis.tail
; RUN: tail -n +2 %t/m.post.ll > %t/m.post.tail
; RUN: diff %t/m.dis.tail %t/m.post.tail

; STATS: cast %p to float addrspace(1)* for call @air.use arg 0 (in @k)
; STATS-NOT: {{^}}cast 
; STATS: stats inst-casts=1 const-casts=0 use-typed=0

; BC-LABEL: <FUNCTION_BLOCK
; BC: <INST_CAST {{.*}}op2=11/>
; BC-NOT: <INST_CAST

; DIS: %[[C:[0-9]+]] = bitcast ptr addrspace(1) %p to ptr addrspace(1)
; DIS-NEXT: call void @air.use(ptr addrspace(1) %[[C]])

;--- m.ll
target triple = "air64-apple-macosx14.0.0"

declare void @air.use(ptr addrspace(1))

define void @k(ptr addrspace(1) %p) {
  call void @air.use(ptr addrspace(1) %p)
  ret void
}
;--- m.types
f @air.use void (float addrspace(1)*)
f @k void (i32 addrspace(1)*)
