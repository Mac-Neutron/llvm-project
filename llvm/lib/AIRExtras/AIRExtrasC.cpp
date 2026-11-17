//===- AIRExtrasC.cpp - C entry points LLVM's C API lacks -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Added by Mac-Neutron (2026).
//
//===----------------------------------------------------------------------===//
//
// Each function is one LLVM call under a C name (llvm-c/AIRExtras.h). None
// holds a decision of its caller.
//
//===----------------------------------------------------------------------===//

#include "llvm-c/AIRExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/FMF.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ModuleSlotTracker.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LowerAtomic.h"
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

using namespace llvm;

DEFINE_SIMPLE_CONVERSION_FUNCTIONS(ModuleSlotTracker, LLVMAIRSlotTrackerRef)
DEFINE_SIMPLE_CONVERSION_FUNCTIONS(FunctionAnalysisManager,
                                   LLVMAIRFunctionAnalysesRef)

// A message as LLVMDisposeMessage frees it, with its length (the text need
// not be a C string).
static void setMessage(const std::string &Text, char **Out, size_t *OutLen) {
  if (OutLen)
    *OutLen = 0;
  if (!Out)
    return;
  *Out = static_cast<char *>(malloc(Text.size() + 1));
  if (!*Out)
    return;
  memcpy(*Out, Text.data(), Text.size());
  (*Out)[Text.size()] = '\0';
  if (OutLen)
    *OutLen = Text.size();
}

//===----------------------------------------------------------------------===//
// Fast-math flags
//===----------------------------------------------------------------------===//

// llvm-c/Core.h's LLVMFastMathFlags are FastMathFlags' bits; Core.cpp maps
// them with a static function, repeated here.
static_assert(FastMathFlags::AllowReassoc == LLVMFastMathAllowReassoc &&
                  FastMathFlags::NoNaNs == LLVMFastMathNoNaNs &&
                  FastMathFlags::NoInfs == LLVMFastMathNoInfs &&
                  FastMathFlags::NoSignedZeros == LLVMFastMathNoSignedZeros &&
                  FastMathFlags::AllowReciprocal ==
                      LLVMFastMathAllowReciprocal &&
                  FastMathFlags::AllowContract == LLVMFastMathAllowContract &&
                  FastMathFlags::ApproxFunc == LLVMFastMathApproxFunc,
              "LLVMFastMathFlags are FastMathFlags' bits");

static FastMathFlags mapFromLLVMFastMathFlags(LLVMFastMathFlags FMF) {
  FastMathFlags NewFMF;
  NewFMF.setAllowReassoc((FMF & LLVMFastMathAllowReassoc) != 0);
  NewFMF.setNoNaNs((FMF & LLVMFastMathNoNaNs) != 0);
  NewFMF.setNoInfs((FMF & LLVMFastMathNoInfs) != 0);
  NewFMF.setNoSignedZeros((FMF & LLVMFastMathNoSignedZeros) != 0);
  NewFMF.setAllowReciprocal((FMF & LLVMFastMathAllowReciprocal) != 0);
  NewFMF.setAllowContract((FMF & LLVMFastMathAllowContract) != 0);
  NewFMF.setApproxFunc((FMF & LLVMFastMathApproxFunc) != 0);
  return NewFMF;
}

void LLVMAIRBuilderSetFastMathFlags(LLVMBuilderRef B, LLVMFastMathFlags FMF) {
  unwrap(B)->setFastMathFlags(mapFromLLVMFastMathFlags(FMF));
}

void LLVMAIRCopyFastMathFlags(LLVMValueRef Inst, LLVMFastMathFlags FMF) {
  auto *I = dyn_cast<Instruction>(unwrap(Inst));
  if (I && isa<FPMathOperator>(I))
    I->copyFastMathFlags(mapFromLLVMFastMathFlags(FMF));
}

//===----------------------------------------------------------------------===//
// Bitcode
//===----------------------------------------------------------------------===//

LLVMBool LLVMAIRParseBitcode(LLVMContextRef C, const char *Data, size_t Size,
                             const char *Name, size_t NameLen,
                             LLVMAIRDataLayoutCallback Callback, void *Ctx,
                             LLVMModuleRef *OutModule, char **OutMessage,
                             size_t *OutMessageLen) {
  *OutModule = nullptr;
  if (OutMessage)
    *OutMessage = nullptr;
  if (OutMessageLen)
    *OutMessageLen = 0;
  ParserCallbacks Callbacks;
  if (Callback) {
    Callbacks.DataLayout =
        [Callback, Ctx](StringRef Triple,
                        StringRef Layout) -> std::optional<std::string> {
      size_t Len = 0;
      const char *Replaced = Callback(Ctx, Triple.data(), Triple.size(),
                                      Layout.data(), Layout.size(), &Len);
      if (!Replaced)
        return std::nullopt;
      return std::string(Replaced, Len);
    };
  }
  Expected<std::unique_ptr<Module>> M = parseBitcodeFile(
      MemoryBufferRef(StringRef(Data, Size), StringRef(Name, NameLen)),
      *unwrap(C), Callbacks);
  if (M) {
    *OutModule = wrap(M->release());
    return 0;
  }
  setMessage(toString(M.takeError()), OutMessage, OutMessageLen);
  return 1;
}

//===----------------------------------------------------------------------===//
// Linker
//===----------------------------------------------------------------------===//

static_assert(static_cast<unsigned>(Linker::Flags::None) == LLVMAIRLinkNone &&
                  static_cast<unsigned>(Linker::Flags::OverrideFromSrc) ==
                      LLVMAIRLinkOverrideFromSrc &&
                  static_cast<unsigned>(Linker::Flags::LinkOnlyNeeded) ==
                      LLVMAIRLinkOnlyNeeded,
              "LLVMAIRLinkFlags are Linker::Flags");

LLVMBool LLVMAIRLinkModules(LLVMModuleRef Dest, LLVMModuleRef Src,
                            unsigned Flags) {
  return Linker::linkModules(*unwrap(Dest), std::unique_ptr<Module>(unwrap(Src)),
                             Flags);
}

//===----------------------------------------------------------------------===//
// Module contents
//===----------------------------------------------------------------------===//

void LLVMAIREraseNamedMetadata(LLVMModuleRef M, const char *Name,
                               size_t NameLen) {
  if (NamedMDNode *MD = unwrap(M)->getNamedMetadata(StringRef(Name, NameLen)))
    unwrap(M)->eraseNamedMetadata(MD);
}

void LLVMAIRDestroyConstant(LLVMValueRef C) {
  unwrap<Constant>(C)->destroyConstant();
}

size_t LLVMAIRGetIdentifiedStructTypes(LLVMModuleRef M, LLVMTypeRef *Out,
                                       size_t Capacity) {
  std::vector<StructType *> Types = unwrap(M)->getIdentifiedStructTypes();
  for (size_t I = 0; I < Types.size() && I < Capacity; ++I)
    Out[I] = wrap(Types[I]);
  return Types.size();
}

const char *LLVMAIRGetStructName(LLVMTypeRef Struct, size_t *NameLen) {
  auto *ST = unwrap<StructType>(Struct);
  *NameLen = 0;
  if (!ST->hasName())
    return nullptr;
  StringRef Name = ST->getName();
  *NameLen = Name.size();
  return Name.data();
}

void LLVMAIRStructSetName(LLVMTypeRef Struct, const char *Name,
                          size_t NameLen) {
  auto *ST = unwrap<StructType>(Struct);
  if (!ST->isLiteral())
    ST->setName(StringRef(Name, NameLen));
}

//===----------------------------------------------------------------------===//
// Instructions and blocks
//===----------------------------------------------------------------------===//

LLVMValueRef LLVMAIRConstantExprAsInstruction(LLVMValueRef Expr,
                                              LLVMValueRef InsertBefore) {
  Instruction *I = unwrap<ConstantExpr>(Expr)->getAsInstruction();
  if (auto *Before = cast_or_null<Instruction>(unwrap(InsertBefore)))
    I->insertBefore(Before->getIterator());
  return wrap(I);
}

LLVMBasicBlockRef LLVMAIRSplitBasicBlock(LLVMBasicBlockRef BB,
                                         LLVMValueRef Before) {
  BasicBlock *Block = unwrap(BB);
  return wrap(Block->splitBasicBlock(
      Before ? unwrap<Instruction>(Before)->getIterator() : Block->end()));
}

LLVMValueRef LLVMAIRSplitBlockAndInsertIfThen(LLVMValueRef Cond,
                                              LLVMValueRef SplitBefore,
                                              LLVMBool Unreachable) {
  return wrap(SplitBlockAndInsertIfThen(
      unwrap(Cond), unwrap<Instruction>(SplitBefore)->getIterator(),
      Unreachable != 0));
}

// Core.cpp's mapping, static there.
static std::optional<AtomicRMWInst::BinOp> mapFromLLVMRMWBinOp(
    LLVMAtomicRMWBinOp Op) {
  switch (Op) {
  case LLVMAtomicRMWBinOpXchg:
    return AtomicRMWInst::Xchg;
  case LLVMAtomicRMWBinOpAdd:
    return AtomicRMWInst::Add;
  case LLVMAtomicRMWBinOpSub:
    return AtomicRMWInst::Sub;
  case LLVMAtomicRMWBinOpAnd:
    return AtomicRMWInst::And;
  case LLVMAtomicRMWBinOpNand:
    return AtomicRMWInst::Nand;
  case LLVMAtomicRMWBinOpOr:
    return AtomicRMWInst::Or;
  case LLVMAtomicRMWBinOpXor:
    return AtomicRMWInst::Xor;
  case LLVMAtomicRMWBinOpMax:
    return AtomicRMWInst::Max;
  case LLVMAtomicRMWBinOpMin:
    return AtomicRMWInst::Min;
  case LLVMAtomicRMWBinOpUMax:
    return AtomicRMWInst::UMax;
  case LLVMAtomicRMWBinOpUMin:
    return AtomicRMWInst::UMin;
  case LLVMAtomicRMWBinOpFAdd:
    return AtomicRMWInst::FAdd;
  case LLVMAtomicRMWBinOpFSub:
    return AtomicRMWInst::FSub;
  case LLVMAtomicRMWBinOpFMax:
    return AtomicRMWInst::FMax;
  case LLVMAtomicRMWBinOpFMin:
    return AtomicRMWInst::FMin;
  default:
    return std::nullopt;
  }
}

LLVMValueRef LLVMAIRBuildAtomicRMWValue(LLVMBuilderRef B,
                                        LLVMAtomicRMWBinOp Op,
                                        LLVMValueRef Loaded,
                                        LLVMValueRef Val) {
  std::optional<AtomicRMWInst::BinOp> BinOp = mapFromLLVMRMWBinOp(Op);
  if (!BinOp)
    return nullptr;
  return wrap(
      buildAtomicRMWValue(*BinOp, *unwrap(B), unwrap(Loaded), unwrap(Val)));
}

//===----------------------------------------------------------------------===//
// Local slot numbers
//===----------------------------------------------------------------------===//

LLVMAIRSlotTrackerRef LLVMAIRCreateSlotTracker(LLVMValueRef Fn) {
  auto *F = unwrap<Function>(Fn);
  auto *Tracker = new ModuleSlotTracker(F->getParent());
  Tracker->incorporateFunction(*F);
  return wrap(Tracker);
}

int LLVMAIRGetLocalSlot(LLVMAIRSlotTrackerRef Tracker, LLVMValueRef V) {
  return unwrap(Tracker)->getLocalSlot(unwrap(V));
}

void LLVMAIRDisposeSlotTracker(LLVMAIRSlotTrackerRef Tracker) {
  delete unwrap(Tracker);
}

//===----------------------------------------------------------------------===//
// Passes
//===----------------------------------------------------------------------===//

namespace {
// A function pass that is its caller's callback.
class CallbackFunctionPass : public PassInfoMixin<CallbackFunctionPass> {
  std::string Name;
  LLVMAIRFunctionPassCallback Run;
  void *Ctx;

public:
  explicit CallbackFunctionPass(const LLVMAIRFunctionPass &Pass)
      : Name(Pass.Name, Pass.NameLen), Run(Pass.Run), Ctx(Pass.Ctx) {}

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    return Run(Ctx, wrap(&F), wrap(&AM)) ? PreservedAnalyses::none()
                                         : PreservedAnalyses::all();
  }

  void printPipeline(raw_ostream &OS,
                     function_ref<StringRef(StringRef)>) const {
    OS << Name;
  }
};
} // namespace

// The pipeline's text as a ModulePassManager, Passes known by name.
static Error parsePipeline(PassBuilder &PB, ModulePassManager &MPM,
                           StringRef Pipeline,
                           ArrayRef<LLVMAIRFunctionPass> Passes) {
  PB.registerPipelineParsingCallback(
      [Passes](StringRef Name, FunctionPassManager &FPM,
               ArrayRef<PassBuilder::PipelineElement>) {
        for (const LLVMAIRFunctionPass &Pass : Passes) {
          if (Name == StringRef(Pass.Name, Pass.NameLen)) {
            FPM.addPass(CallbackFunctionPass(Pass));
            return true;
          }
        }
        return false;
      });
  return PB.parsePassPipeline(MPM, Pipeline);
}

LLVMBool LLVMAIRRunPasses(LLVMModuleRef M, const char *Pipeline,
                          size_t PipelineLen, const LLVMAIRFunctionPass *Passes,
                          size_t NumPasses, char **OutMessage,
                          size_t *OutMessageLen) {
  if (OutMessage)
    *OutMessage = nullptr;
  if (OutMessageLen)
    *OutMessageLen = 0;

  // Declared in this order so that they are destroyed in the correct order
  // (inter-analysis-manager references).
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;

  PassInstrumentationCallbacks PIC;
  PassBuilder PB(nullptr, PipelineTuningOptions(), {}, &PIC);

  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  ModulePassManager MPM;
  if (Error E = parsePipeline(PB, MPM, StringRef(Pipeline, PipelineLen),
                              ArrayRef(Passes, NumPasses))) {
    setMessage(toString(std::move(E)), OutMessage, OutMessageLen);
    return 1;
  }
  MPM.run(*unwrap(M), MAM);
  return 0;
}

LLVMBool LLVMAIRPrintPipeline(const char *Pipeline, size_t PipelineLen,
                              const LLVMAIRFunctionPass *Passes,
                              size_t NumPasses, char **OutMessage,
                              size_t *OutMessageLen) {
  PassInstrumentationCallbacks PIC;
  PassBuilder PB(nullptr, PipelineTuningOptions(), {}, &PIC);
  ModulePassManager MPM;
  if (Error E = parsePipeline(PB, MPM, StringRef(Pipeline, PipelineLen),
                              ArrayRef(Passes, NumPasses))) {
    setMessage(toString(std::move(E)), OutMessage, OutMessageLen);
    return 1;
  }
  std::string Text;
  raw_string_ostream OS(Text);
  MPM.printPipeline(OS, [&PIC](StringRef ClassName) {
    StringRef PassName = PIC.getPassNameForClassName(ClassName);
    return PassName.empty() ? ClassName : PassName;
  });
  setMessage(Text, OutMessage, OutMessageLen);
  return 0;
}

LLVMValueRef LLVMAIRGetMemoryDefInstruction(LLVMAIRFunctionAnalysesRef AM,
                                            LLVMValueRef Fn,
                                            LLVMValueRef Inst) {
  MemorySSA &MSSA =
      unwrap(AM)->getResult<MemorySSAAnalysis>(*unwrap<Function>(Fn)).getMSSA();
  MemoryUseOrDef *Access = MSSA.getMemoryAccess(unwrap<Instruction>(Inst));
  if (!Access)
    return nullptr;
  auto *Def = dyn_cast<MemoryDef>(Access->getDefiningAccess());
  return Def ? wrap(Def->getMemoryInst()) : nullptr;
}
