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

/// Typed types by name, all in one context.
struct AIRRefTable {
  StringMap<FunctionType *> Functions;
  StringMap<Type *> Globals;
  StringMap<StructType *> Bodies;
};

Expected<AIRRefTable> readTypedRefs(LLVMContext &C,
                                    ArrayRef<MemoryBufferRef> Refs);

} // namespace air
} // namespace llvm

#endif // LLVM_LIB_BITCODE_AIRWRITER_AIRTYPEDREFS_H
