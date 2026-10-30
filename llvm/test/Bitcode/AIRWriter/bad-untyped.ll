; What stays untyped after P5's derivation is an identified struct with
; pointer fields and no typed body (P4 requires its s line, and P5 types no
; identified struct's fields): an error naming it. A pointer argument with
; no entry, the error this test checked before P5, is now derived from its
; uses (here a store of i32).
; RUN: split-file %s %t
; RUN: not llvm-air-write %t/struct.ll -o %t/struct.bc 2>&1 | FileCheck %s --check-prefix=STRUCT
; RUN: llvm-air-write %t/arg.ll --explain -o %t/arg.bc 2>&1 | FileCheck %s --check-prefix=ARG

; STRUCT: error: struct %struct.S: pointer fields with no typed body
; STRUCT-NOT: stats

; ARG: derived %tg: i32 addrspace(3)* (in @k)
; ARG: stats inst-casts=0 const-casts=0 use-typed=0

;--- struct.ll
target triple = "air64-apple-macosx14.0.0"

%struct.S = type { ptr addrspace(1), i32 }

define i32 @k(ptr addrspace(2) %s) {
  %f = getelementptr inbounds %struct.S, ptr addrspace(2) %s, i64 0, i32 1
  %v = load i32, ptr addrspace(2) %f, align 4
  ret i32 %v
}
;--- arg.ll
target triple = "air64-apple-macosx14.0.0"

define void @k(i32 %n, ptr addrspace(3) %tg) {
  store i32 %n, ptr addrspace(3) %tg, align 4
  ret void
}
