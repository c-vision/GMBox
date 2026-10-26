#ifndef VMFC_VMDK_H
#define VMFC_VMDK_H

#include <stddef.h>
#include <stdint.h>

int vmdk_probe(const char *path, uint64_t *total_sectors, char *err, size_t errlen);
int vmdk_to_raw(const char *path, const char *raw_path, uint64_t *total_sectors,
                char *err, size_t errlen);
int vmdk_detect_bitlocker(const char *path, int *partition_index, uint64_t *offset,
                          char *err, size_t errlen);

#endif
