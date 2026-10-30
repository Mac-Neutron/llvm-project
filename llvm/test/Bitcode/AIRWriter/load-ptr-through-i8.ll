; A pointer loaded through an i8* slot: the slot's pointee says nothing about
; it, so the fixpoint types it from its use (an i32 load), and the slot is cast
; to point to that type. Nothing is use-typed.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/m.ll --types %t/m.types --explain --emit-ll %t/m -o %t/m.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-dis %t/m.bc -o %t/m.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t/m.dis.ll
; RUN: tail -n +2 %t/m.dis.ll > %t/m.dis.tail
; RUN: tail -n +2 %t/m.post.ll > %t/m.post.tail
; RUN: diff %t/m.dis.tail %t/m.post.tail

; STATS: derived %p: i32 addrspace(1)* (in @k)
; STATS: cast %slot to i32 addrspace(1)* addrspace(2)* for load %p (in @k)
; STATS-NOT: {{^}}cast 
; STATS: stats inst-casts=1 const-casts=0 use-typed=0

; DIS: %[[C:[0-9]+]] = bitcast ptr addrspace(2) %slot to ptr addrspace(2)
; DIS-NEXT: %p = load ptr addrspace(1), ptr addrspace(2) %[[C]], align 8

;--- m.ll
target triple = "air64-apple-macosx14.0.0"

define i32 @k(ptr addrspace(2) %slot) {
  %p = load ptr addrspace(1), ptr addrspace(2) %slot, align 8
  %v = load i32, ptr addrspace(1) %p, align 4
  ret i32 %v
}
;--- m.types
f @k i32 (i8 addrspace(2)*)
