#include "uploader.h"

#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "uploader";

esp_err_t post_jpeg_bytes(const uint8_t *jpg, uint32_t jpg_len)
{
    if (!jpg || jpg_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t cfg = {
        .url = UPLOADER_URL,  
        .timeout_ms = 3000,
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;

    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "image/jpeg");

    esp_err_t err = esp_http_client_open(c, (int)jpg_len);
    if (err != ESP_OK) { esp_http_client_cleanup(c); return err; }

    int w = esp_http_client_write(c, (const char *)jpg, (int)jpg_len);
    if (w != (int)jpg_len) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }

    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);

    ESP_LOGI(TAG, "POST status=%d sent=%d", status, w);

    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    return (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}