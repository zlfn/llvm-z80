//===-- Z80Features.td - Z80 Features ---------------------*- tablegen -*-===//
//
// Part of LLVM-Z80, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//
// Z80 Features and Subtarget Definitions
//===---------------------------------------------------------------------===//

// Base Z80 feature - standard Z80 instruction set
def FeatureZ80
    : SubtargetFeature<"z80", "HasZ80", "true",
                       "Standard Z80 instruction set">;

// Z180 extensions (includes Z80)
def FeatureZ180
    : SubtargetFeature<"z180", "HasZ180", "true",
                       "Z180 extended instruction set",
                       [FeatureZ80]>;

// R800 extensions (MSX TurboR) - includes Z80 plus multiply
def FeatureR800
    : SubtargetFeature<"r800", "HasR800", "true",
                       "R800 instruction set with multiply",
                       [FeatureZ80]>;

// eZ80 extensions - 24-bit addressing mode
def FeatureEZ80
    : SubtargetFeature<"ez80", "HasEZ80", "true",
                       "eZ80 24-bit addressing extensions",
                       [FeatureZ80]>;

// Sharp SM83 (Game Boy CPU) - shares core Z80 opcodes but lacks
// ED/DD/FD prefixes (no IX/IY), exchange group, block transfer, I/O,
// and sign/parity conditions. Adds unique instructions (LDH, SWAP, etc.)
def FeatureSM83
    : SubtargetFeature<"sm83", "HasSM83", "true",
                       "Sharp SM83 (Game Boy) instruction set">;

// Undocumented Z80 instructions: IXH/IXL/IYH/IYL half-index registers,
// SLL, IN/OUT with C register, etc. Absent on SM83.
def FeatureUndocumented
    : SubtargetFeature<"undocumented", "HasUndocumented", "true",
                       "Enable undocumented Z80 instructions (IXH, IXL, IYH, IYL, etc.)">;

// Static stack allocation
def FeatureStaticStack
    : SubtargetFeature<"static-stack", "StaticStack", "true",
                       "Use statically-allocated stack frames if possible">;
