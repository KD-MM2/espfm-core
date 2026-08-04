#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* Minimal ESP-IDF esp_littlefs.h stub for host-based unit tests. */

typedef struct {
    const char *base_path;
    const char *partition_label;
    bool format_if_mount_failed;
    bool dont_mount;
    bool read_only;
    bool grab_mount;
    size_t max_files;
} esp_vfs_littlefs_conf_t;

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *conf);
esp_err_t esp_littlefs_info(const char *partition_label, size_t *total_bytes, size_t *used_bytes);
