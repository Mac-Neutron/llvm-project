; P6: the writer never invents an addrspacecast. In verified opaque IR every
; use's outermost address space is its operand's own (table entries and refs
; are erasure-checked; load, store, GEP and calls keep the operand's address
; space), so a differing address space is a nested one, and a cast in the
; operand's own address space reaches it: a store of an addrspace(3) pointer
; into a slot typed to hold addrspace(1) pointers casts the slot, in
; addrspace(1), to point to an addrspace(3) pointer. What no cast reaches is
; an aggregate: a struct holding a pointer typed one way, returned where the
; function's typed type says another, is an error naming it.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/nested.ll --types %t/nested.types --explain --emit-ll %t/nested -o %t/nested.bc 2>&1 | FileCheck %s --check-prefix=NESTED
; RUN: llvm-dis %t/nested.bc -o %t/nested.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t/nested.dis.ll
; RUN: tail -n +2 %t/nested.dis.ll > %t/nested.dis.tail
; RUN: tail -n +2 %t/nested.post.ll > %t/nested.post.tail
; RUN: diff %t/nested.dis.tail %t/nested.post.tail
; RUN: not llvm-air-write %t/agg.ll --types %t/agg.types -o %t/agg.bc 2>&1 | FileCheck %s --check-prefix=AGG

; NESTED: cast %slot to i32 addrspace(3)* addrspace(1)* for store (in @k)
; NESTED: stats inst-casts=1 const-casts=0 use-typed=0

; DIS-NOT: addrspacecast
; DIS: %[[C:[0-9]+]] = bitcast ptr addrspace(1) %slot to ptr addrspace(1)
; DIS-NEXT: store ptr addrspace(3) %v, ptr addrspace(1) %[[C]], align 8
; DIS-NOT: addrspacecast

; AGG: error: ret: needs { float addrspace(1)*, i32 }, %s is { i32 addrspace(1)*, i32 }, and no cast reaches an aggregate (in @k)
; AGG-NOT: stats

;--- nested.ll
target triple = "air64-apple-macosx14.0.0"

define void @k(ptr addrspace(1) %slot, ptr addrspace(3) %v) {
  store ptr addrspace(3) %v, ptr addrspace(1) %slot, align 8
  ret void
}
;--- nested.types
f @k void (float addrspace(1)* addrspace(1)*, i32 addrspace(3)*)
;--- agg.ll
target triple = "air64-apple-macosx14.0.0"

define { ptr addrspace(1), i32 } @k({ ptr addrspace(1), i32 } %s) {
  ret { ptr addrspace(1), i32 } %s
}
;--- agg.types
f @k { float addrspace(1)*, i32 } ({ i32 addrspace(1)*, i32 })
