/*
 * Intel ACPI Component Architecture
 * AML/ASL+ Disassembler version 20260408 (64-bit version)
 * Copyright (c) 2000 - 2026 Intel Corporation
 * 
 * Disassembling to symbolic ASL+ operators
 *
 * Disassembly of tools/dsdt.aml
 *
 * Original Table Header:
 *     Signature        "DSDT"
 *     Length           0x00000097 (151)
 *     Revision         0x02
 *     Checksum         0x4F
 *     OEM ID           "GMBOX"
 *     OEM Table ID     "GMBOX"
 *     OEM Revision     0x00000001 (1)
 *     Compiler ID      "INTL"
 *     Compiler Version 0x20260408 (539362312)
 */
DefinitionBlock ("", "DSDT", 2, "GMBOX", "GMBOX", 0x00000001)
{
    Scope (_SB)
    {
        Device (PCI0)
        {
            Name (_HID, EisaId ("PNP0A08") /* PCI Express Bus */)  // _HID: Hardware ID
            Name (_CID, EisaId ("PNP0A03") /* PCI Bus */)  // _CID: Compatible ID
            Name (_UID, Zero)  // _UID: Unique ID
            Name (_CRS, Buffer (0x40)  // _CRS: Current Resource Settings
            {
                /* 0000 */  0x88, 0x0D, 0x00, 0x02, 0x0C, 0x00, 0x00, 0x00,  // ........
                /* 0008 */  0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x10, 0x00,  // ........
                /* 0010 */  0x8A, 0x2B, 0x00, 0x00, 0x0C, 0x03, 0x00, 0x00,  // .+......
                /* 0018 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                /* 0020 */  0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF,  // ........
                /* 0028 */  0xFF, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // .>......
                /* 0030 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                /* 0038 */  0x00, 0x2F, 0x00, 0x00, 0x00, 0x00, 0x79, 0x00   // ./....y.
            })
        }
    }
}

