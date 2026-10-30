; The sampler pair, as null.cs.dxil reads it (cut down, made opaque): a
; literal { sampler, sampler, i64, i64 } descriptor whose field 1 is the
; sampler the air.* call takes and whose field 2 is an i64. Field 0, which
; nothing reads, is i8 addrspace(2)*. After LLVM 23's instcombine the fields
; are i8 GEPs; the sampler and the texture are still never cast.
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

; STATS-DAG: derived %smp: %struct._sampler_t addrspace(2)* (in @main)
; STATS-DAG: derived source of %s1: { i8 addrspace(2)*, %struct._sampler_t addrspace(2)*, i64, i64 } (in @main)
; STATS-DAG: derived %sdesc: { i8 addrspace(2)*, %struct._sampler_t addrspace(2)*, i64, i64 } addrspace(2)* (in @main)
; STATS-DAG: derived %tex: %struct._texture_cube_t addrspace(1)* (in @main)
; STATS-NOT: {{^}}cast 
; STATS: stats inst-casts=0 const-casts=0 use-typed=0

; INST-DAG: derived %smp: %struct._sampler_t addrspace(2)* (in @main)
; INST-DAG: derived %tex: %struct._texture_cube_t addrspace(1)* (in @main)
; INST-DAG: derived %tdesc: %struct._texture_cube_t addrspace(1)* addrspace(2)* (in @main)
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

%argument_buffer_struct = type { ptr addrspace(2), ptr addrspace(2), ptr addrspace(2) }
%struct._texture_cube_t = type opaque
%struct._sampler_t = type opaque

define <4 x float> @main(<3 x float> %dir, ptr addrspace(2) %ab) {
entry:
  %tslot = getelementptr inbounds %argument_buffer_struct, ptr addrspace(2) %ab, i64 0, i32 1
  %tview = bitcast ptr addrspace(2) %tslot to ptr addrspace(2)
  %tdesc = load ptr addrspace(2), ptr addrspace(2) %tview, align 8
  %tfield = getelementptr { ptr addrspace(1), i64, i64, i64 }, ptr addrspace(2) %tdesc, i64 0, i32 0
  %tex = load ptr addrspace(1), ptr addrspace(2) %tfield, align 8
  %sslot = getelementptr inbounds %argument_buffer_struct, ptr addrspace(2) %ab, i64 0, i32 2
  %sview = bitcast ptr addrspace(2) %sslot to ptr addrspace(2)
  %sdesc = load ptr addrspace(2), ptr addrspace(2) %sview, align 8
  %s1 = getelementptr { ptr addrspace(2), ptr addrspace(2), i64, i64 }, ptr addrspace(2) %sdesc, i64 0, i32 1
  %smp = load ptr addrspace(2), ptr addrspace(2) %s1, align 8
  %s2 = getelementptr { ptr addrspace(2), ptr addrspace(2), i64, i64 }, ptr addrspace(2) %sdesc, i64 0, i32 2
  %bias = load i64, ptr addrspace(2) %s2, align 8
  %b32 = trunc i64 %bias to i32
  %bf = bitcast i32 %b32 to float
  %lod = fadd float %bf, 0.000000e+00
  %r = call { <4 x float>, i8 } @air.sample_texture_cube.v4f32(ptr addrspace(1) %tex, ptr addrspace(2) %smp, <3 x float> %dir, i1 true, float %lod, float 0.000000e+00, i32 0)
  %v = extractvalue { <4 x float>, i8 } %r, 0
  ret <4 x float> %v
}

declare { <4 x float>, i8 } @air.sample_texture_cube.v4f32(ptr addrspace(1), ptr addrspace(2), <3 x float>, i1, float, float, i32)
;--- m.types
s %argument_buffer_struct { i32 addrspace(2)*, i32 addrspace(2)*, i32 addrspace(2)* }
f @main <4 x float> (<3 x float>, %argument_buffer_struct addrspace(2)*)
f @air.sample_texture_cube.v4f32 { <4 x float>, i8 } (%struct._texture_cube_t addrspace(1)*, %struct._sampler_t addrspace(2)*, <3 x float>, i1, float, float, i32)
