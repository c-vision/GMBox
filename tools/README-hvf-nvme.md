# Native HVF EDK2 + NVMe status

`tools/hvf_win.c` is the native Hypervisor.framework VMM (no QEMU at runtime).
It boots `tools/firmware/edk2-aarch64-gmbox-pci1-code.fd` with
`tools/qemu-virt-nvme-gic3-flash.dtb`, emulates GICv3
redistributor/UART/RTC/PCIe ECAM and a full NVMe controller backed by
`vms/Windows 11 Pro.vmfc/disk.raw`.

## Build / run

```sh
make dtb
make hvf-win
GMBOX_WATCHDOG_SEC=12 ./build/hvf_win
# with the CFI-flash node (needed for EDK2 variables):
GMBOX_WATCHDOG_SEC=12 ./build/hvf_win tools/qemu-virt-nvme-gic3-flash.dtb
```

### Firmware fixed-PCD (2026-10-25)

Il firmware QEMU non popola il PCD PCIe prima di `PciBus`; la versione
`tools/firmware/edk2-aarch64-gmbox-pci1-code.fd` (con vars `...-vars.fd`) usa
`BasePciExpressLib`, `FdtPciPcdProducerLib` in PciHostBridge/PciBus e
`PcdPciDisableBusEnumeration=FALSE`. Con questa versione gli accessi ECAM
passano da 0 a 564. La build è documentata in `tools/firmware/README.md` e nel
vault (`Reports/2026-10-25-HVF-EDK2-Fixed-PCD.md`). Non è ancora un boot
Windows: resta l'assert DxeCore TPL durante l'enumerazione PCI.

`GMBOX_WATCHDOG_SEC` performs one `hv_vcpus_exit` at shutdown only; it is not
used to make progress.

## Verified fixes

1. **HVC/SMC return PC.** On current macOS, `hv_vcpu_run` reports PC *after*
   the `hvc` instruction. The old code added 4, skipping the instruction that
   restored LR/SP. EDK2 then returned into its stack; the guest page table
   marked that page NX (`ESR=0x8600000F`, `PC=FAR=stack`). `psci()` now
   inspects the code at PC/PC-4 and advances correctly.
2. **SMCCC TRNG.** `0x84000050` is `TRNG_VERSION` (not PSCI). Returning
   `-1` leaves `ArmTrngLib` uninitialised and `RngDxe` fails with
   `EFI_LOAD_ERROR`. The VMM now returns version 1.0, features, UUID and
   entropy; `RngDxe` starts.
3. **GICv3 redistributor.** Guest GICR accesses are forwarded through
   `hv_gic_{get,set}_redistributor_reg`; EDK2 reaches DXE/BDS.
4. **Device tree.** `qemu-virt-nvme-gic3.dts` is GICv3 (no ITS), one CPU,
   ECAM `0x3f000000` size 16 MiB, `bus-range 0x00..0x0f`, PCIe MMIO32 at
   `0x10000000`, PL011/PL031/fw-cfg and NVMe class device at bus 0 dev 1 fn 0.
5. **NVMe model.** CAP/VS/CC/CSTS/AQA/ASQ/ACQ, admin Identify Controller and
   Namespace, Set/Get Features (Number of Queues), Create I/O CQ/SQ, PRP1,
   PRP2 and PRP-list transfers, Read/Write/Flush, and CQ phase/status are
   implemented. The raw disk is opened read/write; 512-byte LBAs are detected
   from the MBR signature.

## Current blocker (evidence)

Il firmware fixed-PCD ha superato il blocco `ecam=0`:

```text
watchdog: ecam=564 nvme=0 dtb_magic=0xedfe0dd0 bar=0x20000000
loaded: PciBusDxe at RAM+0x7f22f71
loaded: PciHostBridgeDxe at RAM+0x7f106e0
not loaded: bootmgfw
```

La build `RELEASE` (`edk2-aarch64-gmbox-pci1-release-code.fd`) supera l'assert
TPL e arriva a NVMe:

```text
NVMe mmio off=0x14 wnr=1 size=4
NVMe mmio off=0x1000 wnr=1 size=4
NVMe kick qid=0 tail=1 size=2
NVMe admin opc=0x06
NVMe complete q=0 cid=0 status=0 phase=1 tail=0
watchdog: ecam=7292 nvme=13 ... bar=0x1003c000
not loaded: bootmgfw
```

Il fix CAP e' in `hvf_win.c`: `CAP.CSS` bit 0 nell'alto dword (`0x20`), senza
cui `NvmExpressDxe` rifiuta il controller prima di scrivere `CC.EN`.

Corretto anche il layout della completion queue: CDW2 contiene `Sqhd/Sqid`,
CDW3 contiene `Cid/Pt/Sc`; prima il phase tag era nel dword sbagliato e il
driver non vedeva mai il completamento. Dopo il fix `NvmExpressDxe` crea le
code I/O e ripete Identify Namespace; il guest resta poi senza `bootmgfw`.

Il prossimo punto non e' piu' il layout CQ: il controller completa i comandi.
Anche usando le VARS della VM QEMU (con voci `WINDOWS`) il firmware non carica
`bootmgfw`, quindi resta da isolare l'enumerazione del namespace/child
`NvmExpressDxe` o la selezione BDS del device path NVMe. Il vecchio problema `BaseCachingPciExpressLib` e' risolto dal
firmware fixed-PCD.

## Next steps

1. Isolare la consegna IRQ NVMe (GIC SPI/INTID 36, PCI interrupt line) e
   verificare che `NvmExpressDxe` veda il completamento admin.
2. Solo dopo, arrivare a `bootmgfw.efi` con il disco Windows.
3. Per virtio-PCI serve `viostor` ARM64 0.1.271 installato offline; il percorso
   virtio-mmio non e' utilizzabile da Windows standard.

### Bootmgfw

Con `BdsWait` saltato e patch PMCCNTR, BDS carica `bootmgfw.efi` dal NVMe.
Senza patch PMCCNTR il guest va in synchronous exception a RVA `0x3EDD4`;
con la patch arriva a un `b .` (CPU dead loop) a RVA `0x3DA0C`, senza
`winload`. La patch e' solo sperimentale e non e' ancora nel default.

### PMU di bootmgfw

Con BdsWait saltato e le istruzioni PMU neutralizzate nei buffer NVMe,
`bootmgfw` non va piu' in eccezione. BDS lo avvia ma riceve `Not Found`;
quindi il blocco e' passato a bootmgfw (BCD/winload/inizializzazione PMU).

### Ultimo confine

Con le istruzioni PMU neutralizzate, bootmgfw non va in eccezione; BDS lo
avvia ma bootmgfw restituisce `Not Found`. Il prossimo lavoro e' dentro
bootmgfw: BCD/winload o inizializzazione PMU.
