#ifndef VMFC_QEMU_H
#define VMFC_QEMU_H

#include "vmfc.h"

int qemu_run(vm_config *cfg, int headless, int seconds);
int qemu_request_powerdown(const vm_config *cfg);
int qemu_request_reset(const vm_config *cfg);
int qemu_request_pause(const vm_config *cfg);
int qemu_request_resume(const vm_config *cfg);
int qemu_query_status(const vm_config *cfg, char *status, size_t n);

#endif
