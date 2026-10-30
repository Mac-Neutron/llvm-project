; A constant inttoptr has no pointee of its own: it is written at the type its
; use needs (here a load of i32, so i32 addrspace(1)*), as the corpus's
; inttoptr (i64 4 to i32 addrspace(1)*), with no cast.
; RUN: llvm-air-write %s --explain --emit-ll %t -o %t.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t.bc -o %t.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t.dis.ll
; RUN: tail -n +2 %t.dis.ll > %t.dis.tail
; RUN: tail -n +2 %t.post.ll > %t.post.tail
; RUN: diff %t.dis.tail %t.post.tail

; STATS-NOT: {{^}}cast 
; STATS: stats inst-casts=0 const-casts=0 use-typed=0

; BC-LABEL: <FUNCTION_BLOCK
; BC: <CONSTANTS_BLOCK
; BC: <SETTYPE
; BC-NEXT: <INTEGER {{.*}}op0=8/>
; BC-NEXT: <SETTYPE
; BC-NEXT: <CE_CAST {{.*}}op0=10 
; BC-NOT: <INST_CAST

; DIS: %v = load i32, ptr addrspace(1) inttoptr (i64 4 to ptr addrspace(1)), align 4

target triple = "air64-apple-macosx14.0.0"

define i32 @k() {
  %v = load i32, ptr addrspace(1) inttoptr (i64 4 to ptr addrspace(1)), align 4
  ret i32 %v
}
