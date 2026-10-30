//===- AIRTypes.h - Typed types for the AIR writer --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Added by Mac-Neutron (2026).
//
//===----------------------------------------------------------------------===//
//
// A typed type is an LLVM type whose pointers are TypedPointerTypes. The table
// holds the caller's typed types (checked: each erases to its value's type),
// and, for every identified struct with pointers inside, a shadow struct with
// a typed body that the writer writes under the original's name.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_BITCODE_AIRWRITER_AIRTYPES_H
#define LLVM_LIB_BITCODE_AIRWRITER_AIRTYPES_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"
#include <string>
#include <utility>

namespace llvm {

class AllocaInst;
class CallBase;
class Function;
class GEPOperator;
class GlobalVariable;
class Module;
class Value;

namespace air {

/// The caller's table (LLVMAIRTypes in C++ form).
struct AIRTypeInput {
  /// Function: typed function type; global: typed value type; argument or
  /// instruction: its typed type.
  ArrayRef<std::pair<Value *, Type *>> Values;
  /// Identified struct -> typed literal body.
  ArrayRef<std::pair<Type *, Type *>> Structs;
  /// Typed bitcode typing same-named functions, globals and structs.
  ArrayRef<MemoryBufferRef> Refs;
};

/// T with every typed pointer replaced by `ptr` in its address space.
Type *eraseTypedType(Type *T);

/// T as LLVM prints it, for error text.
std::string typeName(const Type *T);

/// Whether T holds an opaque pointer outside identified structs.
bool hasOpaquePointer(Type *T);

class AIRTypeTable {
public:
  static Expected<AIRTypeTable> build(Module &M, const AIRTypeInput &In);

  /// V's typed type (function: function type; global: value type) from an
  /// explicit entry or a typed ref, in typed form; null if there is none.
  Type *entry(const Value *V) const { return Entries.lookup(V); }

  /// T with each identified struct that holds pointers replaced by its shadow.
  /// Opaque pointers stay; the type table refuses them.
  Type *typedForm(Type *T) const;

  /// T erased back to the module's types (shadows to their originals).
  Type *erase(Type *T) const;

  /// The name a struct is written under: a shadow's is its original's.
  StringRef nameOf(const StructType *S) const;

  unsigned refsFilled() const { return RefsFilled; }

private:
  DenseMap<const Value *, Type *> Entries;
  DenseMap<StructType *, StructType *> Shadows;          // original -> shadow
  DenseMap<const StructType *, StructType *> Originals;  // shadow -> original
  mutable DenseMap<Type *, Type *> FormCache;
  unsigned RefsFilled = 0;
};

/// The typed type of every value the writer writes. In this version from the
/// table only: a value with a pointer and no entry is an error.
class AIRModuleTypes {
public:
  static Expected<AIRModuleTypes> run(const Module &M, const AIRTypeTable &T);

  /// V's typed type. For a constant, its type in typed form.
  Type *typeOf(const Value *V) const;
  Type *form(Type *T) const { return Table->typedForm(T); }
  FunctionType *functionType(const Function &F) const;
  Type *valueType(const GlobalVariable &G) const;
  /// A GEP's typed source element type.
  Type *sourceType(const GEPOperator &G) const;
  Type *allocatedType(const AllocaInst &AI) const;
  /// The typed function type a call is written with.
  FunctionType *callType(const CallBase &CB) const;
  StringRef nameOf(const StructType *S) const { return Table->nameOf(S); }

private:
  const AIRTypeTable *Table = nullptr;
  DenseMap<const Value *, Type *> Typed;
  /// Function: typed function type; global: typed value type; GEP: typed
  /// source type; alloca: typed allocated type; call: typed function type.
  DenseMap<const Value *, Type *> Aux;
};

} // namespace air
} // namespace llvm

#endif // LLVM_LIB_BITCODE_AIRWRITER_AIRTYPES_H
