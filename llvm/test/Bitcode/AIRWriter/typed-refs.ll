; A declaration with no f line, typed by a typed ref: Apple's AIR of
; Inputs/helper-ref.metal declares air.atomic.local.add.s.i32 with an
; i32 addrspace(3)* parameter.
; RUN: llvm-air-write %s --ref %S/Inputs/helper-ref.air -o %t.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t.bc | FileCheck %s

; An explicit f line equal to the ref's type: no conflict, nothing filled, the
; same bytes. So the ref's type is exactly this one.
; RUN: echo 'f @air.atomic.local.add.s.i32 i32 (i32 addrspace(3)*, i32, i32, i32, i1)' > %t.types
; RUN: llvm-air-write %s --types %t.types --ref %S/Inputs/helper-ref.air -o %t.agree.bc 2>&1 | FileCheck %s --check-prefix=AGREE
; RUN: cmp %t.bc %t.agree.bc

; Our own output as the ref (version 1: names from the module VST): the same
; bytes.
; RUN: llvm-air-write %s --ref %t.bc -o %t.self.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: cmp %t.bc %t.self.bc

; STATS: stats {{.*}}use-typed=0 {{.*}}refs=1 stale=0
; AGREE: stats {{.*}}use-typed=0 {{.*}}refs=0 stale=0

; CHECK: <TYPE_BLOCK_ID
; CHECK-DAG: <POINTER {{.*}}op1=3/>
; CHECK: </TYPE_BLOCK_ID>
; CHECK-NOT: OPAQUE_POINTER

target triple = "air64-apple-macosx14.0.0"

@tg = internal addrspace(3) global i32 undef, align 4

define i32 @k() {
  %r = call i32 @air.atomic.local.add.s.i32(ptr addrspace(3) @tg, i32 1, i32 0, i32 1, i1 true)
  ret i32 %r
}

declare i32 @air.atomic.local.add.s.i32(ptr addrspace(3), i32, i32, i32, i1)
