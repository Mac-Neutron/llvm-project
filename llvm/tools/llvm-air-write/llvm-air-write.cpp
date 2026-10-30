//===- llvm-air-write.cpp - Write a module as typed-pointer AIR -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Added by Mac-Neutron (2026).
//
//===----------------------------------------------------------------------===//
//
// Reads a module (bitcode or text) and a types file, and writes the module as
// typed-pointer AIR bitcode through LLVMWriteAIRBitcode.
//
// The types file has one entry per line, types in LLVM 15's typed syntax
// (`i32 addrspace(1)*`), names as Module::print writes them (`@x`, `@"..."`,
// `%x`):
//   s <%struct> <typed literal body>
//   g <@global> <typed value type>
//   f <@function> <typed function type>
//   c <@function> <instruction count>
//   i <@function> <k> <typed type of the k-th instruction, 0-based>
//
//===----------------------------------------------------------------------===//

#include "AIRBitcodeWriter.h"
#include "AIRTypes.h"
#include "llvm-c/AIRWriter.h"
#include "llvm-c/Core.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/TypedPointerType.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>
#include <string>
#include <vector>

using namespace llvm;

static cl::OptionCategory Cat("llvm-air-write options");
static cl::opt<std::string> Input(cl::Positional, cl::desc("<in.bc|in.ll>"),
                                  cl::cat(Cat));
static cl::opt<std::string> TypesPath("types", cl::desc("The types file"),
                                      cl::value_desc("file"), cl::cat(Cat));
static cl::opt<bool> NamedOnly("named-only",
                               cl::desc("Read only the s, g and f lines"),
                               cl::cat(Cat));
static cl::list<std::string> RefPaths("ref",
                                      cl::desc("Typed bitcode typing "
                                               "same-named functions, "
                                               "globals and structs"),
                                      cl::value_desc("typed.bc"),
                                      cl::cat(Cat));
static cl::opt<std::string>
    Passes("passes",
           cl::desc("Run this pass pipeline (no target machine) first"),
           cl::value_desc("pipeline"), cl::cat(Cat));
static cl::opt<std::string>
    EmitLL("emit-ll",
           cl::desc("Print <stem>.pre.ll (the module written) and "
                    "<stem>.post.ll (as the writer left it)"),
           cl::value_desc("stem"), cl::cat(Cat));
static cl::opt<bool> Explain("explain",
                             cl::desc("List the writer's casts, derived "
                                      "types and drops"),
                             cl::cat(Cat));
static cl::opt<std::string> Output("o", cl::desc("Output file"),
                                   cl::value_desc("out.bc"), cl::cat(Cat));
static cl::opt<std::string>
    BatchPath("batch",
              cl::desc("Write every job of a list of '<in> <types> <out>' "
                       "lines (types '-' for none); the other options apply "
                       "to each"),
              cl::value_desc("list"), cl::cat(Cat));
static cl::opt<unsigned> Jobs("j", cl::desc("Parallel jobs for --batch"),
                              cl::init(1), cl::cat(Cat));
static cl::opt<bool> SelfTestBroken(
    "self-test-broken-module",
    cl::desc("Write a module the verifier rejects, built through the C API"),
    cl::cat(Cat));

namespace {

/// LLVM 15's typed type syntax, over the module's context.
class TypeParser {
  LLVMContext &Ctx;
  StringRef S;
  std::string Err;

  void ws() { S = S.ltrim(" \t"); }
  bool eat(StringRef Tok) {
    ws();
    return S.consume_front(Tok);
  }
  Type *fail(const Twine &Why) {
    if (Err.empty())
      Err = (Why + " at '" + S.take_front(24) + "'").str();
    return nullptr;
  }
  bool number(uint64_t &N) {
    ws();
    return !S.consumeInteger(10, N);
  }

  Type *elements(StringRef Close, bool Packed) {
    SmallVector<Type *, 8> Elts;
    if (!eat(Close)) {
      do {
        Type *E = type();
        if (!E)
          return nullptr;
        if (!StructType::isValidElementType(E))
          return fail("not a struct element");
        Elts.push_back(E);
      } while (eat(","));
      if (!eat(Close))
        return fail("expected '" + Close + "'");
    }
    return StructType::get(Ctx, Elts, Packed);
  }

  Type *primary() {
    ws();
    uint64_t N;
    if (eat("[")) {
      if (!number(N) || !eat("x"))
        return fail("expected '[N x'");
      Type *E = type();
      if (!E)
        return nullptr;
      if (!ArrayType::isValidElementType(E) || !eat("]"))
        return fail("bad array");
      return ArrayType::get(E, N);
    }
    if (eat("<{"))
      return elements("}>", true);
    if (eat("<")) {
      if (!number(N) || N == 0 || !eat("x"))
        return fail("expected '<N x'");
      Type *E = type();
      if (!E)
        return nullptr;
      if (!VectorType::isValidElementType(E) || !eat(">"))
        return fail("bad vector");
      return FixedVectorType::get(E, N);
    }
    if (eat("{"))
      return elements("}", false);
    if (S.starts_with("%")) {
      std::string Name;
      if (!parseName(S, '%', Name))
        return fail("bad struct name");
      if (StructType *ST = StructType::getTypeByName(Ctx, Name))
        return ST;
      return fail("unknown struct %" + Name);
    }
    size_t Len = S.find_if_not([](char C) { return isAlnum(C) || C == '_'; });
    StringRef Word = S.take_front(Len);
    Type *T = StringSwitch<Type *>(Word)
                  .Case("void", Type::getVoidTy(Ctx))
                  .Case("half", Type::getHalfTy(Ctx))
                  .Case("bfloat", Type::getBFloatTy(Ctx))
                  .Case("float", Type::getFloatTy(Ctx))
                  .Case("double", Type::getDoubleTy(Ctx))
                  .Case("fp128", Type::getFP128Ty(Ctx))
                  .Case("x86_fp80", Type::getX86_FP80Ty(Ctx))
                  .Case("ppc_fp128", Type::getPPC_FP128Ty(Ctx))
                  .Case("label", Type::getLabelTy(Ctx))
                  .Case("metadata", Type::getMetadataTy(Ctx))
                  .Case("token", Type::getTokenTy(Ctx))
                  .Default(nullptr);
    unsigned Bits;
    if (!T && Word.size() > 1 && Word[0] == 'i' &&
        !Word.drop_front().getAsInteger(10, Bits) &&
        Bits >= IntegerType::MIN_INT_BITS && Bits <= IntegerType::MAX_INT_BITS)
      T = IntegerType::get(Ctx, Bits);
    if (!T)
      return fail(Word == "ptr" ? "an opaque pointer in a typed type"
                                : "unknown type");
    S = S.drop_front(Len);
    return T;
  }

public:
  TypeParser(LLVMContext &Ctx, StringRef S) : Ctx(Ctx), S(S) {}

  /// `"`-quoted names take LLVM's `\XX` escapes.
  static bool parseName(StringRef &S, char Sigil, std::string &Out) {
    if (!S.consume_front(StringRef(&Sigil, 1)))
      return false;
    if (S.consume_front("\"")) {
      while (!S.empty() && S[0] != '"') {
        if (S[0] == '\\') {
          unsigned Byte;
          if (S.size() < 3 || S.substr(1, 2).getAsInteger(16, Byte))
            return false;
          Out.push_back(char(Byte));
          S = S.drop_front(3);
        } else {
          Out.push_back(S[0]);
          S = S.drop_front();
        }
      }
      return S.consume_front("\"");
    }
    size_t Len = S.find_if_not([](char C) {
      return isAlnum(C) || C == '-' || C == '$' || C == '.' || C == '_';
    });
    Out = S.take_front(Len).str();
    S = S.drop_front(Len);
    return !Out.empty();
  }

  Type *type() {
    Type *T = primary();
    while (T) {
      ws();
      if (S.starts_with("*") || S.starts_with("addrspace(")) {
        uint64_t AS = 0;
        if (S.consume_front("addrspace(") && (!number(AS) || !eat(")")))
          return fail("bad address space");
        if (!eat("*"))
          return fail("expected '*'");
        if (!TypedPointerType::isValidElementType(T))
          return fail("not a pointee");
        T = TypedPointerType::get(T, AS);
      } else if (S.consume_front("(")) {
        SmallVector<Type *, 8> Params;
        bool VarArg = false;
        if (!eat(")")) {
          do {
            if (eat("...")) {
              VarArg = true;
              break;
            }
            Type *P = type();
            if (!P)
              return nullptr;
            if (!FunctionType::isValidArgumentType(P))
              return fail("not a parameter type");
            Params.push_back(P);
          } while (eat(","));
          if (!eat(")"))
            return fail("expected ')'");
        }
        if (!FunctionType::isValidReturnType(T))
          return fail("not a return type");
        T = FunctionType::get(T, Params, VarArg);
      } else {
        break;
      }
    }
    return T;
  }

  /// The whole text as one type.
  Type *all(std::string &Error) {
    Type *T = type();
    ws();
    if (T && !S.empty())
      T = fail("trailing text");
    Error = Err;
    return T;
  }
};

struct Table {
  std::vector<LLVMAIRValueType> Values;
  std::vector<LLVMAIRStructBody> Structs;
  unsigned Stale = 0;
};

/// Reads the types file into T. Prints positional mismatches and (after
/// --passes) stale entries to Log.
bool readTypes(Module &M, StringRef Path, Table &T, std::string &Log) {
  auto Buf = MemoryBuffer::getFile(Path);
  if (!Buf) {
    Log += ("error: " + Path + ": " + Buf.getError().message() + "\n").str();
    return false;
  }
  LLVMContext &Ctx = M.getContext();
  struct Positional {
    uint64_t K;
    Type *T;
    unsigned Line;
  };
  DenseMap<Function *, uint64_t> Counts;
  // Functions in first-seen order, so entries follow the file.
  SmallVector<Function *, 8> Order;
  DenseMap<Function *, SmallVector<Positional, 0>> Lines;
  unsigned LineNo = 0;
  SmallVector<StringRef, 0> All;
  (*Buf)->getBuffer().split(All, '\n');
  for (StringRef L : All) {
    ++LineNo;
    L = L.trim();
    if (L.empty())
      continue;
    auto Bad = [&](const Twine &Why) {
      Log += (Path + ":" + Twine(LineNo) + ": error: " + Why + "\n").str();
      return false;
    };
    char Kind = L[0];
    L = L.drop_front().ltrim();
    if (NamedOnly && (Kind == 'c' || Kind == 'i'))
      continue;
    std::string Name, TErr;
    if (Kind == 's') {
      if (!TypeParser::parseName(L, '%', Name))
        return Bad("bad struct name");
      StructType *ST = StructType::getTypeByName(Ctx, Name);
      if (!ST)
        return Bad("unknown struct %" + Name);
      Type *B = TypeParser(Ctx, L).all(TErr);
      if (!B)
        return Bad(TErr);
      T.Structs.push_back({wrap(ST), wrap(B)});
      continue;
    }
    if (Kind != 'g' && Kind != 'f' && Kind != 'c' && Kind != 'i')
      return Bad("unknown line kind");
    if (!TypeParser::parseName(L, '@', Name))
      return Bad("bad name");
    GlobalValue *GV = Kind == 'g' ? (GlobalValue *)M.getNamedGlobal(Name)
                                  : (GlobalValue *)M.getFunction(Name);
    if (!GV) {
      if (Passes.empty())
        return Bad("unknown @" + Name);
      // The passes deleted it.
      if (Kind == 'g' || Kind == 'f') {
        Log += "stale @" + Name + "\n";
        ++T.Stale;
      }
      continue;
    }
    uint64_t K = 0;
    if (Kind == 'c' || Kind == 'i') {
      L = L.ltrim();
      if (L.consumeInteger(10, K))
        return Bad("expected a number");
      if (Kind == 'c') {
        if (!Counts.try_emplace(cast<Function>(GV), K).second)
          return Bad("two c lines");
        continue;
      }
    }
    Type *Ty = TypeParser(Ctx, L).all(TErr);
    if (!Ty)
      return Bad(TErr);
    if (Kind == 'i') {
      auto *F = cast<Function>(GV);
      auto [It, New] = Lines.try_emplace(F);
      if (New)
        Order.push_back(F);
      It->second.push_back({K, Ty, LineNo});
      continue;
    }
    if (!Passes.empty()) {
      Type *Want = Kind == 'g' ? cast<GlobalVariable>(GV)->getValueType()
                               : cast<Function>(GV)->getFunctionType();
      if (air::eraseTypedType(Ty) != Want) {
        Log += "stale @" + Name + "\n";
        ++T.Stale;
        continue;
      }
    }
    T.Values.push_back({wrap(GV), wrap(Ty)});
  }
  for (Function *F : Order) {
    auto C = Counts.find(F);
    if (C == Counts.end()) {
      Log += (Path + ": error: @" + F->getName() + " has i lines and no c line\n")
                 .str();
      return false;
    }
    if (C->second != F->getInstructionCount()) {
      Log += ("positional mismatch @" + F->getName() + "\n").str();
      continue;
    }
    std::vector<Instruction *> Insts;
    for (Instruction &I : instructions(*F))
      Insts.push_back(&I);
    for (const Positional &P : Lines[F]) {
      if (P.K >= Insts.size()) {
        Log += (Path + ":" + Twine(P.Line) + ": error: no instruction " +
                Twine(P.K) + "\n")
                   .str();
        return false;
      }
      T.Values.push_back({wrap(Insts[P.K]), wrap(P.T)});
    }
  }
  return true;
}

bool printModule(const Module &M, const Twine &Path, std::string &Log) {
  std::error_code EC;
  raw_fd_ostream OS(Path.str(), EC, sys::fs::OF_TextWithCRLF);
  if (EC) {
    Log += ("error: " + Path + ": " + EC.message() + "\n").str();
    return false;
  }
  M.print(OS, nullptr);
  return true;
}

/// One write. Everything it prints goes to Log.
bool runJob(StringRef In, StringRef Types, StringRef Out, std::string &Log) {
  LLVMContext Ctx;
  SMDiagnostic Diag;
  std::unique_ptr<Module> M = parseIRFile(In, Diag, Ctx);
  if (!M) {
    Log += ("error: " + In + ": " + Diag.getMessage() + "\n").str();
    return false;
  }

  if (!Passes.empty()) {
    LoopAnalysisManager LAM;
    FunctionAnalysisManager FAM;
    CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM;
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
    ModulePassManager MPM;
    if (Error E = PB.parsePassPipeline(MPM, Passes)) {
      Log += "error: --passes: " + toString(std::move(E)) + "\n";
      return false;
    }
    MPM.run(*M, MAM);
  }

  Table T;
  if (!Types.empty() && !readTypes(*M, Types, T, Log))
    return false;

  std::vector<std::unique_ptr<MemoryBuffer>> RefBufs;
  std::vector<LLVMMemoryBufferRef> Refs;
  for (const std::string &Path : RefPaths) {
    auto Buf = MemoryBuffer::getFile(Path);
    if (!Buf) {
      Log += "error: " + Path + ": " + Buf.getError().message() + "\n";
      return false;
    }
    RefBufs.push_back(std::move(*Buf));
    Refs.push_back(wrap(RefBufs.back().get()));
  }

  if (!EmitLL.empty() && !printModule(*M, EmitLL + ".pre.ll", Log))
    return false;

  LLVMAIRTypes CTypes = {T.Values.data(),  T.Values.size(),
                         T.Structs.data(), T.Structs.size(),
                         Refs.data(),      Refs.size()};
  LLVMMemoryBufferRef OutBuf = nullptr;
  LLVMAIRWriteStats Stats = {};
  char *Msg = nullptr;
  air::AIRWriteReport Report;
  // --explain needs the report, which the C interface does not carry: the C
  // entry point's own body with one.
  LLVMBool Failed =
      Explain ? air::writeAIRBitcodeC(wrap(M.get()), &CTypes, &OutBuf, &Stats,
                                      &Msg, &Report)
              : LLVMWriteAIRBitcode(wrap(M.get()), &CTypes, &OutBuf, &Stats,
                                    &Msg);
  if (Failed) {
    Log += ("error: " + StringRef(Msg).rtrim() + "\n").str();
    LLVMDisposeMessage(Msg);
    return false;
  }
  std::unique_ptr<MemoryBuffer> Bytes(unwrap(OutBuf));

  if (!EmitLL.empty() && !printModule(*M, EmitLL + ".post.ll", Log))
    return false;
  std::error_code EC;
  raw_fd_ostream OS(Out, EC, sys::fs::OF_None);
  if (EC) {
    Log += ("error: " + Out + ": " + EC.message() + "\n").str();
    return false;
  }
  OS << Bytes->getBuffer();

  for (const std::string &Line : Report.Explain)
    Log += Line + "\n";
  Log += formatv("stats inst-casts={0} const-casts={1} use-typed={2} "
                 "attrs-dropped={3} flags-dropped={4} refs={5} stale={6}\n",
                 Stats.InstCasts, Stats.ConstCasts, Stats.UseTyped,
                 Stats.AttrsDropped, Stats.FlagsDropped, Stats.RefsFilled,
                 T.Stale)
             .str();
  return true;
}

/// A block with no terminator, built through the C API.
int selfTestBrokenModule() {
  LLVMContextRef C = LLVMContextCreate();
  LLVMModuleRef M = LLVMModuleCreateWithNameInContext("broken", C);
  LLVMSetTarget(M, "air64-apple-macosx14.0.0");
  LLVMTypeRef FT = LLVMFunctionType(LLVMVoidTypeInContext(C), nullptr, 0, 0);
  LLVMValueRef F = LLVMAddFunction(M, "f", FT);
  LLVMAppendBasicBlockInContext(C, F, "entry");
  LLVMAIRTypes Types = {};
  LLVMMemoryBufferRef Out = nullptr;
  char *Msg = nullptr;
  int Ret = 0;
  if (LLVMWriteAIRBitcode(M, &Types, &Out, nullptr, &Msg)) {
    errs() << "error: " << StringRef(Msg).rtrim() << "\n";
    LLVMDisposeMessage(Msg);
    Ret = 1;
  } else {
    LLVMDisposeMemoryBuffer(Out);
  }
  LLVMDisposeModule(M);
  LLVMContextDispose(C);
  return Ret;
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::HideUnrelatedOptions(Cat);
  cl::ParseCommandLineOptions(argc, argv,
                              "writes a module as typed-pointer AIR bitcode\n");

  if (SelfTestBroken)
    return selfTestBrokenModule();

  if (BatchPath.empty()) {
    if (Input.empty() || Output.empty()) {
      errs() << "error: need <in> and -o <out>, or --batch <list>\n";
      return 1;
    }
    std::string Log;
    bool Ok = runJob(Input, TypesPath, Output, Log);
    errs() << Log;
    return Ok ? 0 : 1;
  }

  if (!EmitLL.empty() || !Input.empty() || !Output.empty() ||
      !TypesPath.empty()) {
    errs() << "error: --batch takes its inputs, types and outputs from the "
              "list (and no --emit-ll)\n";
    return 1;
  }
  auto List = MemoryBuffer::getFile(BatchPath);
  if (!List) {
    errs() << "error: " << BatchPath << ": " << List.getError().message()
           << "\n";
    return 1;
  }
  struct Job {
    std::string In, Types, Out, Log;
    bool Ok = false;
  };
  std::vector<Job> JobList;
  SmallVector<StringRef, 0> Lines;
  (*List)->getBuffer().split(Lines, '\n', -1, /*KeepEmpty=*/false);
  for (StringRef L : Lines) {
    SmallVector<StringRef, 3> F;
    L.split(F, ' ', -1, /*KeepEmpty=*/false);
    if (F.size() != 3) {
      errs() << "error: " << BatchPath << ": expected '<in> <types> <out>': "
             << L << "\n";
      return 1;
    }
    JobList.push_back({F[0].str(), F[1].str(), F[2].str(), "", false});
  }
  {
    DefaultThreadPool Pool(hardware_concurrency(Jobs));
    for (Job &J : JobList)
      Pool.async([&J] {
        J.Ok = runJob(J.In, J.Types == "-" ? "" : J.Types, J.Out, J.Log);
      });
    Pool.wait();
  }
  bool AllOk = true;
  for (const Job &J : JobList) {
    SmallVector<StringRef, 4> LogLines;
    StringRef(J.Log).split(LogLines, '\n', -1, /*KeepEmpty=*/false);
    for (StringRef L : LogLines)
      errs() << J.Out << ": " << L << "\n";
    AllOk &= J.Ok;
  }
  return AllOk ? 0 : 1;
}
