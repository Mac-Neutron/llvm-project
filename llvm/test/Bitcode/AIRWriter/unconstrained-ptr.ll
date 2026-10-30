; A pointer no seed reaches (an inttoptr only compared with null): no access
; goes through it, so it is typed i8 in its own address space and counted as
; use-typed, with an --explain line. The null takes the comparison's type.
; RUN: llvm-air-write %s --explain --emit-ll %t -o %t.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-dis %t.bc -o %t.dis.ll
; RUN: tail -n +2 %t.dis.ll > %t.dis.tail
; RUN: tail -n +2 %t.post.ll > %t.post.tail
; RUN: diff %t.dis.tail %t.post.tail

; STATS-NOT: derived
; STATS: use-typed %p: i8 addrspace(3)* (in @k)
; STATS-NOT: {{^}}cast 
; STATS: stats inst-casts=0 const-casts=0 use-typed=1

target triple = "air64-apple-macosx14.0.0"

define i1 @k(i64 %a) {
  %p = inttoptr i64 %a to ptr addrspace(3)
  %c = icmp eq ptr addrspace(3) %p, null
  ret i1 %c
}
