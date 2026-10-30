; Fast-math flags in LLVM 15's FastMathMap bits (reassoc 128, nnan 2, ninf 4,
; nsz 8, arcp 16, contract 32, afn 64): reassoc and contract stay apart and
; afn is kept (the DXIL writer folds both into its legacy bit 0 and drops
; afn). They go on binop, unop, fcmp, select, phi and call (CALL_FMF, bit 17,
; with the flags before the function type).
; RUN: llvm-air-write %s -o %t.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t.bc -o - | FileCheck %s --check-prefix=DIS

; STATS: stats inst-casts=0 const-casts=0 use-typed=0 attrs-dropped=0 flags-dropped=0

; BC-LABEL: <FUNCTION_BLOCK
; BC: <INST_BINOP {{.*}}op2=0 op3=224/>
; BC: <INST_UNOP {{.*}}op1=0 op2=2/>
; BC: <INST_CMP2 {{.*}}op2=4 op3=8/>
; BC: <INST_VSELECT {{.*}}op3=16/>
; BC: <INST_CALL op0=0 op1=163840 op2=64 {{.*}}/>
; BC: <INST_PHI {{.*}}op3=4/>
; BC: <INST_BINOP {{.*}}op2=2 op3=254/>

; DIS: %add = fadd reassoc contract afn float %a, %b
; DIS: %neg = fneg nnan float %add
; DIS: %cmp = fcmp nsz olt float %neg, %b
; DIS: %sel = select arcp i1 %cmp, float %neg, float %b
; DIS: %call = call afn float @ext(float %sel)
; DIS: %phi = phi ninf float [ %call, %entry ]
; DIS: %mul = fmul fast float %phi, %a

target triple = "air64-apple-macosx14.0.0"

declare float @ext(float)

define float @k(float %a, float %b) {
entry:
  %add = fadd reassoc contract afn float %a, %b
  %neg = fneg nnan float %add
  %cmp = fcmp nsz olt float %neg, %b
  %sel = select arcp i1 %cmp, float %neg, float %b
  %call = call afn float @ext(float %sel)
  br label %next

next:
  %phi = phi ninf float [ %call, %entry ]
  %mul = fmul fast float %phi, %a
  ret float %mul
}
