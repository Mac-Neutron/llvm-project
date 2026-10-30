//===- AIRWriterC.cpp - C interface to the AIR writer ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Added by Mac-Neutron (2026).
//
//===----------------------------------------------------------------------===//

#include "AIRBitcodeWriter.h"
#include "AIRTypes.h"
#include "llvm-c/AIRWriter.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/TypedPointerType.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <cstring>

using namespace llvm;
using namespace llvm::air;

LLVMTypeRef LLVMAIRTypedPointerType(LLVMTypeRef Pointee,
                                    unsigned AddressSpace) {
  Type *T = unwrap(Pointee);
  if (!T || !TypedPointerType::isValidElementType(T))
    return nullptr;
  return wrap(TypedPointerType::get(T, AddressSpace));
}

LLVMBool llvm::air::writeAIRBitcodeC(LLVMModuleRef MRef,
                                     const LLVMAIRTypes *Types,
                                     LLVMMemoryBufferRef *OutBuf,
                                     LLVMAIRWriteStats *Stats,
                                     char **ErrorMessage, AIRWriteReport *R) {
  auto Fail = [ErrorMessage](const std::string &Msg) {
    if (ErrorMessage)
      *ErrorMessage = strdup(Msg.c_str());
    return 1;
  };
  if (OutBuf)
    *OutBuf = nullptr;
  if (!MRef || !OutBuf)
    return Fail("arguments: no module or no output buffer");
  Module &M = *unwrap(MRef);

  std::string VerifierText;
  raw_string_ostream VOS(VerifierText);
  if (verifyModule(M, &VOS))
    return Fail(VerifierText);

  SmallVector<std::pair<Value *, Type *>, 0> Values;
  SmallVector<std::pair<Type *, Type *>, 0> Structs;
  SmallVector<MemoryBufferRef, 4> Refs;
  if (Types) {
    if ((Types->NumValues && !Types->Values) ||
        (Types->NumStructs && !Types->Structs) ||
        (Types->NumTypedRefs && !Types->TypedRefs))
      return Fail("table: a count with no array");
    for (size_t I = 0; I != Types->NumValues; ++I)
      Values.push_back(
          {unwrap(Types->Values[I].Value), unwrap(Types->Values[I].Type)});
    for (size_t I = 0; I != Types->NumStructs; ++I)
      Structs.push_back(
          {unwrap(Types->Structs[I].Struct), unwrap(Types->Structs[I].Body)});
    for (size_t I = 0; I != Types->NumTypedRefs; ++I) {
      if (!Types->TypedRefs[I])
        return Fail("typed ref " + std::to_string(I) + ": no buffer");
      Refs.push_back(unwrap(Types->TypedRefs[I])->getMemBufferRef());
    }
  }

  Expected<AIRTypeTable> Table = AIRTypeTable::build(M, {Values, Structs, Refs});
  if (!Table)
    return Fail(toString(Table.takeError()));
  AIRWriteReport Local;
  if (!R)
    R = &Local;
  SmallString<0> Out;
  raw_svector_ostream OS(Out);
  if (Error E = writeAIRBitcode(M, *Table, OS, R))
    return Fail(toString(std::move(E)));
  if (Stats)
    *Stats = R->Stats;
  *OutBuf = wrap(MemoryBuffer::getMemBufferCopy(Out.str(), "air").release());
  return 0;
}

LLVMBool LLVMWriteAIRBitcode(LLVMModuleRef M, const LLVMAIRTypes *Types,
                             LLVMMemoryBufferRef *OutBuf,
                             LLVMAIRWriteStats *Stats, char **ErrorMessage) {
  return writeAIRBitcodeC(M, Types, OutBuf, Stats, ErrorMessage, nullptr);
}
