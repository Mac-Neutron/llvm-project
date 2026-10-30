; LLVM 23's ptradd form, getelementptr i8, over a float addrspace(1)*: the GEP
; is typed from its own source type (i8), so its base is cast to i8* and its
; result, loaded as float, is cast to float*.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/m.ll --types %t/m.types --explain --emit-ll %t/m -o %t/m.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-dis %t/m.bc -o %t/m.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t/m.dis.ll
; RUN: tail -n +2 %t/m.dis.ll > %t/m.dis.tail
; RUN: tail -n +2 %t/m.post.ll > %t/m.post.tail
; RUN: diff %t/m.dis.tail %t/m.post.tail

; STATS: cast %p to i8 addrspace(1)* for getelementptr %g (in @k)
; STATS: cast %g to float addrspace(1)* for load %v (in @k)
; STATS-NOT: {{^}}cast 
; STATS: stats inst-casts=2 const-casts=0 use-typed=0

; DIS: %[[B:[0-9]+]] = bitcast ptr addrspace(1) %p to ptr addrspace(1)
; DIS-NEXT: %g = getelementptr inbounds i8, ptr addrspace(1) %[[B]], i64 16
; DIS-NEXT: %[[R:[0-9]+]] = bitcast ptr addrspace(1) %g to ptr addrspace(1)
; DIS-NEXT: %v = load float, ptr addrspace(1) %[[R]], align 4

;--- m.ll
target triple = "air64-apple-macosx14.0.0"

define float @k(ptr addrspace(1) %p) {
  %g = getelementptr inbounds i8, ptr addrspace(1) %p, i64 16
  %v = load float, ptr addrspace(1) %g, align 4
  ret float %v
}
;--- m.types
f @k float (float addrspace(1)*)
