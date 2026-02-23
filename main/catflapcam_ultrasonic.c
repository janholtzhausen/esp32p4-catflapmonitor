/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "catflapcam_ultrasonic.h"

#if CATFLAPCAM_ULTRASONIC_TRIGGER_ENABLE
static catflapcam_webcam_t *s_web_cam;
static int s_ultrasonic_source_index = -1;

static int select_ultrasonic_source_index(catflapcam_webcam_t *web_cam)
{
    if (!web_cam || web_cam->video_count <= 0) {
        return -1;
    }

    if (CATFLAPCAM_ULTRASONIC_SOURCE_INDEX >= 0 &&
        CATFLAPCAM_ULTRASONIC_SOURCE_INDEX < web_cam->video_count &&
        catflapcam_webcam_is_valid_video(&web_cam->video[CATFLAPCAM_ULTRASONIC_SOURCE_INDEX])) {
        return CATFLAPCAM_ULTRASONIC_SOURCE_INDEX;
    }

    for (int i = 0; i < web_cam->video_count; i++) {
        if (catflapcam_webcam_is_valid_video(&web_cam->video[i])) {
            return i;
        }
    }

    return -1;
}

static esp_err_t ultrasonic_init_gpio(void)
{
    gpio_config_t trig_cfg = {
        .pin_bit_mask = 1ULL << CATFLAPCAM_ULTRASONIC_TRIG_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t echo_cfg = {
        .pin_bit_mask = 1ULL << CATFLAPCAM_ULTRASONIC_ECHO_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&trig_cfg), TAG, "failed to configure ultrasonic trig gpio");
    ESP_RETURN_ON_ERROR(gpio_config(&echo_cfg), TAG, "failed to configure ultrasonic echo gpio");
    ESP_RETURN_ON_ERROR(gpio_set_level(CATFLAPCAM_ULTRASONIC_TRIG_GPIO, 0), TAG, "failed to init trig level");
    return ESP_OK;
}

static esp_err_t ultrasonic_measure_distance_cm(float *distance_cm)
{
    const int64_t timeout_us = 30000;
    int64_t wait_start_us = esp_timer_get_time();

    gpio_set_level(CATFLAPCAM_ULTRASONIC_TRIG_GPIO, 0);
    esp_rom_delay_us(2);
    gpio_set_level(CATFLAPCAM_ULTRASONIC_TRIG_GPIO, 1);
    esp_rom_delay_us(10);
    gpio_set_level(CATFLAPCAM_ULTRASONIC_TRIG_GPIO, 0);

    while (!gpio_get_level(CATFLAPCAM_ULTRASONIC_ECHO_GPIO)) {
        if ((esp_timer_get_time() - wait_start_us) > timeout_us) {
            return ESP_ERR_TIMEOUT;
        }
    }

    int64_t echo_start_us = esp_timer_get_time();
    while (gpio_get_level(CATFLAPCAM_ULTRASONIC_ECHO_GPIO)) {
        if ((esp_timer_get_time() - echo_start_us) > timeout_us) {
            return ESP_ERR_TIMEOUT;
        }
    }

    int64_t echo_end_us = esp_timer_get_time();
    *distance_cm = (float)(echo_end_us - echo_start_us) / 58.0f;
    return ESP_OK;
}

static void ultrasonic_trigger_task(void *arg)
{
    (void)arg;
    int64_t last_capture_us = 0;
    const int64_t min_interval_us = (int64_t)CATFLAPCAM_ULTRASONIC_MIN_INTERVAL_MS * 1000;

    while (1) {
        if (!s_web_cam || s_ultrasonic_source_index < 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        float distance_cm = 0;
        esp_err_t err = ultrasonic_measure_distance_cm(&distance_cm);
        if (err == ESP_OK && distance_cm > 0 && distance_cm <= CATFLAPCAM_ULTRASONIC_DISTANCE_CM) {
            int64_t now_us = esp_timer_get_time();
            if ((now_us - last_capture_us) >= min_interval_us) {
                err = catflapcam_webcam_capture_snapshot(&s_web_cam->video[s_ultrasonic_source_index]);
                if (err == ESP_OK) {
                    last_capture_us = now_us;
                    ESP_LOGI(TAG, "ultrasonic trigger: captured/saved snapshot from source=%d at distance=%.1f cm",
                             s_ultrasonic_source_index, distance_cm);
                } else {
                    ESP_LOGW(TAG, "ultrasonic trigger capture failed: %s", esp_err_to_name(err));
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
#endif

esp_err_t catflapcam_ultrasonic_start(catflapcam_webcam_t *web_cam)
{
#if CATFLAPCAM_ULTRASONIC_TRIGGER_ENABLE
    s_web_cam = web_cam;
    s_ultrasonic_source_index = select_ultrasonic_source_index(s_web_cam);
    if (s_ultrasonic_source_index < 0) {
        ESP_LOGW(TAG, "ultrasonic trigger enabled but no valid camera source found");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ultrasonic_init_gpio(), TAG, "failed to init ultrasonic gpio");
    ESP_LOGI(TAG, "ultrasonic trigger enabled: trig_gpio=%d echo_gpio=%d threshold=%dcm source=%d interval=%dms",
             CATFLAPCAM_ULTRASONIC_TRIG_GPIO, CATFLAPCAM_ULTRASONIC_ECHO_GPIO, CATFLAPCAM_ULTRASONIC_DISTANCE_CM,
             s_ultrasonic_source_index, CATFLAPCAM_ULTRASONIC_MIN_INTERVAL_MS);

    ESP_RETURN_ON_FALSE(xTaskCreate(ultrasonic_trigger_task, "ultra_trigger", 4096, NULL, 5, NULL) == pdPASS,
                        ESP_FAIL, TAG, "failed to create ultrasonic trigger task");
#else
    (void)web_cam;
#endif
    return ESP_OK;
}
