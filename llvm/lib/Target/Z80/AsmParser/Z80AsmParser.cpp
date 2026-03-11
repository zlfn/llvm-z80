//===---- Z80AsmParser.cpp - Parse Z80 assembly to MCInst instructions ----===//
//
// Part of LLVM-Z80, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the Z80 assembly parser implementation.
//
// Z80 assembly syntax examples:
//   ld a, b          ; register to register
//   ld a, 42         ; immediate to register
//   ld a, (hl)       ; indirect load
//   ld a, (ix+5)     ; indexed indirect load
//   ld (0x1234), a   ; store to absolute address
//   jp nz, label     ; conditional jump
//   add a, b         ; 8-bit arithmetic
//   add hl, de       ; 16-bit arithmetic
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/Z80MCTargetDesc.h"
#include "TargetInfo/Z80TargetInfo.h"
#include "Z80.h"
#include "Z80RegisterInfo.h"

#include "llvm/ADT/APInt.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/MC/MCParser/MCParsedAsmOperand.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCValue.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Debug.h"

#include <memory>

#define DEBUG_TYPE "z80-asm-parser"

using namespace llvm;

namespace {

/// Z80Operand - Represents a parsed Z80 operand.
class Z80Operand : public MCParsedAsmOperand {
public:
  enum KindTy {
    k_Token,     // Mnemonic or condition code token
    k_Register,  // Register operand
    k_Immediate, // Immediate value
    k_Memory,    // Memory reference (HL), (BC), (DE), (nn)
    k_Indexed    // Indexed memory reference (IX+d), (IY+d)
  };

private:
  KindTy Kind;
  SMLoc Start, End;

  StringRef Tok;
  unsigned RegNum;
  const MCExpr *ImmVal;
  unsigned MemBaseReg;
  const MCExpr *MemDispVal;

public:
  Z80Operand(KindTy K, SMLoc S, SMLoc E)
      : Kind(K), Start(S), End(E), RegNum(0), ImmVal(nullptr), MemBaseReg(0),
        MemDispVal(nullptr) {}

  // Token operand
  static std::unique_ptr<Z80Operand> CreateToken(StringRef Str, SMLoc S) {
    auto Op = std::make_unique<Z80Operand>(k_Token, S, S);
    Op->Tok = Str;
    return Op;
  }

  // Register operand
  static std::unique_ptr<Z80Operand> CreateReg(unsigned Reg, SMLoc S, SMLoc E) {
    auto Op = std::make_unique<Z80Operand>(k_Register, S, E);
    Op->RegNum = Reg;
    return Op;
  }

  // Immediate operand
  static std::unique_ptr<Z80Operand> CreateImm(const MCExpr *Val, SMLoc S,
                                               SMLoc E) {
    auto Op = std::make_unique<Z80Operand>(k_Immediate, S, E);
    Op->ImmVal = Val;
    return Op;
  }

  // Memory operand: (HL), (BC), (DE), (nn)
  static std::unique_ptr<Z80Operand>
  CreateMem(unsigned BaseReg, const MCExpr *Disp, SMLoc S, SMLoc E) {
    auto Op = std::make_unique<Z80Operand>(k_Memory, S, E);
    Op->MemBaseReg = BaseReg;
    Op->MemDispVal = Disp;
    return Op;
  }

  // Indexed operand: (IX+d), (IY+d)
  static std::unique_ptr<Z80Operand>
  CreateIndexed(unsigned BaseReg, const MCExpr *Offset, SMLoc S, SMLoc E) {
    auto Op = std::make_unique<Z80Operand>(k_Indexed, S, E);
    Op->MemBaseReg = BaseReg;
    Op->MemDispVal = Offset;
    return Op;
  }

  bool isToken() const override { return Kind == k_Token; }
  bool isImm() const override { return Kind == k_Immediate; }
  bool isReg() const override { return Kind == k_Register; }
  bool isMem() const override { return Kind == k_Memory || Kind == k_Indexed; }

  bool isMemory() const { return Kind == k_Memory; }
  bool isIndexed() const { return Kind == k_Indexed; }

  // Immediate predicates for different sizes
  bool isImm3() const {
    if (!isImm())
      return false;
    if (const auto *CE = dyn_cast<MCConstantExpr>(ImmVal))
      return CE->getValue() >= 0 && CE->getValue() <= 7;
    return true; // Allow symbolic expressions
  }

  bool isImm8() const {
    if (!isImm())
      return false;
    if (const auto *CE = dyn_cast<MCConstantExpr>(ImmVal))
      return CE->getValue() >= -128 && CE->getValue() <= 255;
    return true; // Allow symbolic expressions
  }

  bool isImm16() const {
    if (!isImm())
      return false;
    if (const auto *CE = dyn_cast<MCConstantExpr>(ImmVal))
      return CE->getValue() >= -32768 && CE->getValue() <= 65535;
    return true; // Allow symbolic expressions
  }

  bool isPCRel8() const {
    // PC-relative can be any expression
    return isImm();
  }

  bool isAddr16() const {
    // 16-bit address can be any expression
    return isImm();
  }

  bool isDisp8() const {
    if (!isImm())
      return false;
    if (const auto *CE = dyn_cast<MCConstantExpr>(ImmVal))
      return CE->getValue() >= -128 && CE->getValue() <= 127;
    return true;
  }

  bool isPort8() const {
    // 8-bit port address (0-255) for IN/OUT instructions
    if (!isImm())
      return false;
    if (const auto *CE = dyn_cast<MCConstantExpr>(ImmVal))
      return CE->getValue() >= 0 && CE->getValue() <= 255;
    return true;
  }

  StringRef getToken() const {
    assert(Kind == k_Token && "Invalid access!");
    return Tok;
  }

  MCRegister getReg() const override {
    assert(Kind == k_Register && "Invalid access!");
    return RegNum;
  }

  const MCExpr *getImm() const {
    assert(Kind == k_Immediate && "Invalid access!");
    return ImmVal;
  }

  unsigned getMemBase() const {
    assert((Kind == k_Memory || Kind == k_Indexed) && "Invalid access!");
    return MemBaseReg;
  }

  const MCExpr *getMemDisp() const {
    assert((Kind == k_Memory || Kind == k_Indexed) && "Invalid access!");
    return MemDispVal;
  }

  SMLoc getStartLoc() const override { return Start; }
  SMLoc getEndLoc() const override { return End; }

  // Add operands to MCInst
  void addRegOperands(MCInst &Inst, unsigned N) const {
    assert(Kind == k_Register && "Unexpected operand kind");
    assert(N == 1 && "Invalid number of operands!");
    Inst.addOperand(MCOperand::createReg(RegNum));
  }

  void addImmOperands(MCInst &Inst, unsigned N) const {
    assert(Kind == k_Immediate && "Unexpected operand kind");
    assert(N == 1 && "Invalid number of operands!");
    addExpr(Inst, ImmVal);
  }

  // Same as addImmOperands for specific types
  void addImm3Operands(MCInst &Inst, unsigned N) const {
    addImmOperands(Inst, N);
  }

  void addImm8Operands(MCInst &Inst, unsigned N) const {
    addImmOperands(Inst, N);
  }

  void addImm16Operands(MCInst &Inst, unsigned N) const {
    addImmOperands(Inst, N);
  }

  void addPCRel8Operands(MCInst &Inst, unsigned N) const {
    addImmOperands(Inst, N);
  }

  void addAddr16Operands(MCInst &Inst, unsigned N) const {
    addImmOperands(Inst, N);
  }

  void addDisp8Operands(MCInst &Inst, unsigned N) const {
    addImmOperands(Inst, N);
  }

  void addPort8Operands(MCInst &Inst, unsigned N) const {
    addImmOperands(Inst, N);
  }

  void addExpr(MCInst &Inst, const MCExpr *Expr) const {
    if (!Expr)
      Inst.addOperand(MCOperand::createImm(0));
    else if (const auto *CE = dyn_cast<MCConstantExpr>(Expr))
      Inst.addOperand(MCOperand::createImm(CE->getValue()));
    else
      Inst.addOperand(MCOperand::createExpr(Expr));
  }

  void print(raw_ostream &OS, const MCAsmInfo &MAI) const override {
    switch (Kind) {
    case k_Token:
      OS << "Token: " << Tok;
      break;
    case k_Register:
      OS << "Reg: " << RegNum;
      break;
    case k_Immediate:
      OS << "Imm: <expr>";
      break;
    case k_Memory:
      OS << "Mem: base=" << MemBaseReg;
      break;
    case k_Indexed:
      OS << "Idx: base=" << MemBaseReg;
      break;
    }
    OS << "\n";
  }
};

/// Z80AsmParser - The Z80 assembly parser.
class Z80AsmParser : public MCTargetAsmParser {
  MCAsmParser &Parser;
  const MCRegisterInfo *MRI;

public:
  enum Z80MatchResultTy {
    Match_immediate = FIRST_TARGET_MATCH_RESULT_TY,
    Match_InvalidPCRel8,
    Match_InvalidAddr16,
  };

private:
#define GET_ASSEMBLER_HEADER
#include "Z80GenAsmMatcher.inc"

  bool parseRegister(MCRegister &Reg, SMLoc &StartLoc, SMLoc &EndLoc) override;
  ParseStatus tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                               SMLoc &EndLoc) override;

  bool parseInstruction(ParseInstructionInfo &Info, StringRef Name,
                        SMLoc NameLoc, OperandVector &Operands) override;

  bool matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                               OperandVector &Operands, MCStreamer &Out,
                               uint64_t &ErrorInfo,
                               bool MatchingInlineAsm) override;

  unsigned validateTargetOperandClass(MCParsedAsmOperand &Op,
                                      unsigned Kind) override;

  // Helper methods
  MCRegister parseRegisterName(StringRef Name);
  MCRegister tryParseRegisterName();
  bool tryParseRegisterOperand(OperandVector &Operands);
  bool parseOperand(OperandVector &Operands, StringRef Mnemonic);
  bool parseMemoryOperand(OperandVector &Operands);
  bool parseExpression(const MCExpr *&Expr);
  void eatComma();

  bool emit(MCInst &Inst, SMLoc const &Loc, MCStreamer &Out) const;
  bool invalidOperand(SMLoc const &Loc, OperandVector const &Operands,
                      uint64_t const &ErrorInfo);
  bool missingFeature(SMLoc const &Loc, uint64_t const &ErrorInfo);

public:
  Z80AsmParser(const MCSubtargetInfo &STI, MCAsmParser &Parser,
               const MCInstrInfo &MII, const MCTargetOptions &Options)
      : MCTargetAsmParser(Options, STI, MII), Parser(Parser) {
    MCAsmParserExtension::Initialize(Parser);
    MRI = getContext().getRegisterInfo();
    setAvailableFeatures(ComputeAvailableFeatures(STI.getFeatureBits()));
  }

  MCAsmParser &getParser() const { return Parser; }
  AsmLexer &getLexer() const { return Parser.getLexer(); }
};

} // end anonymous namespace

// Auto-generated Match Functions from TableGen
static MCRegister MatchRegisterName(StringRef Name);
static MCRegister MatchRegisterAltName(StringRef Name);

MCRegister Z80AsmParser::parseRegisterName(StringRef Name) {
  // Skip shadow register names with apostrophes - they cause parsing issues
  // and are rarely used directly in hand-written assembly
  if (Name.contains('\''))
    return MCRegister();

  // Try exact match first
  MCRegister Reg = MatchRegisterName(Name);
  if (Reg)
    return Reg;

  // Try alternate names
  Reg = MatchRegisterAltName(Name);
  if (Reg)
    return Reg;

  // Try case-insensitive match (Z80 is case-insensitive)
  Reg = MatchRegisterName(Name.lower());
  if (Reg)
    return Reg;

  Reg = MatchRegisterAltName(Name.lower());
  if (Reg)
    return Reg;

  Reg = MatchRegisterName(Name.upper());
  if (Reg)
    return Reg;

  return MatchRegisterAltName(Name.upper());
}

MCRegister Z80AsmParser::tryParseRegisterName() {
  if (Parser.getTok().isNot(AsmToken::Identifier))
    return MCRegister();

  StringRef Name = Parser.getTok().getString();
  return parseRegisterName(Name);
}

bool Z80AsmParser::tryParseRegisterOperand(OperandVector &Operands) {
  MCRegister Reg = tryParseRegisterName();

  if (!Reg)
    return true;

  SMLoc S = Parser.getTok().getLoc();
  SMLoc E = Parser.getTok().getEndLoc();
  Operands.push_back(Z80Operand::CreateReg(Reg, S, E));
  Parser.Lex(); // Eat register token

  return false;
}

bool Z80AsmParser::parseExpression(const MCExpr *&Expr) {
  return getParser().parseExpression(Expr);
}

bool Z80AsmParser::parseMemoryOperand(OperandVector &Operands) {
  SMLoc S = Parser.getTok().getLoc();

  // Expect '('
  if (Parser.getTok().isNot(AsmToken::LParen))
    return true;
  Parser.Lex(); // Eat '('

  // Check if it's a register or expression
  if (Parser.getTok().is(AsmToken::Identifier)) {
    MCRegister Reg = tryParseRegisterName();
    if (Reg) {
      Parser.Lex(); // Eat register

      // Check for indexed addressing (IX+d) or (IY+d)
      if (Reg == Z80::IX || Reg == Z80::IY) {
        if (Parser.getTok().is(AsmToken::Plus) ||
            Parser.getTok().is(AsmToken::Minus)) {
          bool IsNeg = Parser.getTok().is(AsmToken::Minus);
          Parser.Lex(); // Eat +/-

          const MCExpr *Offset;
          if (parseExpression(Offset))
            return true;

          if (IsNeg) {
            // Negate the offset
            Offset = MCUnaryExpr::createMinus(Offset, getContext());
          }

          if (Parser.getTok().isNot(AsmToken::RParen))
            return Error(Parser.getTok().getLoc(), "expected ')'");
          SMLoc E = Parser.getTok().getEndLoc();
          Parser.Lex(); // Eat ')'

          Operands.push_back(Z80Operand::CreateIndexed(Reg, Offset, S, E));
          return false;
        }
      }

      // Simple register indirect: (HL), (BC), (DE), (IX), (IY), (SP)
      if (Parser.getTok().isNot(AsmToken::RParen))
        return Error(Parser.getTok().getLoc(), "expected ')'");
      SMLoc E = Parser.getTok().getEndLoc();
      Parser.Lex(); // Eat ')'

      Operands.push_back(Z80Operand::CreateMem(Reg, nullptr, S, E));
      return false;
    }
  }

  // Must be an absolute address: (nn)
  const MCExpr *Addr;
  if (parseExpression(Addr))
    return true;

  if (Parser.getTok().isNot(AsmToken::RParen))
    return Error(Parser.getTok().getLoc(), "expected ')'");
  SMLoc E = Parser.getTok().getEndLoc();
  Parser.Lex(); // Eat ')'

  Operands.push_back(Z80Operand::CreateMem(0, Addr, S, E));
  return false;
}

bool Z80AsmParser::parseOperand(OperandVector &Operands, StringRef Mnemonic) {
  LLVM_DEBUG(dbgs() << "parseOperand\n");

  // Check for memory operand (starts with '(')
  if (getLexer().is(AsmToken::LParen)) {
    return parseMemoryOperand(Operands);
  }

  // Check for register
  if (getLexer().is(AsmToken::Identifier)) {
    // Try to parse as register first
    if (!tryParseRegisterOperand(Operands))
      return false;

    // Not a register - parse as expression/symbol
    SMLoc S = Parser.getTok().getLoc();
    const MCExpr *Expr;
    if (parseExpression(Expr))
      return true;
    SMLoc E = SMLoc::getFromPointer(Parser.getTok().getLoc().getPointer() - 1);
    Operands.push_back(Z80Operand::CreateImm(Expr, S, E));
    return false;
  }

  // Parse immediate value
  if (getLexer().is(AsmToken::Integer) || getLexer().is(AsmToken::Minus) ||
      getLexer().is(AsmToken::Plus) || getLexer().is(AsmToken::Hash)) {
    // Skip optional '#' prefix for immediates
    if (getLexer().is(AsmToken::Hash))
      Parser.Lex();

    SMLoc S = Parser.getTok().getLoc();
    const MCExpr *Expr;
    if (parseExpression(Expr))
      return true;
    SMLoc E = SMLoc::getFromPointer(Parser.getTok().getLoc().getPointer() - 1);
    Operands.push_back(Z80Operand::CreateImm(Expr, S, E));
    return false;
  }

  return Error(Parser.getTok().getLoc(), "unexpected token in operand");
}

void Z80AsmParser::eatComma() {
  if (getLexer().is(AsmToken::Comma))
    Parser.Lex();
}

bool Z80AsmParser::parseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                 SMLoc &EndLoc) {
  StartLoc = Parser.getTok().getLoc();
  Reg = tryParseRegisterName();
  EndLoc = Parser.getTok().getEndLoc();

  if (Reg) {
    Parser.Lex(); // Consume register token
    return false;
  }
  return true;
}

ParseStatus Z80AsmParser::tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                           SMLoc &EndLoc) {
  StartLoc = Parser.getTok().getLoc();
  Reg = tryParseRegisterName();
  EndLoc = Parser.getTok().getEndLoc();

  if (!Reg)
    return ParseStatus::NoMatch;

  Parser.Lex(); // Consume register token
  return ParseStatus::Success;
}

bool Z80AsmParser::parseInstruction(ParseInstructionInfo &Info, StringRef Name,
                                    SMLoc NameLoc, OperandVector &Operands) {
  // Add the mnemonic as the first operand
  Operands.push_back(Z80Operand::CreateToken(Name, NameLoc));

  // Parse operands
  int OperandNum = 0;
  while (getLexer().isNot(AsmToken::EndOfStatement)) {
    if (OperandNum > 0)
      eatComma();

    if (parseOperand(Operands, Name)) {
      SMLoc Loc = getLexer().getLoc();
      Parser.eatToEndOfStatement();
      return Error(Loc, "unexpected token in argument list");
    }
    OperandNum++;
  }

  Parser.Lex(); // Consume the EndOfStatement
  return false;
}

bool Z80AsmParser::emit(MCInst &Inst, SMLoc const &Loc, MCStreamer &Out) const {
  Inst.setLoc(Loc);
  Out.emitInstruction(Inst, *STI);
  return false;
}

bool Z80AsmParser::invalidOperand(SMLoc const &Loc,
                                  OperandVector const &Operands,
                                  uint64_t const &ErrorInfo) {
  SMLoc ErrorLoc = Loc;

  if (ErrorInfo != ~0U) {
    if (ErrorInfo >= Operands.size()) {
      return Error(ErrorLoc, "too few operands for instruction");
    } else {
      Z80Operand const &Op =
          static_cast<Z80Operand const &>(*Operands[ErrorInfo]);
      if (Op.getStartLoc() != SMLoc())
        ErrorLoc = Op.getStartLoc();
    }
  }

  return Error(ErrorLoc, "invalid operand for instruction");
}

bool Z80AsmParser::missingFeature(SMLoc const &Loc, uint64_t const &ErrorInfo) {
  return Error(Loc, "instruction requires a CPU feature not currently enabled");
}

bool Z80AsmParser::matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                                           OperandVector &Operands,
                                           MCStreamer &Out, uint64_t &ErrorInfo,
                                           bool MatchingInlineAsm) {
  MCInst Inst;
  unsigned MatchResult =
      MatchInstructionImpl(Operands, Inst, ErrorInfo, MatchingInlineAsm);

  switch (MatchResult) {
  case Match_Success:
    return emit(Inst, IDLoc, Out);
  case Match_MissingFeature:
    return missingFeature(IDLoc, ErrorInfo);
  case Match_InvalidOperand:
    return invalidOperand(IDLoc, Operands, ErrorInfo);
  case Match_MnemonicFail:
    return Error(IDLoc, "invalid instruction");
  case Match_immediate:
    return Error(IDLoc, "immediate operand out of range");
  default:
    return true;
  }
}

unsigned Z80AsmParser::validateTargetOperandClass(MCParsedAsmOperand &AsmOp,
                                                  unsigned ExpectedKind) {
  Z80Operand &Op = static_cast<Z80Operand &>(AsmOp);

  // Handle register classes
  if (Op.isReg()) {
    MCRegister Reg = Op.getReg();
    // Let TableGen handle the register class validation
    (void)Reg;
  }

  return Match_InvalidOperand;
}

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeZ80AsmParser() {
  RegisterMCAsmParser<Z80AsmParser> X(getTheZ80Target());
  RegisterMCAsmParser<Z80AsmParser> Y(getTheSM83Target());
}

#define GET_REGISTER_MATCHER
#define GET_MATCHER_IMPLEMENTATION
#include "Z80GenAsmMatcher.inc"
