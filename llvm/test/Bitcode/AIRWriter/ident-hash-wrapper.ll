; Module framing as LLVM 15 writes it: the Darwin wrapper (magic 0x0B17C0DE,
; version 0, offset 20, CPU type ~0 for air64) with the file padded to 16
; bytes; the identification block (producer LLVM15.0.7, epoch 0); the source
; file name; the module hash. The module record layout stays version 1. A
; triple that is not Darwin gets no wrapper.
; RUN: llvm-air-write %s -o %t.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t.bc | FileCheck %s --check-prefix=BC
; RUN: %python -c "import os, sys; sys.exit(os.path.getsize(sys.argv[1]) % 16)" %t.bc
; RUN: llvm-dis %t.bc -o - | FileCheck %s --check-prefix=DIS
; RUN: sed -e 's/air64-apple-macosx14.0.0/air64-unknown-unknown/' %s > %t.nd.ll
; RUN: llvm-air-write %t.nd.ll -o %t.nd.bc
; RUN: llvm-bcanalyzer -dump %t.nd.bc | FileCheck %s --check-prefix=NODARWIN

; STATS: stats inst-casts=0 const-casts=0 use-typed=0 attrs-dropped=0 flags-dropped=0

; BC: <BITCODE_WRAPPER_HEADER Magic=0x0b17c0de Version=0x00000000 Offset=0x00000014 Size=0x{{[0-9a-f]+}} CPUType=0xffffffff/>
; BC-NEXT: <IDENTIFICATION_BLOCK_ID
; BC-NEXT: <STRING {{.*}}/> record string = 'LLVM15.0.7'
; BC-NEXT: <EPOCH {{.*}}op0=0/>
; BC-NEXT: </IDENTIFICATION_BLOCK_ID>
; BC-NEXT: <MODULE_BLOCK
; BC-NEXT: <VERSION op0=1/>
; BC: <SOURCE_FILENAME {{.*}}/> record string = 'airconv_generated.metal'
; BC: <HASH op0={{[0-9]+}} op1={{[0-9]+}} op2={{[0-9]+}} op3={{[0-9]+}} op4={{[0-9]+}}/>
; BC-NEXT: </MODULE_BLOCK>

; DIS: source_filename = "airconv_generated.metal"

; NODARWIN-NOT: BITCODE_WRAPPER_HEADER
; NODARWIN: <IDENTIFICATION_BLOCK_ID

source_filename = "airconv_generated.metal"
target triple = "air64-apple-macosx14.0.0"

define void @k() {
  ret void
}
