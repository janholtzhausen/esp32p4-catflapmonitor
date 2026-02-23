#ifndef CATFLAPCAM_STORAGE_H
#define CATFLAPCAM_STORAGE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t catflapcam_storage_init(void);
bool catflapcam_storage_is_ready(void);
esp_err_t catflapcam_storage_save_snapshot(const uint8_t *jpg, size_t jpg_len);
char *catflapcam_storage_list_json(size_t limit);
esp_err_t catflapcam_storage_resolve_snapshot_path(const char *name, char *out_path, size_t out_path_len);
esp_err_t catflapcam_storage_delete_snapshot(const char *name);

#endif
