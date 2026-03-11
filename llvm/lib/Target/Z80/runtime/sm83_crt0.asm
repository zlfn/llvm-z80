;===-- sm83_crt0.asm - SM83 (Game Boy) C Runtime Startup -------------------===;
;
; Part of LLVM-Z80, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
;===------------------------------------------------------------------------===;
;
; Minimal C runtime startup for SM83 (Game Boy CPU).
; Linked first so it resides at address 0x0000.
; Sets up the stack pointer and calls main().
;
; Assembled with sdasgb: sdasgb -g -o sm83_crt0.rel sm83_crt0.asm
;
;===------------------------------------------------------------------------===;

	.area _CODE
	.globl _main

	ld	sp,#0xFFFE	; top of WRAM (Game Boy: 0xC000-0xDFFF)
	call	_main
	halt
