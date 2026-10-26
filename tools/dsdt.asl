/*
 * GMBox DSDT: ARM64 virt-like platform for the native HVF VMM.
 *   - CPU0
 *   - PL011 UART at 0x09000000, SPI 1 (GSIV 33)
 *   - PCIe root complex (ECAM 0x3f000000 via MCFG), MMIO32 only, GSI link
 *     devices GSI0..3 on SPI 3..6 (GSIV 35..38), QEMU virt swizzle.
 * Build: make acpi   (iasl -> tools/dsdt.aml -> tools/acpi_*.bin)
 */
DefinitionBlock ("", "DSDT", 2, "GMBOX", "GMBOXACP", 0x00000001)
{
    Scope (\_SB)
    {
        Device (C000)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0)
        }

        Device (COM0)
        {
            Name (_HID, "ARMH0011")
            Name (_CID, "PL011")
            Name (_UID, 0)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x09000000, 0x00001000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 33 }
            })
        }

        Device (GSI0)
        {
            Name (_HID, "PNP0C0F")
            Name (_UID, 0)
            Name (_PRS, ResourceTemplate () {
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 35 } })
            Method (_CRS, 0) { Return (_PRS) }
            Method (_SRS, 1) { }
            Method (_DIS, 0) { }
        }
        Device (GSI1)
        {
            Name (_HID, "PNP0C0F")
            Name (_UID, 1)
            Name (_PRS, ResourceTemplate () {
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 36 } })
            Method (_CRS, 0) { Return (_PRS) }
            Method (_SRS, 1) { }
            Method (_DIS, 0) { }
        }
        Device (GSI2)
        {
            Name (_HID, "PNP0C0F")
            Name (_UID, 2)
            Name (_PRS, ResourceTemplate () {
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 37 } })
            Method (_CRS, 0) { Return (_PRS) }
            Method (_SRS, 1) { }
            Method (_DIS, 0) { }
        }
        Device (GSI3)
        {
            Name (_HID, "PNP0C0F")
            Name (_UID, 3)
            Name (_PRS, ResourceTemplate () {
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 38 } })
            Method (_CRS, 0) { Return (_PRS) }
            Method (_SRS, 1) { }
            Method (_DIS, 0) { }
        }

        Device (PCI0)
        {
            Name (_HID, EisaId ("PNP0A08"))
            Name (_CID, EisaId ("PNP0A03"))
            Name (_SEG, 0)
            Name (_BBN, 0)
            Name (_UID, 0)
            Name (_CCA, 1)

            Name (_PRT, Package ()
            {
                /* slot 0 */
                Package () { 0x0000FFFF, 0, \_SB.GSI0, 0 },
                Package () { 0x0000FFFF, 1, \_SB.GSI1, 0 },
                Package () { 0x0000FFFF, 2, \_SB.GSI2, 0 },
                Package () { 0x0000FFFF, 3, \_SB.GSI3, 0 },
                /* slot 1 (NVMe) */
                Package () { 0x0001FFFF, 0, \_SB.GSI1, 0 },
                Package () { 0x0001FFFF, 1, \_SB.GSI2, 0 },
                Package () { 0x0001FFFF, 2, \_SB.GSI3, 0 },
                Package () { 0x0001FFFF, 3, \_SB.GSI0, 0 },
                /* slot 2 */
                Package () { 0x0002FFFF, 0, \_SB.GSI2, 0 },
                Package () { 0x0002FFFF, 1, \_SB.GSI3, 0 },
                Package () { 0x0002FFFF, 2, \_SB.GSI0, 0 },
                Package () { 0x0002FFFF, 3, \_SB.GSI1, 0 },
                /* slot 3 */
                Package () { 0x0003FFFF, 0, \_SB.GSI3, 0 },
                Package () { 0x0003FFFF, 1, \_SB.GSI0, 0 },
                Package () { 0x0003FFFF, 2, \_SB.GSI1, 0 },
                Package () { 0x0003FFFF, 3, \_SB.GSI2, 0 },
            })

            Name (_CRS, ResourceTemplate ()
            {
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                               0x0000, 0x0000, 0x000F, 0x0000, 0x0010)
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                             NonCacheable, ReadWrite, 0x00000000,
                             0x10000000, 0x3EFEFFFF, 0x00000000, 0x2EFF0000)
            })

            Method (_OSC, 4, NotSerialized)
            {
                CreateDWordField (Arg3, 0, CDW1)
                If (Arg0 == ToUUID ("33DB4D5B-1FF7-401C-9657-7441C03DD766"))
                {
                    CreateDWordField (Arg3, 4, CDW2)
                    CreateDWordField (Arg3, 8, CDW3)
                    /* Grant PCIe native control: hotplug, PME, AER, cap structure */
                    CDW3 &= 0x1F
                    If (Arg1 != 1) { CDW1 |= 0x08 }
                    Return (Arg3)
                }
                Else
                {
                    CDW1 |= 4
                    Return (Arg3)
                }
            }

            Device (RES0)
            {
                Name (_HID, "PNP0C02")
                Name (_CRS, ResourceTemplate ()
                {
                    QWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                                 NonCacheable, ReadWrite, 0x0, 0x3F000000,
                                 0x3FFFFFFF, 0x0, 0x01000000)
                })
            }
        }
    }
}
