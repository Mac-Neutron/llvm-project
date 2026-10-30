; A struct with pointer fields is written once, under its own name, with a
; typed body: both elements are POINTER records, address spaces kept.
; RUN: llvm-air-write %s --types %S/Inputs/struct-typed-body.types -o %t.bc
; RUN: llvm-bcanalyzer -dump %t.bc | FileCheck %s

; CHECK-LABEL: <TYPE_BLOCK_ID
; CHECK-NEXT: <NUMENTRY
; CHECK-NEXT: <VOID/>
; CHECK-NEXT: <INTEGER op0=32/>
; CHECK-NEXT: <POINTER {{.*}}op0=1 op1=2/>
; CHECK-NEXT: <FLOAT/>
; CHECK-NEXT: <POINTER {{.*}}op0=3 op1=1/>
; CHECK-NEXT: <STRUCT_NAME {{.*}}/> record string = 'argument_buffer_struct'
; CHECK-NEXT: <STRUCT_NAMED {{.*}}op0=0 op1=2 op2=4/>
; CHECK-NOT: argument_buffer_struct
; CHECK-NOT: OPAQUE_POINTER
; CHECK-NOT: dxilOpaquePtrReservedName
; CHECK: </TYPE_BLOCK_ID>
; CHECK-NOT: argument_buffer_struct
; CHECK-NOT: dxilOpaquePtrReservedName

target triple = "air64-apple-macosx14.0.0"

%argument_buffer_struct = type { ptr addrspace(2), ptr addrspace(1) }

define void @k(ptr addrspace(2) %ab) {
  %field = getelementptr %argument_buffer_struct, ptr addrspace(2) %ab, i64 0, i32 1
  %out = load ptr addrspace(1), ptr addrspace(2) %field, align 8
  store float 1.0, ptr addrspace(1) %out, align 4
  ret void
}
