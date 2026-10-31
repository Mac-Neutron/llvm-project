//===- AIRBitcodeWriter.cpp - Typed-pointer AIR bitcode writer -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Modified by Mac-Neutron (2026): copied from
// lib/Target/DirectX/DXILWriter/DXILBitcodeWriter.cpp at llvmorg-23.1.4 for
// the AIR writer; namespace llvm::air; every type is written in its typed form
// and every operand as its typed operand (AIRPointerTypes: pointee types from
// the table, refs and derivation, casts and synthetic typed constants; shadow
// structs under their original names), never an i8* default, and an opaque
// pointer in the type table is an error; an air* triple keeps its triple and
// data layout, and a pointer load its typed type (Phase 0 spike, 02e8d34); no
// debug info (a module with any is an error), no summary, string table or
// use-list code. Only the llvm.* intrinsics on LLVM15Intrinsics are written
// (P13). LLVM 15's encodings where the 3.7 layout loses meaning: attribute
// kinds, memory and captures, fast-math flags, unnamed_addr, poison, freeze,
// fneg, call markers, atomics' types and alignment, alloca, the
// identification block, source file name, sync scope names, module hash and
// Darwin wrapper. LLVM 23-only
// attributes and flags are dropped from the module first and counted; what
// LLVM 15 cannot hold is an error, never an llvm_unreachable or assert.
//
//===----------------------------------------------------------------------===//
//
// Bitcode writer implementation.
//
//===----------------------------------------------------------------------===//

#include "AIRBitcodeWriter.h"
#include "AIRPointerTypes.h"
#include "AIRTypes.h"
#include "AIRValueEnumerator.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Bitcode/BitcodeCommon.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/LLVMBitCodes.h"
#include "llvm/Bitstream/BitCodes.h"
#include "llvm/Bitstream/BitstreamWriter.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Comdat.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalIFunc.h"
#include "llvm/IR/GlobalObject.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/TypedPointerType.h"
#include "llvm/IR/UseListOrder.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Support/SHA1.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include <array>

namespace llvm {
namespace air {

class AIRBitcodeWriter {

  /// These are manifest constants used by the bitcode writer. They do not need
  /// to be kept in sync with the reader, but need to be consistent within this
  /// file.
  enum {
    // VALUE_SYMTAB_BLOCK abbrev id's.
    VST_ENTRY_8_ABBREV = bitc::FIRST_APPLICATION_ABBREV,
    VST_ENTRY_7_ABBREV,
    VST_ENTRY_6_ABBREV,
    VST_BBENTRY_6_ABBREV,

    // CONSTANTS_BLOCK abbrev id's.
    CONSTANTS_SETTYPE_ABBREV = bitc::FIRST_APPLICATION_ABBREV,
    CONSTANTS_INTEGER_ABBREV,
    CONSTANTS_CE_CAST_Abbrev,
    CONSTANTS_NULL_Abbrev,

    // FUNCTION_BLOCK abbrev id's.
    FUNCTION_INST_LOAD_ABBREV = bitc::FIRST_APPLICATION_ABBREV,
    FUNCTION_INST_BINOP_ABBREV,
    FUNCTION_INST_BINOP_FLAGS_ABBREV,
    FUNCTION_INST_CAST_ABBREV,
    FUNCTION_INST_RET_VOID_ABBREV,
    FUNCTION_INST_RET_VAL_ABBREV,
    FUNCTION_INST_UNREACHABLE_ABBREV,
    FUNCTION_INST_GEP_ABBREV,
  };

  /// The stream created and owned by the client.
  BitstreamWriter &Stream;

  /// The Module to write to bitcode.
  const Module &M;

  /// The typed type of every value, and each use's typed operand.
  const AIRPointerTypes &Types;

  /// Enumerates ids for all values in the module.
  ValueEnumerator VE;

  /// The first error met while writing; the output is then discarded.
  std::string Err;

  /// llvm-air-write --self-test-late-type: a type never enumerated.
  Type *LateType;

  void fail(const Twine &Msg) {
    if (Err.empty())
      Err = Msg.str();
  }

public:
  AIRBitcodeWriter(const Module &M, const AIRPointerTypes &Types,
                   BitstreamWriter &Stream, Type *LateType)
      : Stream(Stream), M(M), Types(Types), VE(M, Types), LateType(LateType) {
  }

  /// Emit the current module to the bitstream.
  void write();

  /// The writer's first error, else the enumerator's first missed lookup.
  const std::string &error() const { return Err.empty() ? VE.error() : Err; }

  static uint64_t getAttrKindEncoding(Attribute::AttrKind Kind);
  static void writeStringRecord(BitstreamWriter &Stream, unsigned Code,
                                StringRef Str, unsigned AbbrevToUse);
  static void writeIdentificationBlock(BitstreamWriter &Stream);
  static void emitSignedInt64(SmallVectorImpl<uint64_t> &Vals, uint64_t V);
  static unsigned getEncodedUnnamedAddr(const GlobalValue &GV);
  static void emitWideAPInt(SmallVectorImpl<uint64_t> &Vals, const APInt &A);

  static unsigned getEncodedComdatSelectionKind(const Comdat &C);
  static unsigned getEncodedLinkage(const GlobalValue::LinkageTypes Linkage);
  static unsigned getEncodedLinkage(const GlobalValue &GV);
  static unsigned getEncodedVisibility(const GlobalValue &GV);
  static unsigned getEncodedThreadLocalMode(const GlobalValue &GV);
  static unsigned getEncodedDLLStorageClass(const GlobalValue &GV);
  static unsigned getEncodedCastOpcode(unsigned Opcode);
  static unsigned getEncodedUnaryOpcode(unsigned Opcode);
  static unsigned getEncodedBinaryOpcode(unsigned Opcode);
  static unsigned getEncodedRMWOperation(AtomicRMWInst::BinOp Op);
  static unsigned getEncodedOrdering(AtomicOrdering Ordering);
  static uint64_t getOptimizationFlags(const Value *V);

private:
  void writeModuleVersion();
  void writeModuleHash(StringRef View);

  void writeAttributeGroupTable();
  void writeAttributeTable();
  void writeTypeTable();
  void writeComdats();
  void writeValueSymbolTableForwardDecl();
  void writeModuleInfo();
  void writeValueAsMetadata(const ValueAsMetadata *MD,
                            SmallVectorImpl<uint64_t> &Record);
  void writeMDTuple(const MDTuple *N, SmallVectorImpl<uint64_t> &Record,
                    unsigned Abbrev);
  unsigned createMetadataStringsAbbrev();
  void writeMetadataStrings(ArrayRef<const Metadata *> Strings,
                            SmallVectorImpl<uint64_t> &Record);
  void writeMetadataRecords(ArrayRef<const Metadata *> MDs,
                            SmallVectorImpl<uint64_t> &Record);
  void writeModuleMetadata();
  void writeFunctionMetadata(const Function &F);
  void writeFunctionMetadataAttachment(const Function &F);
  void pushGlobalMetadataAttachment(SmallVectorImpl<uint64_t> &Record,
                                    const GlobalObject &GO);
  void writeModuleMetadataKinds();
  void writeOperandBundleTags();
  void writeSyncScopeNames();
  void writeConstants(unsigned FirstVal, unsigned LastVal, bool isGlobal);
  void writeModuleConstants();
  bool pushValueAndType(const Value *V, unsigned InstID,
                        SmallVectorImpl<unsigned> &Vals);
  void writeOperandBundles(const CallBase &CB, unsigned InstID);
  void pushValue(const Value *V, unsigned InstID,
                 SmallVectorImpl<unsigned> &Vals);
  void pushValueSigned(const Value *V, unsigned InstID,
                       SmallVectorImpl<uint64_t> &Vals);
  void writeInstruction(const Instruction &I, unsigned InstID,
                        SmallVectorImpl<unsigned> &Vals);
  void writeFunctionLevelValueSymbolTable(const ValueSymbolTable &VST);
  void writeGlobalValueSymbolTable(
      DenseMap<const Function *, uint64_t> &FunctionToBitcodeIndex);
  void writeFunction(const Function &F);
  void writeBlockInfo();

  unsigned getEncodedSyncScopeID(SyncScope::ID SSID) { return unsigned(SSID); }

  unsigned getEncodedAlign(MaybeAlign Alignment) { return encode(Alignment); }

  /// The id of V's typed type, or (no V) of T's typed form.
  unsigned getTypeID(Type *T, const Value *V = nullptr);
  /// The id of a global's typed value type or a function's typed type.
  unsigned getGlobalObjectValueTypeID(Type *T, const GlobalObject *G);
};

} // namespace air
} // namespace llvm

using namespace llvm;
using namespace llvm::air;

////////////////////////////////////////////////////////////////////////////////
/// Begin AIRBitcodeWriter static helpers
////////////////////////////////////////////////////////////////////////////////

/// The opcode tables return ~0U for an opcode LLVM 15 has no code for; the
/// caller makes that an error naming it (P17).
unsigned AIRBitcodeWriter::getEncodedCastOpcode(unsigned Opcode) {
  switch (Opcode) {
  default:
    return ~0U;
  case Instruction::Trunc:
    return bitc::CAST_TRUNC;
  case Instruction::ZExt:
    return bitc::CAST_ZEXT;
  case Instruction::SExt:
    return bitc::CAST_SEXT;
  case Instruction::FPToUI:
    return bitc::CAST_FPTOUI;
  case Instruction::FPToSI:
    return bitc::CAST_FPTOSI;
  case Instruction::UIToFP:
    return bitc::CAST_UITOFP;
  case Instruction::SIToFP:
    return bitc::CAST_SITOFP;
  case Instruction::FPTrunc:
    return bitc::CAST_FPTRUNC;
  case Instruction::FPExt:
    return bitc::CAST_FPEXT;
  case Instruction::PtrToInt:
    return bitc::CAST_PTRTOINT;
  case Instruction::IntToPtr:
    return bitc::CAST_INTTOPTR;
  case Instruction::BitCast:
    return bitc::CAST_BITCAST;
  case Instruction::AddrSpaceCast:
    return bitc::CAST_ADDRSPACECAST;
  }
}

unsigned AIRBitcodeWriter::getEncodedUnaryOpcode(unsigned Opcode) {
  switch (Opcode) {
  default:
    return ~0U;
  case Instruction::FNeg:
    return bitc::UNOP_FNEG;
  }
}

unsigned AIRBitcodeWriter::getEncodedBinaryOpcode(unsigned Opcode) {
  switch (Opcode) {
  default:
    return ~0U;
  case Instruction::Add:
  case Instruction::FAdd:
    return bitc::BINOP_ADD;
  case Instruction::Sub:
  case Instruction::FSub:
    return bitc::BINOP_SUB;
  case Instruction::Mul:
  case Instruction::FMul:
    return bitc::BINOP_MUL;
  case Instruction::UDiv:
    return bitc::BINOP_UDIV;
  case Instruction::FDiv:
  case Instruction::SDiv:
    return bitc::BINOP_SDIV;
  case Instruction::URem:
    return bitc::BINOP_UREM;
  case Instruction::FRem:
  case Instruction::SRem:
    return bitc::BINOP_SREM;
  case Instruction::Shl:
    return bitc::BINOP_SHL;
  case Instruction::LShr:
    return bitc::BINOP_LSHR;
  case Instruction::AShr:
    return bitc::BINOP_ASHR;
  case Instruction::And:
    return bitc::BINOP_AND;
  case Instruction::Or:
    return bitc::BINOP_OR;
  case Instruction::Xor:
    return bitc::BINOP_XOR;
  }
}

unsigned AIRBitcodeWriter::getTypeID(Type *T, const Value *V) {
  return VE.getTypeID(V ? Types.typedTypeOf(V) : Types.form(T));
}

unsigned AIRBitcodeWriter::getGlobalObjectValueTypeID(Type *T,
                                                      const GlobalObject *G) {
  if (const auto *F = dyn_cast_or_null<Function>(G))
    return VE.getTypeID(Types.functionType(*F));
  if (const auto *GV = dyn_cast_or_null<GlobalVariable>(G))
    return VE.getTypeID(Types.valueType(*GV));
  return VE.getTypeID(Types.form(T));
}

unsigned AIRBitcodeWriter::getEncodedRMWOperation(AtomicRMWInst::BinOp Op) {
  switch (Op) {
  default:
    return ~0U;
  case AtomicRMWInst::Xchg:
    return bitc::RMW_XCHG;
  case AtomicRMWInst::Add:
    return bitc::RMW_ADD;
  case AtomicRMWInst::Sub:
    return bitc::RMW_SUB;
  case AtomicRMWInst::And:
    return bitc::RMW_AND;
  case AtomicRMWInst::Nand:
    return bitc::RMW_NAND;
  case AtomicRMWInst::Or:
    return bitc::RMW_OR;
  case AtomicRMWInst::Xor:
    return bitc::RMW_XOR;
  case AtomicRMWInst::Max:
    return bitc::RMW_MAX;
  case AtomicRMWInst::Min:
    return bitc::RMW_MIN;
  case AtomicRMWInst::UMax:
    return bitc::RMW_UMAX;
  case AtomicRMWInst::UMin:
    return bitc::RMW_UMIN;
  case AtomicRMWInst::FAdd:
    return bitc::RMW_FADD;
  case AtomicRMWInst::FSub:
    return bitc::RMW_FSUB;
  case AtomicRMWInst::FMax:
    return bitc::RMW_FMAX;
  case AtomicRMWInst::FMin:
    return bitc::RMW_FMIN;
  }
}

unsigned AIRBitcodeWriter::getEncodedOrdering(AtomicOrdering Ordering) {
  switch (Ordering) {
  case AtomicOrdering::NotAtomic:
    return bitc::ORDERING_NOTATOMIC;
  case AtomicOrdering::Unordered:
    return bitc::ORDERING_UNORDERED;
  case AtomicOrdering::Monotonic:
    return bitc::ORDERING_MONOTONIC;
  case AtomicOrdering::Acquire:
    return bitc::ORDERING_ACQUIRE;
  case AtomicOrdering::Release:
    return bitc::ORDERING_RELEASE;
  case AtomicOrdering::AcquireRelease:
    return bitc::ORDERING_ACQREL;
  case AtomicOrdering::SequentiallyConsistent:
    return bitc::ORDERING_SEQCST;
  }
  llvm_unreachable("Invalid ordering");
}

void AIRBitcodeWriter::writeStringRecord(BitstreamWriter &Stream,
                                          unsigned Code, StringRef Str,
                                          unsigned AbbrevToUse) {
  SmallVector<unsigned, 64> Vals;

  // Code: [strchar x N]
  for (char C : Str) {
    if (AbbrevToUse && !BitCodeAbbrevOp::isChar6(C))
      AbbrevToUse = 0;
    Vals.push_back(C);
  }

  // Emit the finished record.
  Stream.EmitRecord(Code, Vals, AbbrevToUse);
}

/// LLVM 15's attribute kind table (L15/lib/Bitcode/Writer/BitcodeWriter.cpp
/// getAttrKindEncoding), minus the kinds LLVM 23 removed (argmemonly,
/// inaccessiblememonly, inaccessiblemem_or_argmemonly, nocapture; memory and
/// captures stand for them). Codes 1-84 are the same in both LLVMs. Any other
/// kind gives 0: lowerToLLVM15 drops it (guarantee-only list) or refuses it.
uint64_t AIRBitcodeWriter::getAttrKindEncoding(Attribute::AttrKind Kind) {
  switch (Kind) {
  case Attribute::Alignment:
    return bitc::ATTR_KIND_ALIGNMENT;
  case Attribute::AllocAlign:
    return bitc::ATTR_KIND_ALLOC_ALIGN;
  case Attribute::AllocSize:
    return bitc::ATTR_KIND_ALLOC_SIZE;
  case Attribute::AlwaysInline:
    return bitc::ATTR_KIND_ALWAYS_INLINE;
  case Attribute::Builtin:
    return bitc::ATTR_KIND_BUILTIN;
  case Attribute::ByVal:
    return bitc::ATTR_KIND_BY_VAL;
  case Attribute::Convergent:
    return bitc::ATTR_KIND_CONVERGENT;
  case Attribute::InAlloca:
    return bitc::ATTR_KIND_IN_ALLOCA;
  case Attribute::Cold:
    return bitc::ATTR_KIND_COLD;
  case Attribute::DisableSanitizerInstrumentation:
    return bitc::ATTR_KIND_DISABLE_SANITIZER_INSTRUMENTATION;
  case Attribute::FnRetThunkExtern:
    return bitc::ATTR_KIND_FNRETTHUNK_EXTERN;
  case Attribute::Hot:
    return bitc::ATTR_KIND_HOT;
  case Attribute::ElementType:
    return bitc::ATTR_KIND_ELEMENTTYPE;
  case Attribute::InlineHint:
    return bitc::ATTR_KIND_INLINE_HINT;
  case Attribute::InReg:
    return bitc::ATTR_KIND_IN_REG;
  case Attribute::JumpTable:
    return bitc::ATTR_KIND_JUMP_TABLE;
  case Attribute::MinSize:
    return bitc::ATTR_KIND_MIN_SIZE;
  case Attribute::AllocatedPointer:
    return bitc::ATTR_KIND_ALLOCATED_POINTER;
  case Attribute::AllocKind:
    return bitc::ATTR_KIND_ALLOC_KIND;
  case Attribute::Naked:
    return bitc::ATTR_KIND_NAKED;
  case Attribute::Nest:
    return bitc::ATTR_KIND_NEST;
  case Attribute::NoAlias:
    return bitc::ATTR_KIND_NO_ALIAS;
  case Attribute::NoBuiltin:
    return bitc::ATTR_KIND_NO_BUILTIN;
  case Attribute::NoCallback:
    return bitc::ATTR_KIND_NO_CALLBACK;
  case Attribute::NoDuplicate:
    return bitc::ATTR_KIND_NO_DUPLICATE;
  case Attribute::NoFree:
    return bitc::ATTR_KIND_NOFREE;
  case Attribute::NoImplicitFloat:
    return bitc::ATTR_KIND_NO_IMPLICIT_FLOAT;
  case Attribute::NoInline:
    return bitc::ATTR_KIND_NO_INLINE;
  case Attribute::NoRecurse:
    return bitc::ATTR_KIND_NO_RECURSE;
  case Attribute::NoMerge:
    return bitc::ATTR_KIND_NO_MERGE;
  case Attribute::NonLazyBind:
    return bitc::ATTR_KIND_NON_LAZY_BIND;
  case Attribute::NonNull:
    return bitc::ATTR_KIND_NON_NULL;
  case Attribute::Dereferenceable:
    return bitc::ATTR_KIND_DEREFERENCEABLE;
  case Attribute::DereferenceableOrNull:
    return bitc::ATTR_KIND_DEREFERENCEABLE_OR_NULL;
  case Attribute::NoRedZone:
    return bitc::ATTR_KIND_NO_RED_ZONE;
  case Attribute::NoReturn:
    return bitc::ATTR_KIND_NO_RETURN;
  case Attribute::NoSync:
    return bitc::ATTR_KIND_NOSYNC;
  case Attribute::NoCfCheck:
    return bitc::ATTR_KIND_NOCF_CHECK;
  case Attribute::NoProfile:
    return bitc::ATTR_KIND_NO_PROFILE;
  case Attribute::NoUnwind:
    return bitc::ATTR_KIND_NO_UNWIND;
  case Attribute::NoSanitizeBounds:
    return bitc::ATTR_KIND_NO_SANITIZE_BOUNDS;
  case Attribute::NoSanitizeCoverage:
    return bitc::ATTR_KIND_NO_SANITIZE_COVERAGE;
  case Attribute::NullPointerIsValid:
    return bitc::ATTR_KIND_NULL_POINTER_IS_VALID;
  case Attribute::OptForFuzzing:
    return bitc::ATTR_KIND_OPT_FOR_FUZZING;
  case Attribute::OptimizeForSize:
    return bitc::ATTR_KIND_OPTIMIZE_FOR_SIZE;
  case Attribute::OptimizeNone:
    return bitc::ATTR_KIND_OPTIMIZE_NONE;
  case Attribute::ReadNone:
    return bitc::ATTR_KIND_READ_NONE;
  case Attribute::ReadOnly:
    return bitc::ATTR_KIND_READ_ONLY;
  case Attribute::Returned:
    return bitc::ATTR_KIND_RETURNED;
  case Attribute::ReturnsTwice:
    return bitc::ATTR_KIND_RETURNS_TWICE;
  case Attribute::SExt:
    return bitc::ATTR_KIND_S_EXT;
  case Attribute::Speculatable:
    return bitc::ATTR_KIND_SPECULATABLE;
  case Attribute::StackAlignment:
    return bitc::ATTR_KIND_STACK_ALIGNMENT;
  case Attribute::StackProtect:
    return bitc::ATTR_KIND_STACK_PROTECT;
  case Attribute::StackProtectReq:
    return bitc::ATTR_KIND_STACK_PROTECT_REQ;
  case Attribute::StackProtectStrong:
    return bitc::ATTR_KIND_STACK_PROTECT_STRONG;
  case Attribute::SafeStack:
    return bitc::ATTR_KIND_SAFESTACK;
  case Attribute::ShadowCallStack:
    return bitc::ATTR_KIND_SHADOWCALLSTACK;
  case Attribute::StrictFP:
    return bitc::ATTR_KIND_STRICT_FP;
  case Attribute::StructRet:
    return bitc::ATTR_KIND_STRUCT_RET;
  case Attribute::SanitizeAddress:
    return bitc::ATTR_KIND_SANITIZE_ADDRESS;
  case Attribute::SanitizeHWAddress:
    return bitc::ATTR_KIND_SANITIZE_HWADDRESS;
  case Attribute::SanitizeThread:
    return bitc::ATTR_KIND_SANITIZE_THREAD;
  case Attribute::SanitizeMemory:
    return bitc::ATTR_KIND_SANITIZE_MEMORY;
  case Attribute::SpeculativeLoadHardening:
    return bitc::ATTR_KIND_SPECULATIVE_LOAD_HARDENING;
  case Attribute::SwiftError:
    return bitc::ATTR_KIND_SWIFT_ERROR;
  case Attribute::SwiftSelf:
    return bitc::ATTR_KIND_SWIFT_SELF;
  case Attribute::SwiftAsync:
    return bitc::ATTR_KIND_SWIFT_ASYNC;
  case Attribute::UWTable:
    return bitc::ATTR_KIND_UW_TABLE;
  case Attribute::VScaleRange:
    return bitc::ATTR_KIND_VSCALE_RANGE;
  case Attribute::WillReturn:
    return bitc::ATTR_KIND_WILLRETURN;
  case Attribute::WriteOnly:
    return bitc::ATTR_KIND_WRITEONLY;
  case Attribute::ZExt:
    return bitc::ATTR_KIND_Z_EXT;
  case Attribute::ImmArg:
    return bitc::ATTR_KIND_IMMARG;
  case Attribute::SanitizeMemTag:
    return bitc::ATTR_KIND_SANITIZE_MEMTAG;
  case Attribute::Preallocated:
    return bitc::ATTR_KIND_PREALLOCATED;
  case Attribute::NoUndef:
    return bitc::ATTR_KIND_NOUNDEF;
  case Attribute::ByRef:
    return bitc::ATTR_KIND_BYREF;
  case Attribute::MustProgress:
    return bitc::ATTR_KIND_MUSTPROGRESS;
  case Attribute::PresplitCoroutine:
    return bitc::ATTR_KIND_PRESPLIT_COROUTINE;
  default:
    return 0;
  }
}

void AIRBitcodeWriter::emitSignedInt64(SmallVectorImpl<uint64_t> &Vals,
                                        uint64_t V) {
  if ((int64_t)V >= 0)
    Vals.push_back(V << 1);
  else
    Vals.push_back((-V << 1) | 1);
}

void AIRBitcodeWriter::emitWideAPInt(SmallVectorImpl<uint64_t> &Vals,
                                      const APInt &A) {
  // We have an arbitrary precision integer value to write whose
  // bit width is > 64. However, in canonical unsigned integer
  // format it is likely that the high bits are going to be zero.
  // So, we only write the number of active words.
  unsigned NumWords = A.getActiveWords();
  const uint64_t *RawData = A.getRawData();
  for (unsigned i = 0; i < NumWords; i++)
    emitSignedInt64(Vals, RawData[i]);
}

uint64_t AIRBitcodeWriter::getOptimizationFlags(const Value *V) {
  uint64_t Flags = 0;

  if (const auto *OBO = dyn_cast<OverflowingBinaryOperator>(V)) {
    if (OBO->hasNoSignedWrap())
      Flags |= 1 << bitc::OBO_NO_SIGNED_WRAP;
    if (OBO->hasNoUnsignedWrap())
      Flags |= 1 << bitc::OBO_NO_UNSIGNED_WRAP;
  } else if (const auto *PEO = dyn_cast<PossiblyExactOperator>(V)) {
    if (PEO->isExact())
      Flags |= 1 << bitc::PEO_EXACT;
  } else if (const auto *FPMO = dyn_cast<FPMathOperator>(V)) {
    // LLVM 15's FastMathMap bits; lowerToLLVM15 cleared them where LLVM 15
    // takes none.
    if (FPMO->hasAllowReassoc())
      Flags |= bitc::AllowReassoc;
    if (FPMO->hasNoNaNs())
      Flags |= bitc::NoNaNs;
    if (FPMO->hasNoInfs())
      Flags |= bitc::NoInfs;
    if (FPMO->hasNoSignedZeros())
      Flags |= bitc::NoSignedZeros;
    if (FPMO->hasAllowReciprocal())
      Flags |= bitc::AllowReciprocal;
    if (FPMO->hasAllowContract())
      Flags |= bitc::AllowContract;
    if (FPMO->hasApproxFunc())
      Flags |= bitc::ApproxFunc;
  }

  return Flags;
}

unsigned
AIRBitcodeWriter::getEncodedLinkage(const GlobalValue::LinkageTypes Linkage) {
  switch (Linkage) {
  case GlobalValue::ExternalLinkage:
    return 0;
  case GlobalValue::WeakAnyLinkage:
    return 16;
  case GlobalValue::AppendingLinkage:
    return 2;
  case GlobalValue::InternalLinkage:
    return 3;
  case GlobalValue::LinkOnceAnyLinkage:
    return 18;
  case GlobalValue::ExternalWeakLinkage:
    return 7;
  case GlobalValue::CommonLinkage:
    return 8;
  case GlobalValue::PrivateLinkage:
    return 9;
  case GlobalValue::WeakODRLinkage:
    return 17;
  case GlobalValue::LinkOnceODRLinkage:
    return 19;
  case GlobalValue::AvailableExternallyLinkage:
    return 12;
  }
  llvm_unreachable("Invalid linkage");
}

unsigned AIRBitcodeWriter::getEncodedLinkage(const GlobalValue &GV) {
  return getEncodedLinkage(GV.getLinkage());
}

unsigned AIRBitcodeWriter::getEncodedVisibility(const GlobalValue &GV) {
  switch (GV.getVisibility()) {
  case GlobalValue::DefaultVisibility:
    return 0;
  case GlobalValue::HiddenVisibility:
    return 1;
  case GlobalValue::ProtectedVisibility:
    return 2;
  }
  llvm_unreachable("Invalid visibility");
}

unsigned AIRBitcodeWriter::getEncodedDLLStorageClass(const GlobalValue &GV) {
  switch (GV.getDLLStorageClass()) {
  case GlobalValue::DefaultStorageClass:
    return 0;
  case GlobalValue::DLLImportStorageClass:
    return 1;
  case GlobalValue::DLLExportStorageClass:
    return 2;
  }
  llvm_unreachable("Invalid DLL storage class");
}

unsigned AIRBitcodeWriter::getEncodedThreadLocalMode(const GlobalValue &GV) {
  switch (GV.getThreadLocalMode()) {
  case GlobalVariable::NotThreadLocal:
    return 0;
  case GlobalVariable::GeneralDynamicTLSModel:
    return 1;
  case GlobalVariable::LocalDynamicTLSModel:
    return 2;
  case GlobalVariable::InitialExecTLSModel:
    return 3;
  case GlobalVariable::LocalExecTLSModel:
    return 4;
  }
  llvm_unreachable("Invalid TLS model");
}

unsigned AIRBitcodeWriter::getEncodedUnnamedAddr(const GlobalValue &GV) {
  switch (GV.getUnnamedAddr()) {
  case GlobalValue::UnnamedAddr::None:
    return 0;
  case GlobalValue::UnnamedAddr::Local:
    return 2;
  case GlobalValue::UnnamedAddr::Global:
    return 1;
  }
  llvm_unreachable("Invalid unnamed_addr");
}

/// LLVM 15's attributes for the strongest form that covers ME (access first,
/// then location), and the MemoryEffects LLVM 23's reader rebuilds from them
/// (upgradeOldMemoryAttribute, L23/lib/Bitcode/Reader/BitcodeReader.cpp).
/// ME is exact in LLVM 15 when that is ME itself; unknown means no attribute.
static MemoryEffects memory15(MemoryEffects ME,
                              SmallVectorImpl<uint64_t> *Codes) {
  auto Push = [Codes](uint64_t Code) {
    if (Codes)
      Codes->push_back(Code);
  };
  if (ME.doesNotAccessMemory()) {
    Push(bitc::ATTR_KIND_READ_NONE);
    return MemoryEffects::none();
  }
  MemoryEffects R = MemoryEffects::unknown();
  switch (ME.getModRef()) {
  case ModRefInfo::Ref:
    Push(bitc::ATTR_KIND_READ_ONLY);
    R &= MemoryEffects::readOnly();
    break;
  case ModRefInfo::Mod:
    Push(bitc::ATTR_KIND_WRITEONLY);
    R &= MemoryEffects::writeOnly();
    break;
  default:
    break;
  }
  MemoryEffects NoArg = ME.getWithoutLoc(IRMemLocation::ArgMem);
  if (NoArg.doesNotAccessMemory()) {
    Push(bitc::ATTR_KIND_ARGMEMONLY);
    R &= MemoryEffects::argMemOnly();
  } else if (ME.getWithoutLoc(IRMemLocation::InaccessibleMem)
                 .doesNotAccessMemory()) {
    Push(bitc::ATTR_KIND_INACCESSIBLEMEM_ONLY);
    R &= MemoryEffects::inaccessibleMemOnly();
  } else if (NoArg.getWithoutLoc(IRMemLocation::InaccessibleMem)
                 .doesNotAccessMemory()) {
    Push(bitc::ATTR_KIND_INACCESSIBLEMEM_OR_ARGMEMONLY);
    R &= MemoryEffects::inaccessibleOrArgMemOnly();
  }
  return R;
}

unsigned AIRBitcodeWriter::getEncodedComdatSelectionKind(const Comdat &C) {
  switch (C.getSelectionKind()) {
  case Comdat::Any:
    return bitc::COMDAT_SELECTION_KIND_ANY;
  case Comdat::ExactMatch:
    return bitc::COMDAT_SELECTION_KIND_EXACT_MATCH;
  case Comdat::Largest:
    return bitc::COMDAT_SELECTION_KIND_LARGEST;
  case Comdat::NoDeduplicate:
    return bitc::COMDAT_SELECTION_KIND_NO_DUPLICATES;
  case Comdat::SameSize:
    return bitc::COMDAT_SELECTION_KIND_SAME_SIZE;
  }
  llvm_unreachable("Invalid selection kind");
}

////////////////////////////////////////////////////////////////////////////////
/// Begin AIRBitcodeWriter Implementation
////////////////////////////////////////////////////////////////////////////////

void AIRBitcodeWriter::writeAttributeGroupTable() {
  const std::vector<ValueEnumerator::IndexAndAttrSet> &AttrGrps =
      VE.getAttributeGroups();
  if (AttrGrps.empty())
    return;

  Stream.EnterSubblock(bitc::PARAMATTR_GROUP_BLOCK_ID, 3);

  SmallVector<uint64_t, 64> Record;
  for (ValueEnumerator::IndexAndAttrSet Pair : AttrGrps) {
    unsigned AttrListIndex = Pair.first;
    AttributeSet AS = Pair.second;
    Record.push_back(VE.getAttributeGroupID(Pair));
    Record.push_back(AttrListIndex);

    for (Attribute Attr : AS) {
      if (!Attr.isStringAttribute()) {
        // lowerToLLVM15 left only what LLVM 15 holds exactly; anything else
        // here is an error, not a guess.
        Attribute::AttrKind Kind = Attr.getKindAsEnum();
        if (Kind == Attribute::Memory) {
          SmallVector<uint64_t, 2> Codes;
          if (memory15(Attr.getMemoryEffects(), &Codes) !=
              Attr.getMemoryEffects())
            fail("attribute " + Attr.getAsString() + ": no exact LLVM 15 form");
          for (uint64_t Code : Codes) {
            Record.push_back(0);
            Record.push_back(Code);
          }
          continue;
        }
        if (Kind == Attribute::Captures) {
          if (Attr.getCaptureInfo() != CaptureInfo::none())
            fail("attribute " + Attr.getAsString() + ": no LLVM 15 form");
          Record.push_back(0);
          Record.push_back(bitc::ATTR_KIND_NO_CAPTURE);
          continue;
        }
        uint64_t Code = getAttrKindEncoding(Kind);
        if (!Code || !(Attr.isEnumAttribute() || Attr.isIntAttribute())) {
          fail("attribute " + Attr.getAsString() + ": no LLVM 15 encoding");
          continue;
        }
        Record.push_back(Attr.isIntAttribute() ? 1 : 0);
        Record.push_back(Code);
        if (Attr.isIntAttribute())
          Record.push_back(Attr.getValueAsInt());
      } else {
        StringRef Kind = Attr.getKindAsString();
        StringRef Val = Attr.getValueAsString();

        Record.push_back(Val.empty() ? 3 : 4);
        Record.append(Kind.begin(), Kind.end());
        Record.push_back(0);
        if (!Val.empty()) {
          Record.append(Val.begin(), Val.end());
          Record.push_back(0);
        }
      }
    }

    Stream.EmitRecord(bitc::PARAMATTR_GRP_CODE_ENTRY, Record);
    Record.clear();
  }

  Stream.ExitBlock();
}

void AIRBitcodeWriter::writeAttributeTable() {
  const std::vector<AttributeList> &Attrs = VE.getAttributeLists();
  if (Attrs.empty())
    return;

  Stream.EnterSubblock(bitc::PARAMATTR_BLOCK_ID, 3);

  SmallVector<uint64_t, 64> Record;
  for (AttributeList AL : Attrs) {
    for (unsigned i : AL.indexes()) {
      AttributeSet AS = AL.getAttributes(i);
      if (AS.hasAttributes())
        Record.push_back(VE.getAttributeGroupID({i, AS}));
    }

    Stream.EmitRecord(bitc::PARAMATTR_CODE_ENTRY, Record);
    Record.clear();
  }

  Stream.ExitBlock();
}

/// WriteTypeTable - Write out the type table for a module.
void AIRBitcodeWriter::writeTypeTable() {
  const ValueEnumerator::TypeList &TypeList = VE.getTypes();

  Stream.EnterSubblock(bitc::TYPE_BLOCK_ID_NEW, 4 /*count from # abbrevs */);
  SmallVector<uint64_t, 64> TypeVals;

  uint64_t NumBits = VE.computeBitsRequiredForTypeIndices();

  // Abbrev for TYPE_CODE_POINTER.
  auto Abbv = std::make_shared<BitCodeAbbrev>();
  Abbv->Add(BitCodeAbbrevOp(bitc::TYPE_CODE_POINTER));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, NumBits));
  Abbv->Add(BitCodeAbbrevOp(0)); // Addrspace = 0
  unsigned PtrAbbrev = Stream.EmitAbbrev(std::move(Abbv));

  // Abbrev for TYPE_CODE_FUNCTION.
  Abbv = std::make_shared<BitCodeAbbrev>();
  Abbv->Add(BitCodeAbbrevOp(bitc::TYPE_CODE_FUNCTION));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 1)); // isvararg
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, NumBits));
  unsigned FunctionAbbrev = Stream.EmitAbbrev(std::move(Abbv));

  // Abbrev for TYPE_CODE_STRUCT_ANON.
  Abbv = std::make_shared<BitCodeAbbrev>();
  Abbv->Add(BitCodeAbbrevOp(bitc::TYPE_CODE_STRUCT_ANON));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 1)); // ispacked
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, NumBits));
  unsigned StructAnonAbbrev = Stream.EmitAbbrev(std::move(Abbv));

  // Abbrev for TYPE_CODE_STRUCT_NAME.
  Abbv = std::make_shared<BitCodeAbbrev>();
  Abbv->Add(BitCodeAbbrevOp(bitc::TYPE_CODE_STRUCT_NAME));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Char6));
  unsigned StructNameAbbrev = Stream.EmitAbbrev(std::move(Abbv));

  // Abbrev for TYPE_CODE_STRUCT_NAMED.
  Abbv = std::make_shared<BitCodeAbbrev>();
  Abbv->Add(BitCodeAbbrevOp(bitc::TYPE_CODE_STRUCT_NAMED));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 1)); // ispacked
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, NumBits));
  unsigned StructNamedAbbrev = Stream.EmitAbbrev(std::move(Abbv));

  // Abbrev for TYPE_CODE_ARRAY.
  Abbv = std::make_shared<BitCodeAbbrev>();
  Abbv->Add(BitCodeAbbrevOp(bitc::TYPE_CODE_ARRAY));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 8)); // size
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, NumBits));
  unsigned ArrayAbbrev = Stream.EmitAbbrev(std::move(Abbv));

  // Emit an entry count so the reader can reserve space.
  TypeVals.push_back(TypeList.size());
  Stream.EmitRecord(bitc::TYPE_CODE_NUMENTRY, TypeVals);
  TypeVals.clear();

  // Loop over all of the types, emitting each in turn.
  for (Type *T : TypeList) {
    int AbbrevToUse = 0;
    unsigned Code = 0;

    switch (T->getTypeID()) {
    case Type::BFloatTyID:
    case Type::X86_AMXTyID:
    case Type::TokenTyID:
    case Type::TargetExtTyID:
    case Type::ByteTyID:
      fail("type " + typeName(T) + ": no LLVM 15 AIR encoding");
      continue;
    case Type::VoidTyID:
      Code = bitc::TYPE_CODE_VOID;
      break;
    case Type::HalfTyID:
      Code = bitc::TYPE_CODE_HALF;
      break;
    case Type::FloatTyID:
      Code = bitc::TYPE_CODE_FLOAT;
      break;
    case Type::DoubleTyID:
      Code = bitc::TYPE_CODE_DOUBLE;
      break;
    case Type::X86_FP80TyID:
      Code = bitc::TYPE_CODE_X86_FP80;
      break;
    case Type::FP128TyID:
      Code = bitc::TYPE_CODE_FP128;
      break;
    case Type::PPC_FP128TyID:
      Code = bitc::TYPE_CODE_PPC_FP128;
      break;
    case Type::LabelTyID:
      Code = bitc::TYPE_CODE_LABEL;
      break;
    case Type::MetadataTyID:
      Code = bitc::TYPE_CODE_METADATA;
      break;
    case Type::IntegerTyID:
      // INTEGER: [width]
      Code = bitc::TYPE_CODE_INTEGER;
      TypeVals.push_back(cast<IntegerType>(T)->getBitWidth());
      break;
    case Type::TypedPointerTyID: {
      TypedPointerType *PTy = cast<TypedPointerType>(T);
      // POINTER: [pointee type, address space]
      Code = bitc::TYPE_CODE_POINTER;
      TypeVals.push_back(VE.getTypeID(PTy->getElementType()));
      unsigned AddressSpace = PTy->getAddressSpace();
      TypeVals.push_back(AddressSpace);
      if (AddressSpace == 0)
        AbbrevToUse = PtrAbbrev;
      break;
    }
    case Type::PointerTyID:
      // Every pointer is written typed; an opaque one has no pointee to write.
      fail("type table: opaque pointer");
      Code = bitc::TYPE_CODE_OPAQUE;
      TypeVals.push_back(false);
      break;
    case Type::FunctionTyID: {
      FunctionType *FT = cast<FunctionType>(T);
      // FUNCTION: [isvararg, retty, paramty x N]
      Code = bitc::TYPE_CODE_FUNCTION;
      TypeVals.push_back(FT->isVarArg());
      TypeVals.push_back(VE.getTypeID(FT->getReturnType()));
      for (Type *PTy : FT->params())
        TypeVals.push_back(VE.getTypeID(PTy));
      AbbrevToUse = FunctionAbbrev;
      break;
    }
    case Type::StructTyID: {
      StructType *ST = cast<StructType>(T);
      // STRUCT: [ispacked, eltty x N]
      TypeVals.push_back(ST->isPacked());
      // Output all of the element types.
      for (Type *ElTy : ST->elements())
        TypeVals.push_back(VE.getTypeID(ElTy));

      if (ST->isLiteral()) {
        Code = bitc::TYPE_CODE_STRUCT_ANON;
        AbbrevToUse = StructAnonAbbrev;
      } else {
        if (ST->isOpaque()) {
          Code = bitc::TYPE_CODE_OPAQUE;
        } else {
          Code = bitc::TYPE_CODE_STRUCT_NAMED;
          AbbrevToUse = StructNamedAbbrev;
        }

        // Emit the name if it is present; a shadow struct has its original's.
        StringRef Name = Types.nameOf(ST);
        if (!Name.empty())
          writeStringRecord(Stream, bitc::TYPE_CODE_STRUCT_NAME, Name,
                            StructNameAbbrev);
      }
      break;
    }
    case Type::ArrayTyID: {
      ArrayType *AT = cast<ArrayType>(T);
      // ARRAY: [numelts, eltty]
      Code = bitc::TYPE_CODE_ARRAY;
      TypeVals.push_back(AT->getNumElements());
      TypeVals.push_back(VE.getTypeID(AT->getElementType()));
      AbbrevToUse = ArrayAbbrev;
      break;
    }
    case Type::ScalableVectorTyID:
      fail("type " + typeName(T) +
           ": a scalable vector has no LLVM 15 AIR encoding");
      continue;
    case Type::FixedVectorTyID: {
      VectorType *VT = cast<VectorType>(T);
      // VECTOR [numelts, eltty]
      Code = bitc::TYPE_CODE_VECTOR;
      TypeVals.push_back(VT->getElementCount().getKnownMinValue());
      TypeVals.push_back(VE.getTypeID(VT->getElementType()));
      break;
    }
    }

    // Emit the finished record.
    Stream.EmitRecord(Code, TypeVals, AbbrevToUse);
    TypeVals.clear();
  }

  Stream.ExitBlock();
}

void AIRBitcodeWriter::writeComdats() {
  SmallVector<uint16_t, 64> Vals;
  for (const Comdat *C : VE.getComdats()) {
    // COMDAT: [selection_kind, name]
    Vals.push_back(getEncodedComdatSelectionKind(*C));
    size_t Size = C->getName().size();
    if (!isUInt<16>(Size))
      fail("comdat " + C->getName().take_front(32) +
           "...: a name longer than 65535 bytes");
    Vals.push_back(Size);
    for (char Chr : C->getName())
      Vals.push_back((unsigned char)Chr);
    Stream.EmitRecord(bitc::MODULE_CODE_COMDAT, Vals, /*AbbrevToUse=*/0);
    Vals.clear();
  }
}

void AIRBitcodeWriter::writeValueSymbolTableForwardDecl() {}

/// Emit top-level description of module, including target triple, inline asm,
/// descriptors for global variables, and function prototype info.
/// Returns the bit offset to backpatch with the location of the real VST.
void AIRBitcodeWriter::writeModuleInfo() {
  // Emit various pieces of data attached to a module.

  // We need to hardcode a triple and datalayout that's compatible with the
  // historical DXIL triple and datalayout from DXC.
  StringRef Triple = "dxil-ms-dx";
  StringRef DL = "e-m:e-p:32:32-i1:32-i8:8-i16:16-i32:32-i64:64-"
                 "f16:16-f32:32-f64:64-n8:16:32:64";
  // An AIR (Metal) module keeps its own triple and data layout (02e8d34).
  std::string ModuleTriple = M.getTargetTriple().str(),
              ModuleDL = M.getDataLayoutStr();
  if (StringRef(ModuleTriple).starts_with("air"))
    Triple = ModuleTriple, DL = ModuleDL;
  writeStringRecord(Stream, bitc::MODULE_CODE_TRIPLE, Triple, 0 /*TODO*/);
  writeStringRecord(Stream, bitc::MODULE_CODE_DATALAYOUT, DL, 0 /*TODO*/);

  // The original bitcode writer wrote inline assembly here. Inline assembly
  // isn't valid in DXIL, so this is removed.

  // Emit information about sections and GC, computing how many there are. Also
  // compute the maximum alignment value.
  std::map<std::string, unsigned> SectionMap;
  std::map<std::string, unsigned> GCMap;
  MaybeAlign MaxAlignment;
  unsigned MaxGlobalType = 0;
  const auto UpdateMaxAlignment = [&MaxAlignment](const MaybeAlign A) {
    if (A)
      MaxAlignment = !MaxAlignment ? *A : std::max(*MaxAlignment, *A);
  };
  for (const GlobalVariable &GV : M.globals()) {
    UpdateMaxAlignment(GV.getAlign());
    // Use getGlobalObjectValueTypeID to look up the enumerated type ID for
    // Global Variable types.
    MaxGlobalType = std::max(
        MaxGlobalType, getGlobalObjectValueTypeID(GV.getValueType(), &GV));
    if (GV.hasSection()) {
      // Give section names unique ID's.
      unsigned &Entry = SectionMap[std::string(GV.getSection())];
      if (!Entry) {
        writeStringRecord(Stream, bitc::MODULE_CODE_SECTIONNAME,
                          GV.getSection(), 0 /*TODO*/);
        Entry = SectionMap.size();
      }
    }
  }
  for (const Function &F : M) {
    UpdateMaxAlignment(F.getAlign());
    if (F.hasSection()) {
      // Give section names unique ID's.
      unsigned &Entry = SectionMap[std::string(F.getSection())];
      if (!Entry) {
        writeStringRecord(Stream, bitc::MODULE_CODE_SECTIONNAME, F.getSection(),
                          0 /*TODO*/);
        Entry = SectionMap.size();
      }
    }
    if (F.hasGC()) {
      // Same for GC names.
      unsigned &Entry = GCMap[F.getGC()];
      if (!Entry) {
        writeStringRecord(Stream, bitc::MODULE_CODE_GCNAME, F.getGC(),
                          0 /*TODO*/);
        Entry = GCMap.size();
      }
    }
  }

  // Emit abbrev for globals, now that we know # sections and max alignment.
  unsigned SimpleGVarAbbrev = 0;
  if (!M.global_empty()) {
    // Add an abbrev for common globals with no visibility or thread
    // localness.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::MODULE_CODE_GLOBALVAR));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed,
                              Log2_32_Ceil(MaxGlobalType + 1)));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6));   // AddrSpace << 2
                                                           //| explicitType << 1
                                                           //| constant
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6));   // Initializer.
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 5)); // Linkage.
    if (!MaxAlignment)                                     // Alignment.
      Abbv->Add(BitCodeAbbrevOp(0));
    else {
      unsigned MaxEncAlignment = getEncodedAlign(MaxAlignment);
      Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed,
                                Log2_32_Ceil(MaxEncAlignment + 1)));
    }
    if (SectionMap.empty()) // Section.
      Abbv->Add(BitCodeAbbrevOp(0));
    else
      Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed,
                                Log2_32_Ceil(SectionMap.size() + 1)));
    // Don't bother emitting vis + thread local.
    SimpleGVarAbbrev = Stream.EmitAbbrev(std::move(Abbv));
  }

  SmallVector<unsigned, 64> Vals;
  // Emit the module's source file name, as LLVM 15 does (P7): without it
  // a reader takes the ModuleID, the file's path.
  {
    StringRef Name = M.getSourceFileName();
    bool Char6 =
        all_of(Name, [](char C) { return BitCodeAbbrevOp::isChar6(C); });
    bool Fixed7 = none_of(Name, [](char C) { return (unsigned char)C & 128; });
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::MODULE_CODE_SOURCE_FILENAME));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(Char6    ? BitCodeAbbrevOp(BitCodeAbbrevOp::Char6)
              : Fixed7 ? BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 7)
                       : BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 8));
    unsigned FilenameAbbrev = Stream.EmitAbbrev(std::move(Abbv));
    for (char C : Name)
      Vals.push_back((unsigned char)C);
    Stream.EmitRecord(bitc::MODULE_CODE_SOURCE_FILENAME, Vals, FilenameAbbrev);
    Vals.clear();
  }

  // Emit the global variable information.
  for (const GlobalVariable &GV : M.globals()) {
    unsigned AbbrevToUse = 0;

    // GLOBALVAR: [type, isconst, initid,
    //             linkage, alignment, section, visibility, threadlocal,
    //             unnamed_addr, externally_initialized, dllstorageclass,
    //             comdat]
    Vals.push_back(getGlobalObjectValueTypeID(GV.getValueType(), &GV));
    Vals.push_back(
        GV.getType()->getAddressSpace() << 2 | 2 |
        (GV.isConstant() ? 1 : 0)); // HLSL Change - bitwise | was used with
                                    // unsigned int and bool
    Vals.push_back(GV.isDeclaration()
                       ? 0
                       : (VE.getValueID(
                              Types.typedOperand(GV.getOperandUse(0))) +
                          1));
    Vals.push_back(getEncodedLinkage(GV));
    Vals.push_back(getEncodedAlign(GV.getAlign()));
    Vals.push_back(GV.hasSection() ? SectionMap[std::string(GV.getSection())]
                                   : 0);
    if (GV.isThreadLocal() ||
        GV.getVisibility() != GlobalValue::DefaultVisibility ||
        GV.getUnnamedAddr() != GlobalValue::UnnamedAddr::None ||
        GV.isExternallyInitialized() ||
        GV.getDLLStorageClass() != GlobalValue::DefaultStorageClass ||
        GV.hasComdat()) {
      Vals.push_back(getEncodedVisibility(GV));
      Vals.push_back(getEncodedThreadLocalMode(GV));
      Vals.push_back(getEncodedUnnamedAddr(GV));
      Vals.push_back(GV.isExternallyInitialized());
      Vals.push_back(getEncodedDLLStorageClass(GV));
      Vals.push_back(GV.hasComdat() ? VE.getComdatID(GV.getComdat()) : 0);
    } else {
      AbbrevToUse = SimpleGVarAbbrev;
    }

    Stream.EmitRecord(bitc::MODULE_CODE_GLOBALVAR, Vals, AbbrevToUse);
    Vals.clear();
  }

  // Emit the function proto information.
  for (const Function &F : M) {
    // FUNCTION:  [type, callingconv, isproto, linkage, paramattrs, alignment,
    //             section, visibility, gc, unnamed_addr, prologuedata,
    //             dllstorageclass, comdat, prefixdata, personalityfn]
    Vals.push_back(getGlobalObjectValueTypeID(F.getFunctionType(), &F));
    Vals.push_back(F.getCallingConv());
    Vals.push_back(F.isDeclaration());
    Vals.push_back(getEncodedLinkage(F));
    Vals.push_back(VE.getAttributeListID(F.getAttributes()));
    Vals.push_back(getEncodedAlign(F.getAlign()));
    Vals.push_back(F.hasSection() ? SectionMap[std::string(F.getSection())]
                                  : 0);
    Vals.push_back(getEncodedVisibility(F));
    Vals.push_back(F.hasGC() ? GCMap[F.getGC()] : 0);
    Vals.push_back(getEncodedUnnamedAddr(F));
    Vals.push_back(
        F.hasPrologueData() ? (VE.getValueID(F.getPrologueData()) + 1) : 0);
    Vals.push_back(getEncodedDLLStorageClass(F));
    Vals.push_back(F.hasComdat() ? VE.getComdatID(F.getComdat()) : 0);
    Vals.push_back(F.hasPrefixData() ? (VE.getValueID(F.getPrefixData()) + 1)
                                     : 0);
    Vals.push_back(
        F.hasPersonalityFn() ? (VE.getValueID(F.getPersonalityFn()) + 1) : 0);

    unsigned AbbrevToUse = 0;
    Stream.EmitRecord(bitc::MODULE_CODE_FUNCTION, Vals, AbbrevToUse);
    Vals.clear();
  }

  // Emit the alias information.
  for (const GlobalAlias &A : M.aliases()) {
    // ALIAS: [alias type, aliasee val#, linkage, visibility]
    Vals.push_back(getTypeID(A.getValueType(), &A));
    Vals.push_back(VE.getValueID(Types.typedOperand(A.getOperandUse(0))));
    Vals.push_back(getEncodedLinkage(A));
    Vals.push_back(getEncodedVisibility(A));
    Vals.push_back(getEncodedDLLStorageClass(A));
    Vals.push_back(getEncodedThreadLocalMode(A));
    Vals.push_back(getEncodedUnnamedAddr(A));
    unsigned AbbrevToUse = 0;
    Stream.EmitRecord(bitc::MODULE_CODE_ALIAS_OLD, Vals, AbbrevToUse);
    Vals.clear();
  }
}

void AIRBitcodeWriter::writeValueAsMetadata(
    const ValueAsMetadata *MD, SmallVectorImpl<uint64_t> &Record) {
  // Mimic an MDNode with a value as one operand.
  Value *V = MD->getValue();
  Record.push_back(getTypeID(V->getType(), V));
  Record.push_back(VE.getValueID(V));
  Stream.EmitRecord(bitc::METADATA_VALUE, Record, 0);
  Record.clear();
}

void AIRBitcodeWriter::writeMDTuple(const MDTuple *N,
                                     SmallVectorImpl<uint64_t> &Record,
                                     unsigned Abbrev) {
  for (unsigned i = 0, e = N->getNumOperands(); i != e; ++i) {
    Metadata *MD = N->getOperand(i);
    assert(!(MD && isa<LocalAsMetadata>(MD)) &&
           "Unexpected function-local metadata");
    Record.push_back(VE.getMetadataOrNullID(MD));
  }
  Stream.EmitRecord(N->isDistinct() ? bitc::METADATA_DISTINCT_NODE
                                    : bitc::METADATA_NODE,
                    Record, Abbrev);
  Record.clear();
}

void AIRBitcodeWriter::writeMetadataRecords(ArrayRef<const Metadata *> MDs,
                                            SmallVectorImpl<uint64_t> &Record) {
  for (const Metadata *MD : MDs) {
    if (const MDNode *N = dyn_cast<MDNode>(MD)) {
      // AIR metadata is tuples; debug info was refused before the write.
      if (const auto *T = dyn_cast<MDTuple>(N))
        writeMDTuple(T, Record, 0);
      else
        fail("metadata: a node that is not a tuple");
      continue;
    }
    writeValueAsMetadata(cast<ValueAsMetadata>(MD), Record);
  }
}

unsigned AIRBitcodeWriter::createMetadataStringsAbbrev() {
  auto Abbv = std::make_shared<BitCodeAbbrev>();
  Abbv->Add(BitCodeAbbrevOp(bitc::METADATA_STRING_OLD));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 8));
  return Stream.EmitAbbrev(std::move(Abbv));
}

void AIRBitcodeWriter::writeMetadataStrings(
    ArrayRef<const Metadata *> Strings, SmallVectorImpl<uint64_t> &Record) {
  if (Strings.empty())
    return;

  unsigned MDSAbbrev = createMetadataStringsAbbrev();

  for (const Metadata *MD : Strings) {
    const MDString *MDS = cast<MDString>(MD);
    // Code: [strchar x N]
    Record.append(MDS->bytes_begin(), MDS->bytes_end());

    // Emit the finished record.
    Stream.EmitRecord(bitc::METADATA_STRING_OLD, Record, MDSAbbrev);
    Record.clear();
  }
}

void AIRBitcodeWriter::writeModuleMetadata() {
  if (!VE.hasMDs() && M.named_metadata_empty())
    return;

  Stream.EnterSubblock(bitc::METADATA_BLOCK_ID, 5);

  unsigned NameAbbrev = 0;
  if (!M.named_metadata_empty()) {
    // Abbrev for METADATA_NAME.
    std::shared_ptr<BitCodeAbbrev> Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::METADATA_NAME));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 8));
    NameAbbrev = Stream.EmitAbbrev(std::move(Abbv));
  }

  SmallVector<uint64_t, 64> Record;
  writeMetadataStrings(VE.getMDStrings(), Record);

  writeMetadataRecords(VE.getNonMDStrings(), Record);

  // Write named metadata.
  for (const NamedMDNode &NMD : M.named_metadata()) {
    // Write name.
    StringRef Str = NMD.getName();
    Record.append(Str.bytes_begin(), Str.bytes_end());
    Stream.EmitRecord(bitc::METADATA_NAME, Record, NameAbbrev);
    Record.clear();

    // Write named metadata operands.
    for (const MDNode *N : NMD.operands())
      Record.push_back(VE.getMetadataID(N));
    Stream.EmitRecord(bitc::METADATA_NAMED_NODE, Record, 0);
    Record.clear();
  }

  Stream.ExitBlock();
}

void AIRBitcodeWriter::writeFunctionMetadata(const Function &F) {
  if (!VE.hasMDs())
    return;

  Stream.EnterSubblock(bitc::METADATA_BLOCK_ID, 4);
  SmallVector<uint64_t, 64> Record;
  writeMetadataStrings(VE.getMDStrings(), Record);
  writeMetadataRecords(VE.getNonMDStrings(), Record);
  Stream.ExitBlock();
}

void AIRBitcodeWriter::writeFunctionMetadataAttachment(const Function &F) {
  Stream.EnterSubblock(bitc::METADATA_ATTACHMENT_ID, 3);

  SmallVector<uint64_t, 64> Record;

  // Write metadata attachments
  // METADATA_ATTACHMENT - [m x [value, [n x [id, mdnode]]]
  SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
  F.getAllMetadata(MDs);
  if (!MDs.empty()) {
    for (const auto &I : MDs) {
      if (I.first == LLVMContext::MD_dbg)
        continue;
      Record.push_back(I.first);
      Record.push_back(VE.getMetadataID(I.second));
    }
  }
  if (!Record.empty()) {
    Stream.EmitRecord(bitc::METADATA_ATTACHMENT, Record, 0);
    Record.clear();
  }

  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB) {
      MDs.clear();
      I.getAllMetadataOtherThanDebugLoc(MDs);

      // If no metadata, ignore instruction.
      if (MDs.empty())
        continue;

      Record.push_back(VE.getInstructionID(&I));

      for (unsigned i = 0, e = MDs.size(); i != e; ++i) {
        Record.push_back(MDs[i].first);
        Record.push_back(VE.getMetadataID(MDs[i].second));
      }
      Stream.EmitRecord(bitc::METADATA_ATTACHMENT, Record, 0);
      Record.clear();
    }

  Stream.ExitBlock();
}

void AIRBitcodeWriter::writeModuleMetadataKinds() {
  SmallVector<uint64_t, 64> Record;

  // Write metadata kinds
  // METADATA_KIND - [n x [id, name]]
  SmallVector<StringRef, 8> Names;
  M.getMDKindNames(Names);

  if (Names.empty())
    return;

  Stream.EnterSubblock(bitc::METADATA_BLOCK_ID, 3);

  for (unsigned MDKindID = 0, e = Names.size(); MDKindID != e; ++MDKindID) {
    Record.push_back(MDKindID);
    StringRef KName = Names[MDKindID];
    Record.append(KName.begin(), KName.end());

    Stream.EmitRecord(bitc::METADATA_KIND, Record, 0);
    Record.clear();
  }

  Stream.ExitBlock();
}

void AIRBitcodeWriter::writeConstants(unsigned FirstVal, unsigned LastVal,
                                       bool isGlobal) {
  if (FirstVal == LastVal)
    return;

  Stream.EnterSubblock(bitc::CONSTANTS_BLOCK_ID, 4);

  unsigned AggregateAbbrev = 0;
  unsigned String8Abbrev = 0;
  unsigned CString7Abbrev = 0;
  unsigned CString6Abbrev = 0;
  // If this is a constant pool for the module, emit module-specific abbrevs.
  if (isGlobal) {
    // Abbrev for CST_CODE_AGGREGATE.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::CST_CODE_AGGREGATE));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(
        BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, Log2_32_Ceil(LastVal + 1)));
    AggregateAbbrev = Stream.EmitAbbrev(std::move(Abbv));

    // Abbrev for CST_CODE_STRING.
    Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::CST_CODE_STRING));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 8));
    String8Abbrev = Stream.EmitAbbrev(std::move(Abbv));
    // Abbrev for CST_CODE_CSTRING.
    Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::CST_CODE_CSTRING));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 7));
    CString7Abbrev = Stream.EmitAbbrev(std::move(Abbv));
    // Abbrev for CST_CODE_CSTRING.
    Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::CST_CODE_CSTRING));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Char6));
    CString6Abbrev = Stream.EmitAbbrev(std::move(Abbv));
  }

  SmallVector<uint64_t, 64> Record;

  const ValueEnumerator::ValueList &Vals = VE.getValues();
  Type *LastTy = nullptr;
  for (unsigned i = FirstVal; i != LastVal; ++i) {
    const Value *V = Vals[i].first;
    // If we need to switch types, do so now: typed types, so constants of one
    // opaque type at several typed types each get their own SETTYPE.
    if (Types.typedTypeOf(V) != LastTy) {
      LastTy = Types.typedTypeOf(V);
      Record.push_back(VE.getTypeID(LastTy));
      Stream.EmitRecord(bitc::CST_CODE_SETTYPE, Record,
                        CONSTANTS_SETTYPE_ABBREV);
      Record.clear();
    }

    // A synthetic typed constant (P6): a bitcast, or its constant at its
    // typed type (the SETTYPE above).
    if (const AIRPointerTypes::Synthetic *S = Types.synthetic(V)) {
      unsigned Code = 0, AbbrevToUse = 0;
      auto *CE = dyn_cast<ConstantExpr>(S->C);
      if (S->Cast || (CE && CE->isCast())) {
        Code = bitc::CST_CODE_CE_CAST;
        Record.push_back(S->Cast ? unsigned(bitc::CAST_BITCAST)
                                 : getEncodedCastOpcode(CE->getOpcode()));
        Record.push_back(VE.getTypeID(Types.typedTypeOf(S->Ops[0])));
        Record.push_back(VE.getValueID(S->Ops[0]));
        AbbrevToUse = CONSTANTS_CE_CAST_Abbrev;
      } else if (isa<PoisonValue>(S->C)) {
        Code = bitc::CST_CODE_POISON;
      } else if (isa<UndefValue>(S->C)) {
        Code = bitc::CST_CODE_UNDEF;
      } else if (S->C->isNullValue()) {
        Code = bitc::CST_CODE_NULL;
      } else if (isa<ConstantAggregate>(S->C)) {
        Code = bitc::CST_CODE_AGGREGATE;
        for (const Value *Op : S->Ops)
          Record.push_back(VE.getValueID(Op));
        AbbrevToUse = AggregateAbbrev;
      } else {
        fail("constant: a typed instance with no LLVM 15 encoding");
      }
      Stream.EmitRecord(Code, Record, AbbrevToUse);
      Record.clear();
      continue;
    }

    if (const InlineAsm *IA = dyn_cast<InlineAsm>(V)) {
      Record.push_back(unsigned(IA->hasSideEffects()) |
                       unsigned(IA->isAlignStack()) << 1 |
                       unsigned(IA->getDialect() & 1) << 2);

      // Add the asm string.
      StringRef AsmStr = IA->getAsmString();
      Record.push_back(AsmStr.size());
      Record.append(AsmStr.begin(), AsmStr.end());

      // Add the constraint string.
      StringRef ConstraintStr = IA->getConstraintString();
      Record.push_back(ConstraintStr.size());
      Record.append(ConstraintStr.begin(), ConstraintStr.end());
      Stream.EmitRecord(bitc::CST_CODE_INLINEASM, Record);
      Record.clear();
      continue;
    }
    const Constant *C = cast<Constant>(V);
    auto Op = [&](unsigned N) {
      return Types.typedOperand(C->getOperandUse(N));
    };
    unsigned Code = -1U;
    unsigned AbbrevToUse = 0;
    if (C->isNullValue()) {
      Code = bitc::CST_CODE_NULL;
    } else if (isa<PoisonValue>(C)) {
      Code = bitc::CST_CODE_POISON;
    } else if (isa<UndefValue>(C)) {
      Code = bitc::CST_CODE_UNDEF;
    } else if (const ConstantInt *IV = dyn_cast<ConstantInt>(C)) {
      if (IV->getBitWidth() <= 64) {
        uint64_t V = IV->getSExtValue();
        emitSignedInt64(Record, V);
        Code = bitc::CST_CODE_INTEGER;
        AbbrevToUse = CONSTANTS_INTEGER_ABBREV;
      } else { // Wide integers, > 64 bits in size.
        // We have an arbitrary precision integer value to write whose
        // bit width is > 64. However, in canonical unsigned integer
        // format it is likely that the high bits are going to be zero.
        // So, we only write the number of active words.
        unsigned NWords = IV->getValue().getActiveWords();
        const uint64_t *RawWords = IV->getValue().getRawData();
        for (unsigned i = 0; i != NWords; ++i) {
          emitSignedInt64(Record, RawWords[i]);
        }
        Code = bitc::CST_CODE_WIDE_INTEGER;
      }
    } else if (const ConstantByte *BV = dyn_cast<ConstantByte>(C)) {
      // Note: we downgrade by converting to the equivalent integer - this logic
      // should match the `ConstantInt` case above.
      if (BV->getBitWidth() <= 64) {
        uint64_t V = BV->getSExtValue();
        emitSignedInt64(Record, V);
        Code = bitc::CST_CODE_INTEGER;
        AbbrevToUse = CONSTANTS_INTEGER_ABBREV;
      } else { // Wide bytes, > 64 bits in size.
        unsigned NWords = BV->getValue().getActiveWords();
        const uint64_t *RawWords = BV->getValue().getRawData();
        for (unsigned i = 0; i != NWords; ++i) {
          emitSignedInt64(Record, RawWords[i]);
        }
        Code = bitc::CST_CODE_WIDE_INTEGER;
      }
    } else if (const ConstantFP *CFP = dyn_cast<ConstantFP>(C)) {
      Code = bitc::CST_CODE_FLOAT;
      Type *Ty = CFP->getType()->getScalarType();
      if (Ty->isHalfTy() || Ty->isFloatTy() || Ty->isDoubleTy()) {
        Record.push_back(CFP->getValueAPF().bitcastToAPInt().getZExtValue());
      } else if (Ty->isX86_FP80Ty()) {
        // api needed to prevent premature destruction
        // bits are not in the same order as a normal i80 APInt, compensate.
        APInt api = CFP->getValueAPF().bitcastToAPInt();
        const uint64_t *p = api.getRawData();
        Record.push_back((p[1] << 48) | (p[0] >> 16));
        Record.push_back(p[0] & 0xffffLL);
      } else if (Ty->isFP128Ty() || Ty->isPPC_FP128Ty()) {
        APInt api = CFP->getValueAPF().bitcastToAPInt();
        const uint64_t *p = api.getRawData();
        Record.push_back(p[0]);
        Record.push_back(p[1]);
      } else {
        fail("constant: an FP type with no LLVM 15 encoding, " +
             typeName(Ty));
      }
    } else if (isa<ConstantDataSequential>(C) &&
               cast<ConstantDataSequential>(C)->isString()) {
      const ConstantDataSequential *Str = cast<ConstantDataSequential>(C);
      // Emit constant strings specially.
      unsigned NumElts = Str->getNumElements();
      // If this is a null-terminated string, use the denser CSTRING encoding.
      if (Str->isCString()) {
        Code = bitc::CST_CODE_CSTRING;
        --NumElts; // Don't encode the null, which isn't allowed by char6.
      } else {
        Code = bitc::CST_CODE_STRING;
        AbbrevToUse = String8Abbrev;
      }
      bool isCStr7 = Code == bitc::CST_CODE_CSTRING;
      bool isCStrChar6 = Code == bitc::CST_CODE_CSTRING;
      for (unsigned i = 0; i != NumElts; ++i) {
        unsigned char V = Str->getElementAsInteger(i);
        Record.push_back(V);
        isCStr7 &= (V & 128) == 0;
        if (isCStrChar6)
          isCStrChar6 = BitCodeAbbrevOp::isChar6(V);
      }

      if (isCStrChar6)
        AbbrevToUse = CString6Abbrev;
      else if (isCStr7)
        AbbrevToUse = CString7Abbrev;
    } else if (const ConstantDataSequential *CDS =
                   dyn_cast<ConstantDataSequential>(C)) {
      Code = bitc::CST_CODE_DATA;
      Type *EltTy = CDS->getElementType();
      // As LLVM 15 writes it: FP elements (half included) by their bits.
      if (isa<IntegerType>(EltTy) || isa<ByteType>(EltTy)) {
        for (unsigned i = 0, e = CDS->getNumElements(); i != e; ++i)
          Record.push_back(CDS->getElementAsInteger(i));
      } else {
        for (unsigned i = 0, e = CDS->getNumElements(); i != e; ++i)
          Record.push_back(
              CDS->getElementAsAPFloat(i).bitcastToAPInt().getLimitedValue());
      }
    } else if (isa<ConstantArray>(C) || isa<ConstantStruct>(C) ||
               isa<ConstantVector>(C)) {
      Code = bitc::CST_CODE_AGGREGATE;
      for (unsigned N = 0, E = C->getNumOperands(); N != E; ++N)
        Record.push_back(VE.getValueID(Op(N)));
      AbbrevToUse = AggregateAbbrev;
    } else if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(C)) {
      switch (CE->getOpcode()) {
      default:
        if (Instruction::isCast(CE->getOpcode())) {
          Code = bitc::CST_CODE_CE_CAST;
          if (getEncodedCastOpcode(CE->getOpcode()) == ~0U)
            fail(std::string("constant ") + CE->getOpcodeName() +
                 ": no LLVM 15 encoding");
          Record.push_back(getEncodedCastOpcode(CE->getOpcode()));
          Record.push_back(getTypeID(Op(0)->getType(), Op(0)));
          Record.push_back(VE.getValueID(Op(0)));
          AbbrevToUse = CONSTANTS_CE_CAST_Abbrev;
        } else {
          if (!Instruction::isBinaryOp(CE->getOpcode()) ||
              getEncodedBinaryOpcode(CE->getOpcode()) == ~0U) {
            fail(std::string("constant ") + CE->getOpcodeName() +
                 ": no LLVM 15 encoding");
            Record.clear();
            continue;
          }
          Code = bitc::CST_CODE_CE_BINOP;
          Record.push_back(getEncodedBinaryOpcode(CE->getOpcode()));
          Record.push_back(VE.getValueID(Op(0)));
          Record.push_back(VE.getValueID(Op(1)));
          uint64_t Flags = getOptimizationFlags(CE);
          if (Flags != 0)
            Record.push_back(Flags);
        }
        break;
      case Instruction::GetElementPtr: {
        Code = bitc::CST_CODE_CE_GEP_OLD;
        const auto *GO = cast<GEPOperator>(C);
        if (GO->isInBounds())
          Code = bitc::CST_CODE_CE_INBOUNDS_GEP;
        // Its base must point to its typed source, as prepared (P6); a
        // constant no use prepared (one only metadata holds) may not.
        Type *Src = Types.sourceType(*GO);
        auto *Base = dyn_cast<TypedPointerType>(
            Types.typedTypeOf(Op(0))->getScalarType());
        if (!Base || Base->getElementType() != Src)
          fail("constant getelementptr: its base is not typed as its "
               "source " +
               typeName(Src));
        Record.push_back(VE.getTypeID(Src));
        for (unsigned i = 0, e = CE->getNumOperands(); i != e; ++i) {
          Record.push_back(getTypeID(Op(i)->getType(), Op(i)));
          Record.push_back(VE.getValueID(Op(i)));
        }
        break;
      }
      case Instruction::ExtractElement:
        Code = bitc::CST_CODE_CE_EXTRACTELT;
        Record.push_back(getTypeID(Op(0)->getType(), Op(0)));
        Record.push_back(VE.getValueID(Op(0)));
        Record.push_back(getTypeID(Op(1)->getType(), Op(1)));
        Record.push_back(VE.getValueID(Op(1)));
        break;
      case Instruction::InsertElement:
        Code = bitc::CST_CODE_CE_INSERTELT;
        Record.push_back(VE.getValueID(Op(0)));
        Record.push_back(VE.getValueID(Op(1)));
        Record.push_back(getTypeID(Op(2)->getType(), Op(2)));
        Record.push_back(VE.getValueID(Op(2)));
        break;
      case Instruction::ShuffleVector:
        // If the return type and argument types are the same, this is a
        // standard shufflevector instruction.  If the types are different,
        // then the shuffle is widening or truncating the input vectors, and
        // the argument type must also be encoded.
        if (C->getType() == C->getOperand(0)->getType()) {
          Code = bitc::CST_CODE_CE_SHUFFLEVEC;
        } else {
          Code = bitc::CST_CODE_CE_SHUFVEC_EX;
          Record.push_back(getTypeID(Op(0)->getType(), Op(0)));
        }
        Record.push_back(VE.getValueID(Op(0)));
        Record.push_back(VE.getValueID(Op(1)));
        Record.push_back(VE.getValueID(Op(2)));
        break;
      }
    } else if (const BlockAddress *BA = dyn_cast<BlockAddress>(C)) {
      Code = bitc::CST_CODE_BLOCKADDRESS;
      Record.push_back(
          getTypeID(BA->getFunction()->getType(), BA->getFunction()));
      Record.push_back(VE.getValueID(BA->getFunction()));
      Record.push_back(VE.getGlobalBasicBlockID(BA->getBasicBlock()));
    } else {
      std::string S;
      raw_string_ostream OS(S);
      C->printAsOperand(OS, /*PrintType=*/true, &M);
      fail("constant " + StringRef(S).take_front(80) +
           ": a kind with no LLVM 15 encoding");
      Record.clear();
      continue;
    }
    Stream.EmitRecord(Code, Record, AbbrevToUse);
    Record.clear();
  }

  Stream.ExitBlock();
}

void AIRBitcodeWriter::writeModuleConstants() {
  const ValueEnumerator::ValueList &Vals = VE.getValues();

  // Find the first constant to emit, which is the first non-globalvalue value.
  // We know globalvalues have been emitted by WriteModuleInfo.
  for (unsigned i = 0, e = Vals.size(); i != e; ++i) {
    if (!isa<GlobalValue>(Vals[i].first)) {
      writeConstants(i, Vals.size(), true);
      return;
    }
  }
}

/// pushValueAndType - The file has to encode both the value and type id for
/// many values, because we need to know what type to create for forward
/// references.  However, most operands are not forward references, so this type
/// field is not needed.
///
/// This function adds V's value ID to Vals.  If the value ID is higher than the
/// instruction ID, then it is a forward reference, and it also includes the
/// type ID.  The value ID that is written is encoded relative to the InstID.
bool AIRBitcodeWriter::pushValueAndType(const Value *V, unsigned InstID,
                                         SmallVectorImpl<unsigned> &Vals) {
  unsigned ValID = VE.getValueID(V);
  // Make encoding relative to the InstID.
  Vals.push_back(InstID - ValID);
  if (ValID >= InstID) {
    Vals.push_back(getTypeID(V->getType(), V));
    return true;
  }
  return false;
}

/// pushValue - Like pushValueAndType, but where the type of the value is
/// omitted (perhaps it was already encoded in an earlier operand).
void AIRBitcodeWriter::pushValue(const Value *V, unsigned InstID,
                                  SmallVectorImpl<unsigned> &Vals) {
  unsigned ValID = VE.getValueID(V);
  Vals.push_back(InstID - ValID);
}

void AIRBitcodeWriter::pushValueSigned(const Value *V, unsigned InstID,
                                        SmallVectorImpl<uint64_t> &Vals) {
  unsigned ValID = VE.getValueID(V);
  int64_t diff = ((int32_t)InstID - (int32_t)ValID);
  emitSignedInt64(Vals, diff);
}

/// WriteInstruction - Emit an instruction
void AIRBitcodeWriter::writeInstruction(const Instruction &I, unsigned InstID,
                                         SmallVectorImpl<unsigned> &Vals) {
  unsigned Code = 0;
  unsigned AbbrevToUse = 0;
  VE.setInstructionID(&I);
  // Operands as written: casts are in the module, typed constants stand in.
  auto Op = [&](unsigned N) {
    return Types.typedOperand(I.getOperandUse(N));
  };
  // P17: an opcode the records cannot hold is an error naming it, never a
  // record written from the wrong layout. lowerToLLVM15 refuses these first.
  auto Unencodable = [&] {
    fail(Twine(I.getOpcodeName()) + ": no LLVM 15 encoding (in @" +
         I.getFunction()->getName() + ")");
    Vals.clear();
  };
  switch (I.getOpcode()) {
  default:
    if (Instruction::isCast(I.getOpcode())) {
      if (getEncodedCastOpcode(I.getOpcode()) == ~0U)
        return Unencodable();
      Code = bitc::FUNC_CODE_INST_CAST;
      if (!pushValueAndType(Op(0), InstID, Vals))
        AbbrevToUse = (unsigned)FUNCTION_INST_CAST_ABBREV;
      Vals.push_back(getTypeID(I.getType(), &I));
      Vals.push_back(getEncodedCastOpcode(I.getOpcode()));
    } else {
      if (!isa<BinaryOperator>(I) ||
          getEncodedBinaryOpcode(I.getOpcode()) == ~0U)
        return Unencodable();
      Code = bitc::FUNC_CODE_INST_BINOP;
      if (!pushValueAndType(Op(0), InstID, Vals))
        AbbrevToUse = (unsigned)FUNCTION_INST_BINOP_ABBREV;
      pushValue(Op(1), InstID, Vals);
      Vals.push_back(getEncodedBinaryOpcode(I.getOpcode()));
      uint64_t Flags = getOptimizationFlags(&I);
      if (Flags != 0) {
        if (AbbrevToUse == (unsigned)FUNCTION_INST_BINOP_ABBREV)
          AbbrevToUse = (unsigned)FUNCTION_INST_BINOP_FLAGS_ABBREV;
        Vals.push_back(Flags);
      }
    }
    break;

  case Instruction::FNeg: {
    Code = bitc::FUNC_CODE_INST_UNOP;
    pushValueAndType(Op(0), InstID, Vals);
    Vals.push_back(getEncodedUnaryOpcode(I.getOpcode()));
    uint64_t Flags = getOptimizationFlags(&I);
    if (Flags != 0)
      Vals.push_back(Flags);
    break;
  }
  case Instruction::Freeze:
    Code = bitc::FUNC_CODE_INST_FREEZE;
    pushValueAndType(Op(0), InstID, Vals);
    break;

  case Instruction::GetElementPtr: {
    Code = bitc::FUNC_CODE_INST_GEP;
    AbbrevToUse = (unsigned)FUNCTION_INST_GEP_ABBREV;
    auto &GEPInst = cast<GetElementPtrInst>(I);
    Vals.push_back(GEPInst.isInBounds());
    Vals.push_back(VE.getTypeID(Types.sourceType(cast<GEPOperator>(I))));
    for (unsigned i = 0, e = I.getNumOperands(); i != e; ++i)
      pushValueAndType(Op(i), InstID, Vals);
    break;
  }
  case Instruction::ExtractValue: {
    Code = bitc::FUNC_CODE_INST_EXTRACTVAL;
    pushValueAndType(Op(0), InstID, Vals);
    const ExtractValueInst *EVI = cast<ExtractValueInst>(&I);
    Vals.append(EVI->idx_begin(), EVI->idx_end());
    break;
  }
  case Instruction::InsertValue: {
    Code = bitc::FUNC_CODE_INST_INSERTVAL;
    pushValueAndType(Op(0), InstID, Vals);
    pushValueAndType(Op(1), InstID, Vals);
    const InsertValueInst *IVI = cast<InsertValueInst>(&I);
    Vals.append(IVI->idx_begin(), IVI->idx_end());
    break;
  }
  case Instruction::Select: {
    Code = bitc::FUNC_CODE_INST_VSELECT;
    pushValueAndType(Op(1), InstID, Vals);
    pushValue(Op(2), InstID, Vals);
    pushValueAndType(Op(0), InstID, Vals);
    uint64_t Flags = getOptimizationFlags(&I);
    if (Flags != 0)
      Vals.push_back(Flags);
    break;
  }
  case Instruction::ExtractElement:
    Code = bitc::FUNC_CODE_INST_EXTRACTELT;
    pushValueAndType(Op(0), InstID, Vals);
    pushValueAndType(Op(1), InstID, Vals);
    break;
  case Instruction::InsertElement:
    Code = bitc::FUNC_CODE_INST_INSERTELT;
    pushValueAndType(Op(0), InstID, Vals);
    pushValue(Op(1), InstID, Vals);
    pushValueAndType(Op(2), InstID, Vals);
    break;
  case Instruction::ShuffleVector:
    Code = bitc::FUNC_CODE_INST_SHUFFLEVEC;
    pushValueAndType(Op(0), InstID, Vals);
    pushValue(Op(1), InstID, Vals);
    pushValue(cast<ShuffleVectorInst>(&I)->getShuffleMaskForBitcode(), InstID,
              Vals);
    break;
  case Instruction::ICmp:
  case Instruction::FCmp: {
    // compare returning Int1Ty or vector of Int1Ty
    Code = bitc::FUNC_CODE_INST_CMP2;
    pushValueAndType(Op(0), InstID, Vals);
    pushValue(Op(1), InstID, Vals);
    Vals.push_back(cast<CmpInst>(I).getPredicate());
    uint64_t Flags = getOptimizationFlags(&I);
    if (Flags != 0)
      Vals.push_back(Flags);
    break;
  }

  case Instruction::Ret: {
    Code = bitc::FUNC_CODE_INST_RET;
    unsigned NumOperands = I.getNumOperands();
    if (NumOperands == 0)
      AbbrevToUse = (unsigned)FUNCTION_INST_RET_VOID_ABBREV;
    else if (NumOperands == 1) {
      if (!pushValueAndType(Op(0), InstID, Vals))
        AbbrevToUse = (unsigned)FUNCTION_INST_RET_VAL_ABBREV;
    } else {
      for (unsigned i = 0, e = NumOperands; i != e; ++i)
        pushValueAndType(Op(i), InstID, Vals);
    }
  } break;
  case Instruction::UncondBr:
    Code = bitc::FUNC_CODE_INST_BR;
    Vals.push_back(VE.getValueID(cast<UncondBrInst>(I).getSuccessor()));
    break;
  case Instruction::CondBr: {
    Code = bitc::FUNC_CODE_INST_BR;
    const CondBrInst &II = cast<CondBrInst>(I);
    Vals.push_back(VE.getValueID(II.getSuccessor(0)));
    Vals.push_back(VE.getValueID(II.getSuccessor(1)));
    pushValue(II.getCondition(), InstID, Vals);
  } break;
  case Instruction::Switch: {
    Code = bitc::FUNC_CODE_INST_SWITCH;
    const SwitchInst &SI = cast<SwitchInst>(I);
    Vals.push_back(getTypeID(SI.getCondition()->getType()));
    pushValue(SI.getCondition(), InstID, Vals);
    Vals.push_back(VE.getValueID(SI.getDefaultDest()));
    for (auto Case : SI.cases()) {
      Vals.push_back(VE.getValueID(Case.getCaseValue()));
      Vals.push_back(VE.getValueID(Case.getCaseSuccessor()));
    }
  } break;
  case Instruction::IndirectBr:
    Code = bitc::FUNC_CODE_INST_INDIRECTBR;
    Vals.push_back(getTypeID(Op(0)->getType(), Op(0)));
    // Encode the address operand as relative, but not the basic blocks.
    pushValue(Op(0), InstID, Vals);
    for (unsigned i = 1, e = I.getNumOperands(); i != e; ++i)
      Vals.push_back(VE.getValueID(Op(i)));
    break;

  case Instruction::Invoke: {
    const InvokeInst *II = cast<InvokeInst>(&I);
    const Value *Callee = Types.typedOperand(II->getCalledOperandUse());
    FunctionType *FTy = II->getFunctionType();
    Code = bitc::FUNC_CODE_INST_INVOKE;

    Vals.push_back(VE.getAttributeListID(II->getAttributes()));
    Vals.push_back(II->getCallingConv() | 1 << 13);
    Vals.push_back(VE.getValueID(II->getNormalDest()));
    Vals.push_back(VE.getValueID(II->getUnwindDest()));
    Vals.push_back(VE.getTypeID(Types.callType(*II)));
    pushValueAndType(Callee, InstID, Vals);

    // Emit value #'s for the fixed parameters.
    for (unsigned i = 0, e = FTy->getNumParams(); i != e; ++i)
      pushValue(Op(i), InstID, Vals); // fixed param.

    // Emit type/value pairs for varargs params.
    if (FTy->isVarArg()) {
      for (unsigned i = FTy->getNumParams(), e = I.getNumOperands() - 3; i != e;
           ++i)
        pushValueAndType(Op(i), InstID, Vals); // vararg
    }
    break;
  }
  case Instruction::Resume:
    Code = bitc::FUNC_CODE_INST_RESUME;
    pushValueAndType(Op(0), InstID, Vals);
    break;
  case Instruction::Unreachable:
    Code = bitc::FUNC_CODE_INST_UNREACHABLE;
    AbbrevToUse = (unsigned)FUNCTION_INST_UNREACHABLE_ABBREV;
    break;

  case Instruction::PHI: {
    const PHINode &PN = cast<PHINode>(I);
    Code = bitc::FUNC_CODE_INST_PHI;
    // With the newer instruction encoding, forward references could give
    // negative valued IDs.  This is most common for PHIs, so we use
    // signed VBRs.
    SmallVector<uint64_t, 128> Vals64;
    Vals64.push_back(getTypeID(PN.getType(), &PN));
    for (unsigned i = 0, e = PN.getNumIncomingValues(); i != e; ++i) {
      pushValueSigned(Op(i), InstID, Vals64);
      Vals64.push_back(VE.getValueID(PN.getIncomingBlock(i)));
    }
    uint64_t Flags = getOptimizationFlags(&I);
    if (Flags != 0)
      Vals64.push_back(Flags);
    // Emit a Vals64 vector and exit.
    Stream.EmitRecord(Code, Vals64, AbbrevToUse);
    Vals64.clear();
    return;
  }

  case Instruction::LandingPad: {
    const LandingPadInst &LP = cast<LandingPadInst>(I);
    Code = bitc::FUNC_CODE_INST_LANDINGPAD;
    Vals.push_back(getTypeID(LP.getType(), &LP));
    Vals.push_back(LP.isCleanup());
    Vals.push_back(LP.getNumClauses());
    for (unsigned I = 0, E = LP.getNumClauses(); I != E; ++I) {
      if (LP.isCatch(I))
        Vals.push_back(LandingPadInst::Catch);
      else
        Vals.push_back(LandingPadInst::Filter);
      pushValueAndType(Op(I), InstID, Vals);
    }
    break;
  }

  case Instruction::Alloca: {
    Code = bitc::FUNC_CODE_INST_ALLOCA;
    const AllocaInst &AI = cast<AllocaInst>(I);
    Vals.push_back(VE.getTypeID(Types.allocatedType(AI)));
    Vals.push_back(getTypeID(Op(0)->getType()));
    Vals.push_back(VE.getValueID(Op(0))); // size.
    // As LLVM 15 packs it: alignments past 2^30 too, and swifterror.
    using APV = AllocaPackedValues;
    unsigned Record = 0;
    unsigned EncodedAlign = getEncodedAlign(AI.getAlign());
    Bitfield::set<APV::AlignLower>(
        Record, EncodedAlign & ((1 << APV::AlignLower::Bits) - 1));
    Bitfield::set<APV::AlignUpper>(Record,
                                   EncodedAlign >> APV::AlignLower::Bits);
    Bitfield::set<APV::UsedWithInAlloca>(Record, AI.isUsedWithInAlloca());
    Bitfield::set<APV::ExplicitType>(Record, true);
    Bitfield::set<APV::SwiftError>(Record, AI.isSwiftError());
    Vals.push_back(Record);
    if (AI.getAddressSpace() != M.getDataLayout().getAllocaAddrSpace())
      Vals.push_back(AI.getAddressSpace());
    break;
  }

  case Instruction::Load:
    if (cast<LoadInst>(I).isAtomic()) {
      Code = bitc::FUNC_CODE_INST_LOADATOMIC;
      pushValueAndType(Op(0), InstID, Vals);
    } else {
      Code = bitc::FUNC_CODE_INST_LOAD;
      if (!pushValueAndType(Op(0), InstID, Vals)) // ptr
        AbbrevToUse = (unsigned)FUNCTION_INST_LOAD_ABBREV;
    }
    // A pointer load's type is the loaded value's typed pointer (02e8d34).
    Vals.push_back(getTypeID(I.getType(), &I));
    Vals.push_back(Log2(cast<LoadInst>(I).getAlign()) + 1);
    Vals.push_back(cast<LoadInst>(I).isVolatile());
    if (cast<LoadInst>(I).isAtomic()) {
      Vals.push_back(getEncodedOrdering(cast<LoadInst>(I).getOrdering()));
      Vals.push_back(getEncodedSyncScopeID(cast<LoadInst>(I).getSyncScopeID()));
    }
    break;
  case Instruction::Store:
    if (cast<StoreInst>(I).isAtomic())
      Code = bitc::FUNC_CODE_INST_STOREATOMIC;
    else
      Code = bitc::FUNC_CODE_INST_STORE;
    pushValueAndType(Op(1), InstID, Vals); // ptrty + ptr
    pushValueAndType(Op(0), InstID, Vals); // valty + val
    Vals.push_back(Log2(cast<StoreInst>(I).getAlign()) + 1);
    Vals.push_back(cast<StoreInst>(I).isVolatile());
    if (cast<StoreInst>(I).isAtomic()) {
      Vals.push_back(getEncodedOrdering(cast<StoreInst>(I).getOrdering()));
      Vals.push_back(
          getEncodedSyncScopeID(cast<StoreInst>(I).getSyncScopeID()));
    }
    break;
  case Instruction::AtomicCmpXchg:
    Code = bitc::FUNC_CODE_INST_CMPXCHG;
    pushValueAndType(Op(0), InstID, Vals); // ptrty + ptr
    pushValueAndType(Op(1), InstID, Vals); // cmp.
    pushValue(Op(2), InstID, Vals);        // newval.
    Vals.push_back(cast<AtomicCmpXchgInst>(I).isVolatile());
    Vals.push_back(
        getEncodedOrdering(cast<AtomicCmpXchgInst>(I).getSuccessOrdering()));
    Vals.push_back(
        getEncodedSyncScopeID(cast<AtomicCmpXchgInst>(I).getSyncScopeID()));
    Vals.push_back(
        getEncodedOrdering(cast<AtomicCmpXchgInst>(I).getFailureOrdering()));
    Vals.push_back(cast<AtomicCmpXchgInst>(I).isWeak());
    Vals.push_back(getEncodedAlign(cast<AtomicCmpXchgInst>(I).getAlign()));
    break;
  case Instruction::AtomicRMW:
    // Code 59 with its value's type, as LLVM 15 writes it: without the type
    // a forward-referenced value cannot be read.
    if (getEncodedRMWOperation(cast<AtomicRMWInst>(I).getOperation()) == ~0U)
      return Unencodable();
    Code = bitc::FUNC_CODE_INST_ATOMICRMW;
    pushValueAndType(Op(0), InstID, Vals); // ptrty + ptr
    pushValueAndType(Op(1), InstID, Vals); // valty + val
    Vals.push_back(
        getEncodedRMWOperation(cast<AtomicRMWInst>(I).getOperation()));
    Vals.push_back(cast<AtomicRMWInst>(I).isVolatile());
    Vals.push_back(getEncodedOrdering(cast<AtomicRMWInst>(I).getOrdering()));
    Vals.push_back(
        getEncodedSyncScopeID(cast<AtomicRMWInst>(I).getSyncScopeID()));
    Vals.push_back(getEncodedAlign(cast<AtomicRMWInst>(I).getAlign()));
    break;
  case Instruction::Fence:
    Code = bitc::FUNC_CODE_INST_FENCE;
    Vals.push_back(getEncodedOrdering(cast<FenceInst>(I).getOrdering()));
    Vals.push_back(getEncodedSyncScopeID(cast<FenceInst>(I).getSyncScopeID()));
    break;
  case Instruction::Call: {
    const CallInst &CI = cast<CallInst>(I);
    FunctionType *FTy = CI.getFunctionType();

    Code = bitc::FUNC_CODE_INST_CALL;

    Vals.push_back(VE.getAttributeListID(CI.getAttributes()));
    // LLVM 15's markers: notail and the fast-math flags (before the type).
    unsigned Flags = getOptimizationFlags(&I);
    Vals.push_back(CI.getCallingConv() << bitc::CALL_CCONV |
                   unsigned(CI.isTailCall()) << bitc::CALL_TAIL |
                   unsigned(CI.isMustTailCall()) << bitc::CALL_MUSTTAIL |
                   1 << bitc::CALL_EXPLICIT_TYPE |
                   unsigned(CI.isNoTailCall()) << bitc::CALL_NOTAIL |
                   unsigned(Flags != 0) << bitc::CALL_FMF);
    if (Flags != 0)
      Vals.push_back(Flags);
    Vals.push_back(VE.getTypeID(Types.callType(CI)));
    pushValueAndType(Types.typedOperand(CI.getCalledOperandUse()), InstID,
                     Vals); // Callee

    // Emit value #'s for the fixed parameters.
    for (unsigned i = 0, e = FTy->getNumParams(); i != e; ++i) {
      // Check for labels (can happen with asm labels).
      if (FTy->getParamType(i)->isLabelTy())
        Vals.push_back(VE.getValueID(Op(i)));
      else
        pushValue(Op(i), InstID, Vals); // fixed param.
    }

    // Emit type/value pairs for varargs params.
    if (FTy->isVarArg()) {
      for (unsigned i = FTy->getNumParams(), e = CI.arg_size(); i != e; ++i)
        pushValueAndType(Op(i), InstID, Vals); // varargs
    }
    break;
  }
  case Instruction::VAArg:
    Code = bitc::FUNC_CODE_INST_VAARG;
    Vals.push_back(getTypeID(Op(0)->getType(),
                             Op(0)));    // valistty
    pushValue(Op(0), InstID, Vals);      // valist.
    Vals.push_back(getTypeID(I.getType(), &I));    // restype.
    break;
  }

  Stream.EmitRecord(Code, Vals, AbbrevToUse);
  Vals.clear();
}

// Emit names for globals/functions etc.
void AIRBitcodeWriter::writeFunctionLevelValueSymbolTable(
    const ValueSymbolTable &VST) {
  if (VST.empty())
    return;
  Stream.EnterSubblock(bitc::VALUE_SYMTAB_BLOCK_ID, 4);

  SmallVector<unsigned, 64> NameVals;

  // HLSL Change
  // Read the named values from a sorted list instead of the original list
  // to ensure the binary is the same no matter what values ever existed.
  SmallVector<const ValueName *, 16> SortedTable;

  for (auto &VI : VST)
    SortedTable.push_back(VI.second->getValueName());
  // The keys are unique, so there shouldn't be stability issues.
  llvm::sort(SortedTable, [](const ValueName *A, const ValueName *B) {
    return A->first() < B->first();
  });

  for (const ValueName *SI : SortedTable) {
    auto &Name = *SI;

    // Figure out the encoding to use for the name.
    bool is7Bit = true;
    bool isChar6 = true;
    for (const char *C = Name.getKeyData(), *E = C + Name.getKeyLength();
         C != E; ++C) {
      if (isChar6)
        isChar6 = BitCodeAbbrevOp::isChar6(*C);
      if ((unsigned char)*C & 128) {
        is7Bit = false;
        break; // don't bother scanning the rest.
      }
    }

    unsigned AbbrevToUse = VST_ENTRY_8_ABBREV;

    // VST_ENTRY:   [valueid, namechar x N]
    // VST_BBENTRY: [bbid, namechar x N]
    unsigned Code;
    if (isa<BasicBlock>(SI->getValue())) {
      Code = bitc::VST_CODE_BBENTRY;
      if (isChar6)
        AbbrevToUse = VST_BBENTRY_6_ABBREV;
    } else {
      Code = bitc::VST_CODE_ENTRY;
      if (isChar6)
        AbbrevToUse = VST_ENTRY_6_ABBREV;
      else if (is7Bit)
        AbbrevToUse = VST_ENTRY_7_ABBREV;
    }

    NameVals.push_back(VE.getValueID(SI->getValue()));
    for (const char *P = Name.getKeyData(),
                    *E = Name.getKeyData() + Name.getKeyLength();
         P != E; ++P)
      NameVals.push_back((unsigned char)*P);

    // Emit the finished record.
    Stream.EmitRecord(Code, NameVals, AbbrevToUse);
    NameVals.clear();
  }
  Stream.ExitBlock();
}

/// Emit a function body to the module stream.
void AIRBitcodeWriter::writeFunction(const Function &F) {
  Stream.EnterSubblock(bitc::FUNCTION_BLOCK_ID, 4);
  VE.incorporateFunction(F);

  SmallVector<unsigned, 64> Vals;

  // Emit the number of basic blocks, so the reader can create them ahead of
  // time.
  Vals.push_back(VE.getBasicBlocks().size());
  Stream.EmitRecord(bitc::FUNC_CODE_DECLAREBLOCKS, Vals);
  Vals.clear();

  // If there are function-local constants, emit them now.
  unsigned CstStart, CstEnd;
  VE.getFunctionConstantRange(CstStart, CstEnd);
  writeConstants(CstStart, CstEnd, false);

  // If there is function-local metadata, emit it now.
  writeFunctionMetadata(F);

  // Keep a running idea of what the instruction ID is.
  unsigned InstID = CstEnd;

  bool NeedsMetadataAttachment = F.hasMetadata();

  // Finally, emit all the instructions, in order. (No debug locations: a
  // module with any was refused.)
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB) {
      writeInstruction(I, InstID, Vals);

      if (!I.getType()->isVoidTy())
        ++InstID;

      // If the instruction has metadata, write a metadata attachment later.
      NeedsMetadataAttachment |= I.hasMetadataOtherThanDebugLoc();
    }

  // Emit names for all the instructions etc.
  if (auto *Symtab = F.getValueSymbolTable())
    writeFunctionLevelValueSymbolTable(*Symtab);

  if (NeedsMetadataAttachment)
    writeFunctionMetadataAttachment(F);

  VE.purgeFunction();
  Stream.ExitBlock();
}

// Emit blockinfo, which defines the standard abbreviations etc.
void AIRBitcodeWriter::writeBlockInfo() {
  // We only want to emit block info records for blocks that have multiple
  // instances: CONSTANTS_BLOCK, FUNCTION_BLOCK and VALUE_SYMTAB_BLOCK.
  // Other blocks can define their abbrevs inline.
  Stream.EnterBlockInfoBlock();

  { // 8-bit fixed-width VST_ENTRY/VST_BBENTRY strings.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 3));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 8));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 8));
    if (Stream.EmitBlockInfoAbbrev(bitc::VALUE_SYMTAB_BLOCK_ID,
                                   std::move(Abbv)) != VST_ENTRY_8_ABBREV)
      assert(false && "Unexpected abbrev ordering!");
  }

  { // 7-bit fixed width VST_ENTRY strings.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::VST_CODE_ENTRY));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 8));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 7));
    if (Stream.EmitBlockInfoAbbrev(bitc::VALUE_SYMTAB_BLOCK_ID,
                                   std::move(Abbv)) != VST_ENTRY_7_ABBREV)
      assert(false && "Unexpected abbrev ordering!");
  }
  { // 6-bit char6 VST_ENTRY strings.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::VST_CODE_ENTRY));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 8));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Char6));
    if (Stream.EmitBlockInfoAbbrev(bitc::VALUE_SYMTAB_BLOCK_ID,
                                   std::move(Abbv)) != VST_ENTRY_6_ABBREV)
      assert(false && "Unexpected abbrev ordering!");
  }
  { // 6-bit char6 VST_BBENTRY strings.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::VST_CODE_BBENTRY));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 8));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Char6));
    if (Stream.EmitBlockInfoAbbrev(bitc::VALUE_SYMTAB_BLOCK_ID,
                                   std::move(Abbv)) != VST_BBENTRY_6_ABBREV)
      assert(false && "Unexpected abbrev ordering!");
  }

  { // SETTYPE abbrev for CONSTANTS_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::CST_CODE_SETTYPE));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed,
                              VE.computeBitsRequiredForTypeIndices()));
    if (Stream.EmitBlockInfoAbbrev(bitc::CONSTANTS_BLOCK_ID, std::move(Abbv)) !=
        CONSTANTS_SETTYPE_ABBREV)
      assert(false && "Unexpected abbrev ordering!");
  }

  { // INTEGER abbrev for CONSTANTS_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::CST_CODE_INTEGER));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 8));
    if (Stream.EmitBlockInfoAbbrev(bitc::CONSTANTS_BLOCK_ID, std::move(Abbv)) !=
        CONSTANTS_INTEGER_ABBREV)
      assert(false && "Unexpected abbrev ordering!");
  }

  { // CE_CAST abbrev for CONSTANTS_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::CST_CODE_CE_CAST));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 4)); // cast opc
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed,      // typeid
                              VE.computeBitsRequiredForTypeIndices()));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 8)); // value id

    if (Stream.EmitBlockInfoAbbrev(bitc::CONSTANTS_BLOCK_ID, std::move(Abbv)) !=
        CONSTANTS_CE_CAST_Abbrev)
      assert(false && "Unexpected abbrev ordering!");
  }
  { // NULL abbrev for CONSTANTS_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::CST_CODE_NULL));
    if (Stream.EmitBlockInfoAbbrev(bitc::CONSTANTS_BLOCK_ID, std::move(Abbv)) !=
        CONSTANTS_NULL_Abbrev)
      assert(false && "Unexpected abbrev ordering!");
  }

  // FIXME: This should only use space for first class types!

  { // INST_LOAD abbrev for FUNCTION_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::FUNC_CODE_INST_LOAD));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6)); // Ptr
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed,    // dest ty
                              VE.computeBitsRequiredForTypeIndices()));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 4));   // Align
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 1)); // volatile
    if (Stream.EmitBlockInfoAbbrev(bitc::FUNCTION_BLOCK_ID, std::move(Abbv)) !=
        (unsigned)FUNCTION_INST_LOAD_ABBREV)
      assert(false && "Unexpected abbrev ordering!");
  }
  { // INST_BINOP abbrev for FUNCTION_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::FUNC_CODE_INST_BINOP));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6));   // LHS
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6));   // RHS
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 4)); // opc
    if (Stream.EmitBlockInfoAbbrev(bitc::FUNCTION_BLOCK_ID, std::move(Abbv)) !=
        (unsigned)FUNCTION_INST_BINOP_ABBREV)
      assert(false && "Unexpected abbrev ordering!");
  }
  { // INST_BINOP_FLAGS abbrev for FUNCTION_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::FUNC_CODE_INST_BINOP));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6));   // LHS
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6));   // RHS
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 4)); // opc
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 8)); // flags (15's)
    if (Stream.EmitBlockInfoAbbrev(bitc::FUNCTION_BLOCK_ID, std::move(Abbv)) !=
        (unsigned)FUNCTION_INST_BINOP_FLAGS_ABBREV)
      assert(false && "Unexpected abbrev ordering!");
  }
  { // INST_CAST abbrev for FUNCTION_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::FUNC_CODE_INST_CAST));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6)); // OpVal
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed,    // dest ty
                              VE.computeBitsRequiredForTypeIndices()));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 4)); // opc
    if (Stream.EmitBlockInfoAbbrev(bitc::FUNCTION_BLOCK_ID, std::move(Abbv)) !=
        (unsigned)FUNCTION_INST_CAST_ABBREV)
      assert(false && "Unexpected abbrev ordering!");
  }

  { // INST_RET abbrev for FUNCTION_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::FUNC_CODE_INST_RET));
    if (Stream.EmitBlockInfoAbbrev(bitc::FUNCTION_BLOCK_ID, std::move(Abbv)) !=
        (unsigned)FUNCTION_INST_RET_VOID_ABBREV)
      assert(false && "Unexpected abbrev ordering!");
  }
  { // INST_RET abbrev for FUNCTION_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::FUNC_CODE_INST_RET));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6)); // ValID
    if (Stream.EmitBlockInfoAbbrev(bitc::FUNCTION_BLOCK_ID, std::move(Abbv)) !=
        (unsigned)FUNCTION_INST_RET_VAL_ABBREV)
      assert(false && "Unexpected abbrev ordering!");
  }
  { // INST_UNREACHABLE abbrev for FUNCTION_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::FUNC_CODE_INST_UNREACHABLE));
    if (Stream.EmitBlockInfoAbbrev(bitc::FUNCTION_BLOCK_ID, std::move(Abbv)) !=
        (unsigned)FUNCTION_INST_UNREACHABLE_ABBREV)
      assert(false && "Unexpected abbrev ordering!");
  }
  {
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::FUNC_CODE_INST_GEP));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 1));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, // dest ty
                              Log2_32_Ceil(VE.getTypes().size() + 1)));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6));
    if (Stream.EmitBlockInfoAbbrev(bitc::FUNCTION_BLOCK_ID, std::move(Abbv)) !=
        (unsigned)FUNCTION_INST_GEP_ABBREV)
      assert(false && "Unexpected abbrev ordering!");
  }

  Stream.ExitBlock();
}

void AIRBitcodeWriter::writeModuleVersion() {
  // VERSION: [version#]
  Stream.EmitRecord(bitc::MODULE_CODE_VERSION, ArrayRef<unsigned>{1});
}

/// The identification block as LLVM 15 writes it, naming LLVM 15 as the
/// producer: the records are LLVM 15's (P7).
void AIRBitcodeWriter::writeIdentificationBlock(BitstreamWriter &Stream) {
  Stream.EnterSubblock(bitc::IDENTIFICATION_BLOCK_ID, 5);

  auto Abbv = std::make_shared<BitCodeAbbrev>();
  Abbv->Add(BitCodeAbbrevOp(bitc::IDENTIFICATION_CODE_STRING));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Char6));
  auto StringAbbrev = Stream.EmitAbbrev(std::move(Abbv));
  writeStringRecord(Stream, bitc::IDENTIFICATION_CODE_STRING, "LLVM15.0.7",
                    StringAbbrev);

  Abbv = std::make_shared<BitCodeAbbrev>();
  Abbv->Add(BitCodeAbbrevOp(bitc::IDENTIFICATION_CODE_EPOCH));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6));
  auto EpochAbbrev = Stream.EmitAbbrev(std::move(Abbv));
  constexpr std::array<unsigned, 1> Vals = {{0}};
  Stream.EmitRecord(bitc::IDENTIFICATION_CODE_EPOCH, Vals, EpochAbbrev);
  Stream.ExitBlock();
}

/// LLVM 15's sync scope names block: the context's names, in id order.
void AIRBitcodeWriter::writeSyncScopeNames() {
  SmallVector<StringRef, 8> SSNs;
  M.getContext().getSyncScopeNames(SSNs);
  if (SSNs.empty())
    return;

  Stream.EnterSubblock(bitc::SYNC_SCOPE_NAMES_BLOCK_ID, 2);
  SmallVector<uint64_t, 64> Record;
  for (StringRef SSN : SSNs) {
    Record.append(SSN.begin(), SSN.end());
    Stream.EmitRecord(bitc::SYNC_SCOPE_NAME, Record, 0);
    Record.clear();
  }
  Stream.ExitBlock();
}

/// MODULE_CODE_HASH as LLVM 15 writes it: SHA-1 of the module block so far
/// (no string table here, so nothing else is hashed).
void AIRBitcodeWriter::writeModuleHash(StringRef View) {
  SHA1 Hasher;
  Hasher.update(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(View.data()), View.size()));
  std::array<uint8_t, 20> Hash = Hasher.result();
  uint32_t Vals[5];
  for (int Pos = 0; Pos < 20; Pos += 4)
    Vals[Pos / 4] = support::endian::read32be(Hash.data() + Pos);
  Stream.EmitRecord(bitc::MODULE_CODE_HASH, Vals);
}

/// WriteModule - Emit the specified module to the bitstream.
void AIRBitcodeWriter::write() {
  if (LateType)
    (void)VE.getTypeID(LateType);

  writeIdentificationBlock(Stream);

  Stream.EnterSubblock(bitc::MODULE_BLOCK_ID, 3);
  Stream.markAndBlockFlushing();

  // It is redundant to fully-specify this here, but nice to make it explicit
  // so that it is clear the DXIL module version is different.
  AIRBitcodeWriter::writeModuleVersion();

  // Emit blockinfo, which defines the standard abbreviations etc.
  writeBlockInfo();

  // Emit information about attribute groups.
  writeAttributeGroupTable();

  // Emit information about parameter attributes.
  writeAttributeTable();

  // Emit information describing all of the types in the module.
  writeTypeTable();

  writeComdats();

  // Emit top-level description of module, including target triple, inline asm,
  // descriptors for global variables, and function prototype info.
  writeModuleInfo();

  // Emit constants.
  writeModuleConstants();

  // Emit metadata.
  writeModuleMetadataKinds();

  // Emit metadata.
  writeModuleMetadata();

  writeSyncScopeNames();

  // Emit names for globals/functions etc.
  // DXIL uses the same format for module-level value symbol table as for the
  // function level table.
  writeFunctionLevelValueSymbolTable(M.getValueSymbolTable());

  // Emit function bodies.
  for (const Function &F : M)
    if (!F.isDeclaration())
      writeFunction(F);

  writeModuleHash(Stream.getMarkedBufferAndResumeFlushing());

  Stream.ExitBlock();
}

/// AIR carries no debug info: refuse a module with any (locations, debug
/// records, or metadata other than tuples, strings and values).
static Error checkNoDebugInfo(const Module &M) {
  auto Fail = [](const Twine &What) {
    return createStringError(inconvertibleErrorCode(),
                             "debug info: " + What + ": AIR carries none");
  };
  SmallPtrSet<const Metadata *, 32> Seen;
  SmallVector<const Metadata *, 32> Work;
  auto Push = [&](const Metadata *MD) {
    if (MD && Seen.insert(MD).second)
      Work.push_back(MD);
  };
  for (const NamedMDNode &N : M.named_metadata())
    for (const MDNode *Op : N.operands())
      Push(Op);
  SmallVector<std::pair<unsigned, MDNode *>, 8> MDs;
  for (const GlobalObject &GO : M.global_objects()) {
    MDs.clear();
    GO.getAllMetadata(MDs);
    for (const auto &[Kind, N] : MDs)
      Push(N);
  }
  for (const Function &F : M)
    for (const BasicBlock &BB : F)
      for (const Instruction &I : BB) {
        if (I.getDebugLoc())
          return Fail("a location in @" + F.getName());
        if (I.hasDbgRecords())
          return Fail("a debug record in @" + F.getName());
        MDs.clear();
        I.getAllMetadataOtherThanDebugLoc(MDs);
        for (const auto &[Kind, N] : MDs)
          Push(N);
        for (const Use &Op : I.operands())
          if (const auto *MAV = dyn_cast<MetadataAsValue>(Op.get()))
            Push(MAV->getMetadata());
      }
  while (!Work.empty()) {
    const Metadata *MD = Work.pop_back_val();
    if (const auto *T = dyn_cast<MDTuple>(MD)) {
      for (const MDOperand &Op : T->operands())
        Push(Op.get());
      continue;
    }
    if (isa<MDString>(MD) || isa<ValueAsMetadata>(MD))
      continue;
    std::string S;
    raw_string_ostream OS(S);
    MD->print(OS, &M);
    return Fail(StringRef(S).take_front(80));
  }
  return Error::success();
}

static Error error(const Twine &Msg) {
  return createStringError(inconvertibleErrorCode(), Msg);
}

/// LLVM 23-only attribute kinds that only state a guarantee, so that
/// dropping one loses a fact and never changes behaviour (P8; each reason is
/// in test/Bitcode/AIRWriter/attrs-23-only.ll).
static constexpr Attribute::AttrKind GuaranteeOnly[] = {
    Attribute::DeadOnReturn, Attribute::DeadOnUnwind,
    Attribute::Initializes,  Attribute::NoCreateUndefOrPoison,
    Attribute::NoFPClass,    Attribute::Range,
    Attribute::Writable};

/// The llvm.* intrinsics the writer writes (P13), mangled: each name's LLVM
/// 15 signature (L15/include/llvm/IR/Intrinsics.td) was checked equal to LLVM
/// 23's when a controller ruling added it, except the lifetime markers, which
/// lowerIntrinsicCalls declares in LLVM 15's form (D3). LLVM 15 reads any
/// other llvm.* declaration as a plain function, so only Metal would decide:
/// an error.
static constexpr StringLiteral LLVM15Intrinsics[] = {
    "llvm.copysign.f16",
    "llvm.copysign.f32",
    "llvm.fabs.f32",
    "llvm.fabs.v4f32",
    "llvm.fshl.i32",
    "llvm.is.fpclass.f32",
    "llvm.is.fpclass.v4f32",
    "llvm.lifetime.end.p0i8",
    "llvm.lifetime.start.p0i8",
    "llvm.smax.i32",
    "llvm.smax.i64",
    "llvm.smin.i32",
    "llvm.smin.i64",
    "llvm.umax.i32",
    "llvm.umax.i64",
    "llvm.umin.i32",
    "llvm.umin.i64",
    "llvm.usub.sat.i32",
};

/// AS with what LLVM 15 cannot hold dropped (guarantee-only kinds, captures
/// other than none) or weakened (memory(...) to its strongest LLVM 15
/// cover), each counted once; any other kind is an error.
static Expected<AttributeSet> lowerAttributes(LLVMContext &C, AttributeSet AS,
                                              StringRef Fn,
                                              AIRWriteReport &R) {
  AttrBuilder B(C);
  bool Changed = false;
  for (Attribute A : AS) {
    if (A.isStringAttribute()) {
      B.addAttribute(A);
      continue;
    }
    if (A.isTypeAttribute())
      return error(Twine("attribute ") + A.getAsString() +
                   ": a type attribute is not written (in @" + Fn + ")");
    Attribute::AttrKind K = A.getKindAsEnum();
    if (K == Attribute::Memory) {
      MemoryEffects ME = A.getMemoryEffects();
      MemoryEffects Cover = memory15(ME, nullptr);
      bool None = Cover == MemoryEffects::unknown();
      if (Cover != ME) {
        Changed = true;
        ++R.Stats.AttrsDropped;
        R.Explain.push_back(
            "weaker " + A.getAsString() + " -> " +
            (None ? std::string("no attribute")
                  : Attribute::getWithMemoryEffects(C, Cover).getAsString()) +
            " (in @" + Fn.str() + ")");
      }
      if (!None)
        B.addMemoryAttr(Cover);
      continue;
    }
    if (K == Attribute::Captures ? A.getCaptureInfo() == CaptureInfo::none()
                                 : AIRBitcodeWriter::getAttrKindEncoding(K)) {
      B.addAttribute(A);
      continue;
    }
    if (K != Attribute::Captures && !is_contained(GuaranteeOnly, K))
      return error(Twine("attribute ") + A.getAsString() +
                   ": no LLVM 15 encoding, and not on the guarantee-only "
                   "drop list (in @" +
                   Fn + ")");
    Changed = true;
    ++R.Stats.AttrsDropped;
    R.Explain.push_back("drop attribute " + A.getAsString() + " (in @" +
                        Fn.str() + ")");
  }
  return Changed ? AttributeSet::get(C, B) : AS;
}

/// LLVM 15's FPMathOperator::classof: no casts, and calls, phis and selects
/// only of FP, FP vectors and arrays of them.
static bool takesFMF15(const Instruction &I) {
  switch (I.getOpcode()) {
  case Instruction::PHI:
  case Instruction::Select:
  case Instruction::Call: {
    Type *T = I.getType();
    while (auto *A = dyn_cast<ArrayType>(T))
      T = A->getElementType();
    return T->isFPOrFPVectorTy();
  }
  default:
    return !isa<CastInst>(I);
  }
}

/// C with LLVM 23-only GEP flags dropped (nusw without inbounds, nuw,
/// inrange) through its operands, and the tokens one printed use loses.
static std::pair<Constant *, unsigned>
stripConstant(Constant *C,
              DenseMap<Constant *, std::pair<Constant *, unsigned>> &Memo) {
  if (!isa<ConstantExpr>(C) && !isa<ConstantAggregate>(C))
    return {C, 0};
  if (auto It = Memo.find(C); It != Memo.end())
    return It->second;
  SmallVector<Constant *, 8> Ops;
  unsigned Tokens = 0;
  bool Changed = false;
  for (Value *Op : C->operands()) {
    auto [New, T] = stripConstant(cast<Constant>(Op), Memo);
    Ops.push_back(New);
    Tokens += T;
    Changed |= New != Op;
  }
  Constant *R = C;
  if (auto *G = dyn_cast<GEPOperator>(C)) {
    GEPNoWrapFlags NW = G->getNoWrapFlags();
    unsigned Own = (!NW.isInBounds() && NW.hasNoUnsignedSignedWrap()) +
                   NW.hasNoUnsignedWrap() + G->getInRange().has_value();
    if (Own || Changed) {
      SmallVector<Value *, 8> Idx(drop_begin(Ops));
      R = ConstantExpr::getGetElementPtr(
          G->getSourceElementType(), Ops[0], Idx,
          NW.isInBounds() ? GEPNoWrapFlags::inBounds()
                          : GEPNoWrapFlags::none());
    }
    Tokens += Own;
  } else if (Changed) {
    if (auto *CE = dyn_cast<ConstantExpr>(C))
      R = CE->getWithOperands(Ops);
    else if (auto *CA = dyn_cast<ConstantArray>(C))
      R = ConstantArray::get(CA->getType(), Ops);
    else if (auto *CS = dyn_cast<ConstantStruct>(C))
      R = ConstantStruct::get(CS->getType(), Ops);
    else
      R = ConstantVector::get(Ops);
  }
  return Memo[C] = {R, Tokens};
}

/// Whether a reader replaces F's attributes with its own intrinsic table's:
/// LLVM 23's and LLVM 15's UpgradeIntrinsicFunction both do
/// (L23/lib/IR/AutoUpgrade.cpp:2001-2010,
/// L15/lib/IR/AutoUpgrade.cpp:1068-1072).
static bool readerDefinesAttributes(Function &F) {
  SmallVector<Type *, 4> Overloads;
  return F.getIntrinsicID() != Intrinsic::not_intrinsic &&
         Intrinsic::isSignatureValid(F.getIntrinsicID(), F.getFunctionType(),
                                     Overloads);
}

namespace {
/// What lowerToLLVM15 changes only for the write; restore() then leaves the
/// module as LLVM 23 reads the written bytes (P9's self row).
struct WriteOnly {
  /// Intrinsic declarations, with LLVM 23's table attributes for them.
  SmallVector<std::pair<Function *, AttributeList>, 4> ReaderDefined;
  /// LLVM 15's lifetime markers (D3) and the calls to them.
  SmallVector<Function *, 2> Markers;
  SmallVector<CallInst *, 4> MarkerCalls;
  /// Values taken out of the module, freed only in restore(): the type table
  /// is keyed by pointer, so no freed address may come back during the write.
  SmallVector<Instruction *, 4> DeadInsts;
  SmallVector<Function *, 2> DeadFunctions;

  void restore(Module &M) {
    for (auto [F, AL] : ReaderDefined)
      F->setAttributes(AL);
    // LLVM 23's reader upgrades LLVM 15's markers (L23/lib/IR/AutoUpgrade.cpp
    // :1586-1597, :5856-5875): each declaration appended anew, in module
    // order, then each call put on its cast-stripped alloca.
    SmallVector<Function *, 2> InOrder;
    for (Function &F : M)
      if (is_contained(Markers, &F))
        InOrder.push_back(&F);
    for (Function *F : InOrder)
      Intrinsic::getOrInsertDeclaration(&M, F->getIntrinsicID(),
                                        {F->getArg(1)->getType()});
    for (CallInst *CI : MarkerCalls) {
      Value *P = CI->getArgOperand(1)->stripPointerCasts();
      if (isa<AllocaInst>(P)) {
        IRBuilder<> B(CI);
        if (CI->getCalledFunction()->getIntrinsicID() ==
            Intrinsic::lifetime_start)
          B.CreateLifetimeStart(P);
        else
          B.CreateLifetimeEnd(P);
      }
      CI->eraseFromParent();
    }
    for (Function *F : Markers)
      F->eraseFromParent();
    for (Instruction *I : DeadInsts)
      I->deleteValue();
    for (Function *F : DeadFunctions) {
      M.getFunctionList().push_back(F);
      F->eraseFromParent();
    }
  }
};
} // namespace

/// Takes I out of the module until WO.restore().
static void takeOut(Instruction *I, WriteOnly &WO) {
  I->removeFromParent();
  I->dropAllReferences();
  WO.DeadInsts.push_back(I);
}

/// The intrinsic calls LLVM 15 holds in another form, rewritten: LLVM 23's
/// sizeless lifetime markers as LLVM 15's (D3), llvm.scmp and llvm.ucmp,
/// which LLVM 15 lacks, as icmp and select (D4). A declaration with a use
/// left (not a call) stays, and the P13 check names it.
static void lowerIntrinsicCalls(Module &M, AIRWriteReport &R, WriteOnly &WO) {
  LLVMContext &C = M.getContext();
  Type *I64 = Type::getInt64Ty(C);
  DenseMap<Function *, Function *> Marker15;
  SmallSetVector<Function *, 4> Lowered;
  for (Function &G : M)
    for (Instruction &I : make_early_inc_range(instructions(G))) {
      auto *CI = dyn_cast<CallInst>(&I);
      Function *F = CI ? CI->getCalledFunction() : nullptr;
      if (!F)
        continue;
      std::string In = (" (in @" + G.getName() + ")").str();
      Intrinsic::ID ID = F->getIntrinsicID();
      if ((ID == Intrinsic::lifetime_start || ID == Intrinsic::lifetime_end) &&
          F->arg_size() == 1) {
        // LLVM 15: llvm.lifetime.start/end.p<N>i8(i64 immarg size, i8* p),
        // the size the alloca's (what the sizeless marker covers), -1 for an
        // alloca of no fixed size. A marker on poison marks nothing.
        Lowered.insert(F);
        auto *AI = dyn_cast<AllocaInst>(CI->getArgOperand(0));
        if (!AI) {
          R.Explain.push_back("drop lifetime marker on poison" + In);
          takeOut(CI, WO);
          continue;
        }
        Function *&F15 = Marker15[F];
        if (!F15) {
          Type *P = F->getArg(0)->getType();
          F15 = Function::Create(
              FunctionType::get(Type::getVoidTy(C), {I64, P}, false),
              GlobalValue::ExternalLinkage,
              Twine("llvm.lifetime.") +
                  (ID == Intrinsic::lifetime_start ? "start" : "end") + ".p" +
                  Twine(P->getPointerAddressSpace()) + "i8");
          M.getFunctionList().insert(F->getIterator(), F15);
          WO.Markers.push_back(F15);
        }
        std::optional<TypeSize> Size = AI->getAllocationSize(M.getDataLayout());
        int64_t N = Size && !Size->isScalable() ? int64_t(Size->getFixedValue())
                                                : -1;
        WO.MarkerCalls.push_back(CallInst::Create(
            F15, {ConstantInt::getSigned(I64, N), AI}, "", CI->getIterator()));
        R.Explain.push_back(
            ("lifetime marker on " +
             (AI->hasName() ? "%" + AI->getName() : Twine("an alloca")) +
             ": @" + F15->getName() + " size " + Twine(N) + In)
                .str());
        takeOut(CI, WO);
      } else if (ID == Intrinsic::scmp || ID == Intrinsic::ucmp) {
        // a < b: -1, a > b: 1, else 0, in the call's return type.
        Lowered.insert(F);
        bool S = ID == Intrinsic::scmp;
        StringRef P = S ? "scmp" : "ucmp";
        Value *A = CI->getArgOperand(0), *B = CI->getArgOperand(1);
        Type *RT = CI->getType();
        IRBuilder<> IRB(CI);
        Value *Lt = IRB.CreateICmp(S ? ICmpInst::ICMP_SLT : ICmpInst::ICMP_ULT,
                                   A, B, P + ".lt");
        Value *Gt = IRB.CreateICmp(S ? ICmpInst::ICMP_SGT : ICmpInst::ICMP_UGT,
                                   A, B, P + ".gt");
        Value *Sel = IRB.CreateSelect(Gt, ConstantInt::get(RT, 1),
                                      Constant::getNullValue(RT), P + ".sel");
        Value *Res = IRB.CreateSelect(Lt, Constant::getAllOnesValue(RT), Sel);
        Res->takeName(CI);
        CI->replaceAllUsesWith(Res);
        R.Explain.push_back("lower @" + F->getName().str() +
                            " to icmp and select" + In);
        takeOut(CI, WO);
      }
    }
  for (Function *F : Lowered)
    if (F->use_empty()) {
      F->removeFromParent();
      WO.DeadFunctions.push_back(F);
    }
}

/// Makes M say only what LLVM 15's records can (P8): attributes and flags
/// LLVM 23 added are dropped or weakened in the module itself and counted,
/// so the module left behind is the module written; constructs LLVM 15
/// cannot hold at all are errors. An intrinsic declaration is written with
/// its lowered attributes too, and after the write it gets LLVM 23's table
/// attributes (ReaderDefined): every reader resets them from its own table,
/// so those are what the written module reads as, even where a pass added
/// more (inferattrs adds mustprogress).
static Error lowerToLLVM15(Module &M, AIRWriteReport &R, WriteOnly &WO) {
  LLVMContext &C = M.getContext();
  lowerIntrinsicCalls(M, R, WO);
  for (Function &F : M) {
    if (!F.getName().starts_with("llvm.") ||
        is_contained(LLVM15Intrinsics, F.getName()))
      continue;
    std::string In;
    for (Function &G : M) {
      for (Instruction &I : instructions(G))
        if (auto *CB = dyn_cast<CallBase>(&I);
            CB && CB->getCalledOperand() == &F) {
          In = (" (in @" + G.getName() + ")").str();
          break;
        }
      if (!In.empty())
        break;
    }
    return error("intrinsic @" + F.getName() +
                 ": not on the writer's list of LLVM 15 intrinsics" + In);
  }
  // Per attribute group (index, set), as the bitcode keys them: each
  // dropped entry is counted once however many lists share the group.
  DenseMap<std::pair<unsigned, AttributeSet>, AttributeSet> Lowered;
  auto LowerList = [&](AttributeList AL,
                       StringRef Fn) -> Expected<AttributeList> {
    AttributeList Out = AL;
    for (unsigned Idx : AL.indexes()) {
      AttributeSet AS = AL.getAttributes(Idx);
      if (!AS.hasAttributes())
        continue;
      auto It = Lowered.find({Idx, AS});
      if (It == Lowered.end()) {
        Expected<AttributeSet> L = lowerAttributes(C, AS, Fn, R);
        if (!L)
          return L.takeError();
        It = Lowered.try_emplace({Idx, AS}, *L).first;
      }
      if (It->second != AS)
        Out = Out.setAttributesAtIndex(C, Idx, It->second);
    }
    return Out;
  };

  for (GlobalVariable &G : M.globals())
    if (G.hasAttributes())
      return error("global @" + G.getName() + ": attributes are not written");

  DenseMap<Constant *, std::pair<Constant *, unsigned>> Stripped;
  for (GlobalVariable &G : M.globals())
    if (G.hasInitializer()) {
      auto [New, Tokens] = stripConstant(G.getInitializer(), Stripped);
      if (Tokens) {
        R.Stats.FlagsDropped += Tokens;
        R.Explain.push_back("drop " + std::to_string(Tokens) +
                            " constant getelementptr flag(s) in @" +
                            G.getName().str());
      }
      if (New != G.getInitializer())
        G.setInitializer(New);
    }

  for (Function &F : M) {
    Expected<AttributeList> FL = LowerList(F.getAttributes(), F.getName());
    if (!FL)
      return FL.takeError();
    if (readerDefinesAttributes(F))
      WO.ReaderDefined.push_back(
          {&F, Intrinsic::getAttributes(C, F.getIntrinsicID(),
                                        F.getFunctionType())});
    F.setAttributes(*FL);

    for (Instruction &I : instructions(F)) {
      auto Name = [&] {
        return I.hasName() ? ("%" + I.getName()).str()
                           : std::string("a ") + I.getOpcodeName();
      };
      auto Fail = [&](const Twine &Why) {
        std::string What = I.getOpcodeName();
        if (auto *RMW = dyn_cast<AtomicRMWInst>(&I))
          What += " " +
                  AtomicRMWInst::getOperationName(RMW->getOperation()).str();
        if (I.hasName())
          What += " %" + I.getName().str();
        return error(What + ": " + Why + " (in @" + F.getName() + ")");
      };
      switch (I.getOpcode()) {
      case Instruction::CallBr:
      case Instruction::CatchPad:
      case Instruction::CatchRet:
      case Instruction::CatchSwitch:
      case Instruction::CleanupPad:
      case Instruction::CleanupRet:
      case Instruction::PtrToAddr:
        return Fail("no LLVM 15 encoding");
      default:
        break;
      }
      if (auto *RMW = dyn_cast<AtomicRMWInst>(&I);
          RMW && AIRBitcodeWriter::getEncodedRMWOperation(
                     RMW->getOperation()) == ~0U)
        return Fail("no LLVM 15 encoding");
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        if (CB->hasOperandBundles())
          return Fail("operand bundles are not written");
        Expected<AttributeList> CL =
            LowerList(CB->getAttributes(), F.getName());
        if (!CL)
          return CL.takeError();
        CB->setAttributes(*CL);
      }

      // LLVM 23-only flags, one token each as llvm-dis prints them.
      SmallVector<StringRef, 4> Drop;
      if (isa<PossiblyNonNegInst>(I) && I.hasNonNeg()) {
        I.setNonNeg(false);
        Drop.push_back("nneg");
      }
      if (auto *T = dyn_cast<TruncInst>(&I)) {
        if (T->hasNoUnsignedWrap())
          Drop.push_back("nuw");
        if (T->hasNoSignedWrap())
          Drop.push_back("nsw");
        T->setHasNoUnsignedWrap(false);
        T->setHasNoSignedWrap(false);
      }
      if (auto *D = dyn_cast<PossiblyDisjointInst>(&I); D && D->isDisjoint()) {
        D->setIsDisjoint(false);
        Drop.push_back("disjoint");
      }
      if (auto *IC = dyn_cast<ICmpInst>(&I); IC && IC->hasSameSign()) {
        IC->setSameSign(false);
        Drop.push_back("samesign");
      }
      if (auto *G = dyn_cast<GetElementPtrInst>(&I)) {
        GEPNoWrapFlags NW = G->getNoWrapFlags();
        if (!NW.isInBounds() && NW.hasNoUnsignedSignedWrap())
          Drop.push_back("nusw");
        if (NW.hasNoUnsignedWrap())
          Drop.push_back("nuw");
        G->setNoWrapFlags(NW.isInBounds() ? GEPNoWrapFlags::inBounds()
                                          : GEPNoWrapFlags::none());
      }
      if (isa<FPMathOperator>(I) && !takesFMF15(I)) {
        FastMathFlags FMF = I.getFastMathFlags();
        if (FMF.isFast()) {
          Drop.push_back("fast");
        } else {
          if (FMF.allowReassoc())
            Drop.push_back("reassoc");
          if (FMF.noNaNs())
            Drop.push_back("nnan");
          if (FMF.noInfs())
            Drop.push_back("ninf");
          if (FMF.noSignedZeros())
            Drop.push_back("nsz");
          if (FMF.allowReciprocal())
            Drop.push_back("arcp");
          if (FMF.allowContract())
            Drop.push_back("contract");
          if (FMF.approxFunc())
            Drop.push_back("afn");
        }
        I.copyFastMathFlags(FastMathFlags());
      }
      if (!Drop.empty()) {
        R.Stats.FlagsDropped += Drop.size();
        R.Explain.push_back("drop " + join(Drop, " ") + " on " + Name() +
                            " (in @" + F.getName().str() + ")");
      }

      for (Use &U : I.operands()) {
        auto *Op = dyn_cast<Constant>(U.get());
        if (!Op)
          continue;
        auto [New, Tokens] = stripConstant(Op, Stripped);
        if (Tokens) {
          R.Stats.FlagsDropped += Tokens;
          R.Explain.push_back("drop " + std::to_string(Tokens) +
                              " constant getelementptr flag(s) on " + Name() +
                              " (in @" + F.getName().str() + ")");
        }
        if (New != Op)
          U.set(New);
      }
    }
  }
  return Error::success();
}

/// The Darwin wrapper and trailer, as LLVM 15 writes them
/// (emitDarwinBCHeaderAndTrailer); air64 has no Mach-O CPU type, so ~0.
static void emitDarwinBCHeaderAndTrailer(SmallVectorImpl<char> &Buffer,
                                         const Triple &TT) {
  enum {
    DARWIN_CPU_ARCH_ABI64 = 0x01000000,
    DARWIN_CPU_TYPE_X86 = 7,
    DARWIN_CPU_TYPE_ARM = 12,
    DARWIN_CPU_TYPE_POWERPC = 18
  };
  unsigned CPUType = ~0U;
  Triple::ArchType Arch = TT.getArch();
  if (Arch == Triple::x86_64)
    CPUType = DARWIN_CPU_TYPE_X86 | DARWIN_CPU_ARCH_ABI64;
  else if (Arch == Triple::x86)
    CPUType = DARWIN_CPU_TYPE_X86;
  else if (Arch == Triple::ppc)
    CPUType = DARWIN_CPU_TYPE_POWERPC;
  else if (Arch == Triple::ppc64)
    CPUType = DARWIN_CPU_TYPE_POWERPC | DARWIN_CPU_ARCH_ABI64;
  else if (Arch == Triple::arm || Arch == Triple::thumb)
    CPUType = DARWIN_CPU_TYPE_ARM;

  uint32_t Header[] = {0x0B17C0DE, 0, BWH_HeaderSize,
                       uint32_t(Buffer.size() - BWH_HeaderSize), CPUType};
  for (unsigned I = 0; I != 5; ++I)
    support::endian::write32le(&Buffer[I * 4], Header[I]);

  // If the file is not a multiple of 16 bytes, insert dummy padding.
  while (Buffer.size() & 15)
    Buffer.push_back(0);
}

Error llvm::air::writeAIRBitcode(Module &M, const AIRTypeTable &T,
                                 raw_ostream &OS, AIRWriteReport *R) {
  AIRWriteReport Local;
  if (!R)
    R = &Local;
  if (Error E = checkNoDebugInfo(M))
    return E;
  WriteOnly WO;
  scope_exit Restore([&] { WO.restore(M); });
  if (Error E = lowerToLLVM15(M, *R, WO))
    return E;
  Expected<AIRPointerTypes> Types = AIRPointerTypes::run(M, T, *R);
  if (!Types)
    return Types.takeError();

  SmallVector<char, 0> Buffer;
  Buffer.reserve(256 * 1024);
  // A Darwin triple gets LLVM 15's wrapper; reserve its header.
  const Triple &TT = M.getTargetTriple();
  bool Darwin = TT.isOSDarwin() || TT.isOSBinFormatMachO();
  if (Darwin)
    Buffer.insert(Buffer.begin(), BWH_HeaderSize, 0);
  {
    BitstreamWriter Stream(Buffer);
    // Emit the file header.
    Stream.Emit((unsigned)'B', 8);
    Stream.Emit((unsigned)'C', 8);
    Stream.Emit(0x0, 4);
    Stream.Emit(0xC, 4);
    Stream.Emit(0xE, 4);
    Stream.Emit(0xD, 4);

    AIRBitcodeWriter Writer(M, *Types, Stream, R->SelfTestLateType);
    Writer.write();
    if (!Writer.error().empty())
      return error(Writer.error());
  }
  if (Darwin)
    emitDarwinBCHeaderAndTrailer(Buffer, TT);
  R->Stats.RefsFilled = T.refsFilled();
  OS.write(Buffer.data(), Buffer.size());
  return Error::success();
}
