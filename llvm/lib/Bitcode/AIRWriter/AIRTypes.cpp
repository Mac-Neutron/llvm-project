//===- AIRTypes.cpp - Typed types for the AIR writer ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Added by Mac-Neutron (2026).
//
//===----------------------------------------------------------------------===//

#include "AIRTypes.h"
#include "AIRTypedRefs.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/TypedPointerType.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>

using namespace llvm;
using namespace llvm::air;

static Error error(const Twine &Msg) {
  return createStringError(inconvertibleErrorCode(), Msg);
}

/// T as LLVM 15 prints it (T addrspace(N)* for a typed pointer; an
/// identified struct by its name, a shadow by its original's).
static void printTyped(raw_ostream &OS, const Type *T,
                       const DenseMap<const StructType *, StructType *> *O) {
  auto List = [&](ArrayRef<Type *> Ts) {
    ListSeparator LS;
    for (Type *E : Ts) {
      OS << LS;
      printTyped(OS, E, O);
    }
  };
  switch (T->getTypeID()) {
  case Type::TypedPointerTyID: {
    auto *P = cast<TypedPointerType>(T);
    printTyped(OS, P->getElementType(), O);
    if (P->getAddressSpace())
      OS << " addrspace(" << P->getAddressSpace() << ")";
    OS << '*';
    return;
  }
  case Type::StructTyID: {
    auto *S = cast<StructType>(T);
    if (!S->isLiteral()) {
      const StructType *Named = S;
      if (O)
        if (StructType *Orig = O->lookup(S))
          Named = Orig;
      Named->print(OS, /*IsForDebug=*/false, /*NoDetails=*/true);
      return;
    }
    OS << (S->isPacked() ? "<{" : "{");
    if (S->getNumElements()) {
      OS << ' ';
      List(S->elements());
      OS << ' ';
    }
    OS << (S->isPacked() ? "}>" : "}");
    return;
  }
  case Type::ArrayTyID: {
    auto *A = cast<ArrayType>(T);
    OS << '[' << A->getNumElements() << " x ";
    printTyped(OS, A->getElementType(), O);
    OS << ']';
    return;
  }
  case Type::FixedVectorTyID:
  case Type::ScalableVectorTyID: {
    auto *V = cast<VectorType>(T);
    OS << '<' << (isa<ScalableVectorType>(V) ? "vscale x " : "")
       << V->getElementCount().getKnownMinValue() << " x ";
    printTyped(OS, V->getElementType(), O);
    OS << '>';
    return;
  }
  case Type::FunctionTyID: {
    auto *F = cast<FunctionType>(T);
    printTyped(OS, F->getReturnType(), O);
    OS << " (";
    List(F->params());
    if (F->isVarArg())
      OS << (F->getNumParams() ? ", ..." : "...");
    OS << ')';
    return;
  }
  default:
    T->print(OS);
  }
}

std::string llvm::air::typeName(const Type *T) {
  std::string S;
  raw_string_ostream OS(S);
  printTyped(OS, T, nullptr);
  return S;
}

static std::string str(const Type *T) { return typeName(T); }

static std::string str(const Value *V, const Module *M) {
  std::string S;
  raw_string_ostream OS(S);
  V->printAsOperand(OS, /*PrintType=*/false, M);
  return S;
}

/// T with F applied to each contained type, or T itself if none changed.
/// Identified structs are the caller's business.
template <typename Fn> static Type *rebuild(Type *T, Fn F) {
  switch (T->getTypeID()) {
  case Type::TypedPointerTyID: {
    auto *P = cast<TypedPointerType>(T);
    Type *E = F(P->getElementType());
    return E == P->getElementType()
               ? T
               : TypedPointerType::get(E, P->getAddressSpace());
  }
  case Type::ArrayTyID: {
    auto *A = cast<ArrayType>(T);
    Type *E = F(A->getElementType());
    return E == A->getElementType() ? T
                                    : ArrayType::get(E, A->getNumElements());
  }
  case Type::FixedVectorTyID:
  case Type::ScalableVectorTyID: {
    auto *V = cast<VectorType>(T);
    Type *E = F(V->getElementType());
    return E == V->getElementType()
               ? T
               : VectorType::get(E, V->getElementCount());
  }
  case Type::StructTyID: {
    auto *S = cast<StructType>(T);
    SmallVector<Type *, 8> Elts(map_range(S->elements(), F));
    return equal(Elts, S->elements())
               ? T
               : StructType::get(T->getContext(), Elts, S->isPacked());
  }
  case Type::FunctionTyID: {
    auto *FT = cast<FunctionType>(T);
    Type *R = F(FT->getReturnType());
    SmallVector<Type *, 8> Ps(map_range(FT->params(), F));
    return R == FT->getReturnType() && equal(Ps, FT->params())
               ? T
               : FunctionType::get(R, Ps, FT->isVarArg());
  }
  default:
    return T;
  }
}

Type *llvm::air::mapContained(Type *T, function_ref<Type *(Type *)> F) {
  return rebuild(T, F);
}

static bool isIdentified(Type *T) {
  auto *S = dyn_cast<StructType>(T);
  return S && !S->isLiteral();
}

static Type *eraseImpl(Type *T,
                       const DenseMap<const StructType *, StructType *> *O) {
  if (auto *P = dyn_cast<TypedPointerType>(T))
    return PointerType::get(T->getContext(), P->getAddressSpace());
  if (isIdentified(T)) {
    if (O)
      if (StructType *Orig = O->lookup(cast<StructType>(T)))
        return Orig;
    return T;
  }
  return rebuild(T, [O](Type *E) { return eraseImpl(E, O); });
}

Type *llvm::air::eraseTypedType(Type *T) { return eraseImpl(T, nullptr); }

bool llvm::air::hasOpaquePointer(Type *T) {
  if (isa<PointerType>(T))
    return true;
  if (isIdentified(T))
    return false;
  return any_of(T->subtypes(), hasOpaquePointer);
}

/// Whether T holds an opaque pointer, through identified structs too.
static bool holdsPointer(Type *T, DenseMap<StructType *, bool> &Memo) {
  if (isa<PointerType>(T))
    return true;
  if (isIdentified(T)) {
    auto *S = cast<StructType>(T);
    auto [It, New] = Memo.try_emplace(S, false);
    if (!New)
      return It->second;
    // Opaque bodies cannot recurse (that takes a pointee), so no cycle reaches
    // the provisional false.
    bool R = any_of(S->elements(),
                    [&](Type *E) { return holdsPointer(E, Memo); });
    Memo[S] = R;
    return R;
  }
  return any_of(T->subtypes(), [&](Type *E) { return holdsPointer(E, Memo); });
}

Type *AIRTypeTable::typedForm(Type *T) const {
  if (isIdentified(T)) {
    StructType *Shadow = Shadows.lookup(cast<StructType>(T));
    return Shadow ? Shadow : T;
  }
  if (!T->getNumContainedTypes())
    return T;
  if (Type *Done = FormCache.lookup(T))
    return Done;
  Type *R = rebuild(T, [this](Type *E) { return typedForm(E); });
  FormCache[T] = R;
  return R;
}

Type *AIRTypeTable::erase(Type *T) const { return eraseImpl(T, &Originals); }

std::string AIRTypeTable::print(Type *T) const {
  std::string S;
  raw_string_ostream OS(S);
  printTyped(OS, T, &Originals);
  return S;
}

StringRef AIRTypeTable::nameOf(const StructType *S) const {
  if (StructType *Orig = Originals.lookup(S))
    return Orig->getName();
  return S->getName();
}

Expected<AIRTypeTable> AIRTypeTable::build(Module &M, const AIRTypeInput &In) {
  AIRTypeTable T;
  LLVMContext &Ctx = M.getContext();

  // Explicit entries, each checked against its value's type. Kept in input
  // order, then module order for the refs, so the table is built the same way
  // in every process.
  SmallVector<std::pair<const Value *, Type *>, 0> Raw;
  DenseMap<const Value *, Type *> Seen;
  for (auto [V, Ty] : In.Values) {
    if (!V || !Ty)
      return error("table: an entry has no value or no type");
    Type *Want;
    if (auto *F = dyn_cast<Function>(V))
      Want = F->getFunctionType();
    else if (auto *G = dyn_cast<GlobalVariable>(V))
      Want = G->getValueType();
    else if (isa<Argument>(V) || isa<Instruction>(V))
      Want = V->getType();
    else
      return error("table: " + str(V, &M) +
                   " is not a function, global, argument or instruction");
    if (eraseTypedType(Ty) != Want)
      return error("table: " + str(V, &M) + " does not erase to " +
                   str(Want) + " (its entry: " + str(Ty) + ")");
    auto [It, New] = Seen.try_emplace(V, Ty);
    if (!New) {
      if (It->second != Ty)
        return error("table: " + str(V, &M) + " has two entries");
      continue;
    }
    Raw.push_back({V, Ty});
  }

  DenseMap<StructType *, Type *> Bodies;
  for (auto [STy, BTy] : In.Structs) {
    auto *S = dyn_cast_or_null<StructType>(STy);
    if (!S || S->isLiteral())
      return error("table: a struct entry is not an identified struct");
    auto *B = dyn_cast_or_null<StructType>(BTy);
    if (!B || !B->isLiteral())
      return error("table: " + str(S) + "'s body is not a literal struct");
    if (S->isOpaque() ||
        eraseTypedType(B) != StructType::get(Ctx, S->elements(), S->isPacked()))
      return error("table: " + str(S) + " does not erase to " +
                   (S->isOpaque() ? std::string("an opaque struct")
                                  : str(StructType::get(Ctx, S->elements(),
                                                        S->isPacked()))) +
                   " (its entry: " + str(B) + ")");
    auto [It, New] = Bodies.try_emplace(S, B);
    if (!New && It->second != B)
      return error("table: " + str(S) + " has two entries");
  }

  // Typed refs fill the names that have no entry; a ref that disagrees with
  // an entry is an error (P16).
  Expected<AIRRefTable> Refs = readTypedRefs(Ctx, In.Refs);
  if (!Refs)
    return Refs.takeError();
  auto Fill = [&](const Value *V, Type *Want, Type *Ref) -> Error {
    if (!Ref)
      return Error::success();
    if (Type *Have = Seen.lookup(V)) {
      if (Have != Ref)
        return error("typed ref: " + str(V, &M) + " is " + str(Ref) +
                     ", its entry " + str(Have));
      return Error::success();
    }
    if (!hasOpaquePointer(Want))
      return Error::success();
    if (eraseTypedType(Ref) != Want)
      return error("typed ref: " + str(V, &M) + " does not erase to " +
                   str(Want) + " (the ref: " + str(Ref) + ")");
    Seen[V] = Ref;
    Raw.push_back({V, Ref});
    ++T.RefsFilled;
    return Error::success();
  };
  for (Function &F : M)
    if (Error E = Fill(&F, F.getFunctionType(),
                       Refs->Functions.lookup(F.getName())))
      return std::move(E);
  for (GlobalVariable &G : M.globals())
    if (Error E =
            Fill(&G, G.getValueType(), Refs->Globals.lookup(G.getName())))
      return std::move(E);

  // Every identified struct the writer can meet: the module's, and those the
  // entries and bodies name. Struct bodies the refs give are added as found.
  SetVector<StructType *> U;
  std::function<void(Type *)> Collect = [&](Type *Ty) {
    if (isIdentified(Ty)) {
      auto *S = cast<StructType>(Ty);
      if (!U.insert(S))
        return;
      for (Type *E : S->elements())
        Collect(E);
      if (Type *B = Bodies.lookup(S))
        Collect(B);
      return;
    }
    for (Type *E : Ty->subtypes())
      Collect(E);
  };
  for (StructType *S : M.getIdentifiedStructTypes())
    Collect(S);
  for (auto [V, Ty] : Raw)
    Collect(Ty);

  DenseMap<StructType *, bool> Memo;
  for (size_t I = 0; I != U.size(); ++I) {
    StructType *S = U[I];
    StructType *Ref = S->hasName() ? Refs->Bodies.lookup(S->getName())
                                   : nullptr;
    if (!Ref || !holdsPointer(S, Memo))
      continue;
    if (Type *Have = Bodies.lookup(S)) {
      if (Have != Ref)
        return error("typed ref: " + str(S) + " is " + str(Ref) +
                     ", its entry " + str(Have));
      continue;
    }
    if (S->isOpaque() || eraseTypedType(Ref) !=
                             StructType::get(Ctx, S->elements(), S->isPacked()))
      return error("typed ref: " + str(S) + " does not erase to its body" +
                   " (the ref: " + str(Ref) + ")");
    Bodies[S] = Ref;
    ++T.RefsFilled;
    Collect(Ref);
  }

  // A shadow for each struct that holds a pointer, made before any body so
  // that bodies can name each other. Unnamed, so the context's names are
  // untouched; the writer writes the original's name.
  // ponytail: shadows are made per write and live as long as the context;
  // reuse them per context if one context ever writes many modules.
  for (StructType *S : U)
    if (holdsPointer(S, Memo)) {
      StructType *Shadow = StructType::create(Ctx);
      T.Shadows[S] = Shadow;
      T.Originals[Shadow] = S;
    }
  for (StructType *S : U) {
    StructType *Shadow = T.Shadows.lookup(S);
    if (!Shadow)
      continue;
    SmallVector<Type *, 8> Elts;
    if (Type *B = Bodies.lookup(S)) {
      for (Type *E : cast<StructType>(B)->elements())
        Elts.push_back(T.typedForm(E));
    } else {
      for (Type *E : S->elements()) {
        if (hasOpaquePointer(E))
          return error("struct " + str(S) +
                       ": pointer fields with no typed body");
        Elts.push_back(T.typedForm(E));
      }
    }
    Shadow->setBody(Elts, S->isPacked());
  }

  for (auto [V, Ty] : Raw)
    T.Entries[V] = T.typedForm(Ty);
  return std::move(T);
}
