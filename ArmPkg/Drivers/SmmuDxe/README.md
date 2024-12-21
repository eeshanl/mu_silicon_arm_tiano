# SMMU/IOMMU Driver

This document describes the System Memory Management Unit (SMMU) driver implementation, and how it integrates with the
PCI I/O subsystem. The driver configures the SMMUv3 hardware and implements the IOMMU protocol to provide address
translation and memory protection for DMA operations.

## Architecture Overview

The system consists of three main components working together:

1. **PCI I/O Protocol**: Provides interface for PCI device access and DMA operations
2. **IOMMU Protocol**: Implements DMA remapping and memory protection
3. **SMMU Hardware Driver**: Configures and manages the SMMU hardware

### Component Integration Flow

```text
PCI Device Driver
      ↓
PCI I/O Protocol
      ↓
IOMMU Protocol
      ↓
SMMU Hardware  
```

## IOMMU Protocol Integration

1. **PCI Driver Initiates DMA**:
   - PCI device driver calls PciIo->Map(), PciIo->Unmap()
   - Provides host memory address and operation type

2. **IOMMU Protocol Setup**:
   - Implements the IOMMU protocol:

      ```c
      struct _EDKII_IOMMU_PROTOCOL {
        UINT64                         Revision;
        EDKII_IOMMU_SET_ATTRIBUTE      SetAttribute;
        EDKII_IOMMU_MAP                Map;
        EDKII_IOMMU_UNMAP              Unmap;
        EDKII_IOMMU_ALLOCATE_BUFFER    AllocateBuffer;
        EDKII_IOMMU_FREE_BUFFER        FreeBuffer;
      };
      ```

   - Configures IOMMU page tables with PageTableInit()

### DMA Mapping

- Maintains a 4-level page table to map HostAddress and DeviceAddress
- Identity Mapped

1. **IoMmu Map**:

    ```c
    EFI_STATUS
    EFIAPI
    IoMmuMap (
      IN     EDKII_IOMMU_PROTOCOL   *This,
      IN     EDKII_IOMMU_OPERATION  Operation,
      IN     VOID                   *HostAddress,
      IN OUT UINTN                  *NumberOfBytes,
      OUT    EFI_PHYSICAL_ADDRESS   *DeviceAddress,
      OUT    VOID                   **Mapping
      );
    ```

- Sets access permissions based on operation type:
  - BusMasterRead: READ access
  - BusMasterWrite: WRITE access
  - BusMasterCommonBuffer: READ/WRITE access

- Maps HostAddress to DeviceAddress
- Validates operation type
- Called by PciIo protocol for mapping

### DMA Unmapping

1. **PCI Driver Completes DMA**:
   - Calls PciIo->Unmap()
   - Provides mapping handle

2. **IoMmu Unmap**:

    ```c
    EFI_STATUS
    EFIAPI
    IoMmuUnmap (
      IN  EDKII_IOMMU_PROTOCOL  *This,
      IN  VOID                  *Mapping
      );
    ```

   - Invalidates mapping in Page Table
   - Invalidates TLB entries

## SMMU Configuration

### 1. PlatformPeiLib

The Platform will construct a SMMU config HOB in PEI:

- Essentialy the same as IORT we want to publish
- This structure is consumed by SmmuDxe to configure the SMMU hardware

### 2. SMMUv3 Hardware Setup

The SMMU is configured in stage 2 translation mode with:

- Stream table for device ID mapping
- Command queue for SMMU operations, like TLB management
- Event queue for error handling
- 4KB translation granule

### 3. Page Table Structure

The IOMMU uses a 4-level page table structure for DMA address translation:
<https://developer.arm.com/documentation/101811/0104/Translation-granule/The-starting-level-of-address-translation>

```text
Level 0 Table (L0)
    ↓
Level 1 Table (L1)
    ↓
Level 2 Table (L2)
    ↓
Level 3 Table (L3)
    ↓
Physical Page
```

### 4. Address Translation Process

1. **Device Issues DMA**:
   - Device uses IOVA (I/O Virtual Address)
   - SMMU intercepts access

2. **SMMU Translation**:
   - Looks up Stream Table Entry (STE)
   - Walks 4-level page tables
   - Converts IOVA to PA (Physical Address)

## Memory Protection

The IOMMU protocol provides several protection mechanisms:

1. **Access Control**:
   - Read/Write permissions per mapping
   - Device isolation through Stream IDs

2. **Address Range Protection**:
   - Validates DMA addresses
   - Prevents access outside mapped regions

3. **Error Handling**:
   - Translation faults logged to Event Queue

## Performance Features

The implementation includes optimizations for:

1. **Integration of SmmuV3 with IOMMU Protocol**

2. **TLB Management**:
   - TLB invalidation for unmapped entries

## Configuration Options

Key SMMU settings controlled through the SMMU config HOB:

```text
- Smmu base address, num ID's, etc.
- Stream Table Size: Based on num IDs
- IORT info
```

## Limitations

Current implementation constraints:

1. Fixed 4KB granule size
2. 48-bit address space limit
3. Stage 2 translation only
4. Identity mapped page tables
5. Linear Stream Table

## Future Enhancements

Potential improvements:

1. Multiple translation granule support
2. Stage 1 translation
3. Different page table mapping schemes
4. Updated IoMmu Protocol to optimize redundencies
5. 2-level Stream Tables

## Relevant Docs

- SMMUv3 specification <https://developer.arm.com/documentation/ihi0070/latest/>
- Useful ARM SMMU documentation - <https://developer.arm.com/documentation/109242/0100/Programming-the-SMMU>
- Arm AArch64 memory manegemnt guide - <https://developer.arm.com/documentation/101811/0104>
- ARM a_a-profile_architecture_reference_manual <https://developer.arm.com/documentation/102105/ka-07>
- Intel IOMMU for DMA protection in UEFI <https://www.intel.com/content/dam/develop/external/us/en/documents/intel-whitepaper-using-iommu-for-dma-protection-in-uefi.pdf>
- IORT documentation <https://developer.arm.com/documentation/den0049/latest/>

## Notes

Integration with Qemu:

- SMMU is supported on Qemu but on v9.1.50+ <https://gitlab.com/qemu-project/qemu>
