; nuw, nsw, exact and inbounds are written as LLVM 15 writes them. LLVM
; 23-only flags are dropped from the module and counted, one per token as
; llvm-dis prints it: zext/uitofp nneg, trunc nuw/nsw, or disjoint, icmp
; samesign, getelementptr nusw (without inbounds) and nuw, and fast-math
; flags where LLVM 15 has none (casts, a call returning a struct of floats).
; RUN: split-file %s %t
; RUN: llvm-air-write %t/m.ll --types %t/m.types --explain --emit-ll %t/m -o %t/m.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t/m.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t/m.bc -o %t/m.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t/m.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t/m.post.ll
; RUN: tail -n +2 %t/m.dis.ll > %t/m.dis.tail
; RUN: tail -n +2 %t/m.post.ll > %t/m.post.tail
; RUN: diff %t/m.dis.tail %t/m.post.tail

; STATS: drop nneg on %z (in @k)
; STATS: drop nneg on %f (in @k)
; STATS: drop nuw nsw on %t (in @k)
; STATS: drop disjoint on %o (in @k)
; STATS: drop samesign on %c (in @k)
; STATS: drop nnan on %e (in @k)
; STATS: drop nnan on %s (in @k)
; STATS: drop nusw nuw on %q (in @g)
; STATS: drop nuw on %r (in @g)
; STATS: stats inst-casts=0 const-casts=0 use-typed=0 attrs-dropped=0 flags-dropped=11

; add nuw nsw: OBO flags 3; udiv exact: 1; getelementptr inbounds: 1
; BC-LABEL: <FUNCTION_BLOCK
; BC: <INST_BINOP {{.*}}op2=0 op3=3/>
; BC: <INST_BINOP {{.*}}op2=3 op3=1/>
; BC: <INST_GEP {{.*}}op0=0 op1=
; BC: <INST_GEP {{.*}}op0=1 op1=

; DIS: %z = zext i16 %h to i32
; DIS: %f = uitofp i32 %a to float
; DIS: %t = trunc i64 %b to i32
; DIS: %o = or i32 %z, %t
; DIS: %c = icmp ult i32 %o, %a
; DIS: %e = fpext float %f to double
; DIS: %s = call { float, float } @two()
; DIS: %add = add nuw nsw i32 %o, %a
; DIS: %d = udiv exact i32 %add, 4
; DIS: %q = getelementptr i32, ptr addrspace(1) %p, i64 1
; DIS: %r = getelementptr inbounds i32, ptr addrspace(1) %q, i64 1

;--- m.ll
target triple = "air64-apple-macosx14.0.0"

declare { float, float } @two()

define i32 @k(i32 %a, i64 %b, i16 %h) {
  %z = zext nneg i16 %h to i32
  %f = uitofp nneg i32 %a to float
  %t = trunc nuw nsw i64 %b to i32
  %o = or disjoint i32 %z, %t
  %c = icmp samesign ult i32 %o, %a
  %e = fpext nnan float %f to double
  %s = call nnan { float, float } @two()
  %add = add nuw nsw i32 %o, %a
  %d = udiv exact i32 %add, 4
  %r = select i1 %c, i32 %d, i32 0
  ret i32 %r
}

define void @g(ptr addrspace(1) %p) {
  %q = getelementptr nusw nuw i32, ptr addrspace(1) %p, i64 1
  %r = getelementptr inbounds nuw i32, ptr addrspace(1) %q, i64 1
  store i32 0, ptr addrspace(1) %r, align 4
  ret void
}

;--- m.types
f @g void (i32 addrspace(1)*)
c @g 4
i @g 0 i32 addrspace(1)*
i @g 1 i32 addrspace(1)*
