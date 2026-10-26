CC ?= clang
APP := build/GMBox.app
BIN := $(APP)/Contents/MacOS/vmfc
CLI := build/vmfc
HVF_PROBE := build/hvf_pmu_probe
HVF_MINI := build/hvf_mini
HVF_GIC := build/hvf_gic
HVF_EDK2 := build/hvf_edk2
SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)
CFLAGS := -std=c99 -O2 -g -Wall -Wextra -Wno-unused-parameter -fblocks \
          -mmacosx-version-min=14.0 -Isrc
LDFLAGS := -framework Cocoa -framework Foundation -framework Virtualization

.PHONY: all clean run smoke sign hvf-probe hvf-mini hvf-gic hvf-edk2

all: $(CLI)

$(BIN): $(SRC) $(HDR) resources/Info.plist resources/entitlements.plist
	@mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDFLAGS)
	cp resources/Info.plist $(APP)/Contents/Info.plist
	codesign --force --sign - --entitlements resources/entitlements.plist $(APP)

$(CLI): $(BIN)
	cp $(BIN) $(CLI)
	codesign --force --sign - --entitlements resources/entitlements.plist $(CLI)

run: all
	./$(CLI)

smoke: all
	@rm -rf /tmp/vmfc-smoke
	./$(CLI) create "Smoke Test" --dir /tmp/vmfc-smoke --memory 2048M --disk 2G
	./$(CLI) list --dir /tmp/vmfc-smoke

hvf-probe:
	@mkdir -p build
	$(CC) -std=c99 -O2 -g -Wall -Wextra -mmacosx-version-min=15.0 \
	    -framework Hypervisor -o $(HVF_PROBE) tools/hvf_pmu_probe.c
	codesign --force --sign - --entitlements tools/hvf.entitlements $(HVF_PROBE)
	./$(HVF_PROBE)

hvf-mini:
	@mkdir -p build
	$(CC) -std=c99 -O2 -g -Wall -Wextra -mmacosx-version-min=15.0 \
	    -framework Hypervisor -o $(HVF_MINI) tools/hvf_mini.c
	codesign --force --sign - --entitlements tools/hvf.entitlements $(HVF_MINI)
	./$(HVF_MINI)

hvf-gic:
	@mkdir -p build
	$(CC) -std=c99 -O2 -g -Wall -Wextra -mmacosx-version-min=15.0 \
	    -framework Hypervisor -o $(HVF_GIC) tools/hvf_gic.c
	codesign --force --sign - --entitlements tools/hvf.entitlements $(HVF_GIC)
	./$(HVF_GIC)

clean:
	rm -rf build /tmp/vmfc-smoke

hvf-edk2:
	@mkdir -p build
	$(CC) -std=c99 -O2 -g -Wall -Wextra -mmacosx-version-min=15.0 \
	    -framework Hypervisor -pthread -o $(HVF_EDK2) tools/hvf_edk2.c
	codesign --force --sign - --entitlements tools/hvf.entitlements $(HVF_EDK2)

HVF_WIN := build/hvf_win

hvf-win:
	@mkdir -p build
	$(CC) -std=c99 -O2 -g -Wall -Wextra -mmacosx-version-min=15.0 \
	    -framework Hypervisor -pthread -o $(HVF_WIN) tools/hvf_win.c
	codesign --force --sign - --entitlements tools/hvf.entitlements $(HVF_WIN)

dtb:
	dtc -I dts -O dtb -o tools/qemu-virt-nvme-gic3.dtb tools/qemu-virt-nvme-gic3.dts
	dtc -I dts -O dtb -o tools/qemu-virt-nvme-gic3-flash.dtb tools/qemu-virt-nvme-gic3-flash.dts

.PHONY: hvf-win dtb

# ACPI tables for the native HVF Windows VMM (needs iasl)
acpi:
	@mkdir -p build
	iasl -p build/dsdt tools/dsdt.asl
	cp build/dsdt.aml tools/dsdt.aml
	$(CC) -std=c99 -Wall -Wextra -O2 -o build/acpi_build tools/acpi_build.c
	build/acpi_build

.PHONY: acpi
