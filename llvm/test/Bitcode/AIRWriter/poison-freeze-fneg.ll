; poison is CST_CODE_POISON (26), not undef; freeze is FUNC_CODE_INST_FREEZE
; (58); fneg is FUNC_CODE_INST_UNOP (56), not a binop. llvm-bcanalyzer has no
; names for codes 26 and 58.
; RUN: llvm-air-write %s -o %t.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t.bc -o - | FileCheck %s --check-prefix=DIS

; STATS: stats inst-casts=0 const-casts=0 use-typed=0 attrs-dropped=0 flags-dropped=0

; BC-LABEL: <FUNCTION_BLOCK
; BC: <CONSTANTS_BLOCK
; BC: <UnknownCode26/>
; BC: </CONSTANTS_BLOCK>
; BC: <UnknownCode58 op0=
; BC: <INST_UNOP {{.*}}op1=0/>

; DIS: %f = freeze i32 %x
; DIS: %n = fneg float %y
; DIS: %p = add i32 %s, poison
; DIS: %u = add i32 %p, undef

target triple = "air64-apple-macosx14.0.0"

define i32 @k(i32 %x, float %y) {
  %f = freeze i32 %x
  %n = fneg float %y
  %i = fptosi float %n to i32
  %s = add i32 %f, %i
  %p = add i32 %s, poison
  %u = add i32 %p, undef
  ret i32 %u
}
