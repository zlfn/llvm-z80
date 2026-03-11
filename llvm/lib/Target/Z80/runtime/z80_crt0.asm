;===-- crt0.asm - Z80 C Runtime Startup ------------------------------------===;
;
; Part of LLVM-Z80, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
;===------------------------------------------------------------------------===;
;
; Minimal C runtime startup for Z80.
; Linked first so it resides at address 0x0000 (reset vector).
; Sets up the stack pointer and calls main().
;
; Assembled with sdasz80: sdasz80 -g -o crt0.rel crt0.asm
;
;===------------------------------------------------------------------------===;

	.area _CODE
	.globl _main

	ld	sp,#0		; SP = 0 wraps to 0xFFFE (top of 64KB RAM)
	call	_main
	halt
