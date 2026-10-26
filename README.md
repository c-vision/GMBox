# GMBox

Clone leggero di VMware Fusion per Apple silicon, scritto in **C99** (niente Python).
CLI `vmfc` + GUI AppKit; guest Linux con `Virtualization.framework`; guest
**Windows 11 ARM64** con un VMM nativo su `Hypervisor.framework` (`tools/hvf_win.c`),
senza QEMU a runtime.

## Stato

- **Windows 11 Pro ARM64 arriva al desktop** nel VMM HVF nativo: EDK2/UEFI → Windows Boot Manager →
  kernel → login → desktop (verificato dal framebuffer).
- **Linux (VZ)**: EFI, NVMe, ISO come USB, rete NAT, tastiera/mouse, audio, grafica virtio.
- **Import VMware**: VMDK split-sparse → raw; rilevamento BitLocker.

Il VMM nativo emula: GICv3 (HVF), timer, PL011, PL031, fw_cfg (ACPI generato in C), flash CFI
per le variabili UEFI, PCIe ECAM, controller **NVMe**, controller **USB EHCI** con tastiera e
mouse HID, framebuffer **ramfb**. Il PMU è emulato dal framework (`PMUVer=1`).

## Compilare ed eseguire

```sh
make            # CLI + GUI
make acpi       # rigenera le tabelle ACPI (serve iasl)
make hvf-win    # VMM Windows nativo (build/hvf_win)
```

Esempio (usare **una copia** del disco: Windows scrive sul disco):

```sh
cp -c "vms/Windows 11 Pro.vmfc/disk.raw" /tmp/d.raw
cp -c "vms/Windows 11 Pro.vmfc/vars.fd"  /tmp/v.fd
mkfifo /tmp/in.fifo
GMBOX_INPUT=/tmp/in.fifo GMBOX_AUTOENTER=1 GMBOX_REBOOT_ON_OFF=1 \
GMBOX_FB_DUMP=/tmp/s.bmp GMBOX_FB_INTERVAL=10 \
GMBOX_DISK=/tmp/d.raw GMBOX_VARS=/tmp/v.fd GMBOX_WATCHDOG_SEC=900 build/hvf_win
```

Input da tastiera/mouse: scrivere comandi sulla FIFO (`key space`, `type testo`, `key enter`,
`move dx dy`, `click l`, …). Lo schermo si legge dal BMP (`sips -s format png s.bmp --out s.png`).

Variabili utili: `GMBOX_FW`, `GMBOX_VARS`, `GMBOX_DISK`, `GMBOX_NVME_LOG`, `GMBOX_USBLOG`,
`GMBOX_KDBRK` (stampa i driver caricati dal kernel Windows), `GMBOX_NO_USB`.

## Limitazioni attuali

- Il VMM nativo è un **prototipo**: percorsi di firmware, VARS e disco sono ancora in parte fissi nel codice.
- Avvio lento (circa 3 minuti fino al login) e **una sola vCPU**.
- Display solo `ramfb` 800x600, senza accelerazione 3D né audio; nessun appunti/cartelle condivise.
- Nessuna rete nel VMM nativo, nessun vTPM, Secure Boot, USB passthrough.
- Nessuno snapshot, né cifratura della VM.
- ACPI minimale (senza IORT/DBG2), PCIe senza MSI-X, mouse relativo (non assoluto).
- Se il disco contiene boot falliti, Windows mostra la schermata *Recovery*: serve `GMBOX_AUTOENTER=1`.
- Guest Windows su VZ non supportato (bug iBoot/NVRAM); l'installazione da ISO nel VMM nativo non è testata.
- `vmfc list` non scende nelle sottocartelle; la libreria VM e il wizard di creazione non esistono ancora.

## Struttura

- `src/` CLI `vmfc` e app GUI (VZ, import VMDK).
- `tools/hvf_win.c` e `tools/*.inc` — VMM nativo, USB, tracer, ACPI (`acpi_build.c`, `dsdt.asl`).
- `tools/firmware/` firmware EDK2 (ricetta di build in `tools/firmware/README.md`).

Regola di progetto: solo C99 / shell, nessun Python.
