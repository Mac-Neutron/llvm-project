; LLVM 15's attribute kind table (codes up to 84): enum, integer and string
; attributes keep their LLVM 15 codes, speculatable (53) and nocallback (71)
; included. A type attribute is an error.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/kinds.ll -o %t/kinds.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t/kinds.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t/kinds.bc -o - | FileCheck %s --check-prefix=DIS
; RUN: not llvm-air-write %t/byval.ll --types %t/byval.types -o %t/byval.bc 2>&1 | FileCheck %s --check-prefix=TYPE

; STATS: stats inst-casts=0 const-casts=0 use-typed=0 attrs-dropped=0 flags-dropped=0

; BC-LABEL: <PARAMATTR_GROUP_BLOCK_ID
; convergent mustprogress nocallback nofree nosync nounwind speculatable willreturn
; BC-NEXT: <ENTRY op0=1 op1=4294967295 op2=0 op3=43 op4=0 op5=70 op6=0 op7=71 op8=0 op9=62 op10=0 op11=63 op12=0 op13=18 op14=0 op15=53 op16=0 op17=61/>
; noundef signext on the first parameter, zeroext on the second
; BC-NEXT: <ENTRY op0=2 op1=1 op2=0 op3=68 op4=0 op5=24/>
; BC-NEXT: <ENTRY op0=3 op1=2 op2=0 op3=34/>
; noinline, then alignstack(16) as an integer attribute, then a string one
; BC-NEXT: <ENTRY op0=4 op1=4294967295 op2=0 op3=14 op4=1 op5=25 op6=16 op7=3 op8=110 op9=111 op10=45 op11=98 op12=117 op13=105 op14=108 op15=116 op16=105 op17=110 op18=115 op19=0/>
; BC-NEXT: </PARAMATTR_GROUP_BLOCK_ID>

; DIS: declare i32 @f(i32 noundef signext, i32 zeroext) [[F:#[0-9]+]]
; DIS: define i32 @k(i32 %x) [[K:#[0-9]+]]
; DIS: attributes [[F]] = { convergent mustprogress nocallback nofree nosync nounwind speculatable willreturn }
; DIS: attributes [[K]] = { noinline alignstack=16 "no-builtins" }

; TYPE: error: attribute byval(i32): a type attribute is not written (in @k)
; TYPE-NOT: stats

;--- kinds.ll
target triple = "air64-apple-macosx14.0.0"

declare i32 @f(i32 noundef signext, i32 zeroext) #0

define i32 @k(i32 %x) #1 {
  %r = call i32 @f(i32 noundef signext %x, i32 zeroext %x)
  ret i32 %r
}

attributes #0 = { convergent mustprogress nocallback nofree nosync nounwind speculatable willreturn }
attributes #1 = { noinline alignstack=16 "no-builtins" }

;--- byval.ll
target triple = "air64-apple-macosx14.0.0"

define void @k(ptr byval(i32) %p) {
  ret void
}

;--- byval.types
f @k void (i32*)
