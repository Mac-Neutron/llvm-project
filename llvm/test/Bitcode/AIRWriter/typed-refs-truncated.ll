; A truncated typed ref is an error, not a crash: the wrapped file cut short
; (its wrapper header then names more bytes than there are), and the bitcode
; inside the wrapper cut short.
; RUN: head -c 1000 %S/Inputs/helper-ref.air > %t.wrapped.air
; RUN: not llvm-air-write %s --ref %t.wrapped.air -o %t.bc 2>&1 | FileCheck %s --check-prefix=WRAPPED
; RUN: tail -c +21 %S/Inputs/helper-ref.air | head -c 1500 > %t.raw.air
; RUN: not llvm-air-write %s --ref %t.raw.air -o %t.bc 2>&1 | FileCheck %s --check-prefix=RAW

; WRAPPED: error: typed ref 0: invalid bitcode wrapper header
; WRAPPED-NOT: stats
; RAW: error: typed ref 0: malformed
; RAW-NOT: stats

target triple = "air64-apple-macosx14.0.0"

@tg = internal addrspace(3) global i32 undef, align 4

define i32 @k() {
  %r = call i32 @air.atomic.local.add.s.i32(ptr addrspace(3) @tg, i32 1, i32 0, i32 1, i1 true)
  ret i32 %r
}

declare i32 @air.atomic.local.add.s.i32(ptr addrspace(3), i32, i32, i32, i1)
