#ifndef CATFLAPCAM_WEBCAM_H
#define CATFLAPCAM_WEBCAM_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "catflapcam_video_common.h"
#include "main.h"

typedef struct catflapcam_webcam_video {
    int fd;
    uint8_t index;

    catflapcam_encoder_handle_t encoder_handle;
    catflapcam_encoder_handle_t snapshot_encoder_handle;
    uint8_t *jpeg_out_buf;
    uint32_t jpeg_out_size;
    uint8_t *stream_out_buf;
    uint32_t stream_out_size;
    uint8_t *snapshot_out_buf;
    uint32_t snapshot_out_size;
    uint8_t *snapshot_resize_buf;
    uint32_t snapshot_resize_buf_size;

    uint8_t *buffer[CATFLAPCAM_CAMERA_VIDEO_BUFFER_NUMBER];
    uint32_t buffer_len[CATFLAPCAM_CAMERA_VIDEO_BUFFER_NUMBER];
    uint32_t buffer_size;

    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint8_t jpeg_quality;

    uint32_t frame_rate;

    SemaphoreHandle_t sem;
    SemaphoreHandle_t io_mutex;
    volatile bool capture_priority_active;
    uint32_t support_control_jpeg_quality : 1;
} catflapcam_webcam_video_t;

typedef struct catflapcam_webcam {
    uint8_t video_count;
    catflapcam_webcam_video_t video[0];
} catflapcam_webcam_t;

typedef struct catflapcam_webcam_video_config {
    const char *dev_name;
} catflapcam_webcam_video_config_t;

bool catflapcam_webcam_is_valid_video(catflapcam_webcam_video_t *video);
char *catflapcam_webcam_get_cameras_json(catflapcam_webcam_t *web_cam);
esp_err_t catflapcam_webcam_set_camera_jpeg_quality(catflapcam_webcam_video_t *video, int quality);
esp_err_t catflapcam_webcam_capture_snapshot(catflapcam_webcam_video_t *video);
esp_err_t catflapcam_webcam_new(const catflapcam_webcam_video_config_t *config, int config_count, catflapcam_webcam_t **ret_wc);
void catflapcam_webcam_free(catflapcam_webcam_t *web_cam);

#endif
