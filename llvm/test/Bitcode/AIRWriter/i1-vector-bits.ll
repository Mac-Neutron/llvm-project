; Ruling D5: no i1-vector pattern Metal's compiler crashes on is written.
; LLVM 23's InstCombine turns LLVM 15's bit test, icmp ne (and (extractelement
; <4 x i32> %v, 1), 1), 0, into extractelement (bitcast %v to <128 x i1>), 32:
; first icmp ne (and X, 1), 0 -> trunc X to i1 (foldICmpAndConstConst; LLVM
; 15 does that for vectors only and canonicalizes a scalar trunc to i1 back to
; icmp), then trunc (extractelement) -> extractelement (bitcast)
; (foldVecExtTruncToExtElt, not in LLVM 15). A load whose one user is that
; bitcast becomes a load of <128 x i1> (combineLoadToOperationType). Metal's
; compiler crashed on 5 SMITE 2 opt23 shaders holding them (Task 7 D5,
; XPC_ERROR_CONNECTION_INTERRUPTED); written in LLVM 15's form they build.
; So each constant-index extractelement of such a bitcast or load is written
; as LLVM 15 wrote the bit test: the word's extractelement, and with the bit,
; icmp ne 0; the load loads <K/32 x i32> instead. Any other use, or any other
; bitcast to an i1 vector or load of one, is an error.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/passes.ll --passes 'function(instcombine<no-verify-fixpoint>)' --emit-ll %t/passes -o %t/passes.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: FileCheck %s --check-prefix=PRE < %t/passes.pre.ll
; RUN: llvm-dis %t/passes.bc -o - | FileCheck %s --check-prefix=PASSES
; RUN: FileCheck %s --check-prefix=PASSES < %t/passes.post.ll
; RUN: llvm-air-write %t/direct.ll --explain --emit-ll %t/direct -o %t/direct.bc 2>&1 | FileCheck %s --check-prefix=EXPLAIN
; RUN: llvm-dis %t/direct.bc -o - | FileCheck %s --check-prefix=DIRECT
; RUN: FileCheck %s --check-prefix=DIRECT < %t/direct.post.ll
; RUN: llvm-bcanalyzer -dump %t/direct.bc | FileCheck %s --check-prefix=BC
; RUN: not llvm-air-write %t/use.ll -o %t/use.bc 2>&1 | FileCheck %s --check-prefix=USE
; RUN: not llvm-air-write %t/scalar.ll -o %t/scalar.bc 2>&1 | FileCheck %s --check-prefix=SCALAR
; RUN: not llvm-air-write %t/odd.ll -o %t/odd.bc 2>&1 | FileCheck %s --check-prefix=ODD

; STATS: stats inst-casts=0 const-casts=0 use-typed=0

; InstCombine still makes the pattern (else this test checks nothing).
; PRE-LABEL: define i1 @word(
; PRE: bitcast <4 x i32> %v to <128 x i1>
; PRE: extractelement <128 x i1> {{.*}}, i64 32
; PRE-LABEL: define i1 @load(
; PRE: load <128 x i1>, ptr addrspace(2) %p, align 16
; PRE: extractelement <128 x i1> {{.*}}, i64 64

; PASSES-NOT: x i1>
; PASSES-LABEL: define i1 @word(
; PASSES-NEXT: [[W:%.*]] = extractelement <4 x i32> %v, i64 1
; PASSES-NEXT: [[A:%.*]] = and i32 [[W]], 1
; PASSES-NEXT: [[C:%.*]] = icmp ne i32 [[A]], 0
; PASSES-NEXT: ret i1 [[C]]
; PASSES-LABEL: define i1 @load(
; PASSES-NEXT: [[L:%.*]] = load <4 x i32>, ptr addrspace(2) %p, align 16
; PASSES-NEXT: [[W2:%.*]] = extractelement <4 x i32> [[L]], i64 2
; PASSES-NEXT: [[A2:%.*]] = and i32 [[W2]], 1
; PASSES-NEXT: [[C2:%.*]] = icmp ne i32 [[A2]], 0
; PASSES-NEXT: ret i1 [[C2]]
; PASSES-NOT: x i1>

; EXPLAIN: bit test %c: extractelement <128 x i1> -> extractelement, and, icmp ne (in @k)
; EXPLAIN: bit test %d: extractelement <128 x i1> -> extractelement, and, icmp ne (in @k)
; EXPLAIN: bit test %f: extractelement <64 x i1> -> extractelement, and, icmp ne (in @k)

; DIRECT-NOT: x i1>
; DIRECT-LABEL: define i1 @k(
; DIRECT-NEXT: %l = load <4 x i32>, ptr addrspace(2) %p, align 16
; DIRECT-NEXT: [[W1:%.*]] = extractelement <4 x i32> %l, i64 1
; DIRECT-NEXT: [[A1:%.*]] = and i32 [[W1]], 8
; DIRECT-NEXT: %c = icmp ne i32 [[A1]], 0
; DIRECT-NEXT: [[W2:%.*]] = extractelement <4 x i32> %l, i32 3
; DIRECT-NEXT: [[A2:%.*]] = and i32 [[W2]], -2147483648
; DIRECT-NEXT: %d = icmp ne i32 [[A2]], 0
; DIRECT-NEXT: [[W3:%.*]] = extractelement <2 x i32> %v, i64 1
; DIRECT-NEXT: [[A3:%.*]] = and i32 [[W3]], 64
; DIRECT-NEXT: %f = icmp ne i32 [[A3]], 0
; DIRECT-NEXT: %cd = and i1 %c, %d
; DIRECT-NOT: x i1>

; The load is typed from its new type.
; BC: <TYPE_BLOCK_ID
; BC-NOT: <VECTOR op0=128 op1={{[0-9]+}}/>
; BC: </TYPE_BLOCK_ID>

; USE: error: bitcast %b: <128 x i1> is written only as constant-index bit tests (Metal's compiler crashes on i1-vector bitcasts and loads) (in @k)
; USE-NOT: stats
; SCALAR: error: bitcast %b: an i1 vector from other than an integer vector is not written (Metal's compiler crashes on i1-vector bitcasts and loads) (in @k)
; SCALAR-NOT: stats
; ODD: error: load %l: <12 x i1> is not a whole number of 32-bit words (Metal's compiler crashes on i1-vector bitcasts and loads) (in @k)
; ODD-NOT: stats

;--- passes.ll
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx14.0.0"

define i1 @word(<4 x i32> %v) {
  %e = extractelement <4 x i32> %v, i64 1
  %a = and i32 %e, 1
  %c = icmp ne i32 %a, 0
  ret i1 %c
}

define i1 @load(ptr addrspace(2) %p) {
  %v = load <4 x i32>, ptr addrspace(2) %p, align 16
  %e = extractelement <4 x i32> %v, i64 2
  %a = and i32 %e, 1
  %c = icmp ne i32 %a, 0
  ret i1 %c
}
;--- direct.ll
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx14.0.0"

define i1 @k(ptr addrspace(2) %p, <2 x i32> %v) {
  %l = load <128 x i1>, ptr addrspace(2) %p, align 16
  %c = extractelement <128 x i1> %l, i64 35
  %d = extractelement <128 x i1> %l, i32 127
  %b = bitcast <2 x i32> %v to <64 x i1>
  %f = extractelement <64 x i1> %b, i64 38
  %cd = and i1 %c, %d
  %r = and i1 %cd, %f
  ret i1 %r
}
;--- use.ll
target triple = "air64-apple-macosx14.0.0"

define i1 @k(<4 x i32> %v, i32 %i) {
  %b = bitcast <4 x i32> %v to <128 x i1>
  %c = extractelement <128 x i1> %b, i32 %i
  ret i1 %c
}
;--- scalar.ll
target triple = "air64-apple-macosx14.0.0"

define i1 @k(i32 %v) {
  %b = bitcast i32 %v to <32 x i1>
  %c = extractelement <32 x i1> %b, i64 3
  ret i1 %c
}
;--- odd.ll
target triple = "air64-apple-macosx14.0.0"

define i1 @k(ptr addrspace(2) %p) {
  %l = load <12 x i1>, ptr addrspace(2) %p, align 2
  %c = extractelement <12 x i1> %l, i64 3
  ret i1 %c
}
