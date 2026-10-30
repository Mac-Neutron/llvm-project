//===- AIRTypedRefs.h - Typed types from typed bitcode ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Added by Mac-Neutron (2026).
//
//===----------------------------------------------------------------------===//
//
// Reads typed (LLVM 15 or Apple) bitcode for the typed types of its functions,
// globals and structs, by name. Read per call: no cache (P16).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_BITCODE_AIRWRITER_AIRTYPEDREFS_H
#define LLVM_LIB_BITCODE_AIRWRITER_AIRTYPEDREFS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"

namespace llvm {

class LLVMContext;

namespace air {

/// Typed types by name, all in one context. Identified structs in them are
/// the context's own, by name.
struct AIRRefTable {
  StringMap<FunctionType *> Functions;
  /// Value types.
  StringMap<Type *> Globals;
  /// Typed literal bodies, for the identified structs the context has.
  StringMap<StructType *> Bodies;
};

/// Scans each ref (raw or Darwin-wrapped bitcode, typed pointers). A function,
/// global or body whose type holds a struct the context lacks, or a type this
/// reader does not build, is left out. Malformed input, an opaque pointer, and
/// two refs that give one name two types are errors: "typed ref <n>: <why>".
Expected<AIRRefTable> readTypedRefs(LLVMContext &C,
                                    ArrayRef<MemoryBufferRef> Refs);

} // namespace air
} // namespace llvm

#endif // LLVM_LIB_BITCODE_AIRWRITER_AIRTYPEDREFS_H
