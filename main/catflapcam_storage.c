#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cJSON.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif
#include "catflapcam_config.h"
#include "catflapcam_storage.h"

#define STORAGE_DIR_NAME "snapshots"
#define SNAPSHOT_NAME_PREFIX "snap-"
#define SNAPSHOT_NAME_SUFFIX ".jpg"
#define SNAPSHOT_NAME_MAX_LEN 64

typedef struct snapshot_entry {
    uint64_t seq;
    off_t size;
    char name[SNAPSHOT_NAME_MAX_LEN];
} snapshot_entry_t;

typedef struct storage_state {
    bool enabled;
    bool mounted;
    uint64_t next_seq;
    uint64_t oldest_seq;
    uint32_t file_count;
    SemaphoreHandle_t lock;
    sdmmc_card_t *card;
    sd_pwr_ctrl_handle_t pwr_ctrl_handle;
    char snapshot_dir[96];
} storage_state_t;

static const char *TAG = "catflapcam_storage";
static storage_state_t s_storage = {
    .enabled = CATFLAPCAM_SDCARD_ENABLE,
};

static bool parse_snapshot_seq(const char *name, uint64_t *seq)
{
    size_t prefix_len = strlen(SNAPSHOT_NAME_PREFIX);
    size_t suffix_len = strlen(SNAPSHOT_NAME_SUFFIX);

    if (strncmp(name, SNAPSHOT_NAME_PREFIX, prefix_len) != 0) {
        return false;
    }

    const char *seq_start = name + prefix_len;
    if (!isdigit((unsigned char)*seq_start)) {
        return false;
    }

    errno = 0;
    char *endp = NULL;
    unsigned long long seq_ull = strtoull(seq_start, &endp, 10);
    if (errno != 0 || endp == seq_start) {
        return false;
    }

    if (strcmp(endp, SNAPSHOT_NAME_SUFFIX) == 0) {
        if (seq) {
            *seq = (uint64_t)seq_ull;
        }
        return true;
    }

    if (*endp != '-') {
        return false;
    }

    size_t rem_len = strlen(endp);
    if (rem_len <= suffix_len || strcmp(endp + rem_len - suffix_len, SNAPSHOT_NAME_SUFFIX) != 0) {
        return false;
    }

    if (seq) {
        *seq = (uint64_t)seq_ull;
    }
    return true;
}

static esp_err_t build_snapshot_name(uint64_t seq, char *name, size_t name_len)
{
    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    int year = tm_now.tm_year + 1900;
    int month = tm_now.tm_mon + 1;
    int day = tm_now.tm_mday;
    int hour = tm_now.tm_hour;
    int minute = tm_now.tm_min;
    int second = tm_now.tm_sec;

    int n = snprintf(name, name_len, SNAPSHOT_NAME_PREFIX "%020llu-%04d%02d%02d-%02d%02d%02d" SNAPSHOT_NAME_SUFFIX,
                     (unsigned long long)seq, year, month, day, hour, minute, second);
    return (n > 0 && (size_t)n < name_len) ? ESP_OK : ESP_FAIL;
}

static esp_err_t build_snapshot_path(const char *name, char *path, size_t path_len)
{
    int n = snprintf(path, path_len, "%s/%s", s_storage.snapshot_dir, name);
    return (n > 0 && (size_t)n < path_len) ? ESP_OK : ESP_FAIL;
}

static esp_err_t scan_snapshot_state(uint64_t *oldest_seq, uint64_t *newest_seq, uint32_t *file_count)
{
    DIR *dir = opendir(s_storage.snapshot_dir);
    ESP_RETURN_ON_FALSE(dir, ESP_FAIL, TAG, "failed to open snapshot dir");

    uint64_t min_seq = UINT64_MAX;
    uint64_t max_seq = 0;
    uint32_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        uint64_t seq = 0;
        if (!parse_snapshot_seq(entry->d_name, &seq)) {
            continue;
        }
        if (seq < min_seq) {
            min_seq = seq;
        }
        if (seq > max_seq) {
            max_seq = seq;
        }
        count++;
    }
    closedir(dir);

    if (count == 0) {
        *oldest_seq = 0;
        *newest_seq = 0;
    } else {
        *oldest_seq = min_seq;
        *newest_seq = max_seq;
    }
    *file_count = count;
    return ESP_OK;
}

static esp_err_t delete_oldest_snapshot(void)
{
    DIR *dir = opendir(s_storage.snapshot_dir);
    ESP_RETURN_ON_FALSE(dir, ESP_FAIL, TAG, "failed to open snapshot dir");

    uint64_t min_seq = UINT64_MAX;
    char oldest_name[SNAPSHOT_NAME_MAX_LEN] = {0};
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        uint64_t seq = 0;
        if (!parse_snapshot_seq(entry->d_name, &seq)) {
            continue;
        }
        if (seq < min_seq) {
            min_seq = seq;
            strlcpy(oldest_name, entry->d_name, sizeof(oldest_name));
        }
    }
    closedir(dir);

    ESP_RETURN_ON_FALSE(oldest_name[0] != '\0', ESP_ERR_NOT_FOUND, TAG, "no snapshot found to evict");

    char path[128];
    ESP_RETURN_ON_ERROR(build_snapshot_path(oldest_name, path, sizeof(path)), TAG, "failed to build oldest snapshot path");

    if (unlink(path) != 0) {
        ESP_LOGW(TAG, "failed to delete oldest snapshot '%s': errno=%d", path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t mount_sdcard(int slot, int width)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = slot;
    host.max_freq_khz = CATFLAPCAM_SDCARD_MAX_FREQ_KHZ;

#if SOC_SDMMC_IO_POWER_EXTERNAL
#if CATFLAPCAM_SDCARD_USE_INTERNAL_LDO
    if (!s_storage.pwr_ctrl_handle) {
        sd_pwr_ctrl_ldo_config_t ldo_config = {
            .ldo_chan_id = CATFLAPCAM_SDCARD_LDO_ID,
        };
        esp_err_t pwr_ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_storage.pwr_ctrl_handle);
        if (pwr_ret != ESP_OK) {
            ESP_LOGW(TAG, "failed to init SD internal LDO (id=%d): %s", CATFLAPCAM_SDCARD_LDO_ID, esp_err_to_name(pwr_ret));
            return pwr_ret;
        }
    }
    host.pwr_ctrl_handle = s_storage.pwr_ctrl_handle;
#endif
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = width;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = CATFLAPCAM_SDCARD_FORMAT_IF_MOUNT_FAILED,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    return esp_vfs_fat_sdmmc_mount(CATFLAPCAM_SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_storage.card);
}

esp_err_t catflapcam_storage_init(void)
{
    if (!s_storage.enabled) {
        ESP_LOGI(TAG, "SD snapshot storage disabled");
        return ESP_OK;
    }

#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE
#if CONFIG_ESP_HOSTED_SDIO_SLOT == CATFLAPCAM_SDCARD_SLOT
    ESP_LOGW(TAG, "SD slot %d is also configured for hosted Wi-Fi transport. This can cause bus conflicts.", CATFLAPCAM_SDCARD_SLOT);
#endif
#endif

    if (!s_storage.lock) {
        s_storage.lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_storage.lock, ESP_ERR_NO_MEM, TAG, "failed to create storage mutex");
    }

    esp_err_t ret = mount_sdcard(CATFLAPCAM_SDCARD_SLOT, CATFLAPCAM_SDCARD_BUS_WIDTH);
    if (ret != ESP_OK && CATFLAPCAM_SDCARD_BUS_WIDTH > 1) {
        ESP_LOGW(TAG, "SD mount failed on slot=%d width=%d: %s. Retrying with 1-bit bus.",
                 CATFLAPCAM_SDCARD_SLOT, CATFLAPCAM_SDCARD_BUS_WIDTH, esp_err_to_name(ret));
        ret = mount_sdcard(CATFLAPCAM_SDCARD_SLOT, 1);
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to mount SD card");

    sdmmc_card_print_info(stdout, s_storage.card);
    int n = snprintf(s_storage.snapshot_dir, sizeof(s_storage.snapshot_dir), "%s/%s",
                     CATFLAPCAM_SDCARD_MOUNT_POINT, STORAGE_DIR_NAME);
    ESP_RETURN_ON_FALSE(n > 0 && n < (int)sizeof(s_storage.snapshot_dir), ESP_ERR_INVALID_SIZE, TAG, "snapshot dir path too long");

    if (mkdir(s_storage.snapshot_dir, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "failed to create snapshot dir '%s': errno=%d", s_storage.snapshot_dir, errno);
        return ESP_FAIL;
    }

    uint64_t newest_seq = 0;
    ESP_RETURN_ON_ERROR(scan_snapshot_state(&s_storage.oldest_seq, &newest_seq, &s_storage.file_count), TAG, "failed to scan snapshot state");
    s_storage.next_seq = (s_storage.file_count == 0) ? 1 : (newest_seq + 1);
    s_storage.mounted = true;

    ESP_LOGI(TAG, "SD snapshot storage ready at %s (files=%" PRIu32 ", next_seq=%" PRIu64 ", max_files=%d)",
             s_storage.snapshot_dir, s_storage.file_count, s_storage.next_seq, CATFLAPCAM_SNAPSHOT_MAX_FILES);
    return ESP_OK;
}

bool catflapcam_storage_is_ready(void)
{
    return s_storage.enabled && s_storage.mounted;
}

esp_err_t catflapcam_storage_save_snapshot(const uint8_t *jpg, size_t jpg_len)
{
    ESP_RETURN_ON_FALSE(jpg && jpg_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid jpeg buffer");
    ESP_RETURN_ON_FALSE(catflapcam_storage_is_ready(), ESP_ERR_INVALID_STATE, TAG, "SD storage not ready");

    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_storage.lock, pdMS_TO_TICKS(2000)) == pdPASS,
                        ESP_ERR_TIMEOUT, TAG, "timeout waiting for storage lock");

    esp_err_t ret = ESP_OK;
    if (s_storage.file_count >= CATFLAPCAM_SNAPSHOT_MAX_FILES) {
        ret = delete_oldest_snapshot();
        if (ret == ESP_OK) {
            uint64_t newest_seq = 0;
            ret = scan_snapshot_state(&s_storage.oldest_seq, &newest_seq, &s_storage.file_count);
        }
        ESP_GOTO_ON_ERROR(ret, out, TAG, "failed to evict oldest snapshot");
    }

    char name[SNAPSHOT_NAME_MAX_LEN];
    char path[128];
    ESP_GOTO_ON_ERROR(build_snapshot_name(s_storage.next_seq, name, sizeof(name)), out, TAG, "failed to build snapshot name");
    ESP_GOTO_ON_ERROR(build_snapshot_path(name, path, sizeof(path)), out, TAG, "failed to build snapshot path");

    FILE *fp = fopen(path, "wb");
    ESP_GOTO_ON_FALSE(fp, ESP_FAIL, out, TAG, "failed to open snapshot path '%s'", path);
    size_t written = fwrite(jpg, 1, jpg_len, fp);
    int flush_ret = fflush(fp);
    int close_ret = fclose(fp);
    ESP_GOTO_ON_FALSE(written == jpg_len && flush_ret == 0 && close_ret == 0, ESP_FAIL, out, TAG, "failed to write snapshot '%s'", path);

    s_storage.file_count++;
    if (s_storage.file_count == 1) {
        s_storage.oldest_seq = s_storage.next_seq;
    }
    s_storage.next_seq++;

out:
    xSemaphoreGive(s_storage.lock);
    return ret;
}

static int compare_snapshot_desc(const void *a, const void *b)
{
    const snapshot_entry_t *lhs = (const snapshot_entry_t *)a;
    const snapshot_entry_t *rhs = (const snapshot_entry_t *)b;
    if (lhs->seq > rhs->seq) {
        return -1;
    }
    if (lhs->seq < rhs->seq) {
        return 1;
    }
    return 0;
}

char *catflapcam_storage_list_json(size_t limit)
{
    if (!catflapcam_storage_is_ready()) {
        return strdup("{\"snapshots\":[]}");
    }

    if (limit == 0) {
        limit = 200;
    }
    if (limit > 2000) {
        limit = 2000;
    }

    if (xSemaphoreTake(s_storage.lock, pdMS_TO_TICKS(2000)) != pdPASS) {
        return strdup("{\"snapshots\":[]}");
    }

    DIR *dir = opendir(s_storage.snapshot_dir);
    if (!dir) {
        xSemaphoreGive(s_storage.lock);
        return strdup("{\"snapshots\":[]}");
    }

    snapshot_entry_t *entries = calloc(limit, sizeof(snapshot_entry_t));
    if (!entries) {
        closedir(dir);
        xSemaphoreGive(s_storage.lock);
        return strdup("{\"snapshots\":[]}");
    }

    size_t count = 0;
    size_t min_index = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        uint64_t seq = 0;
        if (!parse_snapshot_seq(entry->d_name, &seq)) {
            continue;
        }

        if (count >= limit && seq <= entries[min_index].seq) {
            continue;
        }

        size_t dst_index = count < limit ? count : min_index;
        snapshot_entry_t *dst = &entries[dst_index];
        memset(dst, 0, sizeof(*dst));
        dst->seq = seq;
        strlcpy(dst->name, entry->d_name, sizeof(dst->name));

        char file_path[128];
        if (build_snapshot_path(entry->d_name, file_path, sizeof(file_path)) == ESP_OK) {
            struct stat st;
            if (stat(file_path, &st) == 0) {
                dst->size = st.st_size;
            }
        }

        if (count < limit) {
            count++;
        }

        if (count == limit) {
            min_index = 0;
            for (size_t i = 1; i < count; i++) {
                if (entries[i].seq < entries[min_index].seq) {
                    min_index = i;
                }
            }
        }
    }
    closedir(dir);
    xSemaphoreGive(s_storage.lock);

    qsort(entries, count, sizeof(snapshot_entry_t), compare_snapshot_desc);

    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "snapshots", array);

    size_t out_count = (count < limit) ? count : limit;
    for (size_t i = 0; i < out_count; i++) {
        char url[112];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", entries[i].name);
        snprintf(url, sizeof(url), "/snapshots/%s", entries[i].name);
        cJSON_AddStringToObject(item, "url", url);
        cJSON_AddNumberToObject(item, "size", (double)entries[i].size);
        cJSON_AddNumberToObject(item, "seq", (double)entries[i].seq);
        cJSON_AddItemToArray(array, item);
    }
    free(entries);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json ? json : strdup("{\"snapshots\":[]}");
}

esp_err_t catflapcam_storage_resolve_snapshot_path(const char *name, char *out_path, size_t out_path_len)
{
    ESP_RETURN_ON_FALSE(catflapcam_storage_is_ready(), ESP_ERR_INVALID_STATE, TAG, "SD storage not ready");
    ESP_RETURN_ON_FALSE(name && out_path && out_path_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    uint64_t seq = 0;
    ESP_RETURN_ON_FALSE(parse_snapshot_seq(name, &seq), ESP_ERR_INVALID_ARG, TAG, "invalid snapshot name");
    (void)seq;
    return build_snapshot_path(name, out_path, out_path_len);
}

esp_err_t catflapcam_storage_delete_snapshot(const char *name)
{
    ESP_RETURN_ON_FALSE(catflapcam_storage_is_ready(), ESP_ERR_INVALID_STATE, TAG, "SD storage not ready");
    ESP_RETURN_ON_FALSE(name, ESP_ERR_INVALID_ARG, TAG, "invalid snapshot name");

    uint64_t seq = 0;
    ESP_RETURN_ON_FALSE(parse_snapshot_seq(name, &seq), ESP_ERR_INVALID_ARG, TAG, "invalid snapshot name");
    (void)seq;

    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_storage.lock, pdMS_TO_TICKS(2000)) == pdPASS,
                        ESP_ERR_TIMEOUT, TAG, "timeout waiting for storage lock");

    esp_err_t ret = ESP_OK;
    char path[128];
    ESP_GOTO_ON_ERROR(build_snapshot_path(name, path, sizeof(path)), out, TAG, "failed to build snapshot path");

    if (unlink(path) != 0) {
        ret = (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        goto out;
    }

    uint64_t newest_seq = 0;
    ESP_GOTO_ON_ERROR(scan_snapshot_state(&s_storage.oldest_seq, &newest_seq, &s_storage.file_count), out, TAG, "failed to rescan snapshot state");
    s_storage.next_seq = (s_storage.file_count == 0) ? 1 : (newest_seq + 1);

out:
    xSemaphoreGive(s_storage.lock);
    return ret;
}
