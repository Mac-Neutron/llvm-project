; Pointer arguments typed through their function's f line: two POINTER records
; with address spaces 1 and 3, no opaque pointer.
; RUN: llvm-air-write %s --types %S/Inputs/typed-pointers.types -o %t.bc
; RUN: llvm-bcanalyzer -dump %t.bc | FileCheck %s

; CHECK: <TYPE_BLOCK_ID
; CHECK-DAG: <POINTER {{.*}}op1=1/>
; CHECK-DAG: <POINTER {{.*}}op1=3/>
; CHECK: </TYPE_BLOCK_ID>
; CHECK-NOT: OPAQUE_POINTER
; CHECK-NOT: dxilOpaquePtrReservedName

target triple = "air64-apple-macosx14.0.0"

define void @k(ptr addrspace(1) %out, ptr addrspace(3) %tg) {
  %v = load i32, ptr addrspace(3) %tg, align 4
  %f = sitofp i32 %v to float
  store float %f, ptr addrspace(1) %out, align 4
  ret void
}
