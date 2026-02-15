#pragma once
#include <stdint.h>
#include "esp_err.h"

#define UPLOADER_URL "http://192.168.1.11:5001/catflap/upload"
esp_err_t post_jpeg_bytes(const uint8_t *jpg, uint32_t jpg_len);
