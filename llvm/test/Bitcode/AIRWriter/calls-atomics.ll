; Calls and atomics as LLVM 15 writes them: the tail (bit 0), musttail (14),
; explicit-type (15) and notail (16) markers; ordering and syncscope, with the
; sync scope names block, so syncscope("agent") reads back; cmpxchg and
; atomicrmw with their align; atomicrmw as code 59 with its value's type. That
; type matters when the value is a forward reference: %v is defined in a later
; block, and without its type the record cannot be read.
; RUN: llvm-air-write %s --types %S/Inputs/calls-atomics.types -o %t.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t.bc -o - | FileCheck %s --check-prefix=DIS

; STATS: stats inst-casts=0 const-casts=0 use-typed=0 attrs-dropped=0 flags-dropped=0

; BC-LABEL: <FUNCTION_BLOCK
; BC: <INST_CALL op0=0 op1=32769 {{.*}}/>
; BC: <INST_CALL op0=0 op1=98304 {{.*}}/>
; cmpxchg: volatile 0, acq_rel (5), system (1), monotonic (2), weak, align 8
; BC: <INST_CMPXCHG {{.*}}op3=0 op4=5 op5=1 op6=2 op7=1 op8=4/>
; atomicrmw: ptr, value (a forward reference), its type, add (1), volatile 0,
; monotonic (2), singlethread (0), align 16
; BC: <INST_ATOMICRMW op0={{[0-9]+}} op1={{[0-9]+}} op2={{[0-9]+}} op3=1 op4=0 op5=2 op6=0 op7=5/>
; BC-LABEL: <FUNCTION_BLOCK
; BC: <INST_CALL op0=0 op1=49153 {{.*}}/>

; DIS: %t = tail call float @ext(float %x)
; DIS: %nt = notail call float @ext(float %t)
; DIS: %l = load atomic i32, ptr addrspace(1) %p syncscope("agent") acquire, align 4
; DIS: store atomic i32 %l, ptr addrspace(1) %p release, align 4
; DIS: %cx = cmpxchg weak ptr addrspace(1) %p, i32 0, i32 %l acq_rel monotonic, align 8
; DIS: %old = atomicrmw add ptr addrspace(1) %p, i32 %v syncscope("singlethread") monotonic, align 16
; DIS: fence syncscope("agent") seq_cst
; DIS: %r = musttail call float @ext(float %x)

target triple = "air64-apple-macosx14.0.0"

declare float @ext(float)

define float @k(ptr addrspace(1) %p, float %x) {
entry:
  %t = tail call float @ext(float %x)
  %nt = notail call float @ext(float %t)
  %l = load atomic i32, ptr addrspace(1) %p syncscope("agent") acquire, align 4
  store atomic i32 %l, ptr addrspace(1) %p release, align 4
  %cx = cmpxchg weak ptr addrspace(1) %p, i32 0, i32 %l acq_rel monotonic, align 8
  br label %fwd

use:
  %old = atomicrmw add ptr addrspace(1) %p, i32 %v syncscope("singlethread") monotonic, align 16
  fence syncscope("agent") seq_cst
  ret float %nt

fwd:
  %v = add i32 %l, 1
  br label %use
}

define float @mt(float %x) {
  %r = musttail call float @ext(float %x)
  ret float %r
}
