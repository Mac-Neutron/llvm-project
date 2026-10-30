//===- AIRTypedRefs.cpp - Typed types from typed bitcode ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Added by Mac-Neutron (2026).
//
//===----------------------------------------------------------------------===//
//
// A stub: no ref is read yet (DXMTRS step 4W, Task 5 replaces this file).
//
//===----------------------------------------------------------------------===//

#include "AIRTypedRefs.h"

using namespace llvm;
using namespace llvm::air;

Expected<AIRRefTable> llvm::air::readTypedRefs(LLVMContext &,
                                               ArrayRef<MemoryBufferRef> Refs) {
  if (Refs.empty())
    return AIRRefTable();
  return createStringError(inconvertibleErrorCode(),
                           "typed ref 0: typed refs are not read yet");
}
