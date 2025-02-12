## @file
#  Dsc file for Dynamic Tables Framework.
#
#  Copyright (c) 2019, Linaro Limited. All rights reserved.<BR>
#  Copyright (c) 2019 - 2022, Arm Limited. All rights reserved.<BR>
#  Copyright (C) 2024 Advanced Micro Devices, Inc. All rights reserved.<BR>
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
##

[Defines]
  PLATFORM_NAME                  = DynamicTables
  PLATFORM_GUID                  = f39096a0-7a0a-442a-9413-cf584ef80cbb
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x0001001a
  OUTPUT_DIRECTORY               = Build/DynamicTables
  SUPPORTED_ARCHITECTURES        = ARM|AARCH64|IA32|X64
  BUILD_TARGETS                  = DEBUG|RELEASE|NOOPT
  SKUID_IDENTIFIER               = DEFAULT

!include DynamicTables.dsc.inc

!include MdePkg/MdeLibs.dsc.inc

[LibraryClasses]
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  NULL|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf # MU_CHANGE: /GS and -fstack-protector support

[LibraryClasses.ARM, LibraryClasses.AARCH64]
  NULL|ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  # NULL|MdePkg/Library/BaseStackCheckLib/BaseStackCheckLib.inf # MU_CHANGE
  PL011UartLib|ArmPlatformPkg/Library/PL011UartLib/PL011UartLib.inf

[Components.common]
  DynamicTablesPkg/Library/Common/AcpiHelperLib/AcpiHelperLib.inf
  DynamicTablesPkg/Library/Common/AmlLib/AmlLib.inf
  DynamicTablesPkg/Library/Common/SsdtPcieSupportLib/SsdtPcieSupportLib.inf
  DynamicTablesPkg/Library/Common/SsdtSerialPortFixupLib/SsdtSerialPortFixupLib.inf
  DynamicTablesPkg/Library/Common/TableHelperLib/TableHelperLib.inf
  DynamicTablesPkg/Library/Common/DynamicPlatRepoLib/DynamicPlatRepoLib.inf
  DynamicTablesPkg/Library/Common/SmbiosStringTableLib/SmbiosStringTableLib.inf

[Components.ARM, Components.AARCH64]
  DynamicTablesPkg/Library/FdtHwInfoParserLib/FdtHwInfoParserLib.inf

[Components.AARCH64]
  DynamicTablesPkg/Library/DynamicTablesScmiInfoLib/DynamicTablesScmiInfoLib.inf

[BuildOptions]
  *_*_*_CC_FLAGS = -D DISABLE_NEW_DEPRECATED_INTERFACES

!ifdef STATIC_ANALYSIS
  # Check all rules
  # Inhibit C6305: Potential mismatch between sizeof and countof quantities.
  *_VS2017_*_CC_FLAGS = /wd6305 /analyze
!endif
