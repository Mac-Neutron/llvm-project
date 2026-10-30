//===- AIRPointerTypes.cpp - Pointee types and casts for AIR --------------===//
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
// Types are partial while the fixpoint runs: a pointer whose pointee nothing
// has given yet points to Hole, an opaque struct of the writer's own. A value
// is refined only by filling holes (and, once, by a handle where it had
// something else), so the fixpoint ends; holes left at the end become i8.
//
// Rounds: the definition facts opaque IR keeps (P5 step 3) run to quiescence,
// then one sweep over the use edges in instruction order (step 4); when a
// sweep changes nothing, a bitcast already in the module that its uses left
// untyped follows its operand, and the rounds go on until nothing changes.
//
//===----------------------------------------------------------------------===//

#include "AIRPointerTypes.h"
#include "AIRBitcodeWriter.h"
#include "AIRTypes.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ModuleSlotTracker.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/TypedPointerType.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <set>
#include <tuple>

using namespace llvm;
using namespace llvm::air;

static Error error(const Twine &Msg) {
  return createStringError(inconvertibleErrorCode(), Msg);
}

static bool isIdentified(Type *T) {
  auto *S = dyn_cast<StructType>(T);
  return S && !S->isLiteral();
}

static Type *pointee(Type *T) {
  auto *P = dyn_cast_or_null<TypedPointerType>(T);
  return P ? P->getElementType() : nullptr;
}

static Type *scalarOf(Type *T) {
  auto *V = dyn_cast_or_null<VectorType>(T);
  return V ? V->getElementType() : T;
}

/// P as a vector if Opaque (the value's own type) is one.
static Type *likeOpaque(Type *Opaque, Type *P) {
  if (auto *V = dyn_cast<VectorType>(Opaque))
    return VectorType::get(P, V->getElementCount());
  return P;
}

static unsigned addrSpace(const Value *V) {
  return V->getType()->getScalarType()->getPointerAddressSpace();
}

/// T's shape with every opaque pointer pointing to Hole: what a value's type
/// says before anything types it. Identified structs are their typed forms.
static Type *skeleton(Type *T, const AIRTypeTable &Tab, Type *Hole) {
  if (auto *P = dyn_cast<PointerType>(T))
    return TypedPointerType::get(Hole, P->getAddressSpace());
  if (isIdentified(T))
    return Tab.typedForm(T);
  if (!T->getNumContainedTypes())
    return T;
  return mapContained(T, [&](Type *E) { return skeleton(E, Tab, Hole); });
}

/// T with every hole an i8.
static Type *fillHoles(Type *T, Type *Hole) {
  if (T == Hole)
    return Type::getInt8Ty(T->getContext());
  if (isIdentified(T) || !T->getNumContainedTypes())
    return T;
  return mapContained(T, [&](Type *E) { return fillHoles(E, Hole); });
}

/// Whether a pointer that is T itself (or an element of the literal
/// aggregate T) still points to Hole: a pointer of the value's own that
/// nothing reached.
static bool ownHole(Type *T, Type *Hole) {
  if (auto *P = dyn_cast<TypedPointerType>(T))
    return P->getElementType() == Hole;
  if (isIdentified(T) || isa<FunctionType>(T))
    return false;
  return any_of(T->subtypes(), [&](Type *E) { return ownHole(E, Hole); });
}

static unsigned pointerDepth(Type *T) {
  if (auto *P = dyn_cast<TypedPointerType>(T))
    return 1 + pointerDepth(P->getElementType());
  if (isIdentified(T))
    return 0;
  unsigned D = 0;
  for (Type *E : T->subtypes())
    D = std::max(D, pointerDepth(E));
  return D;
}

// ponytail: a cap on pointer nesting ends a value typed through itself
// (store %p, %p); raise it if real IR ever nests deeper.
static constexpr unsigned MaxPointerDepth = 16;

static bool isHandle(Type *T, Type *Hole) {
  auto *S = dyn_cast<StructType>(T);
  return S && !S->isLiteral() && S->isOpaque() && T != Hole;
}

namespace {
struct Merge {
  Type *Hole;
  bool MayUpgrade = false;
  bool Upgraded = false;
};
} // namespace

/// A with B's knowledge where A has holes; where both say something
/// different A stays, except that a handle replaces a non-handle once.
static Type *merge(Type *A, Type *B, Merge &S) {
  if (A == B || B == S.Hole)
    return A;
  if (A == S.Hole)
    return B;
  if (A->getTypeID() == B->getTypeID()) {
    switch (A->getTypeID()) {
    case Type::TypedPointerTyID: {
      auto *PA = cast<TypedPointerType>(A), *PB = cast<TypedPointerType>(B);
      if (PA->getAddressSpace() != PB->getAddressSpace())
        break;
      Type *E = merge(PA->getElementType(), PB->getElementType(), S);
      return E == PA->getElementType()
                 ? A
                 : TypedPointerType::get(E, PA->getAddressSpace());
    }
    case Type::StructTyID: {
      auto *SA = cast<StructType>(A), *SB = cast<StructType>(B);
      if (!SA->isLiteral() || !SB->isLiteral() ||
          SA->getNumElements() != SB->getNumElements() ||
          SA->isPacked() != SB->isPacked())
        break;
      SmallVector<Type *, 8> E;
      for (unsigned I = 0, N = SA->getNumElements(); I != N; ++I)
        E.push_back(merge(SA->getElementType(I), SB->getElementType(I), S));
      return equal(E, SA->elements())
                 ? A
                 : StructType::get(A->getContext(), E, SA->isPacked());
    }
    case Type::ArrayTyID: {
      auto *AA = cast<ArrayType>(A), *AB = cast<ArrayType>(B);
      if (AA->getNumElements() != AB->getNumElements())
        break;
      Type *E = merge(AA->getElementType(), AB->getElementType(), S);
      return E == AA->getElementType()
                 ? A
                 : ArrayType::get(E, AA->getNumElements());
    }
    case Type::FixedVectorTyID:
    case Type::ScalableVectorTyID: {
      auto *VA = cast<VectorType>(A), *VB = cast<VectorType>(B);
      if (VA->getElementCount() != VB->getElementCount())
        break;
      Type *E = merge(VA->getElementType(), VB->getElementType(), S);
      return E == VA->getElementType()
                 ? A
                 : VectorType::get(E, VA->getElementCount());
    }
    case Type::FunctionTyID: {
      auto *FA = cast<FunctionType>(A), *FB = cast<FunctionType>(B);
      if (FA->getNumParams() != FB->getNumParams() ||
          FA->isVarArg() != FB->isVarArg())
        break;
      Type *Ret = merge(FA->getReturnType(), FB->getReturnType(), S);
      SmallVector<Type *, 8> Ps;
      for (unsigned I = 0, N = FA->getNumParams(); I != N; ++I)
        Ps.push_back(merge(FA->getParamType(I), FB->getParamType(I), S));
      return Ret == FA->getReturnType() && equal(Ps, FA->params())
                 ? A
                 : FunctionType::get(Ret, Ps, FA->isVarArg());
    }
    default:
      break;
    }
  }
  if (S.MayUpgrade && isHandle(B, S.Hole) && !isHandle(A, S.Hole)) {
    S.MayUpgrade = false;
    S.Upgraded = true;
    return B;
  }
  return A;
}

static unsigned structIndex(const Value *Idx) {
  auto *C = cast<Constant>(Idx);
  if (C->getType()->isVectorTy())
    C = C->getSplatValue();
  return cast<ConstantInt>(C)->getZExtValue();
}

static Type *replaceElement(Type *Agg, unsigned I, Type *New) {
  if (auto *ST = dyn_cast<StructType>(Agg)) {
    // An identified struct's body is the table's.
    if (!ST->isLiteral() || I >= ST->getNumElements())
      return Agg;
    SmallVector<Type *, 8> E(ST->elements());
    E[I] = New;
    return StructType::get(Agg->getContext(), E, ST->isPacked());
  }
  if (auto *AT = dyn_cast<ArrayType>(Agg))
    return ArrayType::get(New, AT->getNumElements());
  if (auto *VT = dyn_cast<VectorType>(Agg))
    return VectorType::get(New, VT->getElementCount());
  return Agg;
}

/// Agg with the element a GEP's indices (after the first) reach set to Elt.
static Type *withAt(Type *Agg, ArrayRef<Value *> Path, Type *Elt) {
  if (Path.empty())
    return Elt;
  Type *Inner = GetElementPtrInst::getTypeAtIndex(Agg, Path.front());
  if (!Inner)
    return Agg;
  Type *New = withAt(Inner, Path.drop_front(), Elt);
  if (New == Inner)
    return Agg;
  return replaceElement(
      Agg, isa<StructType>(Agg) ? structIndex(Path.front()) : 0, New);
}

/// Agg with the element extractvalue's indices reach set to Elt.
static Type *withAtIdx(Type *Agg, ArrayRef<unsigned> Path, Type *Elt) {
  if (Path.empty())
    return Elt;
  Type *Inner = ExtractValueInst::getIndexedType(Agg, Path.take_front());
  if (!Inner)
    return Agg;
  Type *New = withAtIdx(Inner, Path.drop_front(), Elt);
  return New == Inner ? Agg : replaceElement(Agg, Path.front(), New);
}

static SmallVector<Value *, 8> gepIndices(const GEPOperator &G) {
  SmallVector<Value *, 8> Idx;
  for (const Use &U : G.indices())
    Idx.push_back(U.get());
  return Idx;
}

/// The typed type of a GEP with typed source TS: a pointer to what its
/// indices reach, in its address space (a vector of them for a vector GEP).
static Type *gepResult(Type *TS, const GEPOperator &G) {
  Type *E = GetElementPtrInst::getIndexedType(TS, gepIndices(G));
  if (!E || !TypedPointerType::isValidElementType(E))
    return nullptr;
  return likeOpaque(G.getType(),
                    TypedPointerType::get(E, G.getPointerAddressSpace()));
}

/// A constant GEP's typed source: its base's pointee when that erases to
/// the source type, else the source type's skeleton.
static Type *constantSource(const GEPOperator &G, Type *BaseNatural,
                            const AIRTypeTable &Tab, Type *Hole) {
  Type *PE = pointee(scalarOf(BaseNatural));
  if (PE && Tab.erase(PE) == G.getSourceElementType())
    return PE;
  return skeleton(G.getSourceElementType(), Tab, Hole);
}

/// C's own typed type (with holes): a global value's from Leaf; null,
/// undef, poison and inttoptr have no pointee; the rest follow their
/// operands.
static Type *naturalType(const Constant *C, const AIRTypeTable &Tab,
                         Type *Hole,
                         function_ref<Type *(const GlobalValue *)> Leaf) {
  if (auto *GV = dyn_cast<GlobalValue>(C))
    return Leaf(GV);
  auto Self = [&](const Value *Op) {
    return naturalType(cast<Constant>(Op), Tab, Hole, Leaf);
  };
  if (auto *CE = dyn_cast<ConstantExpr>(C)) {
    switch (CE->getOpcode()) {
    case Instruction::GetElementPtr: {
      auto &G = cast<GEPOperator>(*CE);
      Type *TS = constantSource(G, Self(CE->getOperand(0)), Tab, Hole);
      if (Type *R = gepResult(TS, G))
        return R;
      break;
    }
    case Instruction::AddrSpaceCast:
      if (Type *PE = pointee(scalarOf(Self(CE->getOperand(0)))))
        return likeOpaque(CE->getType(),
                          TypedPointerType::get(PE, addrSpace(CE)));
      break;
    default:
      break;
    }
    return skeleton(C->getType(), Tab, Hole);
  }
  if (isa<ConstantAggregate>(C) && !isIdentified(C->getType())) {
    if (auto *ST = dyn_cast<StructType>(C->getType())) {
      SmallVector<Type *, 8> E;
      for (const Use &Op : C->operands())
        E.push_back(Self(Op.get()));
      return StructType::get(C->getContext(), E, ST->isPacked());
    }
    // An array or vector has one element type: the first fact wins.
    Merge S{Hole};
    Type *E = skeleton(C->getType()->getContainedType(0), Tab, Hole);
    for (const Use &Op : C->operands())
      E = merge(E, Self(Op.get()), S);
    if (auto *AT = dyn_cast<ArrayType>(C->getType()))
      return ArrayType::get(E, AT->getNumElements());
    return VectorType::get(E,
                           cast<VectorType>(C->getType())->getElementCount());
  }
  if (auto *BA = dyn_cast<BlockAddress>(C))
    return TypedPointerType::get(Type::getInt8Ty(C->getContext()),
                                 BA->getType()->getPointerAddressSpace());
  return skeleton(C->getType(), Tab, Hole);
}

Type *AIRPointerTypes::natural(const Constant *C) const {
  if (Type *T = Natural.lookup(C))
    return T;
  Type *T = fillHoles(naturalType(C, *Table, Hole,
                             [&](const GlobalValue *GV) -> Type * {
                               if (Type *T = Typed.lookup(GV))
                                 return T;
                               return skeleton(GV->getType(), *Table, Hole);
                             }),
                 Hole);
  Natural[C] = T;
  return T;
}

Type *AIRPointerTypes::typedTypeOf(const Value *V) const {
  if (const Synthetic *S = synthetic(V))
    return S->T;
  if (Type *T = Typed.lookup(V))
    return T;
  if (auto *C = dyn_cast<Constant>(V))
    return natural(C);
  return Table->typedForm(V->getType());
}

const Value *AIRPointerTypes::typedOperand(const Use &U) const {
  if (const Value *V = Operands.lookup(&U))
    return V;
  return U.get();
}

const AIRPointerTypes::Synthetic *
AIRPointerTypes::synthetic(const Value *V) const {
  auto It = SyntheticIndex.find(V);
  return It == SyntheticIndex.end() ? nullptr : &Synthetics[It->second];
}

FunctionType *AIRPointerTypes::functionType(const Function &F) const {
  return cast<FunctionType>(Aux.lookup(&F));
}

Type *AIRPointerTypes::valueType(const GlobalVariable &G) const {
  return pointee(Typed.lookup(&G));
}

Type *AIRPointerTypes::sourceType(const GEPOperator &G) const {
  if (isa<Instruction>(&G))
    return Aux.lookup(&G);
  return fillHoles(constantSource(G, typedTypeOf(G.getPointerOperand()), *Table,
                             Hole),
              Hole);
}

Type *AIRPointerTypes::allocatedType(const AllocaInst &AI) const {
  return pointee(Typed.lookup(&AI));
}

FunctionType *AIRPointerTypes::callType(const CallBase &CB) const {
  return cast<FunctionType>(Aux.lookup(&CB));
}

Type *AIRPointerTypes::form(Type *T) const { return Table->typedForm(T); }

StringRef AIRPointerTypes::nameOf(const StructType *S) const {
  return Table->nameOf(S);
}

class AIRPointerTypes::Solver {
public:
  Solver(Module &M, const AIRTypeTable &T, AIRWriteReport &R,
         AIRPointerTypes &Out)
      : M(M), T(T), R(R), Out(Out), Ctx(M.getContext()), MST(&M, false) {}

  Error run();

private:
  struct Node {
    Type *T;
    bool Fixed = false;
    bool Upgraded = false;
    bool Derived = false;
  };
  enum class Owner { Value, Source, Return };

  Module &M;
  const AIRTypeTable &T;
  AIRWriteReport &R;
  AIRPointerTypes &Out;
  LLVMContext &Ctx;
  Type *Hole = nullptr;
  std::vector<Node> Nodes;
  std::vector<std::pair<const Value *, Owner>> Owners;
  DenseMap<const Value *, unsigned> ValueNode;
  DenseMap<const Value *, unsigned> SourceNode;
  DenseMap<const Function *, unsigned> ReturnNode;
  std::vector<Instruction *> Insts;
  std::map<std::pair<const Constant *, Type *>, const Value *> Instances;
  std::set<const Constant *> Prepared;
  std::map<std::tuple<const Value *, Type *, const Instruction *>,
           Instruction *>
      Casts;
  ModuleSlotTracker MST;
  const Function *Incorporated = nullptr;

  unsigned add(const Value *V, Owner O, Type *Ty, bool Fixed) {
    Nodes.push_back({Ty, Fixed});
    Owners.push_back({V, O});
    return Nodes.size() - 1;
  }
  int nodeOf(const Value *V) const {
    auto It = ValueNode.find(V);
    return It == ValueNode.end() ? -1 : int(It->second);
  }
  Type *skel(Type *Ty) { return skeleton(Ty, T, Hole); }
  bool fits(Type *Typed, Type *Opaque) const {
    return Typed && Typed != Hole && T.erase(Typed) == Opaque;
  }

  bool give(int N, Type *Cand, bool Backward);
  Type *cur(const Value *V);
  FunctionType *fnType(const Function &F);
  Type *returnType(const Function &F);

  Error init();
  bool forward(Instruction &I);
  bool backward(Instruction &I);
  bool fallback(Instruction &I);
  void solve();
  Error finish();

  Type *typedOf(const Value *V) const { return Out.typedTypeOf(V); }
  Type *valueTypeOf(const AtomicCmpXchgInst &I);
  Type *valueTypeOf(const AtomicRMWInst &I);
  Type *required(Instruction &I, unsigned OpNo);
  Expected<const Value *> instance(const Constant *C, Type *Want);
  Error prepare(const Constant *C);
  Error castOperand(Instruction &I, Use &U, Type *Want);

  std::string name(const Value *V);
  std::string user(Instruction &I, const Use &U);
};

bool AIRPointerTypes::Solver::give(int N, Type *Cand, bool Backward) {
  if (N < 0 || !Cand)
    return false;
  Node &Nd = Nodes[N];
  if (Nd.Fixed)
    return false;
  Merge S{Hole};
  S.MayUpgrade = !Nd.Upgraded;
  Type *New = merge(Nd.T, Cand, S);
  if (New == Nd.T || pointerDepth(New) > MaxPointerDepth)
    return false;
  Nd.T = New;
  Nd.Upgraded |= S.Upgraded;
  Nd.Derived |= Backward;
  return true;
}

Type *AIRPointerTypes::Solver::returnType(const Function &F) {
  if (auto *FT = cast_or_null<FunctionType>(T.entry(&F)))
    return FT->getReturnType();
  auto It = ReturnNode.find(&F);
  return It == ReturnNode.end() ? skel(F.getReturnType())
                                : Nodes[It->second].T;
}

FunctionType *AIRPointerTypes::Solver::fnType(const Function &F) {
  if (auto *FT = cast_or_null<FunctionType>(T.entry(&F)))
    return FT;
  SmallVector<Type *, 8> Ps;
  for (const Argument &A : F.args())
    Ps.push_back(cur(&A));
  return FunctionType::get(returnType(F), Ps, F.isVarArg());
}

Type *AIRPointerTypes::Solver::cur(const Value *V) {
  if (int N = nodeOf(V); N >= 0)
    return Nodes[N].T;
  if (auto *F = dyn_cast<Function>(V))
    return TypedPointerType::get(fnType(*F), F->getAddressSpace());
  if (auto *C = dyn_cast<Constant>(V))
    return naturalType(C, T, Hole, [&](const GlobalValue *GV) {
      return cur(GV);
    });
  return skel(V->getType());
}

Error AIRPointerTypes::Solver::init() {
  Hole = StructType::create(Ctx);
  Out.Hole = Hole;
  for (GlobalVariable &G : M.globals()) {
    Type *VT = T.entry(&G);
    ValueNode[&G] = add(
        &G, Owner::Value,
        TypedPointerType::get(VT ? VT : skel(G.getValueType()),
                              G.getAddressSpace()),
        VT != nullptr);
  }
  for (GlobalAlias &A : M.aliases())
    ValueNode[&A] = add(&A, Owner::Value, skel(A.getType()), false);
  for (Function &F : M) {
    auto *FT = cast_or_null<FunctionType>(T.entry(&F));
    for (Argument &A : F.args()) {
      Type *Entry = T.entry(&A);
      if (FT && Entry && Entry != FT->getParamType(A.getArgNo()))
        return error("argument " + name(&A) + ": its entry " + T.print(Entry) +
                     " is not its function's " +
                     T.print(FT->getParamType(A.getArgNo())) + " (in @" +
                     F.getName() + ")");
      if (FT)
        Entry = FT->getParamType(A.getArgNo());
      if (Entry || hasOpaquePointer(A.getType()))
        ValueNode[&A] =
            add(&A, Owner::Value, Entry ? Entry : skel(A.getType()),
                Entry != nullptr);
    }
    if (!FT && hasOpaquePointer(F.getReturnType()))
      ReturnNode[&F] = add(&F, Owner::Return, skel(F.getReturnType()), false);
    for (Instruction &I : instructions(F)) {
      Insts.push_back(&I);
      if (!I.getType()->isVoidTy()) {
        Type *Entry = T.entry(&I);
        if (Entry || hasOpaquePointer(I.getType())) {
          Type *Init = Entry;
          if (!Init)
            if (auto *AI = dyn_cast<AllocaInst>(&I))
              Init = TypedPointerType::get(skel(AI->getAllocatedType()),
                                           AI->getAddressSpace());
          ValueNode[&I] = add(&I, Owner::Value, Init ? Init : skel(I.getType()),
                              Entry != nullptr);
        }
      }
      if (auto *G = dyn_cast<GetElementPtrInst>(&I))
        SourceNode[&I] =
            add(&I, Owner::Source, skel(G->getSourceElementType()), false);
    }
  }
  return Error::success();
}

bool AIRPointerTypes::Solver::forward(Instruction &I) {
  int N = nodeOf(&I);
  auto Give = [&](Type *Cand) { return give(N, Cand, false); };
  switch (I.getOpcode()) {
  case Instruction::Load:
    if (Type *PE = pointee(cur(I.getOperand(0))); fits(PE, I.getType()))
      return Give(PE);
    return false;
  case Instruction::GetElementPtr: {
    auto &G = cast<GEPOperator>(I);
    unsigned S = SourceNode[&I];
    bool Changed = false;
    if (Type *PE = pointee(scalarOf(cur(G.getPointerOperand())));
        fits(PE, G.getSourceElementType()))
      Changed |= give(S, PE, false);
    Changed |= Give(gepResult(Nodes[S].T, G));
    return Changed;
  }
  case Instruction::AddrSpaceCast:
    if (Type *PE = pointee(scalarOf(cur(I.getOperand(0)))))
      return Give(
          likeOpaque(I.getType(), TypedPointerType::get(PE, addrSpace(&I))));
    return false;
  case Instruction::ExtractValue:
    return Give(ExtractValueInst::getIndexedType(
        cur(I.getOperand(0)), cast<ExtractValueInst>(I).getIndices()));
  case Instruction::InsertValue:
  case Instruction::InsertElement:
  case Instruction::Freeze:
    return Give(cur(I.getOperand(0)));
  case Instruction::ExtractElement:
    if (auto *VT = dyn_cast<VectorType>(cur(I.getOperand(0))))
      return Give(VT->getElementType());
    return false;
  case Instruction::ShuffleVector:
    if (auto *VT = dyn_cast<VectorType>(cur(I.getOperand(0))))
      return Give(
          VectorType::get(VT->getElementType(),
                          cast<VectorType>(I.getType())->getElementCount()));
    return false;
  case Instruction::AtomicCmpXchg: {
    Type *I1 = Type::getInt1Ty(Ctx);
    bool A = Give(StructType::get(Ctx, {cur(I.getOperand(1)), I1}));
    return Give(StructType::get(Ctx, {cur(I.getOperand(2)), I1})) || A;
  }
  case Instruction::AtomicRMW:
    return Give(cur(I.getOperand(1)));
  case Instruction::Call:
  case Instruction::Invoke:
    if (const Function *F = cast<CallBase>(I).getCalledFunction())
      return Give(returnType(*F));
    return false;
  default:
    return false;
  }
}

bool AIRPointerTypes::Solver::backward(Instruction &I) {
  bool Changed = false;
  auto G = [&](const Value *V, Type *Cand) {
    Changed |= give(nodeOf(V), Cand, true);
  };
  auto PtrTo = [](Type *E, const Value *P) -> Type * {
    if (!E || !TypedPointerType::isValidElementType(E))
      return nullptr;
    return likeOpaque(P->getType(), TypedPointerType::get(E, addrSpace(P)));
  };
  switch (I.getOpcode()) {
  case Instruction::Load:
    G(I.getOperand(0), PtrTo(cur(&I), I.getOperand(0)));
    break;
  case Instruction::Store: {
    Value *V = I.getOperand(0), *P = I.getOperand(1);
    G(P, PtrTo(cur(V), P));
    if (Type *PE = pointee(cur(P)); fits(PE, V->getType()))
      G(V, PE);
    break;
  }
  case Instruction::AtomicCmpXchg: {
    Value *P = I.getOperand(0), *Cmp = I.getOperand(1), *New = I.getOperand(2);
    G(P, PtrTo(cur(Cmp), P));
    G(P, PtrTo(cur(New), P));
    G(Cmp, cur(New));
    G(New, cur(Cmp));
    if (Type *PE = pointee(cur(P)); fits(PE, Cmp->getType())) {
      G(Cmp, PE);
      G(New, PE);
    }
    if (auto *ST = dyn_cast<StructType>(cur(&I))) {
      G(Cmp, ST->getElementType(0));
      G(New, ST->getElementType(0));
    }
    break;
  }
  case Instruction::AtomicRMW: {
    Value *P = I.getOperand(0), *V = I.getOperand(1);
    G(P, PtrTo(cur(V), P));
    if (Type *PE = pointee(cur(P)); fits(PE, V->getType()))
      G(V, PE);
    G(V, cur(&I));
    break;
  }
  case Instruction::GetElementPtr: {
    auto &GEP = cast<GEPOperator>(I);
    unsigned S = SourceNode[&I];
    G(GEP.getPointerOperand(), PtrTo(Nodes[S].T, GEP.getPointerOperand()));
    if (Type *E = pointee(scalarOf(cur(&I)))) {
      SmallVector<Value *, 8> Idx = gepIndices(GEP);
      Changed |= give(S, withAt(Nodes[S].T, ArrayRef(Idx).drop_front(), E),
                      true);
    }
    break;
  }
  case Instruction::PHI:
    for (const Use &U : I.operands()) {
      G(&I, cur(U.get()));
      G(U.get(), cur(&I));
    }
    break;
  case Instruction::Select:
    G(&I, cur(I.getOperand(1)));
    G(&I, cur(I.getOperand(2)));
    G(I.getOperand(1), cur(&I));
    G(I.getOperand(2), cur(&I));
    break;
  case Instruction::Freeze:
    G(I.getOperand(0), cur(&I));
    break;
  case Instruction::BitCast:
  case Instruction::AddrSpaceCast:
    if (I.getType()->isPtrOrPtrVectorTy())
      G(I.getOperand(0),
        PtrTo(pointee(scalarOf(cur(&I))), I.getOperand(0)));
    break;
  case Instruction::ExtractValue:
    G(I.getOperand(0), withAtIdx(cur(I.getOperand(0)),
                                 cast<ExtractValueInst>(I).getIndices(),
                                 cur(&I)));
    break;
  case Instruction::InsertValue: {
    ArrayRef<unsigned> Idx = cast<InsertValueInst>(I).getIndices();
    G(I.getOperand(0), cur(&I));
    G(I.getOperand(1), ExtractValueInst::getIndexedType(cur(&I), Idx));
    G(&I, withAtIdx(cur(&I), Idx, cur(I.getOperand(1))));
    break;
  }
  case Instruction::ExtractElement:
    if (auto *VT = dyn_cast<VectorType>(I.getOperand(0)->getType()))
      G(I.getOperand(0), VectorType::get(cur(&I), VT->getElementCount()));
    break;
  case Instruction::InsertElement: {
    G(I.getOperand(0), cur(&I));
    auto *VT = cast<VectorType>(cur(&I));
    G(I.getOperand(1), VT->getElementType());
    G(&I, VectorType::get(cur(I.getOperand(1)), VT->getElementCount()));
    break;
  }
  case Instruction::ShuffleVector: {
    Type *E = cast<VectorType>(cur(&I))->getElementType();
    for (unsigned Op = 0; Op != 2; ++Op)
      G(I.getOperand(Op),
        VectorType::get(E, cast<VectorType>(I.getOperand(Op)->getType())
                               ->getElementCount()));
    break;
  }
  case Instruction::ICmp:
    if (hasOpaquePointer(I.getOperand(0)->getType())) {
      G(I.getOperand(0), cur(I.getOperand(1)));
      G(I.getOperand(1), cur(I.getOperand(0)));
    }
    break;
  case Instruction::Call:
  case Instruction::Invoke: {
    auto &CB = cast<CallBase>(I);
    if (const Function *F = CB.getCalledFunction()) {
      FunctionType *FT = F->getFunctionType();
      bool HasEntry = T.entry(F);
      for (unsigned A = 0, E = FT->getNumParams(); A != E; ++A) {
        G(CB.getArgOperand(A), HasEntry ? cast<FunctionType>(T.entry(F))
                                              ->getParamType(A)
                                        : cur(F->getArg(A)));
        if (!HasEntry)
          G(F->getArg(A), cur(CB.getArgOperand(A)));
      }
      if (auto It = ReturnNode.find(F); It != ReturnNode.end())
        Changed |= give(It->second, cur(&CB), true);
    } else if (nodeOf(CB.getCalledOperand()) >= 0) {
      FunctionType *FT = CB.getFunctionType();
      SmallVector<Type *, 8> Ps;
      for (unsigned A = 0, E = FT->getNumParams(); A != E; ++A)
        Ps.push_back(cur(CB.getArgOperand(A)));
      G(CB.getCalledOperand(),
        PtrTo(FunctionType::get(cur(&CB), Ps, FT->isVarArg()),
              CB.getCalledOperand()));
    }
    break;
  }
  case Instruction::Ret:
    if (I.getNumOperands()) {
      const Function &F = *I.getFunction();
      if (auto It = ReturnNode.find(&F); It != ReturnNode.end())
        Changed |= give(It->second, cur(I.getOperand(0)), true);
      G(I.getOperand(0), returnType(F));
    }
    break;
  default:
    break;
  }
  return Changed;
}

bool AIRPointerTypes::Solver::fallback(Instruction &I) {
  if (I.getOpcode() != Instruction::BitCast ||
      !I.getType()->isPtrOrPtrVectorTy())
    return false;
  Type *PE = pointee(scalarOf(cur(I.getOperand(0))));
  if (!PE)
    return false;
  return give(nodeOf(&I),
              likeOpaque(I.getType(), TypedPointerType::get(PE, addrSpace(&I))),
              false);
}

void AIRPointerTypes::Solver::solve() {
  SmallVector<GlobalVariable *, 8> Inits;
  for (GlobalVariable &G : M.globals())
    if (G.hasInitializer() && hasOpaquePointer(G.getValueType()))
      Inits.push_back(&G);
  for (;;) {
    bool Changed;
    do {
      Changed = false;
      for (Instruction *I : Insts)
        Changed |= forward(*I);
    } while (Changed);
    for (Instruction *I : Insts)
      Changed |= backward(*I);
    for (GlobalVariable *G : Inits)
      Changed |= give(nodeOf(G),
                      TypedPointerType::get(cur(G->getInitializer()),
                                            G->getAddressSpace()),
                      true);
    for (GlobalAlias &A : M.aliases())
      Changed |= give(nodeOf(&A), cur(A.getAliasee()), false);
    if (Changed)
      continue;
    for (Instruction *I : Insts)
      Changed |= fallback(*I);
    if (!Changed)
      return;
  }
}

std::string AIRPointerTypes::Solver::name(const Value *V) {
  const Function *F = nullptr;
  if (auto *I = dyn_cast<Instruction>(V))
    F = I->getFunction();
  else if (auto *A = dyn_cast<Argument>(V))
    F = A->getParent();
  else if (auto *BB = dyn_cast<BasicBlock>(V))
    F = BB->getParent();
  if (F && F != Incorporated) {
    MST.incorporateFunction(*F);
    Incorporated = F;
  }
  std::string S;
  raw_string_ostream OS(S);
  V->printAsOperand(OS, /*PrintType=*/false, MST);
  return StringRef(S).take_front(80).str();
}

std::string AIRPointerTypes::Solver::user(Instruction &I, const Use &U) {
  std::string D = I.getOpcodeName();
  if (auto *CB = dyn_cast<CallBase>(&I)) {
    D += " " + name(CB->getCalledOperand());
    if (CB->isArgOperand(&U))
      D += " arg " + std::to_string(CB->getArgOperandNo(&U));
    else if (CB->isCallee(&U))
      D += " callee";
    return D;
  }
  if (!I.getType()->isVoidTy())
    D += " " + name(&I);
  if (auto *PN = dyn_cast<PHINode>(&I))
    D += " from " + name(PN->getIncomingBlock(U));
  return D;
}

/// cmpxchg's value type: its first non-constant value operand's, else what
/// its pointer points to, else its comparand's own.
Type *AIRPointerTypes::Solver::valueTypeOf(const AtomicCmpXchgInst &I) {
  for (const Value *V : {I.getCompareOperand(), I.getNewValOperand()})
    if (!isa<Constant>(V))
      return typedOf(V);
  if (Type *PE = pointee(typedOf(I.getPointerOperand()));
      fits(PE, I.getCompareOperand()->getType()))
    return PE;
  return typedOf(I.getCompareOperand());
}

Type *AIRPointerTypes::Solver::valueTypeOf(const AtomicRMWInst &I) {
  if (!isa<Constant>(I.getValOperand()))
    return typedOf(I.getValOperand());
  if (Type *PE = pointee(typedOf(I.getPointerOperand()));
      fits(PE, I.getValOperand()->getType()))
    return PE;
  return typedOf(I.getValOperand());
}

/// The typed type operand OpNo of I must have, or null for none.
Type *AIRPointerTypes::Solver::required(Instruction &I, unsigned OpNo) {
  auto PtrTo = [](Type *E, const Value *P) -> Type * {
    return likeOpaque(P->getType(), TypedPointerType::get(E, addrSpace(P)));
  };
  switch (I.getOpcode()) {
  case Instruction::Load:
    return PtrTo(typedOf(&I), I.getOperand(0));
  case Instruction::Store: {
    Value *V = I.getOperand(0);
    Type *VT = typedOf(V);
    if (isa<Constant>(V))
      if (Type *PE = pointee(typedOf(I.getOperand(1))); fits(PE, V->getType()))
        VT = PE;
    return OpNo == 0 ? VT : PtrTo(VT, I.getOperand(1));
  }
  case Instruction::AtomicCmpXchg: {
    Type *VT = valueTypeOf(cast<AtomicCmpXchgInst>(I));
    return OpNo == 0 ? PtrTo(VT, I.getOperand(0)) : VT;
  }
  case Instruction::AtomicRMW: {
    Type *VT = valueTypeOf(cast<AtomicRMWInst>(I));
    return OpNo == 0 ? PtrTo(VT, I.getOperand(0)) : VT;
  }
  case Instruction::GetElementPtr:
    return OpNo == 0 ? PtrTo(Out.Aux.lookup(&I), I.getOperand(0)) : nullptr;
  case Instruction::Call:
  case Instruction::Invoke: {
    auto &CB = cast<CallBase>(I);
    FunctionType *CT = Out.callType(CB);
    const Use &U = I.getOperandUse(OpNo);
    if (CB.isCallee(&U))
      return PtrTo(CT, CB.getCalledOperand());
    if (CB.isArgOperand(&U) && CB.getArgOperandNo(&U) < CT->getNumParams())
      return CT->getParamType(CB.getArgOperandNo(&U));
    return nullptr;
  }
  case Instruction::Ret:
    return Out.functionType(*I.getFunction())->getReturnType();
  case Instruction::ICmp:
    if (!hasOpaquePointer(I.getOperand(0)->getType()))
      return nullptr;
    return typedOf(isa<Constant>(I.getOperand(0)) &&
                         !isa<Constant>(I.getOperand(1))
                     ? I.getOperand(1)
                     : I.getOperand(0));
  case Instruction::Select:
    return OpNo == 0 ? nullptr : typedOf(&I);
  case Instruction::PHI:
  case Instruction::Freeze:
    return typedOf(&I);
  case Instruction::InsertValue:
    return OpNo == 0 ? typedOf(&I)
                     : ExtractValueInst::getIndexedType(
                           typedOf(&I), cast<InsertValueInst>(I).getIndices());
  case Instruction::InsertElement:
    if (OpNo == 2)
      return nullptr;
    return OpNo == 0 ? typedOf(&I)
                     : cast<VectorType>(typedOf(&I))->getElementType();
  case Instruction::ShuffleVector:
    if (OpNo > 1)
      return nullptr;
    return VectorType::get(
        cast<VectorType>(typedOf(&I))->getElementType(),
        cast<VectorType>(I.getOperand(OpNo)->getType())->getElementCount());
  default:
    return nullptr;
  }
}

/// Why no bitcast turns Have into Want, or an empty string if one does.
static std::string uncastable(Type *Have, Type *Want) {
  auto *A = dyn_cast<TypedPointerType>(scalarOf(Have));
  auto *B = dyn_cast<TypedPointerType>(scalarOf(Want));
  if (!A || !B || isa<VectorType>(Have) != isa<VectorType>(Want) ||
      (isa<VectorType>(Have) && cast<VectorType>(Have)->getElementCount() !=
                                    cast<VectorType>(Want)->getElementCount()))
    return "no cast reaches an aggregate";
  if (A->getAddressSpace() != B->getAddressSpace())
    return "address space " + std::to_string(B->getAddressSpace()) +
           " is not its address space " +
           std::to_string(A->getAddressSpace()) +
           ", and the writer invents no addrspacecast";
  return "";
}

Error AIRPointerTypes::Solver::prepare(const Constant *C) {
  if (isa<GlobalValue>(C) || !Prepared.insert(C).second)
    return Error::success();
  Type *Own = Out.natural(C);
  for (const Use &U : C->operands()) {
    auto *Op = dyn_cast<Constant>(U.get());
    if (!Op)
      continue;
    Type *Want = nullptr;
    if (auto *G = dyn_cast<GEPOperator>(C); G && U.getOperandNo() == 0)
      Want = likeOpaque(Op->getType(),
                        TypedPointerType::get(Out.sourceType(*G),
                                              G->getPointerAddressSpace()));
    else if (isa<ConstantAggregate>(C))
      Want = isa<StructType>(Own)
                 ? cast<StructType>(Own)->getElementType(U.getOperandNo())
                 : Own->getContainedType(0);
    if (!Want) {
      if (Error E = prepare(Op))
        return E;
      continue;
    }
    Expected<const Value *> V = instance(Op, Want);
    if (!V)
      return V.takeError();
    if (*V != Op)
      Out.Operands[&U] = *V;
  }
  return Error::success();
}

Expected<const Value *> AIRPointerTypes::Solver::instance(const Constant *C,
                                                           Type *Want) {
  Type *Own = Out.natural(C);
  if (Own == Want) {
    if (Error E = prepare(C))
      return std::move(E);
    return C;
  }
  auto Key = std::make_pair(C, Want);
  if (auto It = Instances.find(Key); It != Instances.end())
    return It->second;
  Synthetic S{C, Want, false, {}};
  auto Sub = [&](const Value *Op, Type *W) -> Error {
    Expected<const Value *> V = instance(cast<Constant>(Op), W);
    if (!V)
      return V.takeError();
    S.Ops.push_back(*V);
    return Error::success();
  };
  auto *CE = dyn_cast<ConstantExpr>(C);
  unsigned Opc = CE ? CE->getOpcode() : 0;
  if (isa<ConstantPointerNull>(C) || isa<UndefValue>(C) ||
      isa<ConstantAggregateZero>(C)) {
    // Written at Want, with no operands.
  } else if (isa<ConstantAggregate>(C)) {
    for (const Use &Op : C->operands())
      if (Error E = Sub(Op.get(),
                        isa<StructType>(Want)
                            ? cast<StructType>(Want)->getElementType(
                                  Op.getOperandNo())
                            : Want->getContainedType(0)))
        return std::move(E);
  } else if (Opc == Instruction::IntToPtr) {
    if (Error E = prepare(CE->getOperand(0)))
      return std::move(E);
    S.Ops.push_back(CE->getOperand(0));
  } else if (Opc == Instruction::AddrSpaceCast) {
    Type *PE = pointee(scalarOf(Want));
    const Value *Op = CE->getOperand(0);
    if (Error E = Sub(Op, likeOpaque(Op->getType(),
                                     TypedPointerType::get(PE, addrSpace(Op)))))
      return std::move(E);
  } else {
    // A constant with a pointee of its own: a bitcast of it.
    std::string Why = uncastable(Own, Want);
    if (!Why.empty())
      return error("constant " + name(C) + ": needs " + T.print(Want) +
                   ", is " + T.print(Own) + ", and " + Why);
    if (Error E = prepare(C))
      return std::move(E);
    S.Cast = true;
    S.Ops.push_back(C);
    ++R.Stats.ConstCasts;
    R.Explain.push_back("cast " + name(C) + " to " + T.print(Want) +
                        " (constant)");
  }
  Out.Proxies.emplace_back(new Argument(C->getType()));
  const Value *Proxy = Out.Proxies.back().get();
  Out.SyntheticIndex[Proxy] = Out.Synthetics.size();
  Out.Synthetics.push_back(std::move(S));
  Instances[Key] = Proxy;
  return Proxy;
}

Error AIRPointerTypes::Solver::castOperand(Instruction &I, Use &U,
                                            Type *Want) {
  Value *V = U.get();
  Type *Have = typedOf(V);
  std::string Why = uncastable(Have, Want);
  if (!Why.empty())
    return error(user(I, U) + ": needs " + T.print(Want) + ", " + name(V) +
                 " is " + T.print(Have) + ", and " + Why + " (in @" +
                 I.getFunction()->getName() + ")");
  Instruction *At = &I;
  if (auto *PN = dyn_cast<PHINode>(&I))
    At = PN->getIncomingBlock(U)->getTerminator();
  Instruction *&Cast = Casts[{V, Want, At}];
  if (!Cast) {
    R.Explain.push_back("cast " + name(V) + " to " + T.print(Want) + " for " +
                        user(I, U) + " (in @" +
                        I.getFunction()->getName().str() + ")");
    Cast = new BitCastInst(V, V->getType(), "", At->getIterator());
    Out.Typed[Cast] = Want;
    ++R.Stats.InstCasts;
  }
  U.set(Cast);
  return Error::success();
}

Error AIRPointerTypes::Solver::finish() {
  // Holes left are i8; a pointer of a value's own that nothing reached is
  // counted (a global's: in its value type).
  std::vector<bool> UseTyped;
  for (unsigned I = 0, E = Nodes.size(); I != E; ++I) {
    auto [V, O] = Owners[I];
    Type *Own = isa<GlobalVariable>(V) ? pointee(Nodes[I].T) : Nodes[I].T;
    UseTyped.push_back(O != Owner::Source && ownHole(Own, Hole));
    Nodes[I].T = fillHoles(Nodes[I].T, Hole);
  }
  // Values, then functions (whose types are built from their arguments'),
  // then what follows from both.
  for (unsigned I = 0, E = Nodes.size(); I != E; ++I) {
    auto [V, O] = Owners[I];
    if (O == Owner::Value)
      Out.Typed[V] = Nodes[I].T;
    else if (O == Owner::Source)
      Out.Aux[V] = Nodes[I].T;
  }
  for (Function &F : M) {
    FunctionType *FT = fnType(F);
    Out.Aux[&F] = FT;
    Out.Typed[&F] = TypedPointerType::get(FT, F.getAddressSpace());
  }
  // Types the LLVM 15 reader derives from operands follow them, whatever
  // the fixpoint said; passes until none changes (only extracts chain).
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (Instruction *I : Insts) {
      Type *New = nullptr;
      switch (I->getOpcode()) {
      case Instruction::GetElementPtr:
        New = gepResult(Out.Aux.lookup(I), cast<GEPOperator>(*I));
        break;
      case Instruction::ExtractValue:
        New = ExtractValueInst::getIndexedType(
            typedOf(I->getOperand(0)), cast<ExtractValueInst>(I)->getIndices());
        break;
      case Instruction::ExtractElement:
        New = cast<VectorType>(typedOf(I->getOperand(0)))->getElementType();
        break;
      case Instruction::AtomicCmpXchg:
        New = StructType::get(
            Ctx, {valueTypeOf(*cast<AtomicCmpXchgInst>(I)),
                  Type::getInt1Ty(Ctx)});
        break;
      case Instruction::AtomicRMW:
        New = valueTypeOf(*cast<AtomicRMWInst>(I));
        break;
      case Instruction::Call:
      case Instruction::Invoke: {
        auto *CB = cast<CallBase>(I);
        FunctionType *CT;
        if (const Function *F = CB->getCalledFunction()) {
          CT = Out.functionType(*F);
        } else {
          FunctionType *FT = CB->getFunctionType();
          SmallVector<Type *, 8> Ps;
          for (unsigned A = 0, E = FT->getNumParams(); A != E; ++A)
            Ps.push_back(typedOf(CB->getArgOperand(A)));
          CT = FunctionType::get(typedOf(I), Ps, FT->isVarArg());
        }
        if (Out.Aux.lookup(I) != CT) {
          Out.Aux[I] = CT;
          Changed = true;
        }
        if (!I->getType()->isVoidTy())
          New = CT->getReturnType();
        break;
      }
      default:
        break;
      }
      if (New && New != typedOf(I)) {
        Out.Typed[I] = New;
        Changed = true;
      }
    }
  }

  for (unsigned I = 0, E = Nodes.size(); I != E; ++I) {
    auto [V, O] = Owners[I];
    std::string Fn;
    if (auto *Inst = dyn_cast<Instruction>(V))
      Fn = " (in @" + Inst->getFunction()->getName().str() + ")";
    else if (auto *A = dyn_cast<Argument>(V))
      Fn = " (in @" + A->getParent()->getName().str() + ")";
    std::string What = O == Owner::Source   ? "source of " + name(V)
                       : O == Owner::Return ? "return of " + name(V)
                                            : name(V);
    Type *Shown = O == Owner::Value ? typedOf(V) : Nodes[I].T;
    if (Nodes[I].Derived)
      R.Explain.push_back("derived " + What + ": " + T.print(Shown) + Fn);
    if (UseTyped[I]) {
      ++R.Stats.UseTyped;
      R.Explain.push_back("use-typed " + What + ": " + T.print(Shown) + Fn);
    }
  }

  // Each use as its user needs it: a cast before the user, or a typed
  // constant in the constant's place.
  for (Function &F : M) {
    // Explain lines number F's values as they were before its casts.
    MST.incorporateFunction(F);
    Incorporated = &F;
    SmallVector<Instruction *, 0> Body;
    for (Instruction &I : instructions(F))
      Body.push_back(&I);
    for (Instruction *I : Body)
      for (Use &U : I->operands()) {
        Value *V = U.get();
        if (isa<BasicBlock>(V) || isa<MetadataAsValue>(V) || isa<InlineAsm>(V))
          continue;
        Type *Want = required(*I, U.getOperandNo());
        if (auto *C = dyn_cast<Constant>(V)) {
          if (!Want) {
            if (Error E = prepare(C))
              return E;
            continue;
          }
          Expected<const Value *> TV = instance(C, Want);
          if (!TV)
            return TV.takeError();
          if (*TV != C)
            Out.Operands[&U] = *TV;
          continue;
        }
        if (Want && typedOf(V) != Want)
          if (Error E = castOperand(*I, U, Want))
            return E;
      }
  }
  for (GlobalVariable &G : M.globals())
    if (G.hasInitializer()) {
      Expected<const Value *> TV =
          instance(G.getInitializer(), Out.valueType(G));
      if (!TV)
        return TV.takeError();
      if (*TV != G.getInitializer())
        Out.Operands[&G.getOperandUse(0)] = *TV;
    }
  for (GlobalAlias &A : M.aliases()) {
    Expected<const Value *> TV = instance(A.getAliasee(), typedOf(&A));
    if (!TV)
      return TV.takeError();
    if (*TV != A.getAliasee())
      Out.Operands[&A.getOperandUse(0)] = *TV;
  }
  return Error::success();
}

Error AIRPointerTypes::Solver::run() {
  if (Error E = init())
    return E;
  solve();
  return finish();
}

Expected<AIRPointerTypes> AIRPointerTypes::run(Module &M,
                                               const AIRTypeTable &T,
                                               AIRWriteReport &R) {
  AIRPointerTypes Out;
  Out.Table = &T;
  Solver S(M, T, R, Out);
  if (Error E = S.run())
    return std::move(E);
  return std::move(Out);
}
