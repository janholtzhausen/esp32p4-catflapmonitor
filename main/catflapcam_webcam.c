/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "catflapcam_config.h"
#include "catflapcam_storage.h"
#include "catflapcam_webcam.h"

bool catflapcam_webcam_is_valid_video(catflapcam_webcam_video_t *video)
{
    return video && video->fd != -1;
}

static esp_err_t ensure_buffer_capacity(uint8_t **buf, uint32_t *buf_size, uint32_t required_size)
{
    if (*buf_size >= required_size) {
        return ESP_OK;
    }

    uint8_t *new_buf = realloc(*buf, required_size);
    ESP_RETURN_ON_FALSE(new_buf, ESP_ERR_NO_MEM, TAG, "failed to allocate %" PRIu32 " bytes", required_size);
    *buf = new_buf;
    *buf_size = required_size;
    return ESP_OK;
}

static int bytes_per_pixel(uint32_t pixel_format)
{
    switch (pixel_format) {
    case V4L2_PIX_FMT_GREY:
    case V4L2_PIX_FMT_SBGGR8:
        return 1;
    case V4L2_PIX_FMT_RGB565:
    case V4L2_PIX_FMT_YUV422P:
        return 2;
    case V4L2_PIX_FMT_RGB24:
        return 3;
    default:
        return 0;
    }
}

static void resize_nearest_gray(const uint8_t *src, int src_w, int src_h, uint8_t *dst, int dst_w, int dst_h)
{
    for (int y = 0; y < dst_h; y++) {
        int sy = (y * src_h) / dst_h;
        const uint8_t *src_row = src + (sy * src_w);
        uint8_t *dst_row = dst + (y * dst_w);
        for (int x = 0; x < dst_w; x++) {
            int sx = (x * src_w) / dst_w;
            dst_row[x] = src_row[sx];
        }
    }
}

static void resize_nearest_rgb565(const uint8_t *src, int src_w, int src_h, uint8_t *dst, int dst_w, int dst_h)
{
    const uint16_t *src16 = (const uint16_t *)src;
    uint16_t *dst16 = (uint16_t *)dst;

    for (int y = 0; y < dst_h; y++) {
        int sy = (y * src_h) / dst_h;
        const uint16_t *src_row = src16 + (sy * src_w);
        uint16_t *dst_row = dst16 + (y * dst_w);
        for (int x = 0; x < dst_w; x++) {
            int sx = (x * src_w) / dst_w;
            dst_row[x] = src_row[sx];
        }
    }
}

static void resize_nearest_rgb24(const uint8_t *src, int src_w, int src_h, uint8_t *dst, int dst_w, int dst_h)
{
    for (int y = 0; y < dst_h; y++) {
        int sy = (y * src_h) / dst_h;
        const uint8_t *src_row = src + (sy * src_w * 3);
        uint8_t *dst_row = dst + (y * dst_w * 3);
        for (int x = 0; x < dst_w; x++) {
            int sx = (x * src_w) / dst_w;
            const uint8_t *src_px = src_row + (sx * 3);
            uint8_t *dst_px = dst_row + (x * 3);
            dst_px[0] = src_px[0];
            dst_px[1] = src_px[1];
            dst_px[2] = src_px[2];
        }
    }
}

static void resize_nearest_yuyv(const uint8_t *src, int src_w, int src_h, uint8_t *dst, int dst_w, int dst_h)
{
    for (int y = 0; y < dst_h; y++) {
        int sy = (y * src_h) / dst_h;
        for (int x = 0; x < dst_w; x += 2) {
            int sx0 = (x * src_w) / dst_w;
            int sx1 = ((x + 1) * src_w) / dst_w;
            int sx_even = sx0 & ~1;
            int src_pair_offset = (sy * src_w + sx_even) * 2;

            uint8_t y0 = src[src_pair_offset + 0];
            uint8_t u = src[src_pair_offset + 1];
            uint8_t y1 = src[src_pair_offset + 2];
            uint8_t v = src[src_pair_offset + 3];

            if (sx0 != sx_even) {
                y0 = y1;
            }
            if (sx1 == sx_even) {
                y1 = src[src_pair_offset + 0];
            }

            int dst_pair_offset = (y * dst_w + x) * 2;
            dst[dst_pair_offset + 0] = y0;
            dst[dst_pair_offset + 1] = u;
            dst[dst_pair_offset + 2] = y1;
            dst[dst_pair_offset + 3] = v;
        }
    }
}

static esp_err_t resize_frame_for_snapshot(catflapcam_webcam_video_t *video, const uint8_t *src, uint32_t src_size, uint8_t **out_buf, uint32_t *out_size)
{
    int bpp = bytes_per_pixel(video->pixel_format);
    ESP_RETURN_ON_FALSE(bpp > 0, ESP_ERR_NOT_SUPPORTED, TAG, "pixel format not supported for snapshot resize");

    uint32_t expected_src_size = video->width * video->height * bpp;
    ESP_RETURN_ON_FALSE(src_size >= expected_src_size, ESP_ERR_INVALID_SIZE, TAG,
                        "source frame too small (%" PRIu32 " < %" PRIu32 ")", src_size, expected_src_size);

    uint32_t dst_size = CATFLAPCAM_SNAPSHOT_WIDTH * CATFLAPCAM_SNAPSHOT_HEIGHT * bpp;
    ESP_RETURN_ON_ERROR(ensure_buffer_capacity(&video->snapshot_resize_buf, &video->snapshot_resize_buf_size, dst_size),
                        TAG, "failed to alloc resize buffer");

    switch (video->pixel_format) {
    case V4L2_PIX_FMT_GREY:
    case V4L2_PIX_FMT_SBGGR8:
        resize_nearest_gray(src, video->width, video->height, video->snapshot_resize_buf,
                            CATFLAPCAM_SNAPSHOT_WIDTH, CATFLAPCAM_SNAPSHOT_HEIGHT);
        break;
    case V4L2_PIX_FMT_RGB565:
        resize_nearest_rgb565(src, video->width, video->height, video->snapshot_resize_buf,
                              CATFLAPCAM_SNAPSHOT_WIDTH, CATFLAPCAM_SNAPSHOT_HEIGHT);
        break;
    case V4L2_PIX_FMT_RGB24:
        resize_nearest_rgb24(src, video->width, video->height, video->snapshot_resize_buf,
                             CATFLAPCAM_SNAPSHOT_WIDTH, CATFLAPCAM_SNAPSHOT_HEIGHT);
        break;
    case V4L2_PIX_FMT_YUV422P:
        resize_nearest_yuyv(src, video->width, video->height, video->snapshot_resize_buf,
                            CATFLAPCAM_SNAPSHOT_WIDTH, CATFLAPCAM_SNAPSHOT_HEIGHT);
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    *out_buf = video->snapshot_resize_buf;
    *out_size = dst_size;
    return ESP_OK;
}

static void release_video_buffers(catflapcam_webcam_video_t *video)
{
    if (!video) {
        return;
    }
    for (int i = 0; i < CATFLAPCAM_CAMERA_VIDEO_BUFFER_NUMBER; i++) {
        if (video->buffer[i] && video->buffer[i] != MAP_FAILED && video->buffer_len[i] > 0) {
            munmap(video->buffer[i], video->buffer_len[i]);
        }
        video->buffer[i] = NULL;
        video->buffer_len[i] = 0;
    }
    video->buffer_size = 0;
}

char *catflapcam_webcam_get_cameras_json(catflapcam_webcam_t *web_cam)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *cameras = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "cameras", cameras);

    for (int i = 0; i < web_cam->video_count; i++) {
        char src_str[32];

        if (!catflapcam_webcam_is_valid_video(&web_cam->video[i])) {
            continue;
        }

        cJSON *camera = cJSON_CreateObject();
        cJSON_AddNumberToObject(camera, "index", i);
        assert(snprintf(src_str, sizeof(src_str), ":%d/stream", i + 81) > 0);
        cJSON_AddStringToObject(camera, "src", src_str);
        cJSON_AddNumberToObject(camera, "currentFrameRate", web_cam->video[i].frame_rate);
        cJSON_AddNumberToObject(camera, "currentImageFormat", 0);
        assert(snprintf(src_str, sizeof(src_str), "JPEG %" PRIu32 "x%" PRIu32, web_cam->video[i].width, web_cam->video[i].height) > 0);
        cJSON_AddStringToObject(camera, "currentImageFormatDescription", src_str);

        if (web_cam->video[i].support_control_jpeg_quality) {
            cJSON_AddNumberToObject(camera, "currentQuality", web_cam->video[i].jpeg_quality);
        }

        cJSON *current_resolution = cJSON_CreateObject();
        cJSON_AddNumberToObject(current_resolution, "width", web_cam->video[i].width);
        cJSON_AddNumberToObject(current_resolution, "height", web_cam->video[i].height);
        cJSON_AddItemToObject(camera, "currentResolution", current_resolution);

        cJSON *image_formats = cJSON_CreateArray();
        cJSON *image_format = cJSON_CreateObject();
        cJSON_AddNumberToObject(image_format, "id", 0);
        assert(snprintf(src_str, sizeof(src_str), "JPEG %" PRIu32 "x%" PRIu32, web_cam->video[i].width, web_cam->video[i].height) > 0);
        cJSON_AddStringToObject(image_format, "description", src_str);

        if (web_cam->video[i].support_control_jpeg_quality) {
            cJSON *image_format_quality = cJSON_CreateObject();
            int min_quality = 1;
            int max_quality = 100;
            int step_quality = 1;
            int default_quality = CATFLAPCAM_JPEG_ENC_QUALITY;

            if (web_cam->video[i].pixel_format == V4L2_PIX_FMT_JPEG) {
                struct v4l2_query_ext_ctrl qctrl = {0};
                qctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
                if (ioctl(web_cam->video[i].fd, VIDIOC_QUERY_EXT_CTRL, &qctrl) == 0) {
                    min_quality = qctrl.minimum;
                    max_quality = qctrl.maximum;
                    step_quality = qctrl.step;
                    default_quality = qctrl.default_value;
                }
            }

            cJSON_AddNumberToObject(image_format_quality, "min", min_quality);
            cJSON_AddNumberToObject(image_format_quality, "max", max_quality);
            cJSON_AddNumberToObject(image_format_quality, "step", step_quality);
            cJSON_AddNumberToObject(image_format_quality, "default", default_quality);
            cJSON_AddItemToObject(image_format, "quality", image_format_quality);
        }
        cJSON_AddItemToArray(image_formats, image_format);
        cJSON_AddItemToObject(camera, "imageFormats", image_formats);
        cJSON_AddItemToArray(cameras, camera);
    }

    char *output = cJSON_Print(root);
    cJSON_Delete(root);
    return output;
}

esp_err_t catflapcam_webcam_set_camera_jpeg_quality(catflapcam_webcam_video_t *video, int quality)
{
    esp_err_t ret = ESP_OK;
    int quality_reset = quality;

    if (video->pixel_format == V4L2_PIX_FMT_JPEG) {
        struct v4l2_ext_controls controls = {0};
        struct v4l2_ext_control control[1];
        struct v4l2_query_ext_ctrl qctrl = {0};

        qctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
        if (ioctl(video->fd, VIDIOC_QUERY_EXT_CTRL, &qctrl) == 0) {
            if ((quality > qctrl.maximum) || (quality < qctrl.minimum) ||
                (((quality - qctrl.minimum) % qctrl.step) != 0)) {
                if (quality > qctrl.maximum) {
                    quality_reset = qctrl.maximum;
                } else if (quality < qctrl.minimum) {
                    quality_reset = qctrl.minimum;
                } else {
                    quality_reset = qctrl.minimum + ((quality - qctrl.minimum) / qctrl.step) * qctrl.step;
                }
                ESP_LOGW(TAG, "video%d: JPEG compression quality=%d is out of sensor's range, reset to %d", video->index, quality, quality_reset);
            }

            controls.ctrl_class = V4L2_CID_JPEG_CLASS;
            controls.count = 1;
            controls.controls = control;
            control[0].id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
            control[0].value = quality_reset;
            ESP_RETURN_ON_ERROR(ioctl(video->fd, VIDIOC_S_EXT_CTRLS, &controls), TAG, "failed to set jpeg compression quality");
            video->jpeg_quality = quality_reset;
            video->support_control_jpeg_quality = 1;
        } else {
            video->support_control_jpeg_quality = 0;
            ESP_LOGW(TAG, "video%d: JPEG compression quality control is not supported", video->index);
        }
    } else {
        ESP_RETURN_ON_ERROR(catflapcam_encoder_set_jpeg_quality(video->encoder_handle, quality_reset), TAG, "failed to set jpeg quality");
        video->jpeg_quality = quality_reset;
    }

    if (video->support_control_jpeg_quality) {
        ESP_LOGI(TAG, "video%d: set jpeg quality %d success", video->index, quality_reset);
    }

    return ret;
}

esp_err_t catflapcam_webcam_capture_snapshot(catflapcam_webcam_video_t *video)
{
    esp_err_t ret = ESP_OK;
    esp_err_t storage_err = ESP_OK;
    struct v4l2_buffer buf;
    uint32_t jpeg_encoded_size = 0;
    const uint8_t *jpeg_src = NULL;
    int64_t t0_us = esp_timer_get_time();
    int64_t t_capture_done_us = 0;
    int64_t t_encode_done_us = 0;
    int64_t t_save_done_us = 0;
    bool enc_locked = false;

    video->capture_priority_active = true;
    ESP_RETURN_ON_FALSE(catflapcam_storage_is_ready(), ESP_ERR_INVALID_STATE, TAG, "SD snapshot storage not ready");
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    ESP_GOTO_ON_FALSE(xSemaphoreTake(video->io_mutex, pdMS_TO_TICKS(CATFLAPCAM_CAPTURE_IO_WAIT_MS)) == pdPASS,
                      ESP_ERR_TIMEOUT, out, TAG, "failed to take camera io mutex");
    ESP_GOTO_ON_ERROR(ioctl(video->fd, VIDIOC_DQBUF, &buf), out_unlock_io, TAG, "failed to receive video frame");
    if (!(buf.flags & V4L2_BUF_FLAG_DONE)) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto out_qbuf;
    }
    t_capture_done_us = esp_timer_get_time();

    if (video->pixel_format == V4L2_PIX_FMT_JPEG) {
        jpeg_src = (const uint8_t *)video->buffer[buf.index];
        jpeg_encoded_size = buf.bytesused;
        if (video->width != CATFLAPCAM_SNAPSHOT_WIDTH || video->height != CATFLAPCAM_SNAPSHOT_HEIGHT) {
            ESP_LOGW(TAG, "snapshot resize unavailable for JPEG source (%" PRIu32 "x%" PRIu32 "); storing original frame",
                     video->width, video->height);
        }
    } else {
        uint8_t *resize_src = NULL;
        uint32_t resize_src_size = 0;

        ESP_GOTO_ON_ERROR(resize_frame_for_snapshot(video, (const uint8_t *)video->buffer[buf.index], video->buffer_size,
                                                    &resize_src, &resize_src_size),
                          out_qbuf, TAG, "failed to resize frame for snapshot");
        ESP_GOTO_ON_FALSE(xSemaphoreTake(video->sem, pdMS_TO_TICKS(CATFLAPCAM_CAPTURE_ENC_WAIT_MS)) == pdPASS,
                          ESP_ERR_TIMEOUT, out_qbuf, TAG, "failed to take semaphore");
        enc_locked = true;

        ret = catflapcam_encoder_process(video->snapshot_encoder_handle, resize_src, resize_src_size,
                                         video->snapshot_out_buf, video->snapshot_out_size, &jpeg_encoded_size);
        xSemaphoreGive(video->sem);
        enc_locked = false;
        ESP_GOTO_ON_ERROR(ret, out_qbuf, TAG, "failed to encode video frame");
        jpeg_src = (const uint8_t *)video->snapshot_out_buf;
    }
    t_encode_done_us = esp_timer_get_time();

    ESP_GOTO_ON_FALSE(jpeg_src && jpeg_encoded_size > 0, ESP_ERR_INVALID_SIZE, out_qbuf, TAG, "invalid jpeg data");
    storage_err = catflapcam_storage_save_snapshot(jpeg_src, jpeg_encoded_size);
    if (storage_err != ESP_OK) {
        ESP_LOGW(TAG, "failed to save snapshot to SD: %s", esp_err_to_name(storage_err));
    }
    t_save_done_us = esp_timer_get_time();

    ESP_GOTO_ON_ERROR(ioctl(video->fd, VIDIOC_QBUF, &buf), out_unlock_io, TAG, "failed to queue frame buffer back");
    memset(&buf, 0, sizeof(buf));
    xSemaphoreGive(video->io_mutex);
    video->capture_priority_active = false;
    ESP_LOGI(TAG,
             "snapshot saved bytes=%" PRIu32 " capture=%" PRIi64 "ms encode=%" PRIi64 "ms save=%" PRIi64 "ms total=%" PRIi64 "ms",
             jpeg_encoded_size, (t_capture_done_us - t0_us) / 1000, (t_encode_done_us - t_capture_done_us) / 1000,
             (t_save_done_us - t_encode_done_us) / 1000, (t_save_done_us - t0_us) / 1000);
    return storage_err;

out_qbuf:
    if (enc_locked) {
        xSemaphoreGive(video->sem);
    }
    if (ioctl(video->fd, VIDIOC_QBUF, &buf) != ESP_OK) {
        ESP_LOGW(TAG, "failed to queue frame buffer back");
    }
out_unlock_io:
    xSemaphoreGive(video->io_mutex);
out:
    video->capture_priority_active = false;
    memset(&buf, 0, sizeof(buf));
    return ret;
}

static esp_err_t init_web_cam_video(catflapcam_webcam_video_t *video, const catflapcam_webcam_video_config_t *config)
{
    int fd;
    int ret;
    struct v4l2_streamparm sparm;
    struct v4l2_requestbuffers req;
    struct v4l2_format format;
    struct v4l2_captureparm *cparam = &sparm.parm.capture;
    struct v4l2_fract *timeperframe = &cparam->timeperframe;

    fd = open(config->dev_name, O_RDWR);
    ESP_RETURN_ON_FALSE(fd >= 0, ESP_ERR_NOT_FOUND, TAG, "Open video device %s failed", config->dev_name);

    memset(&sparm, 0, sizeof(sparm));
    sparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_GOTO_ON_ERROR(ioctl(fd, VIDIOC_G_PARM, &sparm), fail0, TAG, "failed to get frame rate from %s", config->dev_name);
    ESP_GOTO_ON_FALSE(timeperframe->numerator != 0, ESP_ERR_INVALID_RESPONSE, fail0, TAG, "invalid frame-rate numerator from %s", config->dev_name);
    video->frame_rate = timeperframe->denominator / timeperframe->numerator;

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_GOTO_ON_ERROR(ioctl(fd, VIDIOC_G_FMT, &format), fail0, TAG, "Failed get fmt from %s", config->dev_name);

    memset(&req, 0, sizeof(req));
    req.count = CATFLAPCAM_CAMERA_VIDEO_BUFFER_NUMBER;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_GOTO_ON_ERROR(ioctl(fd, VIDIOC_REQBUFS, &req), fail0, TAG, "failed to req buffers from %s", config->dev_name);

    for (int i = 0; i < CATFLAPCAM_CAMERA_VIDEO_BUFFER_NUMBER; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        ESP_GOTO_ON_ERROR(ioctl(fd, VIDIOC_QUERYBUF, &buf), fail0, TAG, "failed to query vbuf from %s", config->dev_name);

        video->buffer[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        ESP_GOTO_ON_FALSE(video->buffer[i] != MAP_FAILED, ESP_ERR_NO_MEM, fail0, TAG, "failed to mmap buffer");
        video->buffer_len[i] = buf.length;
        video->buffer_size = buf.length;

        ESP_GOTO_ON_ERROR(ioctl(fd, VIDIOC_QBUF, &buf), fail0, TAG, "failed to queue frame vbuf from %s", config->dev_name);
    }

    video->fd = fd;
    video->width = format.fmt.pix.width;
    video->height = format.fmt.pix.height;
    video->pixel_format = format.fmt.pix.pixelformat;
    video->jpeg_quality = CATFLAPCAM_JPEG_ENC_QUALITY;

    if (video->pixel_format == V4L2_PIX_FMT_JPEG) {
        ESP_GOTO_ON_ERROR(catflapcam_webcam_set_camera_jpeg_quality(video, CATFLAPCAM_JPEG_ENC_QUALITY), fail0, TAG, "failed to set jpeg quality");
    } else {
        catflapcam_encoder_config_t encoder_config = {0};
        catflapcam_encoder_config_t snapshot_encoder_config = {0};
        encoder_config.width = video->width;
        encoder_config.height = video->height;
        encoder_config.pixel_format = video->pixel_format;
        encoder_config.quality = CATFLAPCAM_JPEG_ENC_QUALITY;
        ESP_GOTO_ON_ERROR(catflapcam_encoder_init(&encoder_config, &video->encoder_handle), fail0, TAG, "failed to init encoder");

        ESP_GOTO_ON_ERROR(catflapcam_encoder_alloc_output_buffer(video->encoder_handle, &video->jpeg_out_buf, &video->jpeg_out_size),
                          fail1, TAG, "failed to alloc jpeg output buf");
        ESP_GOTO_ON_ERROR(catflapcam_encoder_alloc_output_buffer(video->encoder_handle, &video->stream_out_buf, &video->stream_out_size),
                          fail2, TAG, "failed to alloc stream output buf");

        snapshot_encoder_config.width = CATFLAPCAM_SNAPSHOT_WIDTH;
        snapshot_encoder_config.height = CATFLAPCAM_SNAPSHOT_HEIGHT;
        snapshot_encoder_config.pixel_format = video->pixel_format;
        snapshot_encoder_config.quality = CATFLAPCAM_SNAPSHOT_JPEG_QUALITY;
        ESP_GOTO_ON_ERROR(catflapcam_encoder_init(&snapshot_encoder_config, &video->snapshot_encoder_handle),
                          fail2, TAG, "failed to init snapshot encoder");
        ESP_GOTO_ON_ERROR(catflapcam_encoder_alloc_output_buffer(video->snapshot_encoder_handle, &video->snapshot_out_buf, &video->snapshot_out_size),
                          fail2, TAG, "failed to alloc snapshot output buf");
        video->support_control_jpeg_quality = 1;
    }

    video->sem = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(video->sem, ESP_ERR_NO_MEM, fail2, TAG, "failed to create semaphore");
    xSemaphoreGive(video->sem);
    video->io_mutex = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(video->io_mutex, ESP_ERR_NO_MEM, fail2, TAG, "failed to create camera io mutex");
    return ESP_OK;

fail2:
    if (video->io_mutex) {
        vSemaphoreDelete(video->io_mutex);
        video->io_mutex = NULL;
    }
    if (video->stream_out_buf) {
        if (video->pixel_format != V4L2_PIX_FMT_JPEG && video->encoder_handle) {
            catflapcam_encoder_free_output_buffer(video->encoder_handle, video->stream_out_buf);
        } else {
            free(video->stream_out_buf);
        }
        video->stream_out_buf = NULL;
        video->stream_out_size = 0;
    }
    if (video->snapshot_resize_buf) {
        free(video->snapshot_resize_buf);
        video->snapshot_resize_buf = NULL;
        video->snapshot_resize_buf_size = 0;
    }
    if (video->snapshot_out_buf) {
        if (video->snapshot_encoder_handle) {
            catflapcam_encoder_free_output_buffer(video->snapshot_encoder_handle, video->snapshot_out_buf);
        } else {
            free(video->snapshot_out_buf);
        }
        video->snapshot_out_buf = NULL;
        video->snapshot_out_size = 0;
    }
    if (video->pixel_format != V4L2_PIX_FMT_JPEG) {
        catflapcam_encoder_free_output_buffer(video->encoder_handle, video->jpeg_out_buf);
        video->jpeg_out_buf = NULL;
    }
fail1:
    if (video->snapshot_encoder_handle) {
        catflapcam_encoder_deinit(video->snapshot_encoder_handle);
        video->snapshot_encoder_handle = NULL;
    }
    if (video->pixel_format != V4L2_PIX_FMT_JPEG) {
        catflapcam_encoder_deinit(video->encoder_handle);
        video->encoder_handle = NULL;
    }
fail0:
    release_video_buffers(video);
    close(fd);
    video->fd = -1;
    return ret;
}

static esp_err_t deinit_web_cam_video(catflapcam_webcam_video_t *video)
{
    if (video->sem) {
        vSemaphoreDelete(video->sem);
        video->sem = NULL;
    }
    if (video->io_mutex) {
        vSemaphoreDelete(video->io_mutex);
        video->io_mutex = NULL;
    }

    if (video->snapshot_resize_buf) {
        free(video->snapshot_resize_buf);
        video->snapshot_resize_buf = NULL;
        video->snapshot_resize_buf_size = 0;
    }
    if (video->snapshot_out_buf) {
        if (video->snapshot_encoder_handle) {
            catflapcam_encoder_free_output_buffer(video->snapshot_encoder_handle, video->snapshot_out_buf);
        } else {
            free(video->snapshot_out_buf);
        }
        video->snapshot_out_buf = NULL;
        video->snapshot_out_size = 0;
    }
    if (video->stream_out_buf) {
        if (video->pixel_format != V4L2_PIX_FMT_JPEG && video->encoder_handle) {
            catflapcam_encoder_free_output_buffer(video->encoder_handle, video->stream_out_buf);
        } else {
            free(video->stream_out_buf);
        }
        video->stream_out_buf = NULL;
        video->stream_out_size = 0;
    }

    if (video->snapshot_encoder_handle) {
        catflapcam_encoder_deinit(video->snapshot_encoder_handle);
        video->snapshot_encoder_handle = NULL;
    }
    if (video->pixel_format != V4L2_PIX_FMT_JPEG) {
        catflapcam_encoder_free_output_buffer(video->encoder_handle, video->jpeg_out_buf);
        catflapcam_encoder_deinit(video->encoder_handle);
    }

    release_video_buffers(video);
    close(video->fd);
    video->fd = -1;
    return ESP_OK;
}

esp_err_t catflapcam_webcam_new(const catflapcam_webcam_video_config_t *config, int config_count, catflapcam_webcam_t **ret_wc)
{
    int i;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    esp_err_t ret = ESP_FAIL;
    catflapcam_webcam_t *wc = calloc(1, sizeof(catflapcam_webcam_t) + config_count * sizeof(catflapcam_webcam_video_t));

    ESP_RETURN_ON_FALSE(wc, ESP_ERR_NO_MEM, TAG, "failed to alloc web cam");
    wc->video_count = config_count;

    int valid_videos = 0;
    for (i = 0; i < config_count; i++) {
        wc->video[i].index = i;
        wc->video[i].fd = -1;

        ret = init_web_cam_video(&wc->video[i], &config[i]);
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "failed to find web_cam %d", i);
            continue;
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "failed to initialize web_cam %d", i);
            goto fail0;
        }
        valid_videos++;

        ESP_LOGI(TAG, "video%d: width=%" PRIu32 " height=%" PRIu32 " format=" V4L2_FMT_STR, i, wc->video[i].width,
                 wc->video[i].height, V4L2_FMT_STR_ARG(wc->video[i].pixel_format));
    }

    ESP_GOTO_ON_FALSE(valid_videos > 0, ESP_ERR_NOT_FOUND, fail0, TAG, "no valid camera source found");

    for (i = 0; i < config_count; i++) {
        if (catflapcam_webcam_is_valid_video(&wc->video[i])) {
            ESP_GOTO_ON_ERROR(ioctl(wc->video[i].fd, VIDIOC_STREAMON, &type), fail1, TAG, "failed to start stream");
        }
    }

    *ret_wc = wc;
    return ESP_OK;

fail1:
    for (int j = i - 1; j >= 0; j--) {
        if (catflapcam_webcam_is_valid_video(&wc->video[j])) {
            ioctl(wc->video[j].fd, VIDIOC_STREAMOFF, &type);
        }
    }
    i = config_count;
fail0:
    for (int j = i - 1; j >= 0; j--) {
        if (catflapcam_webcam_is_valid_video(&wc->video[j])) {
            deinit_web_cam_video(&wc->video[j]);
        }
    }
    free(wc);
    return ret;
}

void catflapcam_webcam_free(catflapcam_webcam_t *web_cam)
{
    if (!web_cam) {
        return;
    }

    for (int i = 0; i < web_cam->video_count; i++) {
        if (catflapcam_webcam_is_valid_video(&web_cam->video[i])) {
            deinit_web_cam_video(&web_cam->video[i]);
        }
    }
    free(web_cam);
}
