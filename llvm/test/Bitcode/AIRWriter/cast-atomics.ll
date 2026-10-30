; cmpxchg and atomicrmw need a pointer to their value type: an i8 GEP (LLVM
; 23's ptradd form) feeding cmpxchg i32 and atomicrmw add i32 is cast once for
; each.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/m.ll --types %t/m.types --explain --emit-ll %t/m -o %t/m.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t/m.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t/m.bc -o %t/m.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t/m.dis.ll
; RUN: tail -n +2 %t/m.dis.ll > %t/m.dis.tail
; RUN: tail -n +2 %t/m.post.ll > %t/m.post.tail
; RUN: diff %t/m.dis.tail %t/m.post.tail

; STATS: cast %g to i32 addrspace(1)* for cmpxchg %x (in @k)
; STATS: cast %g to i32 addrspace(1)* for atomicrmw %y (in @k)
; STATS-NOT: {{^}}cast 
; STATS: stats inst-casts=2 const-casts=0 use-typed=0

; BC-LABEL: <FUNCTION_BLOCK
; BC: <INST_CAST {{.*}}op2=11/>
; BC: <INST_CMPXCHG
; BC: <INST_CAST {{.*}}op2=11/>
; BC: <INST_ATOMICRMW
; BC-NOT: <INST_CAST

; DIS: %[[A:[0-9]+]] = bitcast ptr addrspace(1) %g to ptr addrspace(1)
; DIS-NEXT: %x = cmpxchg ptr addrspace(1) %[[A]], i32 0, i32 1 monotonic monotonic, align 4
; DIS-NEXT: %[[B:[0-9]+]] = bitcast ptr addrspace(1) %g to ptr addrspace(1)
; DIS-NEXT: %y = atomicrmw add ptr addrspace(1) %[[B]], i32 1 monotonic, align 4

;--- m.ll
target triple = "air64-apple-macosx14.0.0"

define void @k(ptr addrspace(1) %p, i64 %o) {
  %g = getelementptr inbounds i8, ptr addrspace(1) %p, i64 %o
  %x = cmpxchg ptr addrspace(1) %g, i32 0, i32 1 monotonic monotonic, align 4
  %y = atomicrmw add ptr addrspace(1) %g, i32 1 monotonic, align 4
  ret void
}
;--- m.types
f @k void (i8 addrspace(1)*, i64)
