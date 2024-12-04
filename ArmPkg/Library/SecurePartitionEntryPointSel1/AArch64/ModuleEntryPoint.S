//
//  Copyright (c) 2021, ARM Limited. All rights reserved.
//
//  SPDX-License-Identifier: BSD-2-Clause-Patent
//
//

#include <AsmMacroIoLibV8.h>

  .align 12
StackBase:
  .space 8192
StackEnd:

ASM_FUNC(_ModuleEntryPoint)
  // Stash boot information registers from the SPMC
  mov	x8, x0
  mov	x9, x1
  mov	x10, x2
  mov	x11, x3

  // Initialise SP
  adr	x0, StackEnd
  mov   sp, x0

  // Restore boot information registers from the SPMC
  mov	x0, x8
  mov	x1, x9
  mov	x2, x10
  mov	x3, x11

  // Invoke the C entrypoint
  b     ModuleEntryPoint
