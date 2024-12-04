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

#include <PiMm.h>
#include <Chipset/AArch64.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/ArmMmuLib.h>
#include <Library/SecurePartitionServicesTableLib.h>
#include "ArmMmuLibInternal.h"

// MU_CHANGE: Add ArmSetMemoryRegionNoAccess function
EFI_STATUS
ArmSetMemoryRegionNoAccess (
  IN  EFI_PHYSICAL_ADDRESS  BaseAddress,
  IN  UINT64                Length
  )
{
  return ArmSetMemoryAttributes (
           BaseAddress,
           Length,
           EFI_MEMORY_RP,
           EFI_MEMORY_RP
           );
}

// MU_CHANGE: Add ArmClearMemoryRegionNoAccess function
EFI_STATUS
ArmClearMemoryRegionNoAccess (
  IN  EFI_PHYSICAL_ADDRESS  BaseAddress,
  IN  UINT64                Length
  )
{
  return ArmSetMemoryAttributes (
          BaseAddress,
          Length,
          0,
          EFI_MEMORY_RP
          );
}

EFI_STATUS
ArmSetMemoryRegionNoExec (
  IN  EFI_PHYSICAL_ADDRESS  BaseAddress,
  IN  UINT64                Length
  )
{
  return ArmSetMemoryAttributes (
          BaseAddress,
          Length,
          EFI_MEMORY_XP,
          EFI_MEMORY_XP
          );
}

EFI_STATUS
ArmClearMemoryRegionNoExec (
  IN  EFI_PHYSICAL_ADDRESS  BaseAddress,
  IN  UINT64                Length
  )
{
  return ArmSetMemoryAttributes (
          BaseAddress,
          Length,
          0,
          EFI_MEMORY_XP
          );
}

EFI_STATUS
ArmSetMemoryRegionReadOnly (
  IN  EFI_PHYSICAL_ADDRESS  BaseAddress,
  IN  UINT64                Length
  )
{
  return ArmSetMemoryAttributes (
          BaseAddress,
          Length,
          EFI_MEMORY_RO,
          EFI_MEMORY_RO
          );
}
extern volatile BOOLEAN loop;
EFI_STATUS
ArmClearMemoryRegionReadOnly (
  IN  EFI_PHYSICAL_ADDRESS  BaseAddress,
  IN  UINT64                Length
  )
{
  return ArmSetMemoryAttributes (
          BaseAddress,
          Length,
          0,
          EFI_MEMORY_RO
          );
}

RETURN_STATUS
EFIAPI
ArmMmuLibConstructor (
  IN EFI_HANDLE           ImageHandle,
  IN EFI_MM_SYSTEM_TABLE  *MmSystemTable
  )
{
  extern UINT32  ArmReplaceLiveTranslationEntrySize;

  //
  // The ArmReplaceLiveTranslationEntry () helper function may be invoked
  // with the MMU off so we have to ensure that it gets cleaned to the PoC
  //
  WriteBackDataCacheRange (
    (VOID *)(UINTN)ArmReplaceLiveTranslationEntry,
    ArmReplaceLiveTranslationEntrySize
    );

  return RETURN_SUCCESS;
}
