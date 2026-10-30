; What LLVM 15's records cannot hold is an error naming the construct, never
; an abort, the same with assertions off (P17): a scalable vector, a type
; outside the type table's switch (bfloat), callbr and the other opcodes the
; writer has no case for, ptrtoaddr, an atomicrmw operation LLVM 15 lacks,
; operand bundles, debug info, and a typed type the enumerator never saw
; (made after enumeration, through --self-test-late-type).
; RUN: split-file %s %t
; RUN: not llvm-air-write %t/scalable.ll -o %t/o.bc 2>&1 | FileCheck %s --check-prefix=SCALABLE
; RUN: not llvm-air-write %t/bfloat.ll -o %t/o.bc 2>&1 | FileCheck %s --check-prefix=BFLOAT
; RUN: not llvm-air-write %t/callbr.ll -o %t/o.bc 2>&1 | FileCheck %s --check-prefix=CALLBR
; RUN: not llvm-air-write %t/ptrtoaddr.ll --types %t/ptrtoaddr.types -o %t/o.bc 2>&1 | FileCheck %s --check-prefix=PTRTOADDR
; RUN: not llvm-air-write %t/rmw.ll --types %t/rmw.types -o %t/o.bc 2>&1 | FileCheck %s --check-prefix=RMW
; RUN: not llvm-air-write %t/bundle.ll -o %t/o.bc 2>&1 | FileCheck %s --check-prefix=BUNDLE
; RUN: not llvm-air-write %t/debuginfo.ll -o %t/o.bc 2>&1 | FileCheck %s --check-prefix=DEBUGINFO
; RUN: not llvm-air-write --self-test-late-type 2>&1 | FileCheck %s --check-prefix=LATE

; SCALABLE: error: type <vscale x 4 x i32>: a scalable vector has no LLVM 15 AIR encoding
; BFLOAT: error: type bfloat: no LLVM 15 AIR encoding
; CALLBR: error: callbr: no LLVM 15 encoding (in @k)
; PTRTOADDR: error: ptrtoaddr %a: no LLVM 15 encoding (in @k)
; RMW: error: atomicrmw uinc_wrap %o: no LLVM 15 encoding (in @k)
; BUNDLE: error: call: operand bundles are not written (in @k)
; DEBUGINFO: error: debug info: a location in @k: AIR carries none
; LATE: error: type table: typedptr(i8, 7) was not enumerated
; SCALABLE-NOT: stats
; BFLOAT-NOT: stats
; CALLBR-NOT: stats
; PTRTOADDR-NOT: stats
; RMW-NOT: stats
; BUNDLE-NOT: stats
; DEBUGINFO-NOT: stats

;--- scalable.ll
target triple = "air64-apple-macosx14.0.0"

define void @k(<vscale x 4 x i32> %v) {
  ret void
}

;--- bfloat.ll
target triple = "air64-apple-macosx14.0.0"

define bfloat @k(bfloat %x) {
  ret bfloat %x
}

;--- callbr.ll
target triple = "air64-apple-macosx14.0.0"

define void @k() {
  callbr void asm "", "!i"() to label %a [label %b]
a:
  ret void
b:
  ret void
}

;--- ptrtoaddr.ll
target triple = "air64-apple-macosx14.0.0"

define i64 @k(ptr addrspace(1) %p) {
  %a = ptrtoaddr ptr addrspace(1) %p to i64
  ret i64 %a
}

;--- ptrtoaddr.types
f @k i64 (i32 addrspace(1)*)

;--- rmw.ll
target triple = "air64-apple-macosx14.0.0"

define i32 @k(ptr addrspace(1) %p) {
  %o = atomicrmw uinc_wrap ptr addrspace(1) %p, i32 1 monotonic, align 4
  ret i32 %o
}

;--- rmw.types
f @k i32 (i32 addrspace(1)*)

;--- bundle.ll
target triple = "air64-apple-macosx14.0.0"

declare void @f()

define void @k() {
  call void @f() [ "deopt"() ]
  ret void
}

;--- debuginfo.ll
target triple = "air64-apple-macosx14.0.0"

define void @k() !dbg !4 {
  ret void, !dbg !6
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!3}
!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "t", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "t.c", directory: "/")
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = distinct !DISubprogram(name: "k", scope: !1, file: !1, line: 1, type: !5, unit: !0)
!5 = !DISubroutineType(types: !{})
!6 = !DILocation(line: 1, scope: !4)
