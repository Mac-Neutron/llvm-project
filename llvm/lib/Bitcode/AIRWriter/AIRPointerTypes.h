//===- AIRPointerTypes.h - Pointee types and casts for AIR ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Added by Mac-Neutron (2026), after DirectX's PointerTypeAnalysis.cpp and
// DXILPrepare.cpp at llvmorg-23.1.4, without their i8* defaults.
//
//===----------------------------------------------------------------------===//
//
// The typed type of every value an opaque module holds (P5): the table, then
// typed refs (both through AIRTypeTable), then the facts opaque IR keeps,
// then one fixpoint over use edges; a pointer nothing reaches is i8 in its
// address space and counted. Where a use needs another typed type than its
// operand has, a bitcast is inserted into the module, or, for a constant, a
// synthetic typed constant stands in for it (P6).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_BITCODE_AIRWRITER_AIRPOINTERTYPES_H
#define LLVM_LIB_BITCODE_AIRWRITER_AIRPOINTERTYPES_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Error.h"
#include <memory>
#include <vector>

namespace llvm {

class AllocaInst;
class CallBase;
class Constant;
class Function;
class FunctionType;
class GEPOperator;
class GlobalVariable;
class Module;
class StructType;
class Type;
class Use;

namespace air {

class AIRTypeTable;
struct AIRWriteReport;

class AIRPointerTypes {
public:
  /// A constant written at a typed type its opaque self cannot carry: a typed
  /// null, undef, poison or aggregate (C at T, Ops its typed operands), or a
  /// bitcast of Ops[0] to T (Cast). Its identity is a value of C's opaque type
  /// that no module holds.
  struct Synthetic {
    const Constant *C;
    Type *T;
    bool Cast;
    SmallVector<const Value *, 4> Ops;
  };

  /// Types M and inserts its casts; M is modified and must not be written
  /// again. Stats and --explain lines go to R.
  static Expected<AIRPointerTypes> run(Module &M, const AIRTypeTable &T,
                                       AIRWriteReport &R);

  /// V's typed type: a value's, a synthetic constant's, or a constant's own.
  Type *typedTypeOf(const Value *V) const;
  /// The value U is written as: its operand, or a synthetic typed constant.
  const Value *typedOperand(const Use &U) const;
  /// V's synthetic constant record, if V is one.
  const Synthetic *synthetic(const Value *V) const;

  FunctionType *functionType(const Function &F) const;
  Type *valueType(const GlobalVariable &G) const;
  /// A GEP's typed source element type (instruction or constant).
  Type *sourceType(const GEPOperator &G) const;
  Type *allocatedType(const AllocaInst &AI) const;
  /// The typed function type a call is written with.
  FunctionType *callType(const CallBase &CB) const;
  /// T in typed form (identified structs to their shadows).
  Type *form(Type *T) const;
  StringRef nameOf(const StructType *S) const;

private:
  class Solver;
  const AIRTypeTable *Table = nullptr;
  DenseMap<const Value *, Type *> Typed;
  /// GEP instruction: typed source; call: typed function type; function:
  /// typed function type.
  DenseMap<const Value *, Type *> Aux;
  DenseMap<const Use *, const Value *> Operands;
  DenseMap<const Value *, unsigned> SyntheticIndex;
  std::vector<Synthetic> Synthetics;
  std::vector<std::unique_ptr<Value, ValueDeleter>> Proxies;
  /// Constants' own typed types, filled as they are asked for.
  mutable DenseMap<const Value *, Type *> Natural;
  Type *Hole = nullptr;

  Type *natural(const Constant *C) const;
};

} // namespace air
} // namespace llvm

#endif // LLVM_LIB_BITCODE_AIRWRITER_AIRPOINTERTYPES_H
