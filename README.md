<img width="2560" height="1280" alt="LLVM-Z80" src="https://github.com/user-attachments/assets/0425ae08-9ecb-4270-840d-90145678df09" />


LLVM-Z80 is a LLVM fork supporting the Zilog Z80 series of microprocessors.

## Notice

The llvm-z80 project is not officially affiliated with or endorsed by the LLVM
Foundation or LLVM project. Our project is a fork of LLVM that provides a new
backend/target; our project is based on LLVM, not a part of LLVM. Our use of
LLVM or other related trademarks does not imply affiliation or endorsement.

**Generative AI played significant role in generating code for the development of LLVM-Z80**,
which is currently is an experimental stage. Although the project aims to produce highly optimized,
production-grade code, it is not yet stable enough for use in professional production environments.

If you require a Z80 C compiler for production purposes, please use [SDCC](https://sdcc.sourceforge.net/)

## Target
**Z80, binary-compatible CPUs, and the SM83 (GBZ80).**  
Z80, Z180, eZ80, Z80N, R800, μPD780, LH0080, SM83 (GBZ80), AP (Analogue Pocket). <sup><a href="#architecture-todo">†1</a></sup>

Physical Hardware Validation: : TODO  
Emulation Tested : Z80, SM83

## Feature
* Integer arithmetic for types **up to 128-bit width.** <sup><a href="#integer-arithmetic">†2</a></sup>
* Half and single-precision floating-point arithmetic in **full compliance with the IEEE 754 standard.** <sup><a href="#float-arithmetic">†3</a></sup><sup><a href="#fast-math">†4</a></sup>
* SDCC `__sdcccall(1)` / `__sdcccall(0)` compatible calling convention
* SDCC-compatible sdasz80 assembly output with Clang driver integration (`clang --target=z80`)
* GlobalISel-based code generation pipeline with register bank selection and instruction selection
* standard LLVM optimizations including constant folding, dead code elimination, copy propagation, and global register allocation
  
# Building LLVM-Z80

## Prerequisites
* CMake 3.20+
* Ninja (recommended)
* SDCC toolchain (`sdasz80`, `sdldz80`): required for assembling and linking Z80 binaries

## Clone the LLVM-Z80 repository

On Linux and MacOS:

```bash
git clone https://github.com/zlfn/llvm-z80.git
```

On Windows:

```bash
git clone --config core.autocrlf=false https://github.com/zlfn/llvm-z80.git
```

If you fail to use the --config flag as above, then verification tests will fail
on Windows.

## Build the LLVM-Z80 project

```bash
cmake -C clang/cmake/caches/Z80.cmake -G Ninja -S llvm -B build
ninja -C build # Build llc + clang + Z80Runtime
```

## Usage

### Z80
```bash
# Compile C to Intel HEX (clang auto-links runtime)
clang --target=z80 -O1 input.c -o output.ihx

# Execute with z88dk-ticks
makebin output.ihx output.bin
z88dk-ticks -trace output.bin

# Or step by step
clang --target=z80 -O1 -S -emit-llvm input.c -o input.ll
llc -mtriple=z80 -O1 -z80-asm-format=sdasz80 input.ll -o input.s
sdasz80 -g -o input.rel input.s
sdldz80 -i output input.rel build/lib/z80/z80_rt.lib
```

### SM83 (Game Boy)
```bash
clang --target=sm83 -O1 input.c -o output.ihx
makebin output.ihx output.bin
z88dk-ticks -mgbz80 -trace output.bin
```

Runtime libraries are built at `build/lib/z80/z80_rt.lib` and `build/lib/sm83/sm83_rt.lib`.

### Related Works
LLVM-Z80 stands on the shoulders of the following projects.
* [LLVM](https://llvm.org/) : The LLVM Compiler Infrastructure
* [LLVM-MOS](https://llvm-mos.org/) : Another attempt to create an LLVM backend for a classic CPU, and the upstream for this project.
* [LLVM-eZ80](https://github.com/jacobly0/llvm-project) / [ez80-clang](https://github.com/dinoboards/ez80-clang) : The most mature LLVM backend for the eZ80.
* [gb-llvm](https://github.com/DaveDuck321/gb-llvm) : An LLVM backend for the Nintendo Game Boy (SM83), including actual game ROM examples.
* [Rust-GB](https://github.com/zlfn/rust-gb) : Compiling Rust for the Game Boy, The reason this project was born.

**Many Other Experimental LLVM-Z80 Backends**
* [gt-retro-computing](https://github.com/gt-retro-computing/llvm-z80), [euclio](https://github.com/euclio/llvm-gbz80), [Bevinsky](https://github.com/Bevinsky/llvm-gbz80)

### Footnotes
- <sup><a id="architecture-todo" href="#architecture-todo">†1</a></sup>Planned optimizations for specific CPU cores (eZ80, Z180, Z80N, etc.) and upcoming support for derivative architectures (Rabbit 2000, TLCS-90).
- <sup><a id="integer-arithmetic" href="#integer-arithmetic">†2</a></sup>Primary support for i8, i16, i32, and i64; experimental support for i128.
- <sup><a id="float-arithmetic" href="#float-arithmetic">†3</a></sup>Primary support for f32, experimental support for f16, and planned support for f64.
- <sup><a id="fast-math" href="#fast-math">†4</a></sup>The `-ffast-math` flag allows you to disable strict IEEE 754 compliance. While this improves code efficiency, it may cause floating point operations to behave unexpectedly in certain edge cases.

---

<sub><font color="gray">
LLVM is a trademark of the LLVM Foundation.
Zilog and Z80 are registered trademarks of Littelfuse, Inc.
Use of these names is for identification and compatibility purposes only.
This project is an independent, community-driven effort and is not affiliated with, sponsored, or endorsed by the LLVM Foundation or Littelfuse, Inc.
</font></sub>

