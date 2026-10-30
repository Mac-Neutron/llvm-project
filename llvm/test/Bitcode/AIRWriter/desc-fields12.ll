; A texture descriptor's fields 1 and 2 (the i64s) beside its handle, as
; lod.ps.dxil reads them (cut down, made opaque): GEPs into the same literal
; descriptor type over one base. The handle the air.* call takes types the
; base; the i64 fields need nothing more. After LLVM 23's instcombine the
; fields are i8 GEPs (ptradd) and their loads are cast; the handle never is.
; RUN: split-file %s %t
; RUN: llvm-air-write %t/m.ll --types %t/m.types --named-only --explain --emit-ll %t/m -o %t/m.bc > %t/m.log 2>&1
; RUN: FileCheck %s --check-prefix=STATS < %t/m.log
; RUN: FileCheck %s --check-prefix=NOAIR < %t/m.log
; RUN: llvm-bcanalyzer -dump %t/m.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t/m.bc -o %t/m.dis.ll
; RUN: tail -n +2 %t/m.dis.ll > %t/m.dis.tail
; RUN: tail -n +2 %t/m.post.ll > %t/m.post.tail
; RUN: diff %t/m.dis.tail %t/m.post.tail
; RUN: llvm-air-write %t/m.ll --types %t/m.types --named-only --passes instcombine --explain --emit-ll %t/i -o %t/i.bc > %t/i.log 2>&1
; RUN: FileCheck %s --check-prefix=INST < %t/i.log
; RUN: FileCheck %s --check-prefix=NOAIR < %t/i.log
; RUN: llvm-bcanalyzer -dump %t/i.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t/i.bc -o %t/i.dis.ll
; RUN: tail -n +2 %t/i.dis.ll > %t/i.dis.tail
; RUN: tail -n +2 %t/i.post.ll > %t/i.post.tail
; RUN: diff %t/i.dis.tail %t/i.post.tail

; STATS-DAG: derived %tex: %struct._texture_2d_t addrspace(1)* (in @main)
; STATS-DAG: derived source of %tfield: { %struct._texture_2d_t addrspace(1)*, i64, i64, i64 } (in @main)
; STATS-DAG: derived %tdesc: { %struct._texture_2d_t addrspace(1)*, i64, i64, i64 } addrspace(2)* (in @main)
; STATS-NOT: {{^}}cast 
; STATS: stats inst-casts=0 const-casts=0 use-typed=0

; INST-DAG: derived %tex: %struct._texture_2d_t addrspace(1)* (in @main)
; INST-DAG: cast %f1 to i64 addrspace(2)* for load %bias (in @main)
; INST-DAG: cast %f2 to i64 addrspace(2)* for load %minlod (in @main)
; INST: stats {{.*}}use-typed=0

; NOAIR-NOT: for call @air.
; NOAIR: stats

; BC: <TYPE_BLOCK_ID
; BC-NOT: OPAQUE_POINTER
; BC: </TYPE_BLOCK_ID>
; BC-NOT: OPAQUE_POINTER

;--- m.ll
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx14.0.0"

%argument_buffer_struct = type { ptr addrspace(2), ptr addrspace(2) }
%struct._texture_2d_t = type opaque
%struct._sampler_t = type opaque

define <4 x float> @main(<2 x float> %uv, ptr addrspace(2) %ab, ptr addrspace(2) %smp) {
entry:
  %tview = bitcast ptr addrspace(2) %ab to ptr addrspace(2)
  %tdesc = load ptr addrspace(2), ptr addrspace(2) %tview, align 8
  %tfield = getelementptr { ptr addrspace(1), i64, i64, i64 }, ptr addrspace(2) %tdesc, i64 0, i32 0
  %tex = load ptr addrspace(1), ptr addrspace(2) %tfield, align 8
  %f1 = getelementptr { ptr addrspace(1), i64, i64, i64 }, ptr addrspace(2) %tdesc, i64 0, i32 1
  %bias = load i64, ptr addrspace(2) %f1, align 8
  %f2 = getelementptr { ptr addrspace(1), i64, i64, i64 }, ptr addrspace(2) %tdesc, i64 0, i32 2
  %minlod = load i64, ptr addrspace(2) %f2, align 8
  %b32 = trunc i64 %bias to i32
  %bf = bitcast i32 %b32 to float
  %m32 = trunc i64 %minlod to i32
  %mf = bitcast i32 %m32 to float
  %r = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %tex, ptr addrspace(2) %smp, <2 x float> %uv, i1 true, <2 x i32> zeroinitializer, i1 false, float %bf, float %mf, i32 0)
  %v = extractvalue { <4 x float>, i8 } %r, 0
  ret <4 x float> %v
}

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)
;--- m.types
s %argument_buffer_struct { i32 addrspace(2)*, i32 addrspace(2)* }
f @main <4 x float> (<2 x float>, %argument_buffer_struct addrspace(2)*, %struct._sampler_t addrspace(2)*)
f @air.sample_texture_2d.v4f32 { <4 x float>, i8 } (%struct._texture_2d_t addrspace(1)*, %struct._sampler_t addrspace(2)*, <2 x float>, i1, <2 x i32>, i1, float, float, i32)
