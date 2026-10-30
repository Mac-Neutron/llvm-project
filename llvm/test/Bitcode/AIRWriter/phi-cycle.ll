; Loop-carried pointer phis: the fixpoint ends, and each phi takes its entry
; operand's typed type. %q's loop operand agrees (a float GEP); %r's does not
; (an i8 GEP), so the GEP's base is cast to i8* and the loop operand back to
; float* at the end of its incoming block, before the terminator.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/m.ll --types %t/m.types --explain --emit-ll %t/m -o %t/m.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-dis %t/m.bc -o %t/m.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t/m.dis.ll
; RUN: tail -n +2 %t/m.dis.ll > %t/m.dis.tail
; RUN: tail -n +2 %t/m.post.ll > %t/m.post.tail
; RUN: diff %t/m.dis.tail %t/m.post.tail

; STATS-DAG: derived %q: float addrspace(1)* (in @k)
; STATS-DAG: derived %r: float addrspace(1)* (in @k)
; STATS: cast %r.next to float addrspace(1)* for phi %r from %loop (in @k)
; STATS: cast %r to i8 addrspace(1)* for getelementptr %r.next (in @k)
; STATS-NOT: {{^}}cast 
; STATS: stats inst-casts=2 const-casts=0 use-typed=0

; DIS: %r = phi ptr addrspace(1) [ %p, %entry ], [ %[[BACK:[0-9]+]], %loop ]
; DIS: %[[BASE:[0-9]+]] = bitcast ptr addrspace(1) %r to ptr addrspace(1)
; DIS-NEXT: %r.next = getelementptr i8, ptr addrspace(1) %[[BASE]], i64 4
; DIS: %[[BACK]] = bitcast ptr addrspace(1) %r.next to ptr addrspace(1)
; DIS-NEXT: br i1 %done, label %exit, label %loop

;--- m.ll
target triple = "air64-apple-macosx14.0.0"

define void @k(ptr addrspace(1) %p, i32 %n) {
entry:
  br label %loop

loop:
  %q = phi ptr addrspace(1) [ %p, %entry ], [ %q.next, %loop ]
  %r = phi ptr addrspace(1) [ %p, %entry ], [ %r.next, %loop ]
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  store float 1.000000e+00, ptr addrspace(1) %q, align 4
  %q.next = getelementptr float, ptr addrspace(1) %q, i64 1
  %r.next = getelementptr i8, ptr addrspace(1) %r, i64 4
  %i.next = add i32 %i, 1
  %done = icmp eq i32 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
;--- m.types
f @k void (float addrspace(1)*, i32)
