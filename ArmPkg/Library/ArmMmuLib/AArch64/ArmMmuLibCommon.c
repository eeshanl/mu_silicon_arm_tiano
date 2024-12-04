/** @file
*  File managing the MMU for ARMv8 architecture
*
*  Copyright (c) 2011-2020, ARM Limited. All rights reserved.
*  Copyright (c) 2016, Linaro Limited. All rights reserved.
*  Copyright (c) 2017, Intel Corporation. All rights reserved.<BR>
*
*  SPDX-License-Identifier: BSD-2-Clause-Patent
*
**/

#include <Uefi.h>
#include <Pi/PiMultiPhase.h>
#include <Chipset/AArch64.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/ArmLib.h>
#include <Library/ArmMmuLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include "ArmMmuLibInternal.h"

RETURN_STATUS
EFIAPI
ArmMmuBaseLibConstructor (
  VOID
  )
{
  extern UINT32  ArmReplaceLiveTranslationEntrySize;
  VOID           *Hob;

  Hob = GetFirstGuidHob (&gArmMmuReplaceLiveTranslationEntryFuncGuid);
  if (Hob != NULL) {
    mReplaceLiveEntryFunc = *(ARM_REPLACE_LIVE_TRANSLATION_ENTRY *)GET_GUID_HOB_DATA (Hob);
  } else {
    //
    // The ArmReplaceLiveTranslationEntry () helper function may be invoked
    // with the MMU off so we have to ensure that it gets cleaned to the PoC
    //
    WriteBackDataCacheRange (
      (VOID *)(UINTN)ArmReplaceLiveTranslationEntry,
      ArmReplaceLiveTranslationEntrySize
      );
  }

  return RETURN_SUCCESS;
}
