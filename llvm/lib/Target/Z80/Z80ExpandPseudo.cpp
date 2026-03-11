//===-- Z80ExpandPseudo.cpp - Z80 Pseudo Expansion Pass -------------------===//
//
// Part of LLVM-Z80, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the Z80 pseudo instruction expansion pass.
// It handles pseudo instructions that require MBB splitting, such as
// variable shift loops (SHL8_VAR, LSHR8_VAR, etc.) which expand to
// DJNZ-based loops.
//
//===----------------------------------------------------------------------===//

#include "Z80ExpandPseudo.h"

#include "MCTargetDesc/Z80MCTargetDesc.h"
#include "Z80.h"
#include "Z80InstrInfo.h"
#include "Z80OpcodeUtils.h"
#include "Z80Subtarget.h"

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

#define DEBUG_TYPE "z80-expand-pseudo"

using namespace llvm;

namespace {

class Z80ExpandPseudo : public MachineFunctionPass {
public:
  static char ID;

  Z80ExpandPseudo() : MachineFunctionPass(ID) {
    llvm::initializeZ80ExpandPseudoPass(*PassRegistry::getPassRegistry());
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  bool expandVarShift(MachineBasicBlock &MBB, MachineInstr &MI,
                      const Z80InstrInfo &TII);
  bool expandMul8(MachineBasicBlock &MBB, MachineInstr &MI,
                  const Z80InstrInfo &TII);
  bool expandUDivMod8(MachineBasicBlock &MBB, MachineInstr &MI,
                      const Z80InstrInfo &TII, bool IsDiv);
  bool expandSDivMod8(MachineBasicBlock &MBB, MachineInstr &MI,
                      const Z80InstrInfo &TII, bool IsDiv);
  bool expandSatArith8(MachineBasicBlock &MBB, MachineInstr &MI,
                       const Z80InstrInfo &TII);
};

bool Z80ExpandPseudo::runOnMachineFunction(MachineFunction &MF) {
  const auto &STI = MF.getSubtarget<Z80Subtarget>();
  const auto &TII = *STI.getInstrInfo();
  bool Modified = false;

  // Iterate MBBs safely: advance iterator before processing so that
  // MBB splitting doesn't invalidate it. New MBBs inserted after the
  // current one will be visited naturally.
  for (auto MBI = MF.begin(); MBI != MF.end(); ++MBI) {
    MachineBasicBlock &MBB = *MBI;

    for (auto MI = MBB.begin(), ME = MBB.end(); MI != ME;) {
      MachineInstr &Inst = *MI;
      ++MI; // Advance before potential erase

      switch (Inst.getOpcode()) {
      case Z80::SHL8_VAR:
      case Z80::LSHR8_VAR:
      case Z80::ASHR8_VAR:
      case Z80::ROTL8_VAR:
      case Z80::ROTR8_VAR:
      case Z80::SHL16_VAR:
      case Z80::LSHR16_VAR:
      case Z80::ASHR16_VAR:
        Modified |= expandVarShift(MBB, Inst, TII);
        MI = MBB.end();
        break;
      case Z80::MUL8:
        Modified |= expandMul8(MBB, Inst, TII);
        MI = MBB.end();
        break;
      case Z80::UDIV8:
        Modified |= expandUDivMod8(MBB, Inst, TII, /*IsDiv=*/true);
        MI = MBB.end();
        break;
      case Z80::UMOD8:
        Modified |= expandUDivMod8(MBB, Inst, TII, /*IsDiv=*/false);
        MI = MBB.end();
        break;
      case Z80::SDIV8:
        Modified |= expandSDivMod8(MBB, Inst, TII, /*IsDiv=*/true);
        MI = MBB.end();
        break;
      case Z80::SMOD8:
        Modified |= expandSDivMod8(MBB, Inst, TII, /*IsDiv=*/false);
        MI = MBB.end();
        break;
      case Z80::UADDSAT8:
      case Z80::USUBSAT8:
      case Z80::SADDSAT8:
      case Z80::SSUBSAT8:
        Modified |= expandSatArith8(MBB, Inst, TII);
        MI = MBB.end();
        break;
      default:
        break;
      }
    }
  }

  return Modified;
}

bool Z80ExpandPseudo::expandVarShift(MachineBasicBlock &MBB, MachineInstr &MI,
                                     const Z80InstrInfo &TII) {
  // Expand variable shift pseudo into a loop:
  //
  //   HeadMBB (original):
  //     ...
  //     inc b
  //     dec b          ; sets Z flag if B == 0, restores B
  //     jr z, TailMBB  ; skip loop if shift amount is 0
  //   LoopMBB:
  //     <shift instruction(s)>
  //     djnz LoopMBB   ; Z80: B--; loop if B != 0
  //     — or —
  //     dec b           ; SM83: B-- (SM83 lacks DJNZ)
  //     jr nz, LoopMBB  ; SM83: loop if B != 0
  //   TailMBB:
  //     ...             ; rest of original MBB

  MachineFunction *MF = MBB.getParent();
  const auto &STI = MF->getSubtarget<Z80Subtarget>();
  DebugLoc DL = MI.getDebugLoc();

  // Create new basic blocks.
  MachineBasicBlock *LoopMBB = MF->CreateMachineBasicBlock();
  MachineBasicBlock *TailMBB = MF->CreateMachineBasicBlock();

  // Insert after current MBB.
  MachineFunction::iterator InsertPos = std::next(MBB.getIterator());
  MF->insert(InsertPos, LoopMBB);
  MF->insert(InsertPos, TailMBB);

  // Move everything after MI to TailMBB.
  TailMBB->splice(TailMBB->begin(), &MBB,
                  std::next(MachineBasicBlock::iterator(MI)), MBB.end());
  TailMBB->transferSuccessorsAndUpdatePHIs(&MBB);

  // HeadMBB: test B for zero and branch.
  // INC B / DEC B sets Z flag based on original B value without changing it.
  BuildMI(&MBB, DL, TII.get(Z80::INC_B));
  BuildMI(&MBB, DL, TII.get(Z80::DEC_B));
  BuildMI(&MBB, DL, TII.get(Z80::JR_Z_e)).addMBB(TailMBB);
  MBB.addSuccessor(LoopMBB);
  MBB.addSuccessor(TailMBB);

  // LoopMBB: emit shift instruction(s).
  switch (MI.getOpcode()) {
  case Z80::SHL8_VAR:
    BuildMI(LoopMBB, DL, TII.get(Z80::SLA_A));
    break;
  case Z80::LSHR8_VAR:
    BuildMI(LoopMBB, DL, TII.get(Z80::SRL_A));
    break;
  case Z80::ASHR8_VAR:
    BuildMI(LoopMBB, DL, TII.get(Z80::SRA_A));
    break;
  case Z80::ROTL8_VAR:
    BuildMI(LoopMBB, DL, TII.get(Z80::RLCA));
    break;
  case Z80::ROTR8_VAR:
    BuildMI(LoopMBB, DL, TII.get(Z80::RRCA));
    break;
  case Z80::SHL16_VAR:
    BuildMI(LoopMBB, DL, TII.get(Z80::ADD_HL_HL));
    break;
  case Z80::LSHR16_VAR:
    BuildMI(LoopMBB, DL, TII.get(Z80::SRL_H));
    BuildMI(LoopMBB, DL, TII.get(Z80::RR_L));
    break;
  case Z80::ASHR16_VAR:
    BuildMI(LoopMBB, DL, TII.get(Z80::SRA_H));
    BuildMI(LoopMBB, DL, TII.get(Z80::RR_L));
    break;
  }

  if (STI.hasSM83()) {
    // SM83 lacks DJNZ; use DEC B + JR NZ instead.
    BuildMI(LoopMBB, DL, TII.get(Z80::DEC_B));
    BuildMI(LoopMBB, DL, TII.get(Z80::JR_NZ_e)).addMBB(LoopMBB);
  } else {
    BuildMI(LoopMBB, DL, TII.get(Z80::DJNZ_e)).addMBB(LoopMBB);
  }
  LoopMBB->addSuccessor(LoopMBB); // loop back
  LoopMBB->addSuccessor(TailMBB); // fall through when done

  MI.eraseFromParent();
  return true;
}

bool Z80ExpandPseudo::expandMul8(MachineBasicBlock &MBB, MachineInstr &MI,
                                 const Z80InstrInfo &TII) {
  // Expand MUL8 pseudo into an 8-bit shift-add multiply loop.
  // Input: A = multiplier, E = multiplicand
  // Output: A = result (low 8 bits of multiplier * multiplicand)
  //
  //   HeadMBB:
  //     ld d, a       ; D = multiplier (will be shifted out)
  //     xor a         ; A = 0 (accumulator)
  //     ld b, #8      ; 8-bit counter
  //   LoopMBB:
  //     add a, a      ; A <<= 1
  //     rl d          ; D <<= 1, MSB -> carry
  //     jr nc, SkipMBB
  //     add a, e      ; A += multiplicand
  //   SkipMBB:
  //     djnz LoopMBB  ; B--; loop if B != 0
  //   TailMBB:
  //     ...

  MachineFunction *MF = MBB.getParent();
  const auto &STI = MF->getSubtarget<Z80Subtarget>();
  DebugLoc DL = MI.getDebugLoc();

  MachineBasicBlock *LoopMBB = MF->CreateMachineBasicBlock();
  MachineBasicBlock *SkipMBB = MF->CreateMachineBasicBlock();
  MachineBasicBlock *TailMBB = MF->CreateMachineBasicBlock();

  MachineFunction::iterator InsertPos = std::next(MBB.getIterator());
  MF->insert(InsertPos, LoopMBB);
  MF->insert(InsertPos, SkipMBB);
  MF->insert(InsertPos, TailMBB);

  // Move everything after MI to TailMBB.
  TailMBB->splice(TailMBB->begin(), &MBB,
                  std::next(MachineBasicBlock::iterator(MI)), MBB.end());
  TailMBB->transferSuccessorsAndUpdatePHIs(&MBB);

  // HeadMBB: setup
  BuildMI(&MBB, DL, TII.get(Z80::LD_D_A));           // D = multiplier
  BuildMI(&MBB, DL, TII.get(Z80::XOR_A));            // A = 0
  BuildMI(&MBB, DL, TII.get(Z80::LD_B_n)).addImm(8); // B = 8
  MBB.addSuccessor(LoopMBB);

  // LoopMBB: shift and conditionally add
  BuildMI(LoopMBB, DL, TII.get(Z80::ADD_A_A)); // A <<= 1
  BuildMI(LoopMBB, DL, TII.get(Z80::RL_D));    // D <<= 1, MSB -> carry
  BuildMI(LoopMBB, DL, TII.get(Z80::JR_NC_e)).addMBB(SkipMBB);
  BuildMI(LoopMBB, DL, TII.get(Z80::ADD_A_E)); // A += multiplicand
  LoopMBB->addSuccessor(SkipMBB);              // jr nc taken
  LoopMBB->addSuccessor(SkipMBB);              // fall through (after add)

  // SkipMBB: loop back
  if (STI.hasSM83()) {
    BuildMI(SkipMBB, DL, TII.get(Z80::DEC_B));
    BuildMI(SkipMBB, DL, TII.get(Z80::JR_NZ_e)).addMBB(LoopMBB);
  } else {
    BuildMI(SkipMBB, DL, TII.get(Z80::DJNZ_e)).addMBB(LoopMBB);
  }
  SkipMBB->addSuccessor(LoopMBB); // loop back
  SkipMBB->addSuccessor(TailMBB); // fall through when done

  MI.eraseFromParent();
  return true;
}

bool Z80ExpandPseudo::expandUDivMod8(MachineBasicBlock &MBB, MachineInstr &MI,
                                     const Z80InstrInfo &TII, bool IsDiv) {
  // Expand UDIV8/UMOD8 pseudo into an 8-bit restoring division loop.
  // Input: A = dividend, E = divisor
  // Output: A = quotient (UDIV8) or remainder (UMOD8)
  //
  //   HeadMBB:
  //     ld d, a       ; D = dividend (shifted out as quotient bits)
  //     xor a         ; A = 0 (remainder)
  //     ld b, #8      ; 8-bit counter
  //   LoopMBB:
  //     sla d         ; shift dividend MSB into carry
  //     rla           ; remainder = remainder*2 + carry
  //     cp e          ; compare remainder with divisor
  //     jr c, SkipMBB ; if remainder < divisor, skip
  //     sub e         ; remainder -= divisor
  //     inc d         ; set quotient bit 0
  //   SkipMBB:
  //     djnz LoopMBB
  //   TailMBB:
  //     ld a, d       ; (UDIV8 only: move quotient to A)

  MachineFunction *MF = MBB.getParent();
  const auto &STI = MF->getSubtarget<Z80Subtarget>();
  DebugLoc DL = MI.getDebugLoc();

  MachineBasicBlock *LoopMBB = MF->CreateMachineBasicBlock();
  MachineBasicBlock *SkipMBB = MF->CreateMachineBasicBlock();
  MachineBasicBlock *TailMBB = MF->CreateMachineBasicBlock();

  MachineFunction::iterator InsertPos = std::next(MBB.getIterator());
  MF->insert(InsertPos, LoopMBB);
  MF->insert(InsertPos, SkipMBB);
  MF->insert(InsertPos, TailMBB);

  TailMBB->splice(TailMBB->begin(), &MBB,
                  std::next(MachineBasicBlock::iterator(MI)), MBB.end());
  TailMBB->transferSuccessorsAndUpdatePHIs(&MBB);

  // HeadMBB: setup
  BuildMI(&MBB, DL, TII.get(Z80::LD_D_A)); // D = dividend
  BuildMI(&MBB, DL, TII.get(Z80::XOR_A));  // A = 0 (remainder)
  BuildMI(&MBB, DL, TII.get(Z80::LD_B_n)).addImm(8);
  MBB.addSuccessor(LoopMBB);

  // LoopMBB: restoring division step
  BuildMI(LoopMBB, DL, TII.get(Z80::SLA_D)); // shift dividend, MSB->carry
  BuildMI(LoopMBB, DL, TII.get(Z80::RLA));   // remainder = remainder*2 + carry
  BuildMI(LoopMBB, DL, TII.get(Z80::CP_E));  // compare remainder vs divisor
  BuildMI(LoopMBB, DL, TII.get(Z80::JR_C_e)).addMBB(SkipMBB);
  BuildMI(LoopMBB, DL, TII.get(Z80::SUB_E)); // remainder -= divisor
  BuildMI(LoopMBB, DL, TII.get(Z80::INC_D)); // set quotient bit
  LoopMBB->addSuccessor(SkipMBB);            // jr c taken
  LoopMBB->addSuccessor(SkipMBB);            // fall through

  // SkipMBB: loop back
  if (STI.hasSM83()) {
    BuildMI(SkipMBB, DL, TII.get(Z80::DEC_B));
    BuildMI(SkipMBB, DL, TII.get(Z80::JR_NZ_e)).addMBB(LoopMBB);
  } else {
    BuildMI(SkipMBB, DL, TII.get(Z80::DJNZ_e)).addMBB(LoopMBB);
  }
  SkipMBB->addSuccessor(LoopMBB);
  SkipMBB->addSuccessor(TailMBB);

  // TailMBB: for UDIV8, move quotient from D to A
  if (IsDiv) {
    BuildMI(*TailMBB, TailMBB->begin(), DL, TII.get(Z80::LD_A_D));
  }
  // For UMOD8, remainder is already in A

  MI.eraseFromParent();
  return true;
}

bool Z80ExpandPseudo::expandSDivMod8(MachineBasicBlock &MBB, MachineInstr &MI,
                                     const Z80InstrInfo &TII, bool IsDiv) {
  // Expand SDIV8/SMOD8 pseudo into sign-handling + unsigned restoring division.
  // Input: A = dividend, E = divisor
  // Output: A = quotient (SDIV8) or remainder (SMOD8)
  //
  // Register allocation: B = loop counter (DJNZ), C = sign info, D = working
  // quotient
  //   SDIV: C = dividend XOR divisor (quotient sign in bit 7)
  //   SMOD: C = original dividend (remainder sign in bit 7)
  //
  //   HeadMBB:
  //     SDIV: xor e; ld c, a; xor e  (C = dvd XOR dsr, A = dvd restored)
  //     SMOD: ld c, a                 (C = original dividend)
  //     or a
  //     jp p, DvdPosMBB
  //     neg
  //   DvdPosMBB:
  //     ld d, a          ; D = |dividend|
  //     bit 7, e
  //     jr z, DsrPosMBB
  //     xor a; sub e; ld e, a  ; E = |divisor|
  //   DsrPosMBB:
  //     xor a            ; A = 0 (remainder)
  //     ld b, #8         ; loop counter
  //   LoopMBB:
  //     sla d; rla; cp e; jr c, SkipMBB; sub e; inc d
  //   SkipMBB:
  //     djnz LoopMBB
  //   SignMBB:
  //     SDIV: ld a, d; bit 7, c; jr z, TailMBB; neg
  //     SMOD: bit 7, c; jr z, TailMBB; neg
  //   TailMBB:
  //     (result in A)

  MachineFunction *MF = MBB.getParent();
  const auto &STI = MF->getSubtarget<Z80Subtarget>();
  DebugLoc DL = MI.getDebugLoc();
  bool IsSM83 = STI.hasSM83();

  MachineBasicBlock *DvdPosMBB = MF->CreateMachineBasicBlock();
  MachineBasicBlock *DsrPosMBB = MF->CreateMachineBasicBlock();
  MachineBasicBlock *LoopMBB = MF->CreateMachineBasicBlock();
  MachineBasicBlock *SkipMBB = MF->CreateMachineBasicBlock();
  MachineBasicBlock *SignMBB = MF->CreateMachineBasicBlock();
  MachineBasicBlock *TailMBB = MF->CreateMachineBasicBlock();

  MachineFunction::iterator InsertPos = std::next(MBB.getIterator());
  MF->insert(InsertPos, DvdPosMBB);
  MF->insert(InsertPos, DsrPosMBB);
  MF->insert(InsertPos, LoopMBB);
  MF->insert(InsertPos, SkipMBB);
  MF->insert(InsertPos, SignMBB);
  MF->insert(InsertPos, TailMBB);

  TailMBB->splice(TailMBB->begin(), &MBB,
                  std::next(MachineBasicBlock::iterator(MI)), MBB.end());
  TailMBB->transferSuccessorsAndUpdatePHIs(&MBB);

  // HeadMBB: save sign info to C, make dividend positive
  if (IsDiv) {
    // C = dividend XOR divisor (quotient sign in bit 7)
    BuildMI(&MBB, DL, TII.get(Z80::XOR_E));  // A = dividend XOR divisor
    BuildMI(&MBB, DL, TII.get(Z80::LD_C_A)); // C = XOR result
    BuildMI(&MBB, DL,
            TII.get(Z80::XOR_E)); // A = dividend (XOR is self-inverse)
  } else {
    // C = original dividend (remainder sign in bit 7)
    BuildMI(&MBB, DL, TII.get(Z80::LD_C_A)); // C = dividend
  }
  if (IsSM83) {
    // SM83: no JP P — use BIT 7,A + JR Z (jump if positive)
    BuildMI(&MBB, DL, TII.get(Z80::BIT_7_A));
    BuildMI(&MBB, DL, TII.get(Z80::JR_Z_e)).addMBB(DvdPosMBB);
    // SM83: no NEG — use CPL + INC A
    BuildMI(&MBB, DL, TII.get(Z80::CPL));
    BuildMI(&MBB, DL, TII.get(Z80::INC_A));
  } else {
    BuildMI(&MBB, DL, TII.get(Z80::OR_A)); // set flags for sign test
    BuildMI(&MBB, DL, TII.get(Z80::JP_P_nn)).addMBB(DvdPosMBB);
    BuildMI(&MBB, DL, TII.get(Z80::NEG)); // A = -dividend
  }
  MBB.addSuccessor(DvdPosMBB); // branch taken
  MBB.addSuccessor(DvdPosMBB); // fall through after negate

  // DvdPosMBB: save |dividend|, make divisor positive
  BuildMI(DvdPosMBB, DL, TII.get(Z80::LD_D_A)); // D = |dividend|
  BuildMI(DvdPosMBB, DL, TII.get(Z80::BIT_7_E));
  BuildMI(DvdPosMBB, DL, TII.get(Z80::JR_Z_e)).addMBB(DsrPosMBB);
  BuildMI(DvdPosMBB, DL, TII.get(Z80::XOR_A));
  BuildMI(DvdPosMBB, DL, TII.get(Z80::SUB_E));  // A = 0 - divisor = -divisor
  BuildMI(DvdPosMBB, DL, TII.get(Z80::LD_E_A)); // E = |divisor|
  DvdPosMBB->addSuccessor(DsrPosMBB);           // jr z taken
  DvdPosMBB->addSuccessor(DsrPosMBB);           // fall through

  // DsrPosMBB: setup for unsigned division loop
  BuildMI(DsrPosMBB, DL, TII.get(Z80::XOR_A));            // A = 0 (remainder)
  BuildMI(DsrPosMBB, DL, TII.get(Z80::LD_B_n)).addImm(8); // B = loop counter
  DsrPosMBB->addSuccessor(LoopMBB);

  // LoopMBB: restoring division step
  BuildMI(LoopMBB, DL, TII.get(Z80::SLA_D));
  BuildMI(LoopMBB, DL, TII.get(Z80::RLA));
  BuildMI(LoopMBB, DL, TII.get(Z80::CP_E));
  BuildMI(LoopMBB, DL, TII.get(Z80::JR_C_e)).addMBB(SkipMBB);
  BuildMI(LoopMBB, DL, TII.get(Z80::SUB_E));
  BuildMI(LoopMBB, DL, TII.get(Z80::INC_D));
  LoopMBB->addSuccessor(SkipMBB); // jr c taken
  LoopMBB->addSuccessor(SkipMBB); // fall through

  // SkipMBB: loop control
  if (IsSM83) {
    BuildMI(SkipMBB, DL, TII.get(Z80::DEC_B));
    BuildMI(SkipMBB, DL, TII.get(Z80::JR_NZ_e)).addMBB(LoopMBB);
  } else {
    BuildMI(SkipMBB, DL, TII.get(Z80::DJNZ_e)).addMBB(LoopMBB);
  }
  SkipMBB->addSuccessor(LoopMBB);
  SkipMBB->addSuccessor(SignMBB);

  // SignMBB: apply sign to result
  if (IsDiv) {
    // Quotient sign = XOR of dividend and divisor signs (saved in C bit 7)
    BuildMI(SignMBB, DL, TII.get(Z80::LD_A_D)); // A = unsigned quotient
    BuildMI(SignMBB, DL, TII.get(Z80::BIT_7_C));
    BuildMI(SignMBB, DL, TII.get(Z80::JR_Z_e)).addMBB(TailMBB);
    if (IsSM83) {
      BuildMI(SignMBB, DL, TII.get(Z80::CPL));
      BuildMI(SignMBB, DL, TII.get(Z80::INC_A));
    } else {
      BuildMI(SignMBB, DL, TII.get(Z80::NEG));
    }
  } else {
    // Remainder sign = dividend sign (saved in C bit 7)
    BuildMI(SignMBB, DL, TII.get(Z80::BIT_7_C));
    BuildMI(SignMBB, DL, TII.get(Z80::JR_Z_e)).addMBB(TailMBB);
    if (IsSM83) {
      BuildMI(SignMBB, DL, TII.get(Z80::CPL));
      BuildMI(SignMBB, DL, TII.get(Z80::INC_A));
    } else {
      BuildMI(SignMBB, DL, TII.get(Z80::NEG));
    }
  }
  SignMBB->addSuccessor(TailMBB); // jr z taken
  SignMBB->addSuccessor(TailMBB); // fall through after negate

  MI.eraseFromParent();
  return true;
}

static unsigned getADD8Opc(Register Reg) {
  static const unsigned T[] = {Z80::ADD_A_A, Z80::ADD_A_B, Z80::ADD_A_C,
                               Z80::ADD_A_D, Z80::ADD_A_E, Z80::ADD_A_H,
                               Z80::ADD_A_L};
  return T[Z80::gr8RegToIndex(Reg)];
}

static unsigned getSUBOpc(Register Reg) {
  static const unsigned T[] = {Z80::SUB_A, Z80::SUB_B, Z80::SUB_C, Z80::SUB_D,
                               Z80::SUB_E, Z80::SUB_H, Z80::SUB_L};
  return T[Z80::gr8RegToIndex(Reg)];
}

bool Z80ExpandPseudo::expandSatArith8(MachineBasicBlock &MBB, MachineInstr &MI,
                                      const Z80InstrInfo &TII) {
  // Expand i8 saturating arithmetic pseudos.
  //
  // UADDSAT8 src:  ADD A,src; JR NC,.done; LD A,#0xFF; .done:
  // USUBSAT8 src:  SUB src; JR NC,.done; XOR A; .done:
  // SADDSAT8 src:  ADD A,src; JP PO,.done; RLCA; SBC A,A; XOR #0x80; .done:
  // SSUBSAT8 src:  SUB src; JP PO,.done; RLCA; SBC A,A; XOR #0x80; .done:

  MachineFunction *MF = MBB.getParent();
  DebugLoc DL = MI.getDebugLoc();
  unsigned Opc = MI.getOpcode();
  Register SrcReg = MI.getOperand(0).getReg();

  bool IsAdd = (Opc == Z80::UADDSAT8 || Opc == Z80::SADDSAT8);
  bool IsSigned = (Opc == Z80::SADDSAT8 || Opc == Z80::SSUBSAT8);

  // Create the tail MBB (rest of the original block after the pseudo).
  MachineBasicBlock *TailMBB = MF->CreateMachineBasicBlock();
  MachineFunction::iterator InsertPos = std::next(MBB.getIterator());
  MF->insert(InsertPos, TailMBB);

  // Move everything after MI to TailMBB.
  TailMBB->splice(TailMBB->begin(), &MBB,
                  std::next(MachineBasicBlock::iterator(MI)), MBB.end());
  TailMBB->transferSuccessorsAndUpdatePHIs(&MBB);

  // Emit the arithmetic instruction.
  if (IsAdd)
    BuildMI(&MBB, DL, TII.get(getADD8Opc(SrcReg)));
  else
    BuildMI(&MBB, DL, TII.get(getSUBOpc(SrcReg)));

  if (IsSigned) {
    // Signed: JP PO,.done (P/V=0 means no overflow)
    BuildMI(&MBB, DL, TII.get(Z80::JP_PO_nn)).addMBB(TailMBB);

    // Create saturation MBB.
    MachineBasicBlock *SatMBB = MF->CreateMachineBasicBlock();
    MF->insert(TailMBB->getIterator(), SatMBB);

    // Saturation: RLCA; SBC A,A; XOR #0x80
    // If result was negative (S=1): RLCA puts 1 in CF, SBC A,A = 0xFF,
    //   XOR 0x80 = 0x7F (positive overflow → max positive)
    // If result was positive (S=0): RLCA puts 0 in CF, SBC A,A = 0x00,
    //   XOR 0x80 = 0x80 (negative overflow → min negative)
    BuildMI(SatMBB, DL, TII.get(Z80::RLCA));
    BuildMI(SatMBB, DL, TII.get(Z80::SBC_A_A));
    BuildMI(SatMBB, DL, TII.get(Z80::XOR_n)).addImm(0x80);
    SatMBB->addSuccessor(TailMBB);

    MBB.addSuccessor(TailMBB); // JP PO taken (no overflow)
    MBB.addSuccessor(SatMBB);  // fall through (overflow)
  } else {
    // Unsigned: JR NC,.done (no carry = no saturation)
    BuildMI(&MBB, DL, TII.get(Z80::JR_NC_e)).addMBB(TailMBB);

    // Create saturation MBB.
    MachineBasicBlock *SatMBB = MF->CreateMachineBasicBlock();
    MF->insert(TailMBB->getIterator(), SatMBB);

    if (IsAdd) {
      // UADDSAT: saturate to 0xFF
      BuildMI(SatMBB, DL, TII.get(Z80::LD_A_n)).addImm(0xFF);
    } else {
      // USUBSAT: saturate to 0x00
      BuildMI(SatMBB, DL, TII.get(Z80::XOR_A));
    }
    SatMBB->addSuccessor(TailMBB);

    MBB.addSuccessor(TailMBB); // JR NC taken (no saturation)
    MBB.addSuccessor(SatMBB);  // fall through (saturation)
  }

  MI.eraseFromParent();
  return true;
}

} // namespace

char Z80ExpandPseudo::ID = 0;

INITIALIZE_PASS(Z80ExpandPseudo, DEBUG_TYPE,
                "Expand Z80 pseudo instructions requiring MBB splitting", false,
                false)

MachineFunctionPass *llvm::createZ80ExpandPseudoPass() {
  return new Z80ExpandPseudo();
}
