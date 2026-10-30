; unnamed_addr is LLVM 15's 0/1/2 (2 is local_unnamed_addr) for globals,
; functions and aliases, where the DXIL writer writes a bool. In the 3.7
; layout it is a global's op8, a function's op9 and an alias's op6 (ALIAS_OLD,
; code 9, which bcanalyzer does not name).
; RUN: llvm-air-write %s -o %t.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t.bc -o - | FileCheck %s --check-prefix=DIS

; STATS: stats inst-casts=0 const-casts=0 use-typed=0 attrs-dropped=0 flags-dropped=0

; BC: <GLOBALVAR {{.*}}op8=2 op9=0
; BC: <GLOBALVAR {{.*}}op8=1 op9=0
; BC: <FUNCTION op0={{[0-9]+}} op1=0 op2=0 op3=0 op4=0 op5=0 op6=0 op7=0 op8=0 op9=2
; BC: <FUNCTION op0={{[0-9]+}} op1=0 op2=0 op3=0 op4=0 op5=0 op6=0 op7=0 op8=0 op9=1
; BC: <FUNCTION op0={{[0-9]+}} op1=0 op2=1 op3=0 op4=0 op5=0 op6=0 op7=0 op8=0 op9=0
; BC: <UnknownCode9 {{.*}}op6=2/>
; BC: <UnknownCode9 {{.*}}op6=1/>

; DIS: @g = internal local_unnamed_addr global i32 0
; DIS: @h = internal unnamed_addr constant i32 1
; DIS: @n = internal global i32 2
; DIS: @a = internal local_unnamed_addr alias i32, ptr @g
; DIS: @b = internal unnamed_addr alias i32, ptr @h
; DIS: define void @f() local_unnamed_addr {
; DIS: define void @u() unnamed_addr {
; DIS: declare void @d(){{$}}

target triple = "air64-apple-macosx14.0.0"

@g = internal local_unnamed_addr global i32 0
@h = internal unnamed_addr constant i32 1
@n = internal global i32 2
@a = internal local_unnamed_addr alias i32, ptr @g
@b = internal unnamed_addr alias i32, ptr @h

define void @f() local_unnamed_addr {
  ret void
}

define void @u() unnamed_addr {
  ret void
}

declare void @d()
