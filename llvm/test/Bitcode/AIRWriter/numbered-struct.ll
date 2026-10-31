; An unnamed identified struct is named in the types file as Module::print
; numbers it (%0, %1, ...; air15 types writes the same names), numbered in the
; module as read, before --passes: here globaldce deletes @dead, after which
; Module::print would call @f's struct %0.
; RUN: llvm-air-write %s --types %S/Inputs/numbered-struct.types -o %t.bc
; RUN: llvm-dis %t.bc -o - | FileCheck %s
; RUN: llvm-air-write %s --types %S/Inputs/numbered-struct.types --passes globaldce -o %t.dce.bc 2>&1 | FileCheck --check-prefix=DCE %s
; RUN: echo 'f @f %0 (i32 addrspace(1)*)' > %t.swapped.types
; RUN: not llvm-air-write %s --types %t.swapped.types -o %t.bad.bc 2>&1 | FileCheck --check-prefix=SWAPPED %s
; RUN: echo 'f @f %2 (i32 addrspace(1)*)' > %t.past.types
; RUN: not llvm-air-write %s --types %t.past.types -o %t.bad.bc 2>&1 | FileCheck --check-prefix=PAST %s

; CHECK: %0 = type { float, i8 }
; CHECK: %1 = type { <2 x float>, i8, i8 }
; CHECK: define internal %0 @dead(ptr addrspace(1) %p)
; CHECK: define %1 @f(ptr addrspace(1) %p)

; DCE: stale @dead
; DCE: stats {{.*}}stale=1

; SWAPPED: error: table: @f does not erase to

; PAST: error: unknown struct %2

target triple = "air64-apple-macosx14.0.0"

%0 = type { float, i8 }
%1 = type { <2 x float>, i8, i8 }

define internal %0 @dead(ptr addrspace(1) %p) {
  store i32 0, ptr addrspace(1) %p, align 4
  ret %0 zeroinitializer
}

define %1 @f(ptr addrspace(1) %p) {
  store i32 1, ptr addrspace(1) %p, align 4
  ret %1 zeroinitializer
}
