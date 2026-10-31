; LLVM 23's sizeless lifetime markers, llvm.lifetime.start/end.p0(ptr %a),
; are written in LLVM 15's form, llvm.lifetime.start/end.p0i8(i64 <size>,
; i8* <a cast of %a>), as the C++ AIR has them (controller ruling D3): the
; size is the alloca's allocation size, what the sizeless marker covers, and
; -1 (LLVM 15's "unknown") for an alloca of no fixed size; the call keeps its
; attributes (InstCombine's nonnull, here). A marker on poison marks nothing
; and is dropped. LLVM 23's reader upgrades LLVM 15's form back
; (AutoUpgrade: the declaration appended, the call on the cast-stripped
; alloca), and the module left behind says what it reads (P9's self row). A
; types file naming LLVM 15's declarations, as the C++ AIR's does, is
; accepted: the writer declares them itself.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/m.ll --types %t/m.types --explain --emit-ll %t/m -o %t/m.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t/m.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-bcanalyzer -dump %t/m.bc | FileCheck %s --check-prefix=NOP0
; RUN: llvm-dis %t/m.bc -o %t/m.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t/m.dis.ll
; RUN: tail -n +2 %t/m.dis.ll > %t/m.dis.tail
; RUN: tail -n +2 %t/m.post.ll > %t/m.post.tail
; RUN: diff %t/m.dis.tail %t/m.post.tail

; STATS: lifetime marker on %a: @llvm.lifetime.start.p0i8 size 16 (in @k)
; STATS-NEXT: lifetime marker on %d: @llvm.lifetime.start.p0i8 size -1 (in @k)
; STATS-NEXT: drop lifetime marker on poison (in @k)
; STATS-NEXT: lifetime marker on %d: @llvm.lifetime.end.p0i8 size -1 (in @k)
; STATS-NEXT: lifetime marker on %a: @llvm.lifetime.end.p0i8 size 16 (in @k)
; STATS: cast %a to i8* for call @llvm.lifetime.start.p0i8 arg 1 (in @k)
; STATS: cast %d to i8* for call @llvm.lifetime.start.p0i8 arg 1 (in @k)
; STATS: cast %d to i8* for call @llvm.lifetime.end.p0i8 arg 1 (in @k)
; STATS: cast %a to i8* for call @llvm.lifetime.end.p0i8 arg 1 (in @k)
; STATS: stats inst-casts=4 const-casts=0 use-typed=0

; BC-LABEL: <PARAMATTR_GROUP_BLOCK_ID
; BC: <ENTRY op0={{[0-9]+}} op1=2 op2=0 op3=39/>
; BC-LABEL: </PARAMATTR_GROUP_BLOCK_ID>
; BC-DAG: record string = 'llvm.lifetime.start.p0i8'
; BC-DAG: record string = 'llvm.lifetime.end.p0i8'
; BC: <CONSTANTS_BLOCK
; BC: <SETTYPE abbrevid=4 op0=1/>
; BC-NEXT: <INTEGER abbrevid=5 op0=32/>
; BC-NEXT: <INTEGER abbrevid=5 op0=3/>
; NOP0-NOT: .p0'

; DIS: %0 = bitcast ptr %a to ptr
; DIS-NEXT: call void @llvm.lifetime.start.p0(ptr %a)
; DIS-NEXT: %1 = bitcast ptr %d to ptr
; DIS-NEXT: call void @llvm.lifetime.start.p0(ptr %d)
; DIS-NEXT: %p = getelementptr
; DIS: %2 = bitcast ptr %d to ptr
; DIS-NEXT: call void @llvm.lifetime.end.p0(ptr %d)
; DIS-NEXT: %3 = bitcast ptr %a to ptr
; DIS-NEXT: call void @llvm.lifetime.end.p0(ptr %a)
; DIS: declare void @llvm.lifetime.start.p0(ptr captures(none))
; DIS: declare void @llvm.lifetime.end.p0(ptr captures(none))
; DIS-NOT: p0i8

;--- m.ll
target triple = "air64-apple-macosx14.0.0"

declare void @llvm.lifetime.start.p0(ptr captures(none))
declare void @llvm.lifetime.end.p0(ptr captures(none))

define i32 @k(i32 %i, i32 %n) {
entry:
  %a = alloca [4 x i32], align 4
  %d = alloca i32, i32 %n, align 4
  call void @llvm.lifetime.start.p0(ptr nonnull %a)
  call void @llvm.lifetime.start.p0(ptr %d)
  call void @llvm.lifetime.start.p0(ptr poison)
  %p = getelementptr inbounds [4 x i32], ptr %a, i32 0, i32 %i
  store i32 %i, ptr %p, align 4
  store i32 %n, ptr %d, align 4
  %v = load i32, ptr %p, align 4
  call void @llvm.lifetime.end.p0(ptr %d)
  call void @llvm.lifetime.end.p0(ptr %a)
  ret i32 %v
}
;--- m.types
f @llvm.lifetime.start.p0i8 void (i64, i8*)
f @llvm.lifetime.end.p0i8 void (i64, i8*)
