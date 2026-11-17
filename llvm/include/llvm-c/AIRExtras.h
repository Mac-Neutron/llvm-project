/*===-- llvm-c/AIRExtras.h - C entry points LLVM's C API lacks ----*- C -*-===*\
|*                                                                            *|
|* Part of the LLVM Project, under the Apache License v2.0 with LLVM          *|
|* Exceptions.                                                                *|
|* See https://llvm.org/LICENSE.txt for license information.                  *|
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception                    *|
|*                                                                            *|
|*===----------------------------------------------------------------------===*|
|*                                                                            *|
|* This header declares the C interface to libLLVMAIRExtras.a: LLVM C++ calls *|
|* that llvm-c/Core.h and its siblings do not reach, each under a C name.     *|
|* Added by Mac-Neutron (2026).                                               *|
|*                                                                            *|
|* Every function here is one LLVM call (named in its comment) and holds no   *|
|* policy of its caller. Lengths are in bytes; no string need end in a NUL.   *|
|* A message handed back is freed with LLVMDisposeMessage.                    *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

#ifndef LLVM_C_AIREXTRAS_H
#define LLVM_C_AIREXTRAS_H

#include "llvm-c/Core.h"
#include "llvm-c/ExternC.h"
#include "llvm-c/Types.h"
#include <stddef.h>

LLVM_C_EXTERN_C_BEGIN

/* ---- Fast-math flags ---------------------------------------------------- */

/* IRBuilder::setFastMathFlags: every floating-point instruction B makes from
   now on carries FMF. */
void LLVMAIRBuilderSetFastMathFlags(LLVMBuilderRef B, LLVMFastMathFlags FMF);

/* Instruction::copyFastMathFlags: Inst's flags become exactly FMF (where
   LLVMSetFastMathFlags only adds). A value that is not a floating-point
   instruction is left alone. */
void LLVMAIRCopyFastMathFlags(LLVMValueRef Inst, LLVMFastMathFlags FMF);

/* ---- Bitcode ------------------------------------------------------------ */

/* ParserCallbacks::DataLayout. Called with the module's target triple and its
   data-layout string; answers the layout to parse with (*OutLen bytes that
   stay valid until the callback's next call or the parse's end, copied at
   once), or NULL to keep the module's own. */
typedef const char *(*LLVMAIRDataLayoutCallback)(void *Ctx, const char *Triple,
                                                 size_t TripleLen,
                                                 const char *Layout,
                                                 size_t LayoutLen,
                                                 size_t *OutLen);

/* parseBitcodeFile: Size bytes of bitcode at Data, in a buffer named Name,
   into a module of C, read in full. Callback may be NULL. Answers 0 with the
   module in *OutModule, or 1 with toString of the error in *OutMessage
   (*OutMessageLen bytes). */
LLVMBool LLVMAIRParseBitcode(LLVMContextRef C, const char *Data, size_t Size,
                             const char *Name, size_t NameLen,
                             LLVMAIRDataLayoutCallback Callback, void *Ctx,
                             LLVMModuleRef *OutModule, char **OutMessage,
                             size_t *OutMessageLen);

/* ---- Linker ------------------------------------------------------------- */

typedef enum {
  LLVMAIRLinkNone = 0,
  LLVMAIRLinkOverrideFromSrc = 1 << 0,
  LLVMAIRLinkOnlyNeeded = 1 << 1
} LLVMAIRLinkFlags;

/* Linker::linkModules(Dest, Src, Flags): Src is taken, whatever the answer.
   Answers 1 on an error, which the context's diagnostic handler has seen. */
LLVMBool LLVMAIRLinkModules(LLVMModuleRef Dest, LLVMModuleRef Src,
                            unsigned Flags);

/* ---- Module contents ---------------------------------------------------- */

/* Module::eraseNamedMetadata of the node named Name, if M has one. */
void LLVMAIREraseNamedMetadata(LLVMModuleRef M, const char *Name,
                               size_t NameLen);

/* Constant::destroyConstant: C must have no user left. */
void LLVMAIRDestroyConstant(LLVMValueRef C);

/* Module::getIdentifiedStructTypes: answers how many M has and writes the
   first Capacity of them to Out, in that function's order. */
size_t LLVMAIRGetIdentifiedStructTypes(LLVMModuleRef M, LLVMTypeRef *Out,
                                       size_t Capacity);

/* StructType::getName with its length (LLVMGetStructName answers a C string,
   which ends at a NUL the name may hold). NULL and 0 for a struct with no
   name. */
const char *LLVMAIRGetStructName(LLVMTypeRef Struct, size_t *NameLen);

/* StructType::setName on an identified struct; a literal struct is left
   alone. */
void LLVMAIRStructSetName(LLVMTypeRef Struct, const char *Name,
                          size_t NameLen);

/* ---- Instructions and blocks ------------------------------------------- */

/* ConstantExpr::getAsInstruction, the instruction inserted before
   InsertBefore when that is not NULL. */
LLVMValueRef LLVMAIRConstantExprAsInstruction(LLVMValueRef Expr,
                                              LLVMValueRef InsertBefore);

/* BasicBlock::splitBasicBlock before the instruction Before, or at BB's end
   when Before is NULL. Answers the new block. */
LLVMBasicBlockRef LLVMAIRSplitBasicBlock(LLVMBasicBlockRef BB,
                                         LLVMValueRef Before);

/* SplitBlockAndInsertIfThen(Cond, SplitBefore, Unreachable). Answers the
   terminator of the new conditional block. */
LLVMValueRef LLVMAIRSplitBlockAndInsertIfThen(LLVMValueRef Cond,
                                              LLVMValueRef SplitBefore,
                                              LLVMBool Unreachable);

/* buildAtomicRMWValue (LowerAtomic.h): the value Op stores, from Loaded and
   Val, built at B's position. NULL for an operation it has no case for
   (where the C++ is unreachable). */
LLVMValueRef LLVMAIRBuildAtomicRMWValue(LLVMBuilderRef B,
                                        LLVMAtomicRMWBinOp Op,
                                        LLVMValueRef Loaded, LLVMValueRef Val);

/* ---- Local slot numbers ------------------------------------------------- */

typedef struct LLVMAIROpaqueSlotTracker *LLVMAIRSlotTrackerRef;

/* A ModuleSlotTracker of Fn's module with Fn incorporated: the numbers the
   printer gives Fn's unnamed values. */
LLVMAIRSlotTrackerRef LLVMAIRCreateSlotTracker(LLVMValueRef Fn);
/* ModuleSlotTracker::getLocalSlot: V's number, or -1 when it has none. */
int LLVMAIRGetLocalSlot(LLVMAIRSlotTrackerRef Tracker, LLVMValueRef V);
void LLVMAIRDisposeSlotTracker(LLVMAIRSlotTrackerRef Tracker);

/* ---- Passes ------------------------------------------------------------- */

/* The FunctionAnalysisManager of a running pass; valid during the callback
   that received it. */
typedef struct LLVMAIROpaqueFunctionAnalyses *LLVMAIRFunctionAnalysesRef;

/* A function pass. Answers non-zero when it changed Fn
   (PreservedAnalyses::none()), zero when it did not (all()). */
typedef LLVMBool (*LLVMAIRFunctionPassCallback)(void *Ctx, LLVMValueRef Fn,
                                                LLVMAIRFunctionAnalysesRef AM);

/* A function pass a pipeline's text may name: Name (NameLen bytes) where a
   function pass can stand. */
typedef struct {
  const char *Name;
  size_t NameLen;
  LLVMAIRFunctionPassCallback Run;
  void *Ctx;
} LLVMAIRFunctionPass;

/* PassBuilder::parsePassPipeline of Pipeline (a module pipeline) with Passes
   known by name, then ModulePassManager::run on M, with the four analysis
   managers cross-registered. No StandardInstrumentations: nothing skips an
   optnone function, prints, or verifies. Answers 0, or 1 with the parse error
   in *OutMessage (*OutMessageLen bytes) and M untouched. */
LLVMBool LLVMAIRRunPasses(LLVMModuleRef M, const char *Pipeline,
                          size_t PipelineLen, const LLVMAIRFunctionPass *Passes,
                          size_t NumPasses, char **OutMessage,
                          size_t *OutMessageLen);

/* The same parse, then ModulePassManager::printPipeline: *OutMessage holds the
   pipeline's text on 0 and the parse error on 1. A pass of Passes prints as
   its name. */
LLVMBool LLVMAIRPrintPipeline(const char *Pipeline, size_t PipelineLen,
                              const LLVMAIRFunctionPass *Passes,
                              size_t NumPasses, char **OutMessage,
                              size_t *OutMessageLen);

/* MemorySSA (AM's MemorySSAAnalysis result for Fn, computed on first use):
   the instruction of Inst's defining access when that access is a MemoryDef
   with an instruction. NULL when Inst has no memory access, when its defining
   access is a MemoryPhi, or when it is the function's live-on-entry
   definition. */
LLVMValueRef LLVMAIRGetMemoryDefInstruction(LLVMAIRFunctionAnalysesRef AM,
                                            LLVMValueRef Fn, LLVMValueRef Inst);

LLVM_C_EXTERN_C_END

#endif
