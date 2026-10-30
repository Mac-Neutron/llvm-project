/*===-- llvm-c/AIRWriter.h - Typed-pointer AIR writer C Interface -*- C -*-===*\
|*                                                                            *|
|* Part of the LLVM Project, under the Apache License v2.0 with LLVM          *|
|* Exceptions.                                                                *|
|* See https://llvm.org/LICENSE.txt for license information.                  *|
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception                    *|
|*                                                                            *|
|*===----------------------------------------------------------------------===*|
|*                                                                            *|
|* This header declares the C interface to libLLVMAIRBitWriter.a, which       *|
|* writes an opaque-pointer module as typed-pointer AIR (Metal) bitcode.      *|
|* Added by Mac-Neutron (2026).                                               *|
|*                                                                            *|
|* A typed type is an ordinary type whose pointers are typed pointers, made   *|
|* with LLVMAIRTypedPointerType. The structs below are an in-process ABI that *|
|* callers mirror by hand: their field order is fixed.                        *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

#ifndef LLVM_C_AIRWRITER_H
#define LLVM_C_AIRWRITER_H

#include "llvm-c/ExternC.h"
#include "llvm-c/Types.h"
#include <stddef.h>
#include <stdint.h>

LLVM_C_EXTERN_C_BEGIN

/* A typed pointer to Pointee in AddressSpace, or NULL if Pointee cannot be
   pointed to (void, label, metadata, token). */
LLVMTypeRef LLVMAIRTypedPointerType(LLVMTypeRef Pointee, unsigned AddressSpace);

typedef struct { LLVMValueRef Value; LLVMTypeRef Type; } LLVMAIRValueType;   /* function: typed function type;
  global: typed value type; argument or instruction: the value's typed type */
typedef struct { LLVMTypeRef Struct; LLVMTypeRef Body; } LLVMAIRStructBody;  /* identified struct -> typed literal body */
typedef struct {
  const LLVMAIRValueType *Values; size_t NumValues;
  const LLVMAIRStructBody *Structs; size_t NumStructs;
  const LLVMMemoryBufferRef *TypedRefs; size_t NumTypedRefs;  /* typed bitcode typing same-named functions, globals, structs */
} LLVMAIRTypes;
typedef struct {
  uint32_t InstCasts, ConstCasts, UseTyped, AttrsDropped, FlagsDropped, RefsFilled;
} LLVMAIRWriteStats;
/* Writes M as typed-pointer AIR bitcode (Darwin wrapper included) into *OutBuf. M is verified first and modified
   (pointer casts); it must not be written again. 0 on success; 1 with *ErrorMessage (LLVMDisposeMessage) otherwise.
   Never aborts. Stats may be null. */
LLVMBool LLVMWriteAIRBitcode(LLVMModuleRef M, const LLVMAIRTypes *Types, LLVMMemoryBufferRef *OutBuf,
                             LLVMAIRWriteStats *Stats, char **ErrorMessage);

LLVM_C_EXTERN_C_END

#endif
