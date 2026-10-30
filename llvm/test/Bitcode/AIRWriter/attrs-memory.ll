; memory(...) becomes LLVM 15's attributes: readnone (20), readonly (21),
; writeonly (52), argmemonly (45), inaccessiblememonly (49) and
; inaccessiblemem_or_argmemonly (50). Each exact form reads back as itself.
; A form LLVM 15 cannot say becomes the strongest sound cover, applied to the
; module and counted: @weaker gets both locations readwrite, @weakest no
; attribute at all.
; RUN: llvm-air-write %s --explain --emit-ll %t -o %t.bc 2>&1 | FileCheck %s --check-prefix=STATS
; RUN: llvm-bcanalyzer -dump %t.bc | FileCheck %s --check-prefix=BC
; RUN: llvm-dis %t.bc -o %t.dis.ll
; RUN: FileCheck %s --check-prefix=DIS < %t.dis.ll
; RUN: FileCheck %s --check-prefix=POST < %t.post.ll
; The module the writer left is what was written (P9's self row).
; RUN: tail -n +2 %t.dis.ll > %t.dis.tail
; RUN: tail -n +2 %t.post.ll > %t.post.tail
; RUN: diff %t.dis.tail %t.post.tail

; STATS: weaker memory(argmem: read, inaccessiblemem: write) -> memory(argmem: readwrite, inaccessiblemem: readwrite) (in @weaker)
; STATS: weaker memory(read, argmem: readwrite) -> no attribute (in @weakest)
; STATS: stats inst-casts=0 const-casts=0 use-typed=0 attrs-dropped=2 flags-dropped=0

; BC-LABEL: <PARAMATTR_GROUP_BLOCK_ID
; BC-NEXT: <ENTRY op0=1 op1=4294967295 op2=0 op3=20/>
; BC-NEXT: <ENTRY op0=2 op1=4294967295 op2=0 op3=21/>
; BC-NEXT: <ENTRY op0=3 op1=4294967295 op2=0 op3=52/>
; BC-NEXT: <ENTRY op0=4 op1=4294967295 op2=0 op3=45/>
; BC-NEXT: <ENTRY op0=5 op1=4294967295 op2=0 op3=21 op4=0 op5=45/>
; BC-NEXT: <ENTRY op0=6 op1=4294967295 op2=0 op3=52 op4=0 op5=45/>
; BC-NEXT: <ENTRY op0=7 op1=4294967295 op2=0 op3=49/>
; BC-NEXT: <ENTRY op0=8 op1=4294967295 op2=0 op3=21 op4=0 op5=49/>
; BC-NEXT: <ENTRY op0=9 op1=4294967295 op2=0 op3=50/>
; BC-NEXT: </PARAMATTR_GROUP_BLOCK_ID>

; DIS-DAG: declare void @none() [[NONE:#[0-9]+]]
; DIS-DAG: declare void @read() [[READ:#[0-9]+]]
; DIS-DAG: declare void @write() [[WRITE:#[0-9]+]]
; DIS-DAG: declare void @arg() [[ARG:#[0-9]+]]
; DIS-DAG: declare void @argread() [[ARGREAD:#[0-9]+]]
; DIS-DAG: declare void @argwrite() [[ARGWRITE:#[0-9]+]]
; DIS-DAG: declare void @inacc() [[INACC:#[0-9]+]]
; DIS-DAG: declare void @inaccread() [[INACCREAD:#[0-9]+]]
; DIS-DAG: declare void @either() [[EITHER:#[0-9]+]]
; DIS-DAG: declare void @weaker() [[EITHER]]
; DIS-DAG: declare void @weakest(){{$}}
; DIS-DAG: attributes [[NONE]] = { memory(none) }
; DIS-DAG: attributes [[READ]] = { memory(read) }
; DIS-DAG: attributes [[WRITE]] = { memory(write) }
; DIS-DAG: attributes [[ARG]] = { memory(argmem: readwrite) }
; DIS-DAG: attributes [[ARGREAD]] = { memory(argmem: read) }
; DIS-DAG: attributes [[ARGWRITE]] = { memory(argmem: write) }
; DIS-DAG: attributes [[INACC]] = { memory(inaccessiblemem: readwrite) }
; DIS-DAG: attributes [[INACCREAD]] = { memory(inaccessiblemem: read) }
; DIS-DAG: attributes [[EITHER]] = { memory(argmem: readwrite, inaccessiblemem: readwrite) }

; POST-DAG: declare void @weaker() [[W:#[0-9]+]]
; POST-DAG: declare void @weakest(){{$}}
; POST-DAG: attributes [[W]] = { memory(argmem: readwrite, inaccessiblemem: readwrite) }

target triple = "air64-apple-macosx14.0.0"

declare void @none() memory(none)
declare void @read() memory(read)
declare void @write() memory(write)
declare void @arg() memory(argmem: readwrite)
declare void @argread() memory(argmem: read)
declare void @argwrite() memory(argmem: write)
declare void @inacc() memory(inaccessiblemem: readwrite)
declare void @inaccread() memory(inaccessiblemem: read)
declare void @either() memory(argmem: readwrite, inaccessiblemem: readwrite)
declare void @weaker() memory(argmem: read, inaccessiblemem: write)
declare void @weakest() memory(read, argmem: readwrite)
