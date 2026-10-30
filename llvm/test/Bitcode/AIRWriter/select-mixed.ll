; A select of two pointers of different typed types: the select takes the
; first operand's type (ties go by instruction order) and the other operand is
; cast to it.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/m.ll --types %t/m.types --explain --emit-ll %t/m -o %t/m.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-dis %t/m.bc -o %t/m.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t/m.dis.ll
; RUN: tail -n +2 %t/m.dis.ll > %t/m.dis.tail
; RUN: tail -n +2 %t/m.post.ll > %t/m.post.tail
; RUN: diff %t/m.dis.tail %t/m.post.tail

; STATS: derived %s: i32 addrspace(1)* (in @k)
; STATS: cast %b to i32 addrspace(1)* for select %s (in @k)
; STATS-NOT: {{^}}cast 
; STATS: stats inst-casts=1 const-casts=0 use-typed=0

; DIS: %[[C:[0-9]+]] = bitcast ptr addrspace(1) %b to ptr addrspace(1)
; DIS-NEXT: %s = select i1 %c, ptr addrspace(1) %a, ptr addrspace(1) %[[C]]

;--- m.ll
target triple = "air64-apple-macosx14.0.0"

define void @k(i1 %c, ptr addrspace(1) %a, ptr addrspace(1) %b) {
  %s = select i1 %c, ptr addrspace(1) %a, ptr addrspace(1) %b
  store i32 1, ptr addrspace(1) %s, align 4
  ret void
}
;--- m.types
f @k void (i1, i32 addrspace(1)*, float addrspace(1)*)
