#ifndef VMFC_QEMU_H
#define VMFC_QEMU_H

#include "vmfc.h"

int qemu_run(vm_config *cfg, int headless, int seconds);
int qemu_request_powerdown(const vm_config *cfg);

#endif
