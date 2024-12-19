/** @file
    File for SMMU config structures.

    This SMMU_CONFIG structure is used to pass the SMMU configuration data from
    the platform to the SMMU driver. The Smmu driver will use this data to install
    the IORT table and configure the SMMU hardware.

    Given the IORT is configurable and platform dependent, the SMMU_CONFIG structure contains
    all info relevant to the IORT table and SMMUv3 platform specific configuration.

    See <https://developer.arm.com/documentation/den0049/latest/> for IORT spec.

    Copyright (c) Microsoft Corporation.
    SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef SMMU_CONFIG_GUID_H_
#define SMMU_CONFIG_GUID_H_

#include <IndustryStandard/IoRemappingTable.h>

/* See <https://developer.arm.com/documentation/den0049/latest/> for IORT spec for
   information on the IORT ACPI node structure. Platform must populate all the below
   ACPI structures to pass the SMMU configuration data to the SMMU driver.
*/

typedef struct _PLATFORM_ACPI_6_0_IO_REMAPPING_ITS_NODE {
  EFI_ACPI_6_0_IO_REMAPPING_ITS_NODE    Node;        // ITS Node
  UINT32                                Identifiers; // ITS Node identifiers
} PLATFORM_ACPI_6_0_IO_REMAPPING_ITS_NODE;

typedef struct _PLATFORM_ACPI_6_0_IO_REMAPPING_SMMU3_NODE {
  EFI_ACPI_6_0_IO_REMAPPING_SMMU3_NODE    SmmuNode;  // SMMUV3 Node
  EFI_ACPI_6_0_IO_REMAPPING_ID_TABLE      SmmuIdMap; // SMMUV3 ID Mapping
} PLATFORM_ACPI_6_0_IO_REMAPPING_SMMU3_NODE;

typedef struct _PLATFORM_ACPI_6_0_IO_REMAPPING_RC_NODE {
  EFI_ACPI_6_0_IO_REMAPPING_RC_NODE     RcNode;  // Root Complex Node
  EFI_ACPI_6_0_IO_REMAPPING_ID_TABLE    RcIdMap; // ROot Complex ID Mapping
} PLATFORM_ACPI_6_0_IO_REMAPPING_RC_NODE;

typedef struct _PLATFORM_IO_REMAPPING_STRUCTURE {
  EFI_ACPI_6_0_IO_REMAPPING_TABLE              Iort;     // IORT table header
  PLATFORM_ACPI_6_0_IO_REMAPPING_ITS_NODE      ItsNode;  // ITS Node platform wrapper
  PLATFORM_ACPI_6_0_IO_REMAPPING_SMMU3_NODE    SmmuNode; // Smmu Node platform wrapper
  PLATFORM_ACPI_6_0_IO_REMAPPING_RC_NODE       RcNode;   // Root Complex Node platform wrapper
} PLATFORM_IO_REMAPPING_STRUCTURE;

typedef struct _SMMU_CONFIG {
  PLATFORM_IO_REMAPPING_STRUCTURE    Config;
  UINT32                             VersionMajor;
  UINT32                             VersionMinor;
} SMMU_CONFIG;

#define SMMU_CONFIG_GUID \
  { 0xcd56ec8f, 0x75f1, 0x440a, { 0xaa, 0x48, 0x09, 0x58, 0xb1, 0x1c, 0x9a, 0xa7 } }

extern EFI_GUID  gSmmuConfigGuid;

#endif
