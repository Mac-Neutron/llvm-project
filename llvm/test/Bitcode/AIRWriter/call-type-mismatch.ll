; A call whose function type is not its callee's (opaque IR allows it): LLVM
; 15 needs the call's explicit type to be the callee's pointee, so the callee
; is a constant bitcast to the call's typed function type, and the arguments
; are typed from that type, never from the callee's own parameters.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/m.ll --types %t/m.types --explain --emit-ll %t/m -o %t/m.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t/m.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t/m.bc -o %t/m.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t/m.dis.ll
; RUN: tail -n +2 %t/m.dis.ll > %t/m.dis.tail
; RUN: tail -n +2 %t/m.post.ll > %t/m.post.tail
; RUN: diff %t/m.dis.tail %t/m.post.tail

; STATS: cast @ext to void (i32 addrspace(1)*, i32)* (constant)
; STATS-NOT: {{^}}cast 
; STATS: stats inst-casts=0 const-casts=1 use-typed=0

; BC-LABEL: <FUNCTION_BLOCK
; BC: <CONSTANTS_BLOCK
; BC: <CE_CAST {{.*}}op0=11 
; BC: </CONSTANTS_BLOCK>
; BC-NOT: <INST_CAST
; BC: <INST_CALL

; DIS: call void @ext(ptr addrspace(1) %p, i32 %x)

;--- m.ll
target triple = "air64-apple-macosx14.0.0"

declare void @ext(ptr addrspace(1))

define void @k(ptr addrspace(1) %p, i32 %x) {
  call void @ext(ptr addrspace(1) %p, i32 %x)
  ret void
}
;--- m.types
f @ext void (float addrspace(1)*)
f @k void (i32 addrspace(1)*, i32)
