#
# GMBox EDK2 overrides for a deterministic PCIe ECAM/PciBus bring-up.
#
# Use from ArmVirtPkg/ArmVirtQemu.dsc after ArmVirt.dsc.inc:
#   !include /path/to/GMBox/tools/edk2-fixed-pcd.dsc.inc
#
# Requires a DTB whose pci-host-ecam-generic node describes the ECAM used by
# the VMM. The FdtPciPcdProducerLib constructor must run in PciHostBridgeDxe
# and PciBusDxe before any PciExpressLib access.

[LibraryClasses]
  # Avoid BaseCachingPciExpressLib: it snapshots PcdPciExpressBaseAddress in
  # PciExpressLibInitialize(), which can run before the FDT producer.
  PciExpressLib|MdePkg/Library/BasePciExpressLib/BasePciExpressLib.inf

[PcdsDynamicDefault]
  # The FDT producer sets this to FALSE when the PCIe node is valid.
  gEfiMdeModulePkgTokenSpaceGuid.PcdPciDisableBusEnumeration|FALSE

[Components]
  MdeModulePkg/Bus/Pci/PciHostBridgeDxe/PciHostBridgeDxe.inf {
    <LibraryClasses>
      NULL|OvmfPkg/Fdt/FdtPciPcdProducerLib/FdtPciPcdProducerLib.inf
  }

  MdeModulePkg/Bus/Pci/PciBusDxe/PciBusDxe.inf {
    <LibraryClasses>
      NULL|OvmfPkg/Fdt/FdtPciPcdProducerLib/FdtPciPcdProducerLib.inf
  }
