//===- AIRTypedRefs.cpp - Typed types from typed bitcode ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Added by Mac-Neutron (2026).
//
//===----------------------------------------------------------------------===//
//
// Scans typed bitcode (LLVM 15's or Apple's) with a BitstreamCursor: its type
// table, its functions and globals, and their names (STRTAB in version 2, the
// module VST in version 1). No IR is read, because LLVM 23's reader makes
// every pointer opaque. Identified structs are the context's own, by name.
//
//===----------------------------------------------------------------------===//

#include "AIRTypedRefs.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/LLVMBitCodes.h"
#include "llvm/Bitstream/BitstreamReader.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/TypedPointerType.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace llvm::air;

namespace {

/// One type table entry: every record but NUMENTRY and STRUCT_NAME.
struct TypeRec {
  unsigned Code;
  SmallVector<uint64_t, 8> Ops;
  std::string Name; // STRUCT_NAMED, OPAQUE, TARGET_TYPE: the STRUCT_NAME before
};

/// One module-level value, in value id order.
struct ValueRec {
  enum KindTy { Global, Function, Other } Kind = Other;
  uint64_t TypeID = 0;
  uint64_t Flags = 0; // global: isconst | explicit type << 1 | addrspace << 2
  uint64_t NameOff = 0, NameSize = 0; // version 2
  std::string Name;                   // version 1, from the VST
};

class RefReader {
public:
  RefReader(LLVMContext &C, size_t N) : C(C), N(N) {}
  Error read(MemoryBufferRef Buf, AIRRefTable &Out);

private:
  LLVMContext &C;
  size_t N;
  BitstreamCursor Stream;
  BitstreamBlockInfo BlockInfo;
  uint64_t Version = 0;
  bool HaveModule = false, HaveTypes = false, HaveStrtab = false;
  std::vector<TypeRec> TypeRecs;
  std::vector<ValueRec> Values;
  SmallVector<std::pair<uint64_t, std::string>, 0> VSTNames;
  StringRef Strtab;
  std::vector<Type *> Types;

  Error err(const Twine &Why) const {
    return createStringError(inconvertibleErrorCode(),
                             "typed ref " + Twine(N) + ": " + Why);
  }
  Error malformed(Error E) const {
    return err("malformed bitcode: " + toString(std::move(E)));
  }
  Error malformed(const Twine &Why) const {
    return err("malformed bitcode: " + Why);
  }

  template <typename RecordFn, typename SubBlockFn>
  Error readBlock(unsigned ID, StringRef What, RecordFn OnRecord,
                  SubBlockFn OnSubBlock);
  Error skipSubBlock(unsigned) {
    if (Error E = Stream.SkipBlock())
      return malformed(std::move(E));
    return Error::success();
  }
  Error readTopBlock(unsigned ID);
  Error readModule();
  Error finish(AIRRefTable &Out);
  Error readTypeRecord(unsigned Code, ArrayRef<uint64_t> Ops,
                       std::string &PendingName);
  Error readValueRecord(unsigned Code, ArrayRef<uint64_t> Ops);
  Error resolveTypes();
  Error typeAt(size_t I, uint64_t K, Type *&Out) const;
  Error buildType(size_t I, const TypeRec &R, Type *&T) const;
};

std::string str(const Type *T) {
  std::string S;
  raw_string_ostream OS(S);
  T->print(OS);
  return S;
}

std::string chars(ArrayRef<uint64_t> Ops) {
  std::string S;
  for (uint64_t Ch : Ops)
    S += char(Ch);
  return S;
}

/// The type of a record with no type operands, or null.
Type *leafType(LLVMContext &C, unsigned Code) {
  switch (Code) {
  case bitc::TYPE_CODE_VOID:
    return Type::getVoidTy(C);
  case bitc::TYPE_CODE_HALF:
    return Type::getHalfTy(C);
  case bitc::TYPE_CODE_BFLOAT:
    return Type::getBFloatTy(C);
  case bitc::TYPE_CODE_FLOAT:
    return Type::getFloatTy(C);
  case bitc::TYPE_CODE_DOUBLE:
    return Type::getDoubleTy(C);
  case bitc::TYPE_CODE_X86_FP80:
    return Type::getX86_FP80Ty(C);
  case bitc::TYPE_CODE_FP128:
    return Type::getFP128Ty(C);
  case bitc::TYPE_CODE_PPC_FP128:
    return Type::getPPC_FP128Ty(C);
  case bitc::TYPE_CODE_LABEL:
    return Type::getLabelTy(C);
  case bitc::TYPE_CODE_METADATA:
    return Type::getMetadataTy(C);
  case bitc::TYPE_CODE_TOKEN:
    return Type::getTokenTy(C);
  case bitc::TYPE_CODE_X86_AMX:
    return Type::getX86_AMXTy(C);
  case bitc::TYPE_CODE_X86_MMX: // As LLVM 23's reader.
    return FixedVectorType::get(Type::getInt64Ty(C), 1);
  default:
    return nullptr;
  }
}

} // namespace

/// Reads the records of the block about to be entered. Subblocks go to
/// OnSubBlock, which must enter or skip them.
template <typename RecordFn, typename SubBlockFn>
Error RefReader::readBlock(unsigned ID, StringRef What, RecordFn OnRecord,
                           SubBlockFn OnSubBlock) {
  if (Error E = Stream.EnterSubBlock(ID))
    return malformed(std::move(E));
  SmallVector<uint64_t, 64> Rec;
  while (true) {
    Expected<BitstreamEntry> Entry = Stream.advance();
    if (!Entry)
      return malformed(Entry.takeError());
    switch (Entry->Kind) {
    case BitstreamEntry::Error:
      return malformed("the " + What + " block ends early");
    case BitstreamEntry::EndBlock:
      return Error::success();
    case BitstreamEntry::SubBlock:
      if (Error E = OnSubBlock(Entry->ID))
        return E;
      break;
    case BitstreamEntry::Record: {
      Rec.clear();
      StringRef Blob;
      Expected<unsigned> Code = Stream.readRecord(Entry->ID, Rec, &Blob);
      if (!Code)
        return malformed(Code.takeError());
      if (Error E = OnRecord(*Code, ArrayRef<uint64_t>(Rec), Blob))
        return E;
      break;
    }
    }
  }
}

Error RefReader::readTypeRecord(unsigned Code, ArrayRef<uint64_t> Ops,
                                std::string &PendingName) {
  switch (Code) {
  case bitc::TYPE_CODE_NUMENTRY:
    return Error::success();
  case bitc::TYPE_CODE_STRUCT_NAME:
    PendingName = chars(Ops);
    return Error::success();
  default:
    break;
  }
  TypeRec R{Code, SmallVector<uint64_t, 8>(Ops), {}};
  // As LLVM's reader: these three take the name; other records leave it.
  if (Code == bitc::TYPE_CODE_STRUCT_NAMED || Code == bitc::TYPE_CODE_OPAQUE ||
      Code == bitc::TYPE_CODE_TARGET_TYPE)
    R.Name = std::exchange(PendingName, std::string());
  TypeRecs.push_back(std::move(R));
  return Error::success();
}

Error RefReader::readValueRecord(unsigned Code, ArrayRef<uint64_t> Ops) {
  ValueRec V;
  switch (Code) {
  case bitc::MODULE_CODE_GLOBALVAR:
    V.Kind = ValueRec::Global;
    break;
  case bitc::MODULE_CODE_FUNCTION:
    V.Kind = ValueRec::Function;
    break;
  case bitc::MODULE_CODE_ALIAS:
  case bitc::MODULE_CODE_ALIAS_OLD:
  case bitc::MODULE_CODE_IFUNC:
    // Each holds a value id; its type is not needed.
    Values.push_back(std::move(V));
    return Error::success();
  default:
    return Error::success();
  }
  // Version 2 names its values in the string table.
  if (Version >= 2) {
    if (Ops.size() < 2)
      return malformed("a global or function record is too short");
    V.NameOff = Ops[0];
    V.NameSize = Ops[1];
    Ops = Ops.drop_front(2);
  }
  if (Ops.size() < (V.Kind == ValueRec::Global ? 2u : 1u))
    return malformed("a global or function record is too short");
  V.TypeID = Ops[0];
  if (V.Kind == ValueRec::Global)
    V.Flags = Ops[1];
  Values.push_back(std::move(V));
  return Error::success();
}

Error RefReader::readModule() {
  auto OnRecord = [&](unsigned Code, ArrayRef<uint64_t> Ops,
                      StringRef) -> Error {
    if (Code != bitc::MODULE_CODE_VERSION)
      return readValueRecord(Code, Ops);
    if (Ops.empty())
      return malformed("an empty VERSION record");
    Version = Ops[0];
    if (Version > 2)
      return err("module version " + Twine(Version) + " (0 to 2 are read)");
    return Error::success();
  };
  auto OnSubBlock = [&](unsigned ID) -> Error {
    switch (ID) {
    case bitc::BLOCKINFO_BLOCK_ID: {
      Expected<std::optional<BitstreamBlockInfo>> BI =
          Stream.ReadBlockInfoBlock();
      if (!BI)
        return malformed(BI.takeError());
      if (!*BI)
        return malformed("the BLOCKINFO block");
      BlockInfo = std::move(**BI);
      return Error::success();
    }
    case bitc::TYPE_BLOCK_ID_NEW: {
      if (HaveTypes)
        return malformed("two type tables");
      HaveTypes = true;
      std::string PendingName;
      return readBlock(
          ID, "type", [&](unsigned Code, ArrayRef<uint64_t> Ops, StringRef) {
            return readTypeRecord(Code, Ops, PendingName);
          },
          [&](unsigned Sub) { return skipSubBlock(Sub); });
    }
    case bitc::VALUE_SYMTAB_BLOCK_ID:
      // Module level: [valueid, namechar x N] and, before version 2,
      // [valueid, offset, namechar x N] for functions.
      return readBlock(
          ID, "value symbol table",
          [&](unsigned Code, ArrayRef<uint64_t> Ops, StringRef) -> Error {
            if (Code == bitc::VST_CODE_ENTRY && !Ops.empty())
              VSTNames.push_back({Ops[0], chars(Ops.drop_front())});
            else if (Code == bitc::VST_CODE_FNENTRY && Ops.size() >= 2)
              VSTNames.push_back({Ops[0], chars(Ops.drop_front(2))});
            return Error::success();
          },
          [&](unsigned Sub) { return skipSubBlock(Sub); });
    default:
      return skipSubBlock(ID);
    }
  };
  return readBlock(bitc::MODULE_BLOCK_ID, "module", OnRecord, OnSubBlock);
}

/// Type K, as type I's operand. Only named structs, which resolve by name,
/// may be referenced before they are defined.
Error RefReader::typeAt(size_t I, uint64_t K, Type *&Out) const {
  if (K >= Types.size() || (K >= I &&
                            TypeRecs[K].Code != bitc::TYPE_CODE_STRUCT_NAMED &&
                            TypeRecs[K].Code != bitc::TYPE_CODE_OPAQUE))
    return malformed("type " + Twine(I) + " refers to type " + Twine(K));
  Out = Types[K];
  return Error::success();
}

/// Type I, from its record. Null when it holds a type this reader does not
/// build (a struct the context lacks, a target type, an unknown code): the
/// functions, globals and bodies that use it are left out.
Error RefReader::buildType(size_t I, const TypeRec &R, Type *&T) const {
  ArrayRef<uint64_t> Ops = R.Ops;
  auto Need = [&](size_t Min) -> Error {
    if (Ops.size() < Min)
      return malformed("type " + Twine(I) + ": a short record");
    return Error::success();
  };
  // Operand types; All is false if one is null.
  auto Operands = [&](ArrayRef<uint64_t> Ids, SmallVectorImpl<Type *> &Tys,
                      bool &All) -> Error {
    All = true;
    for (uint64_t K : Ids) {
      Type *E;
      if (Error Err = typeAt(I, K, E))
        return Err;
      All &= E != nullptr;
      Tys.push_back(E);
    }
    return Error::success();
  };
  auto Invalid = [&](StringRef What) {
    return malformed("type " + Twine(I) + ": an invalid " + What);
  };

  T = leafType(C, R.Code);
  if (T)
    return Error::success();
  switch (R.Code) {
  case bitc::TYPE_CODE_INTEGER:
    if (Error E = Need(1))
      return E;
    if (Ops[0] < IntegerType::MIN_INT_BITS ||
        Ops[0] > IntegerType::MAX_INT_BITS)
      return Invalid("integer width");
    T = IntegerType::get(C, unsigned(Ops[0]));
    return Error::success();
  case bitc::TYPE_CODE_OPAQUE_POINTER:
    return err("type " + Twine(I) +
               ": an opaque pointer (a typed ref holds typed pointers only)");
  case bitc::TYPE_CODE_POINTER: {
    if (Error E = Need(1))
      return E;
    uint64_t AS = Ops.size() > 1 ? Ops[1] : 0;
    Type *E;
    if (Error Err = typeAt(I, Ops[0], E))
      return Err;
    if (!E)
      return Error::success();
    if (!TypedPointerType::isValidElementType(E) || AS > UINT32_MAX)
      return Invalid("pointer");
    T = TypedPointerType::get(E, unsigned(AS));
    return Error::success();
  }
  case bitc::TYPE_CODE_FUNCTION_OLD:
  case bitc::TYPE_CODE_FUNCTION: {
    // [vararg, attrid (old only), retty, paramty x N]
    size_t Skip = R.Code == bitc::TYPE_CODE_FUNCTION_OLD ? 2 : 1;
    if (Error E = Need(Skip + 1))
      return E;
    SmallVector<Type *, 8> Tys;
    bool All;
    if (Error E = Operands(Ops.drop_front(Skip), Tys, All))
      return E;
    if (!All)
      return Error::success();
    ArrayRef<Type *> Params = ArrayRef(Tys).drop_front();
    if (!FunctionType::isValidReturnType(Tys[0]) ||
        !all_of(Params, FunctionType::isValidArgumentType))
      return Invalid("function type");
    T = FunctionType::get(Tys[0], Params, Ops[0] != 0);
    return Error::success();
  }
  case bitc::TYPE_CODE_STRUCT_ANON: {
    if (Error E = Need(1))
      return E;
    SmallVector<Type *, 8> Tys;
    bool All;
    if (Error E = Operands(Ops.drop_front(), Tys, All))
      return E;
    if (!All)
      return Error::success();
    if (!all_of(Tys, StructType::isValidElementType))
      return Invalid("struct");
    T = StructType::get(C, Tys, Ops[0] != 0);
    return Error::success();
  }
  case bitc::TYPE_CODE_ARRAY: {
    if (Error E = Need(2))
      return E;
    Type *E;
    if (Error Err = typeAt(I, Ops[1], E))
      return Err;
    if (!E)
      return Error::success();
    if (!ArrayType::isValidElementType(E))
      return Invalid("array");
    T = ArrayType::get(E, Ops[0]);
    return Error::success();
  }
  case bitc::TYPE_CODE_VECTOR: {
    if (Error E = Need(2))
      return E;
    Type *E;
    if (Error Err = typeAt(I, Ops[1], E))
      return Err;
    if (!E)
      return Error::success();
    if (!Ops[0] || Ops[0] > UINT32_MAX || !VectorType::isValidElementType(E))
      return Invalid("vector");
    T = VectorType::get(E, unsigned(Ops[0]), Ops.size() > 2 && Ops[2]);
    return Error::success();
  }
  default:
    // Named structs are built by name (resolveTypes); a target type, a byte
    // type or an unknown code keeps its id and builds nothing.
    return Error::success();
  }
}

Error RefReader::resolveTypes() {
  // Named structs first: they are the context's own, so forward references to
  // them resolve. Null when the context has no struct of that name.
  Types.assign(TypeRecs.size(), nullptr);
  for (size_t I = 0; I != TypeRecs.size(); ++I) {
    const TypeRec &R = TypeRecs[I];
    if ((R.Code == bitc::TYPE_CODE_STRUCT_NAMED ||
         R.Code == bitc::TYPE_CODE_OPAQUE) &&
        !R.Name.empty())
      Types[I] = StructType::getTypeByName(C, R.Name);
  }
  for (size_t I = 0; I != TypeRecs.size(); ++I)
    if (TypeRecs[I].Code != bitc::TYPE_CODE_STRUCT_NAMED &&
        TypeRecs[I].Code != bitc::TYPE_CODE_OPAQUE)
      if (Error E = buildType(I, TypeRecs[I], Types[I]))
        return E;
  return Error::success();
}

Error RefReader::read(MemoryBufferRef Buf, AIRRefTable &Out) {
  const auto *Ptr =
      reinterpret_cast<const unsigned char *>(Buf.getBufferStart());
  const auto *End =
      reinterpret_cast<const unsigned char *>(Buf.getBufferEnd());
  if (End - Ptr >= 4 && isBitcodeWrapper(Ptr, End) &&
      SkipBitcodeWrapperHeader(Ptr, End, /*VerifyBufferSize=*/true))
    return err("invalid bitcode wrapper header");
  if (End - Ptr < 4 || !isRawBitcode(Ptr, End))
    return err("not bitcode");
  Stream = BitstreamCursor(ArrayRef<uint8_t>(Ptr, End));
  Stream.setBlockInfo(&BlockInfo);
  if (Error E = Stream.JumpToBit(32))
    return malformed(std::move(E));

  // Top level. As LLVM's reader, a tail too short to hold a block is padding.
  while (Stream.getCurrentByteNo() + 8 < Stream.getBitcodeBytes().size()) {
    Expected<BitstreamEntry> Entry = Stream.advance();
    if (!Entry)
      return malformed(Entry.takeError());
    if (Entry->Kind == BitstreamEntry::Record) {
      if (Expected<unsigned> Skipped = Stream.skipRecord(Entry->ID); !Skipped)
        return malformed(Skipped.takeError());
      continue;
    }
    if (Entry->Kind != BitstreamEntry::SubBlock)
      return malformed("the stream ends early");
    if (Error E = readTopBlock(Entry->ID))
      return E;
  }
  return finish(Out);
}

Error RefReader::readTopBlock(unsigned ID) {
  switch (ID) {
  case bitc::IDENTIFICATION_BLOCK_ID:
    return readBlock(
        ID, "identification",
        [&](unsigned Code, ArrayRef<uint64_t> Ops, StringRef) -> Error {
          if (Code == bitc::IDENTIFICATION_CODE_EPOCH && !Ops.empty() &&
              Ops[0] != bitc::BITCODE_CURRENT_EPOCH)
            return err("epoch " + Twine(Ops[0]) + " (only " +
                       Twine(bitc::BITCODE_CURRENT_EPOCH) + " is read)");
          return Error::success();
        },
        [&](unsigned Sub) { return skipSubBlock(Sub); });
  case bitc::MODULE_BLOCK_ID:
    if (HaveModule)
      return err("more than one module");
    HaveModule = true;
    return readModule();
  case bitc::STRTAB_BLOCK_ID:
    return readBlock(
        ID, "string table",
        [&](unsigned Code, ArrayRef<uint64_t>, StringRef Blob) -> Error {
          if (Code == bitc::STRTAB_BLOB) {
            Strtab = Blob;
            HaveStrtab = true;
          }
          return Error::success();
        },
        [&](unsigned Sub) { return skipSubBlock(Sub); });
  default:
    return skipSubBlock(ID);
  }
}

Error RefReader::finish(AIRRefTable &Out) {
  if (!HaveModule)
    return err("no module");

  // Names.
  if (Version >= 2) {
    if (!HaveStrtab)
      return err("no string table");
    for (ValueRec &V : Values) {
      if (V.Kind == ValueRec::Other)
        continue;
      if (V.NameOff > Strtab.size() || V.NameSize > Strtab.size() - V.NameOff)
        return malformed("a name outside the string table");
      V.Name = Strtab.substr(V.NameOff, V.NameSize).str();
    }
  } else {
    for (auto &[ID, Name] : VSTNames)
      if (ID < Values.size())
        Values[ID].Name = std::move(Name);
  }

  if (Error E = resolveTypes())
    return E;

  // Name -> typed type. A name another ref already gave must have its type.
  auto Merge = [&](auto &Map, StringRef Name, auto *Ty,
                   StringRef Sigil) -> Error {
    auto [It, New] = Map.try_emplace(Name, Ty);
    if (!New && It->second != Ty)
      return err(Twine(Sigil) + Name + " is " + str(Ty) +
                 ", an earlier ref's " + str(It->second));
    return Error::success();
  };
  auto TypeOf = [&](uint64_t K, Type *&T) -> Error {
    if (K >= Types.size())
      return malformed("type " + Twine(K) + " is not in the type table");
    T = Types[K];
    return Error::success();
  };
  for (const ValueRec &V : Values) {
    if (V.Kind == ValueRec::Other || V.Name.empty())
      continue;
    Type *T;
    if (Error E = TypeOf(V.TypeID, T))
      return E;
    if (!T)
      continue;
    if (V.Kind == ValueRec::Global) {
      // Without the explicit-type bit the record holds the pointer type.
      if (!(V.Flags & 2)) {
        auto *P = dyn_cast<TypedPointerType>(T);
        if (!P)
          return malformed("@" + V.Name + ": not a pointer type");
        T = P->getElementType();
      }
      if (Error E = Merge(Out.Globals, V.Name, T, "@"))
        return E;
      continue;
    }
    // Older bitcode gives a function its pointer type.
    if (auto *P = dyn_cast<TypedPointerType>(T))
      T = P->getElementType();
    auto *FT = dyn_cast<FunctionType>(T);
    if (!FT)
      return malformed("@" + V.Name + ": not a function type");
    if (Error E = Merge(Out.Functions, V.Name, FT, "@"))
      return E;
  }

  // Bodies of the named structs the context has, as typed literals.
  for (size_t I = 0; I != TypeRecs.size(); ++I) {
    const TypeRec &R = TypeRecs[I];
    if (R.Code != bitc::TYPE_CODE_STRUCT_NAMED || !Types[I])
      continue;
    if (R.Ops.empty())
      return malformed("type " + Twine(I) + ": a short record");
    SmallVector<Type *, 8> Elts;
    bool All = true;
    for (uint64_t K : ArrayRef(R.Ops).drop_front()) {
      Type *E;
      if (Error Err = typeAt(I, K, E))
        return Err;
      All &= E != nullptr;
      Elts.push_back(E);
    }
    if (!All)
      continue;
    if (!all_of(Elts, StructType::isValidElementType))
      return malformed("type " + Twine(I) + ": an invalid struct");
    if (Error E = Merge(Out.Bodies, R.Name,
                        StructType::get(C, Elts, R.Ops[0] != 0), "%"))
      return E;
  }
  return Error::success();
}

Expected<AIRRefTable> llvm::air::readTypedRefs(LLVMContext &C,
                                               ArrayRef<MemoryBufferRef> Refs) {
  AIRRefTable Out;
  for (size_t I = 0; I != Refs.size(); ++I)
    if (Error E = RefReader(C, I).read(Refs[I], Out))
      return std::move(E);
  return std::move(Out);
}
