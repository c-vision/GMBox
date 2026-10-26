# EDK2 GMBox PCI1 firmware

Firmware EDK2 compilato per il VMM nativo GMBox. Non contiene Python e non
richiede Python a runtime; Python 3.9 è stato usato **solo** come tool di build
in `/tmp`, fuori dal progetto.

## Artefatti

| File | Dimensione | SHA-256 |
|---|---:|---|
| `edk2-aarch64-gmbox-pci1-code.fd` | 64 MiB (sparse) | `f7d7e66529591121b275d8fe1f32cfbf2b7c4da1b28762a45f54c3b950e08900` |
| `edk2-aarch64-gmbox-pci1-vars.fd` | 64 MiB (sparse) | `b3b855c5a80310168051164986855692d1bdb06e67619856177965cd87c6774f` |
| `edk2-aarch64-gmbox-pci1-release-code.fd` | 64 MiB (sparse) | `a59ea1d70dab78f087d643a85a21c9a0f1169d2ce20849289fcd83d5e4366518` |
| `edk2-aarch64-gmbox-pci1-release-vars.fd` | 64 MiB (sparse) | `b3b855c5a80310168051164986855692d1bdb06e67619856177965cd87c6774f` |
| `edk2-aarch64-gmbox-pci2-boot-code.fd` | 64 MiB (sparse) | `8a6c078ee6b376a62f0711d6eeace12ba4df3131cb3969b74688e81f930fa003` |

Le immagini `pci1` sono la build **DEBUG** (assert TPL); `pci1-release` è la
stessa configurazione compilata `RELEASE`, usata per superare l'assert e
proseguire fino a NVMe. I file sono **padded a 64 MiB**. Il padding è
necessario perché `tools/hvf_win.c` mappa code e vars in slot pflash da 64 MiB.
Sono file sparsi: `ls -lh` mostra 64 MiB, `ls -l`/`du` mostrano lo spazio
effettivo.

## Cosa contiene

- EDK2 `edk2-stable202408`, build `ArmVirtQemu-AArch64`, `DEBUG_GCC`; variante `RELEASE_GCC`.
- `PciExpressLib` → `BasePciExpressLib` (evita la cache del PCD).
- `FdtPciPcdProducerLib` forzato in `PciHostBridgeDxe` e `PciBusDxe`.
- `PcdPciDisableBusEnumeration=FALSE`.

Sovrascritture DSC: `tools/edk2-fixed-pcd.dsc.inc`.

## Build riproducibile

Prerequisiti macOS:

- Xcode Command Line Tools (`cc`, `make`)
- `aarch64-elf-gcc` e binutils (Homebrew)
- `iasl`
- `python3` **solo per BaseTools** (`/usr/bin/python3` 3.9 nel test)

Passi:

```sh
# 1. Sorgenti EDK2 e QEMU helper
git clone --depth 1 --branch edk2-stable202408 \
  https://github.com/tianocore/edk2.git /tmp/edk2-gmbox
git clone --depth 1 https://github.com/qemu/qemu.git /tmp/qemu-src

# 2. Inizializzare i submodule usati dalla build ArmVirt.
#    (SubhookLib non serve; il suo URL upstream può non esistere.)
cd /tmp/edk2-gmbox
git submodule update --init --recursive --force

# 3. Installare le sovrascritture GMBox
cp /Users/gaetano/Documents/Projects/GMBox/tools/edk2-fixed-pcd.dsc.inc \
  /tmp/edk2-gmbox/ArmVirtPkg/GMBoxPcd.inc
printf '\n!include ArmVirtPkg/GMBoxPcd.inc\n' \
  >> /tmp/edk2-gmbox/ArmVirtPkg/ArmVirtQemu.dsc

# 4. Build (BaseTools inclusi). GCC 16 richiede il flag extra.
cd /tmp/qemu-src/roms
GCC_AARCH64_PREFIX=/opt/homebrew/bin/aarch64-elf- \
EXTRA_OPTFLAGS='-Wno-macro-redefined' \
/usr/bin/python3 edk2-build.py \
  --config edk2-build.config \
  --core /tmp/edk2-gmbox \
  --match armvirt.aa64 \
  --toolchain GCC \
  --jobs 8

# 5. Output (prima del padding)
ls -lh /tmp/qemu-src/roms/Build/ArmVirtQemu-AARCH64/DEBUG_GCC/FV/QEMU_EFI.fd
ls -lh /tmp/qemu-src/roms/Build/ArmVirtQemu-AARCH64/DEBUG_GCC/FV/QEMU_VARS.fd

# 6. Padding macOS (il truncate GNU usato da edk2-build.py non funziona)
cp Build/ArmVirtQemu-AARCH64/DEBUG_GCC/FV/QEMU_EFI.fd \
  /tmp/gmbox-edk2.fd
cp Build/ArmVirtQemu-AARCH64/DEBUG_GCC/FV/QEMU_VARS.fd \
  /tmp/gmbox-vars.fd
/usr/bin/truncate -s 64m /tmp/gmbox-edk2.fd /tmp/gmbox-vars.fd
```

## Correzioni locali necessarie nel test

Sono correzioni dell'ambiente di build, non del progetto GMBox:

1. `BaseTools/Source/C/BrotliCompress/GNUmakefile`: aggiungere
   `brotli/c/dec/prefix.o`, `brotli/c/dec/static_init.o`,
   `brotli/c/enc/static_dict_lut.o`, `brotli/c/enc/static_init.o`
   (il submodule Brotli è più recente del Makefile EDK2).
2. `MdePkg/Library/BaseFdtLib/LibFdtSupport.h`: sostituire
   `typedef BOOLEAN bool;` con `#define bool _Bool` per GCC 16.
3. `EXTRA_OPTFLAGS=-Wno-macro-redefined` per BaseTools con Apple clang.
4. `edk2-build.py` prova `truncate --size` (GNU); su macOS usare
   `/usr/bin/truncate -s`.

## Stato del test

Con DTB `tools/qemu-virt-nvme-gic3-flash.dtb` e VMM nativo:

- firma `DEBUG`: ECAM 564, `PciBusDxe`/`PciHostBridgeDxe` caricati; resta
  l'assert `DxeCore Tpl.c(66)` (`OldTpl=0x1F`, `NewTpl=0x10`);
- firma `RELEASE`: ECAM 7292, NVMe MMIO 13, `CC.EN`, code admin, Identify
  completato e ripetuto; con il fix CDW2/CDW3 il driver vede i completamenti;
- `bootmgfw` non caricato: il controller e le code rispondono, ma manca
  probabilmente una voce BDS/VARS valida per il boot NVMe.

Quindi questo firmware risolve il PCD/PciBus iniziale, ma non è ancora un
firmware Windows-ready.

Nota: il test con le VARS della VM (`vms/Windows 11 Pro.vmfc/vars.fd`, che
contiene voci `WINDOWS`) arriva agli stessi comandi NVMe ma non a `bootmgfw`;
quindi la VARS vuota non e' l'unica causa.


## pci2-boot

Immagine con `BdsWait` saltato e `PcdPlatformBootTimeOut=0`: serve per testare
BDS/bootmgfw senza il timeout. BDS carica Boot0005, ma bootmgfw restituisce
`Not Found`; non e' un firmware Windows-ready.
