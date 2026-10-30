; An inttoptr has no definition fact; the fixpoint types it from its use (a
; float load), so it is derived, listed by --explain, and not use-typed.
; RUN: llvm-air-write %s --explain --emit-ll %t -o %t.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-dis %t.bc -o %t.dis.ll
; RUN: tail -n +2 %t.dis.ll > %t.dis.tail
; RUN: tail -n +2 %t.post.ll > %t.post.tail
; RUN: diff %t.dis.tail %t.post.tail

; STATS: derived %p: float addrspace(1)* (in @k)
; STATS-NOT: {{^}}cast 
; STATS-NOT: use-typed
; STATS: stats inst-casts=0 const-casts=0 use-typed=0

target triple = "air64-apple-macosx14.0.0"

define float @k(i64 %a) {
  %p = inttoptr i64 %a to ptr addrspace(1)
  %v = load float, ptr addrspace(1) %p, align 4
  ret float %v
}
