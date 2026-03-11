//===-- Z80FixupKinds.cpp - Z80 fixup kinds  ------------------------------===//
//
// Part of LLVM-Z80, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the Z80AsmBackend class.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/Z80FixupKinds.h"
#include "llvm/MC/MCAsmBackend.h"

namespace llvm {
MCFixupKindInfo
Z80FixupKinds::getFixupKindInfo(const Z80::Fixups Kind,
                                const MCAsmBackend *Alternative) {
  const static MCFixupKindInfo Infos[Z80::NumTargetFixupKinds] = {
      // This table *must* be in same the order of fixup_* kinds in
      // Z80FixupKinds.h.
      //
      // name, offset, bits, flags
      {"Imm8", 0, 8, 0},            // An 8-bit immediate value.
      {"Imm16", 0, 16, 0},          // An 16-bit immediate value.
      {"Addr8", 0, 8, 0},           // An 8-bit zero page address.
      {"Addr16", 0, 16, 0},         // A 16-bit address.
      {"Addr16_Low", 0, 8, 0},      // The low byte of a 16-bit address.
      {"Addr16_High", 0, 8, 0},     // The high byte of a 16-bit address.
      {"Addr24", 0, 24, 0},         // A 24-bit address.
      {"Addr24_Bank", 0, 8, 0},     // The bank byte of a 24-bit address.
      {"Addr24_Segment", 0, 16, 0}, // The segment 16-bits of a 24-byte address.
      {"Addr24_Segment_Low", 0, 8,
       0}, // The low byte of the segment of a 24-bit addr
      {"Addr24_Segment_High", 0, 8,
       0},                  // The high byte of the segment of a 24-bit addr
      {"Addr13", 0, 13, 0}, // A 13-bit address.
      {"PCRel8", 0, 8, 0},
      {"PCRel16", 0, 16, 0},
      {"AddrAsciz", 0, 0, 0},
      {"Disp8", 0, 8, 0}};
  if (Kind < static_cast<Z80::Fixups>(FirstTargetFixupKind)) {
    assert(Alternative &&
           "Alternative Z80 backend expected, but none was given!");
    return Alternative->getFixupKindInfo(static_cast<MCFixupKind>(Kind));
  }
  assert(unsigned(Kind - FirstTargetFixupKind) <
             Z80::Fixups::NumTargetFixupKinds &&
         "Invalid kind!");
  return Infos[Kind - FirstTargetFixupKind];
}

} // namespace llvm
