/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "lwip/apps/netbiosns.h"
#include "catflapcam_video_common.h"
#include "catflapcam_http_server.h"
#include "catflapcam_storage.h"
#include "catflapcam_ultrasonic.h"
#include "catflapcam_wifi.h"

static void initialise_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(CATFLAPCAM_MDNS_HOST_NAME));
    ESP_ERROR_CHECK(mdns_instance_name_set(CATFLAPCAM_MDNS_INSTANCE));

    mdns_txt_item_t service_txt_data[] = {
        {"board", CONFIG_IDF_TARGET},
        {"path", "/"},
    };

    ESP_ERROR_CHECK(mdns_service_add("ESP32-WebServer", "_http", "_tcp", 80, service_txt_data,
                                     sizeof(service_txt_data) / sizeof(service_txt_data[0])));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Reset reason: %d", esp_reset_reason());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_err_t net_err = catflapcam_wifi_connect();
    if (net_err == ESP_OK) {
        initialise_mdns();
        netbiosns_init();
        netbiosns_set_name(CATFLAPCAM_MDNS_HOST_NAME);
    } else {
        ESP_LOGE(TAG, "Network init failed: %s", esp_err_to_name(net_err));
        ESP_LOGW(TAG, "Continuing without network. Check main/include/catflapcam_config.h Wi-Fi settings.");
    }
    ESP_ERROR_CHECK(catflapcam_video_init());

    catflapcam_webcam_video_config_t config[] = {
#if CATFLAPCAM_ENABLE_MIPI_CSI_CAM_SENSOR
        {
            .dev_name = ESP_VIDEO_MIPI_CSI_DEVICE_NAME,
        },
#endif
#if CATFLAPCAM_ENABLE_DVP_CAM_SENSOR
        {
            .dev_name = ESP_VIDEO_DVP_DEVICE_NAME,
        },
#endif
#if CATFLAPCAM_ENABLE_SPI_CAM_SENSOR
        {
            .dev_name = ESP_VIDEO_SPI_DEVICE_NAME,
        },
#endif
#if CATFLAPCAM_ENABLE_USB_UVC_CAM_SENSOR
        {
            .dev_name = ESP_VIDEO_USB_UVC_DEVICE_NAME(0),
        },
#endif
    };
    int config_count = sizeof(config) / sizeof(config[0]);
    ESP_ERROR_CHECK_WITHOUT_ABORT(config_count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND);
    if (config_count <= 0) {
        ESP_LOGE(TAG, "No camera interface configured");
        return;
    }

    catflapcam_webcam_t *web_cam = NULL;
    ESP_ERROR_CHECK(catflapcam_webcam_new(config, config_count, &web_cam));
    esp_err_t storage_err = catflapcam_storage_init();
    if (storage_err != ESP_OK) {
        ESP_LOGW(TAG, "SD snapshot storage unavailable: %s", esp_err_to_name(storage_err));
    }
    ESP_ERROR_CHECK(catflapcam_http_server_start(web_cam));
    ESP_ERROR_CHECK(catflapcam_ultrasonic_start(web_cam));

    ESP_LOGI(TAG, "Camera web server starts");
}
