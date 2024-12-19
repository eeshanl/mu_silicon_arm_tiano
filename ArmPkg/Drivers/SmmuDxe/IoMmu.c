/** @file IoMmu.c

    This file contains functions for the IoMmu protocol.

    Copyright (c) Microsoft Corporation. All rights reserved.
    SPDX-License-Identifier: BSD-2-Clause-Patent

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

#define PAGE_TABLE_DEPTH  4                             // Number of levels in the page table
#define PAGE_TABLE_READ_WRITE_FROM_IOMMU_ACCESS(IOMMU_ACCESS)  (IOMMU_ACCESS << 6)
#define PAGE_TABLE_READ_BIT         (0x1 << 6)
#define PAGE_TABLE_WRITE_BIT        (0x1 << 7)
#define PAGE_TABLE_ENTRY_VALID_BIT  0x1
#define PAGE_TABLE_BLOCK_OFFSET     0xFFF
#define PAGE_TABLE_ACCESS_FLAG      (0x1 << 10)
#define PAGE_TABLE_DESCRIPTOR       (0x1 << 1)
#define PAGE_TABLE_INDEX(VA, LEVEL)  (((VA) >> (12 + (9 * (PAGE_TABLE_DEPTH - 1 - (LEVEL))))) & 0x1FF)

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

/**
  Update the flags of a page table entry.

  @param [in]  Table         Pointer to the page table.
  @param [in]  SetFlagsOnly  Boolean to indicate if only flags should be set.
  @param [in]  Flags         Flags to set or clear.
  @param [in]  Index         Index of the entry to update.

  @retval EFI_SUCCESS        Success.
**/
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

  // This boolean is used to explicity update the R/W bits in the page table entry.
  // Allows clearing the R/W bits without affecting the other bits in the entry.
  if (SetFlagsOnly) {
    if (Flags != 0) {
      // Set R/W bits in page table entry
      Table->Entries[Index] |= Flags;
    } else {
      // Clear R/W bits in page table entry
      Table->Entries[Index] &= ~(PAGE_TABLE_READ_BIT | PAGE_TABLE_WRITE_BIT);
    }
  } else {
    // Set R/W bits in page table entry
    Table->Entries[Index] |= Flags;
  }

  return EFI_SUCCESS;
}

/**
  Update the mapping of a virtual address to a physical address in the page table.

  Iterates through the page table levels to find the leaf entry for the given virtual address and
  validates entries along the way as needed. The leaf entry is then updated with the physical address along
  with appropriate flags and valid bit set. The option SetFlagsOnly allows traversal of the page table while
  only updating the flags of the entry, allowing clearing of flag bits as well.

  @param [in]  Root          Pointer to the root page table.
  @param [in]  VA            Virtual address to map.
  @param [in]  PA            Physical address to map to.
  @param [in]  Flags         Flags to set for the mapping.
  @param [in]  Valid         Boolean to indicate if the entry is valid.
  @param [in]  SetFlagsOnly  Boolean to indicate if only flags should be set.

  @retval EFI_SUCCESS        Success.
  @retval EFI_INVALID_PARAMETER  Invalid parameter.
  @retval EFI_OUT_OF_RESOURCES   Out of resources.
**/
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
  EFI_STATUS  Status;
  UINT8       Level;
  UINT64      Index;
  PAGE_TABLE  *Current;

  if (Root == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Current = Root;

  // Traverse the page table to the leaf level
  for (Level = 0; Level < PAGE_TABLE_DEPTH - 1; Level++) {
    Index = PAGE_TABLE_INDEX (VA, Level);

    if (Current->Entries[Index] == 0) {
      PAGE_TABLE  *NewPage = (PAGE_TABLE *)AllocateAlignedPages (1, EFI_PAGE_SIZE);
      if (NewPage == NULL) {
        return EFI_OUT_OF_RESOURCES;
      }

      ZeroMem ((VOID *)NewPage, EFI_PAGE_SIZE);

      Current->Entries[Index] = (PageTableEntry)(UINTN)NewPage;
    }

    if (!SetFlagsOnly) {
      if (Valid) {
        Current->Entries[Index] |= PAGE_TABLE_ENTRY_VALID_BIT; // valid entry
      }
    }

    Status = UpdateFlags (Current, SetFlagsOnly, Flags, Index);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, " %a: UpdateFlags failed.\n", __func__));
      return Status;
    }

    Current = (PAGE_TABLE *)((UINTN)Current->Entries[Index] & ~PAGE_TABLE_BLOCK_OFFSET);
  }

  // leaf level
  if (Current != 0) {
    Index = PAGE_TABLE_INDEX (VA, Level);

    if (Valid && ((Current->Entries[Index] & PAGE_TABLE_ENTRY_VALID_BIT) != 0)) {
      DEBUG ((DEBUG_INFO, "%a: Page already mapped\n", __func__));
    }

    if (!SetFlagsOnly) {
      if (Valid) {
        Current->Entries[Index]  = (PA & ~PAGE_TABLE_BLOCK_OFFSET); // Assign PA
        Current->Entries[Index] |= PAGE_TABLE_ENTRY_VALID_BIT;      // valid entry
      } else {
        Current->Entries[Index] &= ~PAGE_TABLE_ENTRY_VALID_BIT; // only invalidate leaf entry
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

/**
  Update the page table with the given physical address and flags.

  @param [in]  Root              Pointer to the root page table.
  @param [in]  PhysicalAddress   Physical address to map.
  @param [in]  Bytes             Number of bytes to map.
  @param [in]  Flags             Flags to set for the mapping.
  @param [in]  Valid             Boolean to indicate if the entry is valid.
  @param [in]  SetFlagsOnly      Boolean to indicate if only flags should be set.

  @retval EFI_SUCCESS            Success.
  @retval EFI_INVALID_PARAMETER  Invalid parameter.
  @retval EFI_OUT_OF_RESOURCES   Out of resources.
**/
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

/**
  Map a host address to a device address using the IOMMU.

  @param [in]      This            Pointer to the IOMMU protocol instance.
  @param [in]      Operation       The type of IOMMU operation.
  @param [in]      HostAddress     The host address to map.
  @param [in, out] NumberOfBytes   On input, the number of bytes to map. On output, the number of bytes mapped.
  @param [out]     DeviceAddress   The resulting device address.
  @param [out]     Mapping         A handle to the mapping.

  @retval EFI_SUCCESS              Success.
  @retval EFI_INVALID_PARAMETER    Invalid parameter.
  @retval EFI_OUT_OF_RESOURCES     Out of resources.
**/
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
  UINT64  Flags = PAGE_TABLE_ACCESS_FLAG | PAGE_TABLE_DESCRIPTOR;

  PhysicalAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  Status          = UpdatePageTable (mSmmu->PageTableRoot, PhysicalAddress, *NumberOfBytes, Flags, TRUE, FALSE);
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

/**
  Unmap a device address in the Page Table, invalidate the TLB.

  @param [in]  This      Pointer to the IOMMU protocol instance.
  @param [in]  Mapping   The mapping to unmap.

  @retval EFI_SUCCESS    Success.
  @retval EFI_INVALID_PARAMETER  Invalid parameter.
**/
EFI_STATUS
EFIAPI
IoMmuUnmap (
  IN  EDKII_IOMMU_PROTOCOL  *This,
  IN  VOID                  *Mapping
  )
{
  EFI_STATUS          Status;
  SMMUV3_CMD_GENERIC  Command;
  IOMMU_MAP_INFO      *MapInfo;

  if (Mapping == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  MapInfo = (IOMMU_MAP_INFO *)Mapping;

  Status = UpdatePageTable (mSmmu->PageTableRoot, MapInfo->PA, MapInfo->NumberOfBytes, 0, FALSE, FALSE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: UpdatePageTable failed.\n", __func__));
    return Status;
  }

  // Invalidate TLB
  SMMUV3_BUILD_CMD_TLBI_NSNH_ALL (&Command);
  SmmuV3SendCommand (mSmmu, &Command);
  SMMUV3_BUILD_CMD_TLBI_EL2_ALL (&Command);
  SmmuV3SendCommand (mSmmu, &Command);
  // Issue a CMD_SYNC command to guarantee that any previously issued TLB
  // invalidations (CMD_TLBI_*) are completed (SMMUv3.2 spec section 4.6.3).
  SMMUV3_BUILD_CMD_SYNC_NO_INTERRUPT (&Command);
  SmmuV3SendCommand (mSmmu, &Command);

  if (MapInfo != NULL) {
    FreePool (MapInfo);
  }

  return EFI_SUCCESS;
}

/**
  Free a buffer allocated by IoMmuAllocateBuffer.

  @param [in]  This          Pointer to the IOMMU protocol instance.
  @param [in]  Pages         The number of pages to free.
  @param [in]  HostAddress   The host address to free.

  @retval EFI_SUCCESS        Success.
**/
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

/**
  Allocate a buffer for use with the IOMMU.

  @param [in]      This          Pointer to the IOMMU protocol instance.
  @param [in]      Type          The type of allocation to perform.
  @param [in]      MemoryType    The type of memory to allocate.
  @param [in]      Pages         The number of pages to allocate.
  @param [in, out] HostAddress   On input, the desired host address. On output, the allocated host address.
  @param [in]      Attributes    The memory attributes to use for the allocation.

  @retval EFI_SUCCESS            Success.
  @retval EFI_INVALID_PARAMETER  Invalid parameter.
  @retval EFI_OUT_OF_RESOURCES   Out of resources.
**/
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

/**
  Set the R/W attributes for a device handle.

  @param [in]  This          Pointer to the IOMMU protocol instance.
  @param [in]  DeviceHandle  The device handle to set attributes for.
  @param [in]  Mapping       The mapping to set attributes for.
  @param [in]  IoMmuAccess   The IOMMU access attributes for R/W.

  @retval EFI_SUCCESS        Success.
**/
EFI_STATUS
EFIAPI
IoMmuSetAttribute (
  IN EDKII_IOMMU_PROTOCOL  *This,
  IN EFI_HANDLE            DeviceHandle,
  IN VOID                  *Mapping,
  IN UINT64                IoMmuAccess
  )
{
  EFI_STATUS      Status;
  IOMMU_MAP_INFO  *MapInfo;

  if (Mapping == NULL) {
    return EFI_SUCCESS;
  }

  MapInfo = (IOMMU_MAP_INFO *)Mapping;

  Status = UpdatePageTable (
             mSmmu->PageTableRoot,
             MapInfo->PA,
             MapInfo->NumberOfBytes,
             PAGE_TABLE_READ_WRITE_FROM_IOMMU_ACCESS (IoMmuAccess),
             FALSE,
             TRUE
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: UpdatePageTable failed.\n", __func__));
    return Status;
  }

  return Status;
}

/**
  Initialize a page table. Only initializes the root page table.

  @retval A pointer to the initialized page table.
**/
PAGE_TABLE *
EFIAPI
PageTableInit (
  VOID
  )
{
  UINTN       Pages;
  PAGE_TABLE  *PageTable;

  Pages = EFI_SIZE_TO_PAGES (sizeof (PAGE_TABLE));

  PageTable = (PAGE_TABLE *)AllocateAlignedPages (Pages, EFI_PAGE_SIZE);

  if (PageTable == NULL) {
    return NULL;
  }

  ZeroMem (PageTable, EFI_PAGES_TO_SIZE (Pages));

  return PageTable;
}

/**
  Deinitialize a page table.

  @param [in]  Level      The level of the page table to deinitialize.
  @param [in]  PageTable  The page table to deinitialize.
**/
VOID
EFIAPI
PageTableDeInit (
  IN UINT8       Level,
  IN PAGE_TABLE  *PageTable
  )
{
  UINTN  Index;

  if ((Level >= PAGE_TABLE_DEPTH) || (PageTable == NULL)) {
    return;
  }

  for (Index = 0; Index < PAGE_TABLE_SIZE; Index++) {
    PageTableEntry  Entry = PageTable->Entries[Index];

    if (Entry != 0) {
      PageTableDeInit (Level + 1, (PAGE_TABLE *)((UINTN)Entry & ~PAGE_TABLE_BLOCK_OFFSET));
    }
  }

  FreePages (PageTable, EFI_SIZE_TO_PAGES (sizeof (PAGE_TABLE)));
}

/**
  Initialize the IOMMU.

  @retval EFI_SUCCESS  Success.
**/
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
