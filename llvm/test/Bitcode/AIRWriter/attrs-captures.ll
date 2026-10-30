; captures(none) is written as LLVM 15's nocapture (11) and reads back as
; itself; any other captures(...) is dropped from the module and counted.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/m.ll --types %t/m.types --explain --emit-ll %t/m -o %t/m.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t/m.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t/m.bc -o %t/m.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t/m.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t/m.post.ll

; STATS: drop attribute captures(address) (in @f)
; STATS: stats inst-casts=0 const-casts=0 use-typed=0 attrs-dropped=1 flags-dropped=0

; BC-LABEL: <PARAMATTR_GROUP_BLOCK_ID
; BC-NEXT: <ENTRY op0=1 op1=1 op2=0 op3=11/>
; BC-NEXT: </PARAMATTR_GROUP_BLOCK_ID>

; DIS: declare void @f(ptr addrspace(1) captures(none), ptr addrspace(1))

;--- m.ll
target triple = "air64-apple-macosx14.0.0"

declare void @f(ptr addrspace(1) captures(none), ptr addrspace(1) captures(address))

;--- m.types
f @f void (i32 addrspace(1)*, i32 addrspace(1)*)
