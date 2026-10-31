; An explicit f line that disagrees with a typed ref is an error (P16).
; RUN: echo 'f @air.atomic.local.add.s.i32 i32 (float addrspace(3)*, i32, i32, i32, i1)' > %t.types
; RUN: not llvm-air-write %s --types %t.types --ref %S/Inputs/helper-ref.air -o %t.bc 2>&1 | FileCheck %s

; CHECK: error: typed ref: @air.atomic.local.add.s.i32 is i32 (i32 addrspace(3)*, i32, i32, i32, i1), its entry i32 (float addrspace(3)*, i32, i32, i32, i1)
; CHECK-NOT: stats

target triple = "air64-apple-macosx14.0.0"

@tg = internal addrspace(3) global i32 undef, align 4

define i32 @k() {
  %r = call i32 @air.atomic.local.add.s.i32(ptr addrspace(3) @tg, i32 1, i32 0, i32 1, i1 true)
  ret i32 %r
}

declare i32 @air.atomic.local.add.s.i32(ptr addrspace(3), i32, i32, i32, i1)
