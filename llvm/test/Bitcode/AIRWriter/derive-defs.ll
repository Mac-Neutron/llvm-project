; Definition facts that opaque IR keeps type every pointer here from the
; type-level table alone (--named-only: s, g and f lines; the i line below
; would type %p wrongly and is not read): an alloca and a GEP into its type, a
; GEP into a typed struct body and the load of its pointer field, a callee's
; typed return, an extractvalue (follows its aggregate) and an addrspacecast
; (follows its operand). Nothing is derived from uses, cast or use-typed.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/m.ll --types %t/m.types --named-only --explain --emit-ll %t/m -o %t/m.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t/m.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t/m.bc -o %t/m.dis.ll
; RUN: tail -n +2 %t/m.dis.ll > %t/m.dis.tail
; RUN: tail -n +2 %t/m.post.ll > %t/m.post.tail
; RUN: diff %t/m.dis.tail %t/m.post.tail

; STATS-NOT: derived
; STATS-NOT: {{^}}cast 
; STATS-NOT: use-typed
; STATS: stats inst-casts=0 const-casts=0 use-typed=0

; The addrspacecast's result keeps its operand's pointee in address space 4.
; BC: <TYPE_BLOCK_ID
; BC: <POINTER {{.*}}op1=4/>
; BC: </TYPE_BLOCK_ID>
; BC-NOT: OPAQUE_POINTER

;--- m.ll
target triple = "air64-apple-macosx14.0.0"

%struct.S = type { ptr addrspace(1), i32 }

declare { ptr addrspace(1), i32 } @pair(i32)

define void @k(ptr addrspace(2) %s, i32 %i) {
  %a = alloca [4 x float], align 4
  %ap = getelementptr [4 x float], ptr %a, i64 0, i64 1
  store float 1.000000e+00, ptr %ap, align 4
  %fp = getelementptr inbounds %struct.S, ptr addrspace(2) %s, i64 0, i32 0
  %p = load ptr addrspace(1), ptr addrspace(2) %fp, align 8
  %v = load i32, ptr addrspace(1) %p, align 4
  %agg = call { ptr addrspace(1), i32 } @pair(i32 %v)
  %q = extractvalue { ptr addrspace(1), i32 } %agg, 0
  %c = addrspacecast ptr addrspace(1) %q to ptr addrspace(4)
  %f = load float, ptr addrspace(4) %c, align 4
  store float %f, ptr addrspace(1) %q, align 4
  ret void
}
;--- m.types
s %struct.S { i32 addrspace(1)*, i32 }
f @k void (%struct.S addrspace(2)*, i32)
f @pair { float addrspace(1)*, i32 } (i32)
c @k 12
i @k 4 i8 addrspace(1)*
