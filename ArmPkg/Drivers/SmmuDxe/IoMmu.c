/** @file IoMmu.c

    This file contains functions for the IoMmu protocol.

    Copyright (C) Microsoft Corporation. All rights reserved.
    SPDX-License-Identifier: BSD-2-Clause-Patent

    Qemu smmu worked on this sha - a53b931645183bd0c15dd19ae0708fc3c81ecf1d
    QEMU emulator version 9.1.50 (v9.1.0-475-ga53b931645)
**/

#include <Uefi.h>
#include <Library/ArmLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Protocol/IoMmu.h>
#include "IoMmu.h"
#include "SmmuV3Registers.h"

EDKII_IOMMU_PROTOCOL  SmmuIoMmu = {
  EDKII_IOMMU_PROTOCOL_REVISION,
  IoMmuSetAttribute,
  IoMmuMap,
  IoMmuUnmap,
  IoMmuAllocateBuffer,
  IoMmuFreeBuffer,
};

typedef struct IOMMU_MAP_INFO {
  UINTN     NumberOfBytes;
  UINT64    VA;
  UINT64    PA;
} IOMMU_MAP_INFO;

STATIC
EFI_STATUS
EFIAPI
UpdateFlags (
  IN PAGE_TABLE  *Table,
  IN BOOLEAN     SetFlagsOnly,
  IN UINT64      Flags,
  IN UINT64      Index
  )
{
  if (Table == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (SetFlagsOnly) {
    if (Flags != 0) {
      Table->Entries[Index] |= Flags;
    } else {
      Table->Entries[Index] &= ~(0x3 << 6);
    }
  } else {
    Table->Entries[Index] |= Flags;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UpdateMapping (
  IN PAGE_TABLE  *Root,
  IN UINT64      VA,
  IN UINT64      PA,
  IN UINT64      Flags,
  IN BOOLEAN     Valid,
  IN BOOLEAN     SetFlagsOnly
  )
{
  if (Root == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_STATUS  Status;
  UINT64      Index;
  UINTN       Pages    = EFI_SIZE_TO_PAGES (sizeof (PAGE_TABLE));
  PAGE_TABLE  *Current = Root;

  for (UINT8 Level = 0; Level < PAGE_TABLE_DEPTH - 1; Level++) {
    Index = (VA >> (12 + (9 * (PAGE_TABLE_DEPTH - 1 - Level)))) & 0x1FF;

    if (Current->Entries[Index] == 0) {
      PAGE_TABLE  *NewPage = (PAGE_TABLE *)((UINTN)AllocateAlignedPages (Pages, EFI_PAGE_SIZE) & ~0xFFF);
      if (NewPage == 0) {
        return EFI_OUT_OF_RESOURCES;
      }

      ZeroMem ((VOID *)NewPage, EFI_PAGES_TO_SIZE (Pages));

      Current->Entries[Index] = (PageTableEntry)(UINTN)NewPage;
    }

    if (!SetFlagsOnly) {
      if (Valid) {
        Current->Entries[Index] |= 0x1; // valid entry
      }
    }

    Status = UpdateFlags (Current, SetFlagsOnly, Flags, Index);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, " %a: UpdateFlags failed.\n", __func__));
      return Status;
    }

    Current = (PAGE_TABLE *)((UINTN)Current->Entries[Index] & ~0xFFF);
  }

  // leaf level
  if (Current != 0) {
    Index = (VA >> 12) & 0x1FF;

    if (Valid && ((Current->Entries[Index] & 0x1) != 0)) {
      DEBUG ((DEBUG_INFO, "%a: Page already mapped\n", __func__));
    }

    if (!SetFlagsOnly) {
      if (Valid) {
        Current->Entries[Index]  = (PA & ~0xFFF); // Assign PA
        Current->Entries[Index] |= 0x1;           // valid entry
      } else {
        Current->Entries[Index] &= ~0x1; // only invalidate leaf entry
      }
    }

    Status = UpdateFlags (Current, SetFlagsOnly, Flags, Index);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, " %a: UpdateFlags failed.\n", __func__));
      return Status;
    }
  }

  return Status;
}

EFI_STATUS
EFIAPI
UpdatePageTable (
  IN PAGE_TABLE  *Root,
  IN UINT64      PhysicalAddress,
  IN UINT64      Bytes,
  IN UINT64      Flags,
  IN BOOLEAN     Valid,
  IN BOOLEAN     SetFlagsOnly
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  PhysicalAddressEnd;
  EFI_PHYSICAL_ADDRESS  CurPhysicalAddress;

  CurPhysicalAddress = ALIGN_DOWN_BY (PhysicalAddress, EFI_PAGE_SIZE);
  PhysicalAddressEnd = ALIGN_UP_BY (PhysicalAddress + Bytes, EFI_PAGE_SIZE);

  while (CurPhysicalAddress < PhysicalAddressEnd) {
    Status = UpdateMapping (Root, CurPhysicalAddress, CurPhysicalAddress, Flags, Valid, SetFlagsOnly);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    CurPhysicalAddress += EFI_PAGE_SIZE;
  }

  return Status;
}

EFI_STATUS
EFIAPI
IoMmuMap (
  IN     EDKII_IOMMU_PROTOCOL   *This,
  IN     EDKII_IOMMU_OPERATION  Operation,
  IN     VOID                   *HostAddress,
  IN OUT UINTN                  *NumberOfBytes,
  OUT    EFI_PHYSICAL_ADDRESS   *DeviceAddress,
  OUT    VOID                   **Mapping
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  PhysicalAddress;
  IOMMU_MAP_INFO        *MapInfo;

  // Arm Architecture Reference Manual Armv8, for Armv8-A architecture profile:
  // The VMSAv8-64 translation table format descriptors.
  // Bit #10 AF = 1, Table/Page Descriptors for levels 0-3 so set bit #1 to 0b'1 for each entry
  UINT64  Flags = 0x402;

  PhysicalAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  Status          = UpdatePageTable (Smmu->PageTableRoot, PhysicalAddress, *NumberOfBytes, Flags, TRUE, FALSE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: UpdatePageTable failed.\n", __func__));
    return Status;
  }

  *DeviceAddress = PhysicalAddress; // Identity mapping

  MapInfo                = (IOMMU_MAP_INFO *)AllocateZeroPool (sizeof (IOMMU_MAP_INFO));
  MapInfo->NumberOfBytes = *NumberOfBytes;
  MapInfo->VA            = *DeviceAddress;
  MapInfo->PA            = PhysicalAddress;

  *Mapping = MapInfo;
  return Status;
}

EFI_STATUS
EFIAPI
IoMmuUnmap (
  IN  EDKII_IOMMU_PROTOCOL  *This,
  IN  VOID                  *Mapping
  )
{
  IOMMU_MAP_INFO  *MapInfo = (IOMMU_MAP_INFO *)Mapping;

  if (Mapping == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_STATUS          Status;
  SMMUV3_CMD_GENERIC  Command;

  Status = UpdatePageTable (Smmu->PageTableRoot, MapInfo->PA, MapInfo->NumberOfBytes, 0, FALSE, FALSE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: UpdatePageTable failed.\n", __func__));
    return Status;
  }

  // Invalidate TLB
  SMMUV3_BUILD_CMD_TLBI_NSNH_ALL (&Command);
  SmmuV3SendCommand (Smmu, &Command);
  SMMUV3_BUILD_CMD_TLBI_EL2_ALL (&Command);
  SmmuV3SendCommand (Smmu, &Command);
  // Issue a CMD_SYNC command to guarantee that any previously issued TLB
  // invalidations (CMD_TLBI_*) are completed (SMMUv3.2 spec section 4.6.3).
  SMMUV3_BUILD_CMD_SYNC_NO_INTERRUPT (&Command);
  SmmuV3SendCommand (Smmu, &Command);

  if (MapInfo != NULL) {
    FreePool (MapInfo);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
IoMmuFreeBuffer (
  IN  EDKII_IOMMU_PROTOCOL  *This,
  IN  UINTN                 Pages,
  IN  VOID                  *HostAddress
  )
{
  return gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress, Pages);
}

EFI_STATUS
EFIAPI
IoMmuAllocateBuffer (
  IN     EDKII_IOMMU_PROTOCOL  *This,
  IN     EFI_ALLOCATE_TYPE     Type,
  IN     EFI_MEMORY_TYPE       MemoryType,
  IN     UINTN                 Pages,
  IN OUT VOID                  **HostAddress,
  IN     UINT64                Attributes
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  PhysicalAddress;

  Status = gBS->AllocatePages (
                  Type,
                  MemoryType,
                  Pages,
                  &PhysicalAddress
                  );
  if (!EFI_ERROR (Status)) {
    *HostAddress = (VOID *)(UINTN)PhysicalAddress;
  }

  return Status;
}

EFI_STATUS
EFIAPI
IoMmuSetAttribute (
  IN EDKII_IOMMU_PROTOCOL  *This,
  IN EFI_HANDLE            DeviceHandle,
  IN VOID                  *Mapping,
  IN UINT64                IoMmuAccess
  )
{
  IOMMU_MAP_INFO  *MapInfo = (IOMMU_MAP_INFO *)Mapping;

  if (Mapping == NULL) {
    return EFI_SUCCESS;
  }

  EFI_STATUS  Status;

  // R/W bits are bit 6,7 in PageTableEntry
  Status = UpdatePageTable (Smmu->PageTableRoot, MapInfo->PA, MapInfo->NumberOfBytes, IoMmuAccess << 6, FALSE, TRUE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: UpdatePageTable failed.\n", __func__));
    return Status;
  }

  return Status;
}

PAGE_TABLE *
EFIAPI
PageTableInit (
  IN UINT8  Level
  )
{
  if (Level >= PAGE_TABLE_DEPTH) {
    return NULL;
  }

  UINTN  Pages = EFI_SIZE_TO_PAGES (sizeof (PAGE_TABLE));

  PAGE_TABLE  *PageTable = (PAGE_TABLE *)((UINTN)AllocateAlignedPages (Pages, EFI_PAGE_SIZE) & ~0xFFF);

  if (PageTable == NULL) {
    return NULL;
  }

  ZeroMem (PageTable, EFI_PAGES_TO_SIZE (Pages));

  ASSERT (PageTable != NULL);
  return PageTable;
}

VOID
EFIAPI
PageTableDeInit (
  IN UINT8       Level,
  IN PAGE_TABLE  *PageTable
  )
{
  if ((Level >= PAGE_TABLE_DEPTH) || (PageTable == NULL)) {
    return;
  }

  for (UINTN i = 0; i < PAGE_TABLE_SIZE; i++) {
    PageTableEntry  Entry = PageTable->Entries[i];

    if (Entry != 0) {
      PageTableDeInit (Level + 1, (PAGE_TABLE *)((UINTN)Entry & ~0xFFF));
    }
  }

  FreePages (PageTable, EFI_SIZE_TO_PAGES (sizeof (PAGE_TABLE)));
}

EFI_STATUS
EFIAPI
IoMmuInit (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEdkiiIoMmuProtocolGuid,
                  &SmmuIoMmu,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to install gEdkiiIoMmuProtocolGuid\n", __func__));
    return Status;
  }

  return Status;
}
