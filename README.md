# GMBox

> **Stato Windows 11:** Windows 11 Pro ARM64 è stato avviato al desktop tramite
> **QEMU/HVF** (EDK2 + NVMe) sul disco importato, con scritture sul `disk.raw`.
> Questo è un **workaround diagnostico**, non il motore nativo GMBox. VZ/iBoot resta
> bloccato; il motore Windows nativo su Hypervisor.framework è in sviluppo (ADR-003)
> e il probe PMU ha già verificato trap/emulazione.
> Evidenza: `/Users/gaetano/Documents/AiWiki/01-Projects/GMBox/Reports/Boot-EFI-2026-10-24/desktop-gmbox-qemu-2026-10-24.png`.
> Dettaglio: `/Users/gaetano/Documents/AiWiki/01-Projects/GMBox/Decisions/ADR-002-Backend-QEMU-HVF-Windows.md`.

Clone leggero di VMware Fusion per Apple silicon, scritto in **C99** (niente Python),
con CLI + GUI AppKit e `Virtualization.framework` per Linux. Per Windows è in
sviluppo un motore nativo su **Hypervisor.framework** (ADR-003); QEMU/HVF resta
solo un workaround diagnostico.

## Stato verificato

- **Windows 11 Pro ARM64** avviato al desktop dal disco importato con **QEMU/HVF**
  (workaround), EDK2 e NVMe; configurazione 8 GB / 6 vCPU (`sockets=1,cores=6`).
  Il `disk.raw` riceve scritture e `vmfc stop` usa lo spegnimento ACPI QMP.
- **Motore nativo HVF**: EDK2/UEFI avviato senza QEMU; firmware e driver DXE
  (`edk2-stable202408-prebuilt.qemu.org`) partono, ma il boot manager non viene
  raggiunto e lo storage PCIe/NVMe non viene enumerato. Stato e blocco esatti:
  `tools/README-hvf-nvme.md`.
- **Linux (VZ)**: avvio VM EFI con disco NVMe, ISO come USB mass storage, rete NAT,
  tastiera/mouse USB, audio, grafica virtio 1920x1200, NVRAM persistente.
- Test positivo end-to-end: **Alpine Linux 3.22 aarch64** avviata dalla ISO e
  visibile nella finestra della VM (boot, EFI, input, NAT).
- Import VMware: conversione **VMDK split-sparse -> raw** verificata su un disco
  Windows 11 ARM da 64 GiB (22 GiB allocati, conversione in ~35 s).
- Rilevamento **BitLocker** in importazione: l'import si interrompe prima di
  creare una VM inutilizzabile.
- Doppia ISO testata (installer + driver) con Alpine: il firmware avvia
  correttamente il supporto con l'installer anche con la seconda ISO collegata.
- NVRAM QEMU separata (`vars.fd`) da `nvram.bin` VZ; il backend Windows non tocca
  la NVRAM VZ.

## Perché non un hypervisor proprietario

Analisi REA di VMware Fusion 26.0.1 (`/Applications/VMware Fusion.app`):

- Mach-O universale `arm64` + `x86_64`, entitlement `com.apple.security.hypervisor`.
- Dipendenze: `vmnet.framework`, `libvmwarebase`, `libvnetlib`, `libvmwareui`, ecc.
- **Non** linka `Virtualization.framework`.

Fusion è quindi un VMM proprietario costruito direttamente su
`Hypervisor.framework`, con device model proprio (NVMe, SVGA, vmxnet3, vTPM,
ecc.). Reimplementarlo richiede un VMM completo. GMBox usa **VZ** per i guest
Linux e sta costruendo un **motore Windows nativo su Hypervisor.framework**
(ADR-003); QEMU/HVF è solo un workaround diagnostico, non la GUI del prodotto.
La parte "Fusion" (bundle `.vmfc`, `config.vmx`, gestione VM, import VMware, GUI
e menu) è ricostruita in C99.

## Requisiti

- macOS 14 o successivo, Apple silicon (testato su macOS 27 / M5 Max).
- Xcode Command Line Tools (`clang`, `codesign`, `hdiutil`).
- Per Linux: solo VZ, nessuna dipendenza esterna.
- Per Windows: **QEMU 11+** e EDK2, da Homebrew:

  ```sh
  brew install qemu
  ```

  QEMU viene cercato in `/opt/homebrew/bin` e `/usr/local/bin`. I file EDK2
  (`edk2-aarch64-code.fd`, `edk2-arm-vars.fd`) sono forniti dal pacchetto Homebrew.
  Percorsi alternativi: `VMFC_QEMU`, `VMFC_QEMU_FIRMWARE`, `VMFC_QEMU_VARS`.
- Per installazioni nuove: ISO **Windows 11 ARM64** (le ISO x86 non funzionano).

## Build

```sh
make
```

Output:

- `build/GMBox.app` — app GUI firmata ad-hoc con entitlement
  `com.apple.security.virtualization`.
- `build/vmfc` — stessa CLI, fuori dal bundle.

La firma è ad-hoc e non notarizzata: è un build locale.

## Uso rapido

```sh
# Elenca le VM (default: ~/Documents/Virtual Machines)
./build/vmfc list

# Scarica la ISO driver virtio per Windows (una volta sola)
./build/vmfc tools

# Crea una VM Windows 11 ARM da ISO (64 GB di disco sparso, 8 GB RAM).
# Con QEMU la ISO va collegata esplicitamente con VMFC_QEMU_ISO=1.
VMFC_QEMU_ISO=1 ./build/vmfc create "Windows 11" --iso ~/Downloads/Windows11_ARM64.iso \
    --memory 8G --disk 64G --cpus 6 --start

# Avvia / spegni / informazioni
./build/vmfc start "Windows 11"
./build/vmfc stop  "Windows 11"
./build/vmfc info  "Windows 11"

# Test senza finestra (QEMU headless; QMP su /tmp/gmbox-qmp-<uuid8>.sock)
./build/vmfc start "Windows 11" --headless --seconds 30

# Forza il motore (default: QEMU per Windows, VZ per Linux)
./build/vmfc start "Windows 11" --engine qemu
./build/vmfc start "Linux"      --engine vz

# Importa una VM VMware esistente (.vmwarevm, .vmx o .vmdk)
./build/vmfc import "/percorso/Windows 11.vmwarevm" --name "Windows 11 (clone)" --dry-run
./build/vmfc import "/percorso/Windows 11.vmwarevm" --name "Windows 11 (clone)"
```

`--dir CARTELLA` cambia la cartella delle VM. `start --headless --seconds N`
esegue senza finestra (utile per test). `--iso` su `start` aggiorna l'installer.

Per Windows QEMU:

- la ISO non viene collegata automaticamente; usa `VMFC_QEMU_ISO=1` insieme a
  `--iso`/`--drivers`;
- la rete è spenta per default; usa `VMFC_QEMU_NET=1` per NAT `virtio-net-pci`;
- `vmfc gui` apre una finestra QEMU Cocoa; `--headless` usa `-display none`.

### Menu della GUI

- **Macchina virtuale**: Avvia, Pausa, Riprendi, Spegni (chiedi al guest),
  Spegni forzatamente, Scegli immagine ISO.
- **Finestra**: Schermo intero, Riduci a icona.

La chiusura della finestra termina l'app; l'arresto passa da
`requestStop` e, se il guest non risponde, da uno stop forzato.

Questo menu è il percorso **VZ/Linux**. Per Windows/QEMU la finestra è quella
Cocoa di QEMU e i comandi di lifecycle restano `vmfc start/stop`.

## Formato bundle

```
Nome.vmfc/
  config.vmx      configurazione testuale in stile VMware
  disk.raw        disco raw sparso (NON un VMDK)
  nvram.bin       variabili EFI VZ (create/gestite da Virtualization.framework)
  vars.fd         NVRAM EDK2 per QEMU (Windows; creata dalla template Homebrew)
  machine.id      identificatore macchina persistente
  vmfc.pid        PID del processo in esecuzione
  vmware.log      log del clone
```

`config.vmx` usa chiavi VMware leggibili (`displayName`, `guestOS`, `memsize`,
`numvcpus`, `nvme0:0.fileName`, `sata0:1.fileName`, `ethernet0.*`) più chiavi
`vmfc.*`. Non è un VMX eseguibile da VMware Fusion: il disco è raw.

## Hardware virtuale

| Componente | Linux (VZ) | Windows (QEMU/HVF) |
|---|---|---|
| Firmware | EFI VZ + NVRAM `nvram.bin` | EDK2 + NVRAM `vars.fd` |
| Disco principale | NVMe | NVMe (`nvme,serial=GMBOX`) |
| Installer | ISO come USB mass storage | USB storage solo con `VMFC_QEMU_ISO=1` |
| Rete | virtio-net NAT | `virtio-net-pci` user NAT solo con `VMFC_QEMU_NET=1`; default spenta |
| Video | virtio-gpu, 1920x1200 | `ramfb`; finestra QEMU Cocoa o `-display none` |
| Input | USB keyboard + tablet | `qemu-xhci` + `usb-kbd` + `usb-tablet` |
| Audio | virtio-sound -> host | non implementato |
| Cartella condivisa | virtiofs (tag `share`) | non supportata |
| Virtualizzazione annidata | abilitata se supportata (M3+) | `-cpu host` (dipende dall'host) |

## Import da VMware

`vmfc import` accetta:

- bundle `.vmwarevm` (trova il primo `.vmx`);
- file `.vmx`;
- descrittore o extent `.vmdk`.

Supporta descrittori con extent `SPARSE` (split-sparse e monolithic-sparse) e
`FLAT`/`ZERO`. Converte solo i grain allocati in un file raw sparso, quindi il
disco sorgente non viene modificato e lo spazio occupato resta vicino a quello
reale. Snapshot e memoria sospesa non vengono importati: solo il disco base.

Se il disco contiene un volume BitLocker (`-FVE-FS-`), l'import si interrompe.

## Installare Windows 11 ARM64

Serve una ISO **ARM64** (non x86). Microsoft la distribuisce qui:

<https://www.microsoft.com/en-us/software-download/windows11arm64>

Il percorso **verificato** è l'avvio del disco Windows già installato (importato)
con QEMU/HVF. Per una installazione pulita:

1. QEMU usa NVMe (`stornvme` inbox) ed EDK2; non usa virtio-scsi.
2. Collega installer e driver solo con `VMFC_QEMU_ISO=1`:

   ```sh
   VMFC_QEMU_ISO=1 ./build/vmfc start "Windows 11" \
       --iso ~/Downloads/Win11_ARM64.iso \
       --drivers "/percorso/utm-guest-tools.iso"
   ```

3. Per la rete NAT: `VMFC_QEMU_NET=1`; serve il driver `virtio-net` nel guest.
4. TPM e Secure Boot non sono emulati: Windows 11 può richiedere il bypass
   (`Shift+F10` -> `regedit` -> `HKLM\SYSTEM\Setup\LabConfig` ->
   `BypassTPMCheck=1`, `BypassSecureBootCheck=1`).
5. **L'installazione pulita via QEMU non è ancora stata validata end-to-end** in
   GMBox; non dichiararla riuscita senza prove. Il boot del disco esistente è
   invece verificato al desktop.

VZ non è utilizzabile per questo disco Windows: iBoot/NVRAM non crea una voce
di boot utilizzabile e il disco resta senza scritture. La diagnosi completa è in
`/Users/gaetano/Documents/AiWiki/01-Projects/GMBox/Hard-Lessons/VZ-boot-disco-windows-importato.md`.

## BitLocker, vTPM e la VM Windows 11 presente sul Mac

La VM `Windows 11 64-bit Arm.vmwarevm` presente in
`~/Documents/Virtual Machines/Windows 11 Pro/Windows 11 Pro BitLocker/` ha:

- `vtpm.present = "TRUE"`;
- `vmx.encryptionType = "partial"`;
- il volume Windows cifrato con BitLocker (settore di boot `-FVE-FS-`).

La chiave BitLocker è sigillata nel **vTPM emulato da VMware Fusion**.
Né VZ né il backend QEMU/HVF espongono un vTPM utilizzabile con quello di VMware:
il disco BitLocker non è avviabile in questo clone. Non è un problema di grafica
o di driver: il guest non arriva a scrivere sul disco.

Per rendere importabile quella VM:

1. Avvia la VM in **VMware Fusion** (che possiede il vTPM) e accedi a Windows.
2. Pannello **Gestione BitLocker** -> **Disattiva BitLocker**; attendi la fine
   della decifratura.
3. Spegni Windows completamente (non sospendere).
4. Ripeti:

```sh
./build/vmfc import "/Users/gaetano/Documents/Virtual Machines/Windows 11 Pro/Windows 11 Pro BitLocker/Windows 11 64-bit Arm.vmwarevm" \
    --name "Windows 11 ARM"
```

## Limiti noti

- **Nessun vTPM**: BitLocker, VBS/Hyper-V e requisiti TPM di Windows 11 non sono
  supportati né in VZ né in QEMU/HVF. Il setup di Windows 11 può richiedere il
  bypass TPM (`Shift+F10` -> `regedit` ->
  `HKLM\SYSTEM\Setup\LabConfig` -> `BypassTPMCheck=1`,
  `BypassSecureBootCheck=1`).
- **Windows 11 ARM64 (QEMU/HVF)**: avvio del disco importato verificato; NVMe è
  nativo, quindi non serve `vioscsi`. ISO e rete sono opt-in
  (`VMFC_QEMU_ISO=1`, `VMFC_QEMU_NET=1`). L'installazione pulita non è stata
  validata end-to-end. Non ci sono cartelle condivise, audio o snapshot.
  Il display è `ramfb` (niente 3D/GPU); la configurazione 8 GB/6 vCPU con un solo
  socket riduce il collo di bottiglia, ma l'interattività può restare inferiore a
  Fusion/VZ finché non si aggiunge un driver virtio-gpu.
- **Windows 11 ARM64 (VZ)**: percorso bloccato da firmware/iBoot e PMU; non
  usarlo per Windows. Vedi il bug nel vault.
- Niente snapshot, sospensione su disco, USB passthrough, 3D, thin provisioning,
  crittografia del bundle, import di MBR (solo GPT/UEFI).
- La GUI è volutamente minimale: una finestra per VM, non una libreria grafica.
  Per Windows la finestra è quella di QEMU (`-display cocoa`), separata dalla
  GUI AppKit.
- `vmfc stop` e `--seconds` su QEMU inviano `system_powerdown` via QMP e attendono;
  se il guest non risponde, inviano SIGTERM e poi SIGKILL. Per uno shutdown Windows
  pulito usare comunque **Start -> Spegni** nel guest.
- Firma ad-hoc: al primo avvio macOS può richiedere conferma.

## Verifiche

```sh
make smoke       # crea una VM in /tmp e la avvia headless (QEMU; stop ACPI/SIGTERM)
make hvf-probe   # probe nativo Hypervisor.framework: trap/emulazione PMU
make hvf-mini    # mini-VMM HVF: vCPU, MMIO, UART, HVC
make hvf-gic     # HVF: GICv3, GICD_TYPER, generic timer
make hvf-edk2    # compila il boot EDK2/UEFI nativo (senza storage)
./build/vmfc info "Windows 11 Pro" --dir "$PWD/vms"   # Motore e NVRAM
find . -name "*.py" -not -path "./build/*" | wc -l   # deve essere 0
```

## Struttura

```
src/main.c      CLI (create/list/start/stop/info/delete/import)
src/vm.c        bundle, config.vmx, elenco, PID, log, info motore
src/vmdk.c      lettura VMDK sparse/flat, conversione raw, BitLocker
src/vz.c        Virtualization.framework, finestra, menu, delegate
src/qemu.c      backend QEMU/HVF diagnostico per Windows, EDK2, QMP, lifecycle
tools/hvf_pmu_probe.c  probe Hypervisor.framework: trap/emulazione PMU
tools/hvf_mini.c       mini-VMM nativo: RAM, MMIO, UART, HVC
tools/hvf_gic.c        GICv3 nativo + generic timer
tools/hvf_edk2.c       boot EDK2/UEFI su HVF; DTB fixture di riferimento
tools/qemu-virt-reference.dtb  DTB GICv3 di riferimento (EDK2, non esegue QEMU)
tools/qemu-virt-nvme-noint.dtb DTB PCIe/NVMe di lavoro (WIP)
src/util.c      utility C99
resources/      Info.plist e entitlements
```
