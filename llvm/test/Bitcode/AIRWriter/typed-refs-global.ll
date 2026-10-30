; A global typed by a typed ref: written once with its g line, then with no g
; line and that output as the ref (version 1: GLOBALVAR with an explicit type,
; names from the module VST). The same bytes, one name filled.
; RUN: echo 'g @g i32 addrspace(1)*' > %t.types
; RUN: llvm-air-write %s --types %t.types -o %t.bc 2>&1 | FileCheck %s --check-prefix=TABLE
; RUN: llvm-air-write %s --ref %t.bc -o %t.ref.bc 2>&1 | FileCheck %s --check-prefix=REF
; RUN: cmp %t.bc %t.ref.bc

; TABLE: stats {{.*}}refs=0 stale=0
; REF: stats {{.*}}refs=1 stale=0

target triple = "air64-apple-macosx14.0.0"

@g = external addrspace(2) global ptr addrspace(1)
