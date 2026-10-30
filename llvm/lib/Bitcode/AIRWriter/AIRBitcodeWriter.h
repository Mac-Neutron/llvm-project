//===- AIRBitcodeWriter.h - Typed-pointer AIR bitcode writer ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Modified by Mac-Neutron (2026): copied from
// lib/Target/DirectX/DXILWriter/DXILBitcodeWriter.h at llvmorg-23.1.4; the
// interface is now the AIR writer's: one module, a typed-type table, an Error.
//
//===----------------------------------------------------------------------===//
//
// Writes an opaque-pointer module as typed-pointer AIR bitcode, in the DXIL
// writer's LLVM 3.7 record layout. Not installed: the C interface is
// llvm-c/AIRWriter.h.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_BITCODE_AIRWRITER_AIRBITCODEWRITER_H
#define LLVM_LIB_BITCODE_AIRWRITER_AIRBITCODEWRITER_H

#include "llvm-c/AIRWriter.h"
#include "llvm/Support/Error.h"
#include <string>
#include <vector>

namespace llvm {

class Module;
class Type;
class raw_ostream;

namespace air {

class AIRTypeTable;

/// What a write did: the stats, plus one line per cast, value typed by
/// derivation, use-typed value and dropped attribute or flag (--explain).
struct AIRWriteReport {
  LLVMAIRWriteStats Stats = {};
  std::vector<std::string> Explain;
  /// Test only (llvm-air-write --self-test-late-type): a typed type the
  /// writer asks the enumerator for and that was never enumerated.
  Type *SelfTestLateType = nullptr;
};

/// Writes M as typed-pointer AIR bitcode to OS. M must be verified. It may be
/// modified (casts, drops) and must not be written again. Never aborts.
Error writeAIRBitcode(Module &M, const AIRTypeTable &T, raw_ostream &OS,
                      AIRWriteReport *R);

/// LLVMWriteAIRBitcode with a report: the C entry point is this with
/// R == nullptr. llvm-air-write --explain passes one.
LLVMBool writeAIRBitcodeC(LLVMModuleRef M, const LLVMAIRTypes *Types,
                          LLVMMemoryBufferRef *OutBuf,
                          LLVMAIRWriteStats *Stats, char **ErrorMessage,
                          AIRWriteReport *R);

} // namespace air
} // namespace llvm

#endif // LLVM_LIB_BITCODE_AIRWRITER_AIRBITCODEWRITER_H
