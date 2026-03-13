# LLVM-Z80 Test Suite
This crate conducts dynamic testing of LLVM-Z80.

## Prerequisites
* Rust
* Builded LLVM-Z80 binary in ../build directory.
* SDCC Toolchain
* Z88dk Z80 Emulator

## Run
```bash
# Run the basic test suites
cargo run
# Run the benchmark
cargo run bench
```

## Testcases
* [clang](https://github.com/zlfn/llvm-z80/tree/main/z80_test/testcases/clang): Check the behavior of Z80 for various C codes.
* [lcc](https://github.com/zlfn/llvm-z80/tree/main/z80_test/testcases/llc): Check the behavior of Z80 for various LLVM-IR codes.
* [sdcc](https://github.com/zlfn/llvm-z80/tree/main/z80_test/testcases/sdcc): Check interoperability with SDCC.
* [custom](https://github.com/zlfn/llvm-z80/tree/main/z80_test/testcases/custom): Additionally, you can check for normal compilation by inserting LLVM-IR or C code here.
