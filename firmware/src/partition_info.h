// Shared partition-subtype naming, used by both the boot-time banner in
// main.cpp and the `parts` console command in console.cpp — kept in one
// place so there's only one switch statement to keep in sync with the
// partition table.

#pragma once

#include <esp_ota_ops.h>
#include <esp_partition.h>

const char *partitionSubtypeName(const esp_partition_t *p);

// Shared OTA image-state naming, used by the boot-time banner in main.cpp
// and the `info` console command in console.cpp.
const char *otaStateName(esp_ota_img_states_t state);
