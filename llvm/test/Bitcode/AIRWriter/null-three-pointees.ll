; One opaque constant at several typed types: ptr addrspace(1) null used as
; i32*, float* and i64* (the corpus's 131, 3 and 1 uses) is written three
; times, each under its own SETTYPE, and the type table holds the three typed
; pointers. Typed nulls are not casts.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/m.ll --types %t/m.types --explain --emit-ll %t/m -o %t/m.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t/m.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t/m.bc -o %t/m.dis.ll
; RUN: tail -n +2 %t/m.dis.ll > %t/m.dis.tail
; RUN: tail -n +2 %t/m.post.ll > %t/m.post.tail
; RUN: diff %t/m.dis.tail %t/m.post.tail

; STATS-NOT: {{^}}cast 
; STATS: stats inst-casts=0 const-casts=0 use-typed=0

; BC: <TYPE_BLOCK_ID
; BC-COUNT-3: <POINTER {{.*}}op1=1/>
; BC-NOT: <POINTER {{.*}}op1=1/>
; BC: </TYPE_BLOCK_ID>
; BC-LABEL: <FUNCTION_BLOCK
; BC: <CONSTANTS_BLOCK
; BC-NEXT: <SETTYPE {{.*}}op0=[[T1:[0-9]+]]/>
; BC-NEXT: <NULL/>
; BC-NEXT: <SETTYPE {{.*}}op0=[[T2:[0-9]+]]/>
; BC-NEXT: <NULL/>
; BC-NEXT: <SETTYPE {{.*}}op0=[[T3:[0-9]+]]/>
; BC-NEXT: <NULL/>
; BC-NOT: <NULL/>
; BC: </CONSTANTS_BLOCK>

;--- m.ll
target triple = "air64-apple-macosx14.0.0"

define void @k(i1 %c, ptr addrspace(1) %a, ptr addrspace(1) %b, ptr addrspace(1) %d) {
  %x = select i1 %c, ptr addrspace(1) %a, ptr addrspace(1) null
  %y = select i1 %c, ptr addrspace(1) null, ptr addrspace(1) %b
  %z = select i1 %c, ptr addrspace(1) %d, ptr addrspace(1) null
  store i32 1, ptr addrspace(1) %x, align 4
  store float 1.000000e+00, ptr addrspace(1) %y, align 4
  store i64 1, ptr addrspace(1) %z, align 8
  ret void
}
;--- m.types
f @k void (i1, i32 addrspace(1)*, float addrspace(1)*, i64 addrspace(1)*)
