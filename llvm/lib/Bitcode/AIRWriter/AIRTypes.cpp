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
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/TypedPointerType.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>

using namespace llvm;
using namespace llvm::air;

static Error error(const Twine &Msg) {
  return createStringError(inconvertibleErrorCode(), Msg);
}

std::string llvm::air::typeName(const Type *T) {
  std::string S;
  raw_string_ostream OS(S);
  T->print(OS);
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

Type *AIRModuleTypes::typeOf(const Value *V) const {
  if (Type *T = Typed.lookup(V))
    return T;
  return Table->typedForm(V->getType());
}

FunctionType *AIRModuleTypes::functionType(const Function &F) const {
  if (Type *T = Aux.lookup(&F))
    return cast<FunctionType>(T);
  return cast<FunctionType>(Table->typedForm(F.getFunctionType()));
}

Type *AIRModuleTypes::valueType(const GlobalVariable &G) const {
  if (Type *T = Aux.lookup(&G))
    return T;
  return Table->typedForm(G.getValueType());
}

Type *AIRModuleTypes::sourceType(const GEPOperator &G) const {
  if (Type *T = Aux.lookup(&G))
    return T;
  return Table->typedForm(G.getSourceElementType());
}

Type *AIRModuleTypes::allocatedType(const AllocaInst &AI) const {
  if (Type *T = Aux.lookup(&AI))
    return T;
  return Table->typedForm(AI.getAllocatedType());
}

FunctionType *AIRModuleTypes::callType(const CallBase &CB) const {
  if (Type *T = Aux.lookup(&CB))
    return cast<FunctionType>(T);
  return cast<FunctionType>(Table->typedForm(CB.getFunctionType()));
}

Expected<AIRModuleTypes> AIRModuleTypes::run(const Module &M,
                                             const AIRTypeTable &T) {
  AIRModuleTypes R;
  R.Table = &T;
  auto Fail = [&M](const Twine &Construct, const Twine &Why,
                   const Value *In) {
    return error(Construct + ": " + Why +
                 (In ? " (in " + str(In, &M) + ")" : std::string()));
  };
  // A value whose type holds a pointer needs an entry (or a function's).
  auto Typed = [&](const Value *V, Type *Ty, const Twine &Construct,
                   const Value *In) -> Expected<Type *> {
    if (Type *E = T.entry(V))
      return E;
    if (hasOpaquePointer(Ty))
      return Fail(Construct, "pointer with no typed type", In);
    return T.typedForm(Ty);
  };
  SmallPtrSet<const Constant *, 32> Checked;
  std::function<Error(const Constant *, const Value *)> CheckConstant =
      [&](const Constant *C, const Value *In) -> Error {
    if (isa<GlobalValue>(C) || !Checked.insert(C).second)
      return Error::success();
    if (hasOpaquePointer(C->getType()))
      return Fail("constant " + str(C, &M), "pointer with no typed type", In);
    for (const Use &Op : C->operands())
      if (auto *OC = dyn_cast<Constant>(Op.get()))
        if (Error E = CheckConstant(OC, In))
          return E;
    return Error::success();
  };

  for (const GlobalVariable &G : M.globals()) {
    Expected<Type *> VT =
        Typed(&G, G.getValueType(), "global " + str(&G, &M), nullptr);
    if (!VT)
      return VT.takeError();
    R.Aux[&G] = *VT;
    R.Typed[&G] = TypedPointerType::get(*VT, G.getAddressSpace());
    if (G.hasInitializer())
      if (Error E = CheckConstant(G.getInitializer(), &G))
        return std::move(E);
  }

  for (const Function &F : M) {
    auto *FT = cast_or_null<FunctionType>(T.entry(&F));
    if (!FT) {
      if (hasOpaquePointer(F.getReturnType()))
        return Fail("function " + str(&F, &M),
                    "returns a pointer with no typed type", nullptr);
      SmallVector<Type *, 8> Params;
      for (const Argument &A : F.args()) {
        Expected<Type *> AT =
            Typed(&A, A.getType(), "argument " + str(&A, &M), &F);
        if (!AT)
          return AT.takeError();
        Params.push_back(*AT);
      }
      FT = FunctionType::get(T.typedForm(F.getReturnType()), Params,
                             F.isVarArg());
    }
    for (const Argument &A : F.args()) {
      Type *PT = FT->getParamType(A.getArgNo());
      if (Type *AT = T.entry(&A); AT && AT != PT)
        return Fail("argument " + str(&A, &M),
                    "its entry " + str(AT) + " is not its function's " +
                        str(PT),
                    &F);
      R.Typed[&A] = PT;
    }
    R.Aux[&F] = FT;
    R.Typed[&F] = TypedPointerType::get(FT, F.getAddressSpace());
  }

  for (const Function &F : M)
    for (const Instruction &I : instructions(F)) {
      auto Construct = [&] {
        return std::string(I.getOpcodeName()) +
               (I.getType()->isVoidTy() ? "" : " " + str(&I, &M));
      };
      if (!I.getType()->isVoidTy()) {
        Expected<Type *> IT = Typed(&I, I.getType(), Construct(), &F);
        if (!IT)
          return IT.takeError();
        R.Typed[&I] = *IT;
      }
      if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        Type *Src = GEP->getSourceElementType();
        if (!hasOpaquePointer(Src)) {
          R.Aux[&I] = T.typedForm(Src);
        } else {
          // The base's typed pointee, when it erases to the source type.
          auto *P = dyn_cast<TypedPointerType>(
              R.typeOf(GEP->getPointerOperand()));
          if (!P || T.erase(P->getElementType()) != Src)
            return Fail(Construct(),
                        "its source type holds a pointer its base's typed "
                        "type does not give",
                        &F);
          R.Aux[&I] = P->getElementType();
        }
      } else if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        auto *P = dyn_cast<TypedPointerType>(R.Typed.lookup(&I));
        if (!P || T.erase(P->getElementType()) != AI->getAllocatedType())
          return Fail(Construct(),
                      "its typed type does not point to its allocated type",
                      &F);
        R.Aux[&I] = P->getElementType();
      } else if (auto *CB = dyn_cast<CallBase>(&I)) {
        auto *Callee = dyn_cast<Function>(CB->getCalledOperand());
        if (Callee && Callee->getFunctionType() == CB->getFunctionType())
          R.Aux[&I] = R.functionType(*Callee);
        else if (!hasOpaquePointer(CB->getFunctionType()))
          R.Aux[&I] = T.typedForm(CB->getFunctionType());
        else
          return Fail(Construct(),
                      "its function type holds a pointer and is not its "
                      "callee's",
                      &F);
      }
      for (const Use &Op : I.operands())
        if (auto *C = dyn_cast<Constant>(Op.get()))
          if (Error E = CheckConstant(C, &F))
            return std::move(E);
    }
  return std::move(R);
}
