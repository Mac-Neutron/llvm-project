; A load whose type is not its pointer's typed pointee: the pointer is cast
; (bitcast ptr to ptr, inserted before the load) to point to the loaded type.
; One INST_CAST with the bitcast opcode (11); the store needs none.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/m.ll --types %t/m.types --explain --emit-ll %t/m -o %t/m.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t/m.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t/m.bc -o %t/m.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t/m.dis.ll
; RUN: tail -n +2 %t/m.dis.ll > %t/m.dis.tail
; RUN: tail -n +2 %t/m.post.ll > %t/m.post.tail
; RUN: diff %t/m.dis.tail %t/m.post.tail

; STATS: cast %g to i32 addrspace(3)* for load %v (in @k)
; STATS-NOT: {{^}}cast 
; STATS: stats inst-casts=1 const-casts=0 use-typed=0

; BC-LABEL: <FUNCTION_BLOCK
; BC: <INST_CAST {{.*}}op2=11/>
; BC-NOT: <INST_CAST

; DIS: %g = getelementptr [4 x float], ptr addrspace(3) %tg, i64 0, i64 2
; DIS-NEXT: %[[C:[0-9]+]] = bitcast ptr addrspace(3) %g to ptr addrspace(3)
; DIS-NEXT: %v = load i32, ptr addrspace(3) %[[C]], align 4
; DIS-NEXT: store i32 %v, ptr addrspace(1) %out, align 4

;--- m.ll
target triple = "air64-apple-macosx14.0.0"

define void @k(ptr addrspace(3) %tg, ptr addrspace(1) %out) {
  %g = getelementptr [4 x float], ptr addrspace(3) %tg, i64 0, i64 2
  %v = load i32, ptr addrspace(3) %g, align 4
  store i32 %v, ptr addrspace(1) %out, align 4
  ret void
}
;--- m.types
f @k void ([4 x float] addrspace(3)*, i32 addrspace(1)*)
