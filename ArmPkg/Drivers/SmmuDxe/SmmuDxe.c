/** @file SmmuDxe.c

    This file contains functions for the SMMU driver.

    Copyright (C) Microsoft Corporation. All rights reserved.
    SPDX-License-Identifier: BSD-2-Clause-Patent

    Qemu smmu worked on this sha - a53b931645183bd0c15dd19ae0708fc3c81ecf1d
    QEMU emulator version 9.1.50 (v9.1.0-475-ga53b931645)
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Protocol/AcpiTable.h>
#include <Protocol/IoMmu.h>
#include <Guid/SmmuConfig.h>

#include "IoMmu.h"
#include "SmmuV3.h"
#include "SmmuV3Registers.h"

SMMU_INFO  *Smmu;

SMMU_INFO *
EFIAPI
SmmuInit (
  VOID
  )
{
  return (SMMU_INFO *)AllocateZeroPool (sizeof (SMMU_INFO));
}

VOID
EFIAPI
SmmuDeInit (
  SMMU_INFO  *Smmu
  )
{
  FreePool (Smmu);
}

STATIC
VOID
AcpiPlatformChecksum (
  IN UINT8  *Buffer,
  IN UINTN  Size
  )
{
  UINTN  ChecksumOffset;

  ChecksumOffset = OFFSET_OF (EFI_ACPI_DESCRIPTION_HEADER, Checksum);

  // Set checksum field to 0 since it is used as part of the calculation
  Buffer[ChecksumOffset] = 0;

  Buffer[ChecksumOffset] = CalculateCheckSum8 (Buffer, Size);
}

/*
 * A function that add the IORT ACPI table.
 */
EFI_STATUS
AddIortTable (
  IN EFI_ACPI_TABLE_PROTOCOL  *AcpiTable,
  IN SMMU_CONFIG              *SmmuConfig
  )
{
  EFI_STATUS            Status;
  UINTN                 TableHandle;
  UINT32                TableSize;
  EFI_PHYSICAL_ADDRESS  PageAddress;
  UINT8                 *New;

  // Calculate the new table size based on the number of nodes in SMMU_CONFIG struct
  TableSize = sizeof (SmmuConfig->Config.Iort) +
              sizeof (SmmuConfig->Config.ItsNode) +
              sizeof (SmmuConfig->Config.SmmuNode) +
              sizeof (SmmuConfig->Config.RcNode);

  Status = gBS->AllocatePages (
                  AllocateAnyPages,
                  EfiACPIReclaimMemory,
                  EFI_SIZE_TO_PAGES (TableSize),
                  &PageAddress
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to allocate pages for IORT table\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  New = (UINT8 *)(UINTN)PageAddress;
  ZeroMem (New, TableSize);

  // Add the ACPI Description table header
  CopyMem (New, &SmmuConfig->Config.Iort, sizeof (SmmuConfig->Config.Iort));
  ((EFI_ACPI_DESCRIPTION_HEADER *)New)->Length = TableSize;
  New                                         += sizeof (SmmuConfig->Config.Iort);

  // ITS Node
  CopyMem (New, &SmmuConfig->Config.ItsNode, sizeof (SmmuConfig->Config.ItsNode));
  New += sizeof (SmmuConfig->Config.ItsNode);

  // SMMUv3 Node
  CopyMem (New, &SmmuConfig->Config.SmmuNode, sizeof (SmmuConfig->Config.SmmuNode));
  New += sizeof (SmmuConfig->Config.SmmuNode);

  // RC Node
  CopyMem (New, &SmmuConfig->Config.RcNode, sizeof (SmmuConfig->Config.RcNode));
  New += sizeof (SmmuConfig->Config.RcNode);

  AcpiPlatformChecksum ((UINT8 *)(UINTN)PageAddress, TableSize);

  Status = AcpiTable->InstallAcpiTable (
                        AcpiTable,
                        (EFI_ACPI_COMMON_HEADER *)(UINTN)PageAddress,
                        TableSize,
                        &TableHandle
                        );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to install IORT table\n"));
  }

  return Status;
}

VOID *
EFIAPI
SmmuV3AllocateEventQueue (
  IN SMMU_INFO  *SmmuInfo,
  OUT UINT32    *QueueLog2Size
  )
{
  UINT32       QueueSize;
  SMMUV3_IDR1  Idr1;

  Idr1.AsUINT32 = SmmuV3ReadRegister32 (SmmuInfo->SmmuBase, SMMU_IDR1);

  *QueueLog2Size = MIN (Idr1.EventQs, SMMUV3_EVENT_QUEUE_LOG2ENTRIES);
  QueueSize      = SMMUV3_EVENT_QUEUE_SIZE_FROM_LOG2 (*QueueLog2Size);
  return AllocateZeroPool (QueueSize);
}

VOID *
EFIAPI
SmmuV3AllocateCommandQueue (
  IN SMMU_INFO  *SmmuInfo,
  OUT UINT32    *QueueLog2Size
  )
{
  UINT32       QueueSize;
  SMMUV3_IDR1  Idr1;

  Idr1.AsUINT32 = SmmuV3ReadRegister32 (SmmuInfo->SmmuBase, SMMU_IDR1);

  *QueueLog2Size = MIN (Idr1.CmdQs, SMMUV3_COMMAND_QUEUE_LOG2ENTRIES);
  QueueSize      = SMMUV3_COMMAND_QUEUE_SIZE_FROM_LOG2 (*QueueLog2Size);
  return AllocateZeroPool (QueueSize);
}

VOID
EFIAPI
SmmuV3FreeQueue (
  IN VOID  *QueuePtr
  )
{
  FreePool (QueuePtr);
}

EFI_STATUS
EFIAPI
SmmuV3BuildStreamTable (
  IN SMMU_INFO                   *SmmuInfo,
  IN SMMU_CONFIG                 *SmmuConfig,
  OUT SMMUV3_STREAM_TABLE_ENTRY  *StreamEntry
  )
{
  EFI_STATUS   Status;
  UINT32       OutputAddressWidth;
  UINT32       InputSize;
  SMMUV3_IDR0  Idr0;
  SMMUV3_IDR1  Idr1;
  SMMUV3_IDR5  Idr5;

  UINT8   IortCohac = SmmuConfig->Config.SmmuNode.SmmuNode.Flags & EFI_ACPI_IORT_SMMUv3_FLAG_COHAC_OVERRIDE;
  UINT32  CCA       = SmmuConfig->Config.RcNode.RcNode.CacheCoherent;
  UINT8   CPM       = SmmuConfig->Config.RcNode.RcNode.MemoryAccessFlags & BIT0;
  UINT8   DACS      = (SmmuConfig->Config.RcNode.RcNode.MemoryAccessFlags & BIT1) >> 1;

  // UINT8 Httu = 2;

  if ((StreamEntry == NULL) || (SmmuInfo->SmmuBase == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem ((VOID *)StreamEntry, sizeof (SMMUV3_STREAM_TABLE_ENTRY));

  Idr0.AsUINT32 = SmmuV3ReadRegister32 (SmmuInfo->SmmuBase, SMMU_IDR0);
  Idr1.AsUINT32 = SmmuV3ReadRegister32 (SmmuInfo->SmmuBase, SMMU_IDR1);
  Idr5.AsUINT32 = SmmuV3ReadRegister32 (SmmuInfo->SmmuBase, SMMU_IDR5);

  // 0x6 = stage2 translate stage1 bypass
  // 0x4 == stage2 bypass stage1 bypass
  StreamEntry->Config = 0x6;
  StreamEntry->Eats   = 0; // ATS not supported
  StreamEntry->S2Vmid = 1; // Domain->Vmid; Choose a none zero value
  StreamEntry->S2Tg   = 0; // 4KB granule size
  StreamEntry->S2Aa64 = 1; // AArch64 S2 translation tables
  StreamEntry->S2Ttb  = (UINT64)(UINTN)SmmuInfo->PageTableRoot >> 4;
  if ((Idr0.S1p == 1) && (Idr0.S2p == 1)) {
    StreamEntry->S2Ptw = 1;
  }

  // https://developer.arm.com/documentation/101811/0104/Translation-granule/The-starting-level-of-address-translation
  StreamEntry->S2Sl0 = 2;

  //
  // Set the maximum output address width. Per SMMUv3.2 spec (sections 5.2 and
  // 3.4.1), the maximum input address width with AArch64 format is given by
  // SMMU_IDR5.OAS field and capped at:
  // - 48 bits in SMMUv3.0,
  // - 52 bits in SMMUv3.1+. However, an address greater than 48 bits can
  //   only be output from stage 2 when a 64KB translation granule is in use
  //   for that translation table, which is not currently supported (only 4KB
  //   granules).
  //
  //  Thus the maximum input address width is restricted to 48-bits even if
  //  it is advertised to be larger.
  //
  OutputAddressWidth = SmmuV3DecodeAddressWidth (Idr5.Oas);

  if (OutputAddressWidth < 48) {
    StreamEntry->S2Ps = SmmuV3EncodeAddressWidth (OutputAddressWidth);
  } else {
    StreamEntry->S2Ps = SmmuV3EncodeAddressWidth (48);
  }

  InputSize           = OutputAddressWidth;
  StreamEntry->S2T0Sz = 64 - InputSize;
  if (IortCohac != 0) {
    StreamEntry->S2Ir0 = ARM64_RGNCACHEATTR_WRITEBACK_WRITEALLOCATE;
    StreamEntry->S2Or0 = ARM64_RGNCACHEATTR_WRITEBACK_WRITEALLOCATE;
    StreamEntry->S2Sh0 = ARM64_SHATTR_INNER_SHAREABLE;
  } else {
    StreamEntry->S2Ir0 = ARM64_RGNCACHEATTR_NONCACHEABLE;
    StreamEntry->S2Or0 = ARM64_RGNCACHEATTR_NONCACHEABLE;
    StreamEntry->S2Sh0 = ARM64_SHATTR_OUTER_SHAREABLE;
  }

  // StreamEntry->S2Had = Httu;
  StreamEntry->S2Rs = 0x2;   // record faults

  if (Idr1.AttrTypesOvr != 0) {
    StreamEntry->ShCfg = 0x1;
  }

  if ((Idr1.AttrTypesOvr != 0) && ((CCA == 1) && (CPM == 1) && (DACS == 0))) {
    StreamEntry->Mtcfg   = 0x1;
    StreamEntry->MemAttr = 0xF;   // Inner+Outer write-back cached
    StreamEntry->ShCfg   = 0x3;   // Inner shareable
  }

  // StreamEntry->AllocCfg = 0;

  StreamEntry->Valid = 1;

  Status = EFI_SUCCESS;
  return Status;
}

SMMUV3_STREAM_TABLE_ENTRY *
EFIAPI
SmmuV3AllocateStreamTable (
  IN SMMU_INFO    *SmmuInfo,
  IN SMMU_CONFIG  *SmmuConfig,
  OUT UINT32      *Log2Size,
  OUT UINT32      *Size
  )
{
  UINT32  MaxStreamId;
  UINT32  SidMsb;
  UINT32  Alignment;
  UINTN   Pages;

  MaxStreamId = SmmuConfig->Config.SmmuNode.SmmuIdMap.OutputBase + SmmuConfig->Config.SmmuNode.SmmuIdMap.NumIds;
  SidMsb      = HighBitSet32 (MaxStreamId);
  *Log2Size   = SidMsb + 1;
  *Size       = SMMUV3_LINEAR_STREAM_TABLE_SIZE_FROM_LOG2 (*Log2Size);
  *Size       = ROUND_UP (*Size, SMMU_MMIO_PAGE_SIZE);
  Alignment   = ALIGN_UP_BY (*Size, SMMU_MMIO_PAGE_SIZE);
  Pages       = EFI_SIZE_TO_PAGES (*Size);
  VOID  *AllocatedAddress = AllocateAlignedPages (Pages, Alignment);

  ASSERT (AllocatedAddress != NULL);
  ZeroMem (AllocatedAddress, *Size);
  return (SMMUV3_STREAM_TABLE_ENTRY *)AllocatedAddress;
}

VOID
EFIAPI
SmmuV3FreeStreamTable (
  IN SMMUV3_STREAM_TABLE_ENTRY  *StreamTablePtr,
  IN UINT32                     Size
  )
{
  UINTN  Pages;

  Pages = EFI_SIZE_TO_PAGES (Size);
  FreeAlignedPages ((VOID *)StreamTablePtr, Pages);
}

EFI_STATUS
EFIAPI
SmmuV3Configure (
  IN SMMU_INFO    *SmmuInfo,
  IN SMMU_CONFIG  *SmmuConfig
  )
{
  EFI_STATUS                 Status;
  UINT32                     STLog2Size;
  UINT32                     STSize;
  UINT32                     CommandQueueLog2Size;
  UINT32                     EventQueueLog2Size;
  UINT8                      ReadWriteAllocationHint;
  SMMUV3_STRTAB_BASE         StrTabBase;
  SMMUV3_STRTAB_BASE_CFG     StrTabBaseCfg;
  SMMUV3_STREAM_TABLE_ENTRY  *StreamTablePtr;
  SMMUV3_CMDQ_BASE           CommandQueueBase;
  SMMUV3_EVENTQ_BASE         EventQueueBase;
  SMMUV3_STREAM_TABLE_ENTRY  TemplateStreamEntry;
  SMMUV3_CR0                 Cr0;
  SMMUV3_CR1                 Cr1;
  SMMUV3_CR2                 Cr2;
  SMMUV3_IDR0                Idr0;
  SMMUV3_GERROR              GError;
  SMMUV3_CMD_GENERIC         Command;

  if ((SmmuConfig->Config.SmmuNode.SmmuNode.Flags & EFI_ACPI_IORT_SMMUv3_FLAG_COHAC_OVERRIDE) != 0) {
    ReadWriteAllocationHint = 0x1;
  } else {
    ReadWriteAllocationHint = 0x0;
  }

  GError.AsUINT32 = SmmuV3ReadRegister32 (SmmuInfo->SmmuBase, SMMU_GERROR);
  ASSERT (GError.AsUINT32 == 0);

  // Disable SMMU before configuring
  Status = SmmuV3DisableTranslation (SmmuInfo->SmmuBase);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Error SmmuV3Disable: SmmuBase=0x%lx\n", SmmuInfo->SmmuBase));
    goto End;
  }

  Status = SmmuV3DisableInterrupts (SmmuInfo->SmmuBase, TRUE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Error SmmuV3DisableInterrupts: SmmuBase=0x%lx\n", SmmuInfo->SmmuBase));
    goto End;
  }

  // Only Index 16 is being used AFAIK
  StreamTablePtr            = SmmuV3AllocateStreamTable (SmmuInfo, SmmuConfig, &STLog2Size, &STSize);
  Smmu->StreamTable         = StreamTablePtr;
  Smmu->StreamTableSize     = STSize;
  Smmu->StreamTableLog2Size = STLog2Size;
  ASSERT (StreamTablePtr != NULL);

  Smmu->PageTableRoot = PageTableInit (0);
  if (EFI_ERROR (Status)) {
    SmmuV3FreeStreamTable (StreamTablePtr, STSize);
    DEBUG ((DEBUG_ERROR, "Error PageTableInit: SmmuBase=0x%lx\n", SmmuInfo->SmmuBase));
    goto End;
  }

  // Build default STE template
  Status = SmmuV3BuildStreamTable (SmmuInfo, SmmuConfig, &TemplateStreamEntry);
  if (EFI_ERROR (Status)) {
    SmmuV3FreeStreamTable (SmmuInfo->StreamTable, SmmuInfo->StreamTableSize);
    DEBUG ((DEBUG_ERROR, "Error SmmuV3BuildStreamTable: SmmuBase=0x%lx\n", SmmuInfo->SmmuBase));
    goto End;
  }

  // Load default STE values
  // Only Index 16 is being used AFAIK
  for (UINT32 i = 0; i < SMMUV3_COUNT_FROM_LOG2 (STLog2Size); i++) {
    CopyMem (&StreamTablePtr[i], &TemplateStreamEntry, sizeof (SMMUV3_STREAM_TABLE_ENTRY));
  }

  VOID  *CommandQueue = SmmuV3AllocateCommandQueue (SmmuInfo, &CommandQueueLog2Size);
  VOID  *EventQueue   = SmmuV3AllocateEventQueue (SmmuInfo, &EventQueueLog2Size);

  Smmu->CommandQueue         = CommandQueue;
  Smmu->CommandQueueLog2Size = CommandQueueLog2Size;
  Smmu->EventQueue           = EventQueue;
  Smmu->EventQueueLog2Size   = EventQueueLog2Size;
  ASSERT (CommandQueue != NULL);
  ASSERT (EventQueue != NULL);

  // Configure Stream Table Base
  StrTabBaseCfg.AsUINT32 = 0;
  StrTabBaseCfg.Fmt      = 0; // Linear format
  StrTabBaseCfg.Log2Size = STLog2Size;

  SmmuV3WriteRegister32 (SmmuInfo->SmmuBase, SMMU_STRTAB_BASE_CFG, StrTabBaseCfg.AsUINT32);

  StrTabBase.AsUINT64 = 0;
  StrTabBase.Ra       = ReadWriteAllocationHint;
  StrTabBase.Addr     = ((UINT64)(UINTN)Smmu->StreamTable) >> 6;
  SmmuV3WriteRegister64 (SmmuInfo->SmmuBase, SMMU_STRTAB_BASE, StrTabBase.AsUINT64);

  // Configure Command Queue Base
  CommandQueueBase.AsUINT64 = 0;
  CommandQueueBase.Log2Size = Smmu->CommandQueueLog2Size;
  CommandQueueBase.Addr     = ((UINT64)(UINTN)Smmu->CommandQueue) >> 5;
  CommandQueueBase.Ra       = ReadWriteAllocationHint;
  SmmuV3WriteRegister64 (SmmuInfo->SmmuBase, SMMU_CMDQ_BASE, CommandQueueBase.AsUINT64);
  SmmuV3WriteRegister32 (SmmuInfo->SmmuBase, SMMU_CMDQ_PROD, 0);
  SmmuV3WriteRegister32 (SmmuInfo->SmmuBase, SMMU_CMDQ_CONS, 0);

  // Configure Event Queue Base
  EventQueueBase.AsUINT64 = 0;
  EventQueueBase.Log2Size = Smmu->EventQueueLog2Size;
  EventQueueBase.Addr     = ((UINT64)(UINTN)Smmu->EventQueue) >> 5;
  EventQueueBase.Wa       = ReadWriteAllocationHint;
  SmmuV3WriteRegister64 (SmmuInfo->SmmuBase, SMMU_EVENTQ_BASE, EventQueueBase.AsUINT64);
  SmmuV3WriteRegister32 (SmmuInfo->SmmuBase + 0x10000, SMMU_EVENTQ_PROD, 0);
  SmmuV3WriteRegister32 (SmmuInfo->SmmuBase + 0x10000, SMMU_EVENTQ_CONS, 0);

  // Enable GError and event interrupts
  Status = SmmuV3EnableInterrupts (SmmuInfo->SmmuBase);
  if (EFI_ERROR (Status)) {
    SmmuV3FreeStreamTable (StreamTablePtr, STSize);
    SmmuV3FreeQueue (Smmu->CommandQueue);
    SmmuV3FreeQueue (Smmu->EventQueue);
    DEBUG ((DEBUG_ERROR, "Error SmmuV3DisableInterrupts: SmmuBase=0x%lx\n", SmmuInfo->SmmuBase));
    goto End;
  }

  // Configure CR1
  Cr1.AsUINT32  = SmmuV3ReadRegister32 (SmmuInfo->SmmuBase, SMMU_CR1);
  Cr1.AsUINT32 &= ~SMMUV3_CR1_VALID_MASK;
  if ((SmmuConfig->Config.SmmuNode.SmmuNode.Flags & EFI_ACPI_IORT_SMMUv3_FLAG_COHAC_OVERRIDE) != 0) {
    Cr1.QueueIc = ARM64_RGNCACHEATTR_WRITEBACK_WRITEALLOCATE; // WBC
    Cr1.QueueOc = ARM64_RGNCACHEATTR_WRITEBACK_WRITEALLOCATE; // WBC
    Cr1.QueueSh = ARM64_SHATTR_INNER_SHAREABLE;               // Inner-shareable
  }

  SmmuV3WriteRegister32 (SmmuInfo->SmmuBase, SMMU_CR1, Cr1.AsUINT32);

  // Configure CR2
  Cr2.AsUINT32  = SmmuV3ReadRegister32 (SmmuInfo->SmmuBase, SMMU_CR2);
  Cr2.AsUINT32 &= ~SMMUV3_CR2_VALID_MASK;
  Cr2.E2h       = 0;
  Cr2.RecInvSid = 1;   // Record C_BAD_STREAMID for invalid input streams.

  //
  // If broadcast TLB maintenance (BTM) is not enabled, then configure
  // private TLB maintenance (PTM). Per spec (section 6.3.12), the PTM bit is
  // only valid when BTM is indicated as supported.
  //
  Idr0.AsUINT32 = SmmuV3ReadRegister32 (SmmuInfo->SmmuBase, SMMU_IDR0);
  if (Idr0.Btm == 1) {
    DEBUG ((DEBUG_INFO, "BTM = 1\n"));
    Cr2.Ptm = 1;     // Private TLB maintenance.
  }

  SmmuV3WriteRegister32 (SmmuInfo->SmmuBase, SMMU_CR2, Cr2.AsUINT32);

  // Configure CR0 part1
  ArmDataSynchronizationBarrier ();  // DSB

  Cr0.AsUINT32 = SmmuV3ReadRegister32 (SmmuInfo->SmmuBase, SMMU_CR0);
  Cr0.EventQEn = 1;
  Cr0.CmdQEn   = 1;

  SmmuV3WriteRegister32 (SmmuInfo->SmmuBase, SMMU_CR0, Cr0.AsUINT32);
  Status = SmmuV3Poll (SmmuInfo->SmmuBase + SMMU_CR0ACK, 0xC, 0xC);
  if (EFI_ERROR (Status)) {
    SmmuV3FreeStreamTable (StreamTablePtr, STSize);
    SmmuV3FreeQueue (Smmu->CommandQueue);
    SmmuV3FreeQueue (Smmu->EventQueue);
    DEBUG ((DEBUG_ERROR, "Error SmmuV3Poll: 0x%lx\n", SmmuInfo->SmmuBase + SMMU_CR0ACK));
    goto End;
  }

  //
  // Invalidate all cached configuration and TLB entries
  //
  SMMUV3_BUILD_CMD_CFGI_ALL (&Command);
  SmmuV3SendCommand (Smmu, &Command);
  SMMUV3_BUILD_CMD_TLBI_NSNH_ALL (&Command);
  SmmuV3SendCommand (Smmu, &Command);
  SMMUV3_BUILD_CMD_TLBI_EL2_ALL (&Command);
  SmmuV3SendCommand (Smmu, &Command);
  // Issue a CMD_SYNC command to guarantee that any previously issued TLB
  // invalidations (CMD_TLBI_*) are completed (SMMUv3.2 spec section 4.6.3).
  SMMUV3_BUILD_CMD_SYNC_NO_INTERRUPT (&Command);
  SmmuV3SendCommand (Smmu, &Command);

  // Configure CR0 part2
  Cr0.AsUINT32 = SmmuV3ReadRegister32 (SmmuInfo->SmmuBase, SMMU_CR0);
  ArmDataSynchronizationBarrier ();  // DSB

  Cr0.AsUINT32  = Cr0.AsUINT32 & ~SMMUV3_CR0_VALID_MASK;
  Cr0.SmmuEn    = 1;
  Cr0.EventQEn  = 1;
  Cr0.CmdQEn    = 1;
  Cr0.PriQEn    = 0;
  Cr0.Vmw       = 0; // Disable VMID wildcard matching.
  Idr0.AsUINT32 = SmmuV3ReadRegister32 (SmmuInfo->SmmuBase, SMMU_IDR0);
  if (Idr0.Ats != 0) {
    Cr0.AtsChk = 1;     // disable bypass for ATS translated traffic.
  }

  SmmuV3WriteRegister32 (SmmuInfo->SmmuBase, SMMU_CR0, Cr0.AsUINT32);
  Status = SmmuV3Poll (SmmuInfo->SmmuBase + SMMU_CR0ACK, SMMUV3_CR0_SMMU_EN_MASK, SMMUV3_CR0_SMMU_EN_MASK);
  if (EFI_ERROR (Status)) {
    SmmuV3FreeStreamTable (StreamTablePtr, STSize);
    SmmuV3FreeQueue (Smmu->CommandQueue);
    SmmuV3FreeQueue (Smmu->EventQueue);
    DEBUG ((DEBUG_ERROR, "Error SmmuV3Poll: 0x%lx\n", SmmuInfo->SmmuBase + SMMU_CR0ACK));
    goto End;
  }

  ArmDataSynchronizationBarrier ();  // DSB

  GError.AsUINT32 = SmmuV3ReadRegister32 (SmmuInfo->SmmuBase, SMMU_GERROR);
  ASSERT (GError.AsUINT32 == 0);

End:
  return Status;
}

STATIC
SMMU_CONFIG *
EFIAPI
GetSmmuConfigHobData (
  VOID
  )
{
  VOID  *GuidHob = GetFirstGuidHob (&gEfiSmmuConfigGuid);

  if (GuidHob != NULL) {
    return (SMMU_CONFIG *)GET_GUID_HOB_DATA (GuidHob);
  }

  return NULL;
}

VOID
EFIAPI
SmmuV3ExitBootServices (
  IN      EFI_EVENT  Event,
  IN      VOID       *Context
  )
{
  EFI_STATUS  Status;

  Status = SmmuV3DisableTranslation (Smmu->SmmuBase);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to disable smmu translation.\n", __func__));
  }

  Status = SmmuV3SetGlobalBypass (Smmu->SmmuBase);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to set global bypass.\n", __func__));
  }
}

/**
 * Entrypoint for SmmuDxe driver
 * See UEFI specification for the details of the parameters
 */
EFI_STATUS
EFIAPI
InitializeSmmuDxe (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS               Status;
  EFI_EVENT                Event;
  EFI_ACPI_TABLE_PROTOCOL  *AcpiTable;

  SMMU_CONFIG  *SmmuConfig = GetSmmuConfigHobData ();

  if (SmmuConfig == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get SMMU config data from gEfiSmmuConfigGuid\n", __func__));
    return EFI_NOT_FOUND;
  }

  // Check if ACPI Table Protocol has been installed
  Status = gBS->LocateProtocol (
                  &gEfiAcpiTableProtocolGuid,
                  NULL,
                  (VOID **)&AcpiTable
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: Failed to locate ACPI Table Protocol\n",
      __func__
      ));
    return Status;
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  SmmuV3ExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &Event
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to create ExitBootServices event\n", __func__));
    return Status;
  }

  Smmu = SmmuInit ();

  // Get SMMUv3 base address from config HOB
  Smmu->SmmuBase = SmmuConfig->Config.SmmuNode.SmmuNode.Base;

  // Add IORT Table
  Status = AddIortTable (AcpiTable, SmmuConfig);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to add IORT table\n", __func__));
    return Status;
  }

  Status = SmmuV3Configure (Smmu, SmmuConfig);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SmmuV3Configure: Failed to configure\n"));
    return Status;
  }

  Status = IoMmuInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "IommuInit: Failed to initialize IoMmuProtocol\n"));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "%a: Status = %llx\n", __func__, Status));

  return Status;
}
