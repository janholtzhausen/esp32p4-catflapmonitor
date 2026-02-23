/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "catflapcam_config.h"
#include "catflapcam_http_server.h"
#include "catflapcam_storage.h"

typedef struct request_desc {
    int index;
} request_desc_t;

static const char *SNAPSHOTS_PAGE_HTML =
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>catflapcam snapshots</title><style>"
    "body{font-family:Arial,sans-serif;background:#f7f7f7;margin:0;padding:12px}"
    ".top{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:10px}"
    ".card{background:#fff;border-radius:8px;padding:8px;box-shadow:0 1px 3px rgba(0,0,0,.15)}"
    ".card img{width:100%;height:auto;border-radius:6px;background:#ddd}.meta{font-size:12px;color:#444;padding-top:6px}"
    ".actions{display:flex;gap:8px;margin-top:8px}.btn{padding:8px 12px;border:none;border-radius:6px;color:#fff;font-weight:600;cursor:pointer}"
    ".btn-refresh{background:#1f6feb}.btn-open{background:#15803d;display:inline-block;text-decoration:none;text-align:center}.btn-del{background:#b91c1c;flex:1}"
    "</style></head><body><div class='top'><h2>Snapshots</h2><button class='btn btn-refresh' onclick='load()'>Refresh</button></div>"
    "<div id='status'>Loading...</div><div id='grid' class='grid'></div>"
    "<script>"
    "async function delSnapshot(name){if(!confirm('Delete '+name+'?')) return;"
    "const r=await fetch('/api/snapshots/'+encodeURIComponent(name),{method:'DELETE'});if(!r.ok){alert('Delete failed');return;}load();}"
    "async function load(){const status=document.getElementById('status');const grid=document.getElementById('grid');"
    "status.textContent='Loading...';grid.innerHTML='';"
    "try{const r=await fetch('/api/snapshots?limit=300',{cache:'no-store'});const j=await r.json();"
    "status.textContent=`${j.snapshots.length} snapshot(s)`;"
    "for(const s of j.snapshots){const c=document.createElement('div');c.className='card';"
    "const a=document.createElement('a');a.href=s.url;a.target='_blank';const i=document.createElement('img');i.src=s.url;"
    "a.appendChild(i);const m=document.createElement('div');m.className='meta';m.textContent=s.name+' ('+s.size+' bytes)';"
    "const act=document.createElement('div');act.className='actions';"
    "const open=document.createElement('a');open.className='btn btn-open';open.href=s.url;open.target='_blank';open.textContent='Open';"
    "const del=document.createElement('button');del.className='btn btn-del';del.textContent='Delete';del.onclick=()=>delSnapshot(s.name);"
    "act.appendChild(open);act.appendChild(del);"
    "c.appendChild(a);c.appendChild(m);c.appendChild(act);grid.appendChild(c);}}catch(e){status.textContent='Failed to load snapshots';}}"
    "load();</script></body></html>";

static bool constant_time_password_equals(const char *expected, const char *provided)
{
    size_t expected_len = strlen(expected);
    size_t provided_len = strlen(provided);
    size_t max_len = (expected_len > provided_len) ? expected_len : provided_len;
    unsigned diff = (unsigned)(expected_len ^ provided_len);

    for (size_t i = 0; i < max_len; i++) {
        unsigned expected_ch = (i < expected_len) ? (unsigned char)expected[i] : 0U;
        unsigned provided_ch = (i < provided_len) ? (unsigned char)provided[i] : 0U;
        diff |= expected_ch ^ provided_ch;
    }

    return diff == 0U;
}

static esp_err_t ota_authorize_request(httpd_req_t *req)
{
    char provided_password[80];

    if (strlen(CATFLAPCAM_OTA_PASSWORD) == 0) {
        ESP_LOGE(TAG, "CATFLAPCAM_OTA_PASSWORD is empty; OTA endpoint disabled");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "OTA password not configured\n");
        return ESP_FAIL;
    }

    if (httpd_req_get_hdr_value_str(req, "X-OTA-Password", provided_password, sizeof(provided_password)) != ESP_OK) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "Unauthorized\n");
        return ESP_FAIL;
    }

    if (!constant_time_password_equals(CATFLAPCAM_OTA_PASSWORD, provided_password)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "Unauthorized\n");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t ota_update_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    char *rx_buf = NULL;

    if (ota_authorize_request(req) != ESP_OK) {
        return ESP_OK;
    }

    ESP_GOTO_ON_FALSE(req->content_len > 0, ESP_ERR_INVALID_SIZE, fail0, TAG, "invalid OTA content length");
    ESP_GOTO_ON_FALSE(update_partition, ESP_ERR_NOT_FOUND, fail0, TAG, "no OTA partition found");
    ESP_GOTO_ON_FALSE(req->content_len <= (int)update_partition->size, ESP_ERR_INVALID_SIZE, fail0, TAG,
                      "OTA image too large (%d > %" PRIu32 ")", req->content_len, update_partition->size);

    rx_buf = (char *)malloc(4096);
    ESP_GOTO_ON_FALSE(rx_buf, ESP_ERR_NO_MEM, fail0, TAG, "failed to allocate OTA rx buffer");

    ESP_LOGI(TAG, "starting OTA update: size=%d target=%s", req->content_len, update_partition->label);
    ESP_GOTO_ON_ERROR(esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle), fail1, TAG, "esp_ota_begin failed");

    int remaining = req->content_len;
    while (remaining > 0) {
        int to_recv = remaining > 4096 ? 4096 : remaining;
        int recv_len = httpd_req_recv(req, rx_buf, to_recv);
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        ESP_GOTO_ON_FALSE(recv_len > 0, ESP_FAIL, fail2, TAG, "failed to receive OTA data");
        ESP_GOTO_ON_ERROR(esp_ota_write(ota_handle, rx_buf, recv_len), fail2, TAG, "esp_ota_write failed");
        remaining -= recv_len;
    }

    ESP_GOTO_ON_ERROR(esp_ota_end(ota_handle), fail1, TAG, "esp_ota_end failed");
    ota_handle = 0;
    ESP_GOTO_ON_ERROR(esp_ota_set_boot_partition(update_partition), fail1, TAG, "esp_ota_set_boot_partition failed");

    free(rx_buf);
    rx_buf = NULL;

    ESP_LOGI(TAG, "OTA image accepted, rebooting");
    httpd_resp_sendstr(req, "OK\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;

fail2:
    esp_ota_abort(ota_handle);
    ota_handle = 0;
fail1:
    if (rx_buf) {
        free(rx_buf);
    }
fail0:
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA failed");
    return ret;
}

static esp_err_t decode_request(catflapcam_webcam_t *web_cam, httpd_req_t *req, request_desc_t *desc)
{
    char query[64];
    char source_value[8];

    ESP_RETURN_ON_ERROR(httpd_req_get_url_query_str(req, query, sizeof(query)), TAG, "failed to get query string");
    ESP_RETURN_ON_ERROR(httpd_query_key_value(query, "source", source_value, sizeof(source_value)), TAG, "missing source query key");

    char *endp = NULL;
    long index = strtol(source_value, &endp, 10);
    if (endp == source_value || *endp != '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (index < 0 || index >= web_cam->video_count) {
        return ESP_ERR_INVALID_ARG;
    }
    desc->index = (int)index;
    return ESP_OK;
}

static esp_err_t camera_info_handler(httpd_req_t *req)
{
    esp_err_t ret;
    catflapcam_webcam_t *web_cam = (catflapcam_webcam_t *)req->user_ctx;
    char *output = catflapcam_webcam_get_cameras_json(web_cam);

    httpd_resp_set_type(req, "application/json");
    ret = httpd_resp_sendstr(req, output);
    free(output);

    return ret;
}

static esp_err_t snapshots_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, SNAPSHOTS_PAGE_HTML);
}

static esp_err_t snapshots_list_handler(httpd_req_t *req)
{
    char query[64];
    char limit_str[16];
    long limit = 200;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "limit", limit_str, sizeof(limit_str)) == ESP_OK) {
        char *endp = NULL;
        long parsed = strtol(limit_str, &endp, 10);
        if (endp != limit_str && *endp == '\0' && parsed > 0) {
            limit = parsed;
        }
    }

    char *json = catflapcam_storage_list_json((size_t)limit);
    ESP_RETURN_ON_FALSE(json, ESP_ERR_NO_MEM, TAG, "failed to build snapshots list json");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

static esp_err_t extract_snapshot_name_from_uri(httpd_req_t *req, const char *prefix, char *name, size_t name_len)
{
    size_t prefix_len = strlen(prefix);
    if (strncmp(req->uri, prefix, prefix_len) != 0 || strlen(req->uri) <= prefix_len) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *name_start = req->uri + prefix_len;
    const char *query = strchr(name_start, '?');
    size_t file_name_len = query ? (size_t)(query - name_start) : strlen(name_start);
    if (file_name_len == 0 || file_name_len >= name_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(name, name_start, file_name_len);
    name[file_name_len] = '\0';
    if (strchr(name, '/')) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t snapshots_delete_handler(httpd_req_t *req)
{
    if (!catflapcam_storage_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "SD storage unavailable\n");
    }

    char name[64];
    if (extract_snapshot_name_from_uri(req, "/api/snapshots/", name, sizeof(name)) != ESP_OK) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    esp_err_t err = catflapcam_storage_delete_snapshot(name);
    if (err == ESP_OK) {
        return httpd_resp_sendstr(req, "OK\n");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
    return ESP_FAIL;
}

static esp_err_t snapshot_file_handler(httpd_req_t *req)
{
    if (!catflapcam_storage_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "SD storage unavailable\n");
        return ESP_FAIL;
    }

    char name[64];
    if (extract_snapshot_name_from_uri(req, "/snapshots/", name, sizeof(name)) != ESP_OK) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char path[128];
    if (catflapcam_storage_resolve_snapshot_path(name, path, sizeof(path)) != ESP_OK) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGW(TAG, "failed to open snapshot '%s': errno=%d", path, errno);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to open snapshot");
        return ESP_FAIL;
    }

    char len_str[24];
    snprintf(len_str, sizeof(len_str), "%lld", (long long)st.st_size);
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Length", len_str);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t ret = ESP_OK;
    char buf[2048];
    while (!feof(fp)) {
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n == 0) {
            break;
        }
        ret = httpd_resp_send_chunk(req, buf, n);
        if (ret != ESP_OK) {
            break;
        }
    }
    fclose(fp);

    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, NULL, 0);
    }
    return ret;
}

static esp_err_t static_file_handler(httpd_req_t *req)
{
    const char *uri = req->uri;

    if (strcmp(uri, "/") == 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)index_html_gz_start, index_html_gz_end - index_html_gz_start);
    }
    if (strcmp(uri, "/loading.jpg") == 0) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)loading_jpg_gz_start, loading_jpg_gz_end - loading_jpg_gz_start);
    }
    if (strcmp(uri, "/favicon.ico") == 0) {
        httpd_resp_set_type(req, "image/x-icon");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)favicon_ico_gz_start, favicon_ico_gz_end - favicon_ico_gz_start);
    }
    if (strcmp(uri, "/assets/index.js") == 0) {
        httpd_resp_set_type(req, "application/javascript");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)assets_index_js_gz_start, assets_index_js_gz_end - assets_index_js_gz_start);
    }
    if (strcmp(uri, "/assets/index.css") == 0) {
        httpd_resp_set_type(req, "text/css");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)assets_index_css_gz_start, assets_index_css_gz_end - assets_index_css_gz_start);
    }

    ESP_LOGW(TAG, "File not found: %s", uri);
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static esp_err_t image_stream_handler(httpd_req_t *req)
{
    esp_err_t ret;
    struct v4l2_buffer buf;
    char http_string[128];
    bool enc_locked = false;
    bool io_locked = false;
    uint32_t dropped_frames = 0;
    TickType_t last_send_tick = 0;
    catflapcam_webcam_video_t *video = (catflapcam_webcam_video_t *)req->user_ctx;

    ESP_RETURN_ON_FALSE(snprintf(http_string, sizeof(http_string), "%" PRIu32, video->frame_rate) > 0, ESP_FAIL, TAG, "failed to format framerate buffer");
    ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, STREAM_CONTENT_TYPE), TAG, "failed to set content type");
    ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"), TAG, "failed to set access control allow origin");
    ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, "X-Framerate", http_string), TAG, "failed to set x framerate");

    while (1) {
        int hlen;
        struct timespec ts;
        uint32_t jpeg_encoded_size;

        enc_locked = false;
        io_locked = false;
        if (video->capture_priority_active) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (xSemaphoreTake(video->io_mutex, pdMS_TO_TICKS(CATFLAPCAM_STREAM_IO_WAIT_MS)) != pdPASS) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        io_locked = true;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        ESP_GOTO_ON_ERROR(ioctl(video->fd, VIDIOC_DQBUF, &buf), fail0, TAG, "failed to receive video frame");
        if (!(buf.flags & V4L2_BUF_FLAG_DONE)) {
            ESP_GOTO_ON_ERROR(ioctl(video->fd, VIDIOC_QBUF, &buf), fail0, TAG, "failed to queue video frame");
            xSemaphoreGive(video->io_mutex);
            io_locked = false;
            continue;
        }

        if (video->pixel_format == V4L2_PIX_FMT_JPEG) {
            jpeg_encoded_size = buf.bytesused;
            ESP_GOTO_ON_FALSE(jpeg_encoded_size > 0, ESP_ERR_INVALID_SIZE, fail0, TAG, "invalid jpeg frame size");
            ESP_GOTO_ON_FALSE(video->stream_out_buf || (video->stream_out_buf = malloc(jpeg_encoded_size)),
                              ESP_ERR_NO_MEM, fail0, TAG, "failed to alloc stream output buffer");
            if (video->stream_out_size < jpeg_encoded_size) {
                uint8_t *new_buf = realloc(video->stream_out_buf, jpeg_encoded_size);
                ESP_GOTO_ON_FALSE(new_buf, ESP_ERR_NO_MEM, fail0, TAG, "failed to realloc stream output buffer");
                video->stream_out_buf = new_buf;
                video->stream_out_size = jpeg_encoded_size;
            }
            memcpy(video->stream_out_buf, video->buffer[buf.index], jpeg_encoded_size);
        } else {
            if (xSemaphoreTake(video->sem, pdMS_TO_TICKS(CATFLAPCAM_STREAM_ENC_WAIT_MS)) != pdPASS) {
                dropped_frames++;
                if ((dropped_frames % 30) == 0) {
                    ESP_LOGW(TAG, "stream source=%d dropped_frames=%" PRIu32 " due to encoder contention", video->index, dropped_frames);
                }
                ESP_GOTO_ON_ERROR(ioctl(video->fd, VIDIOC_QBUF, &buf), fail0, TAG, "failed to queue video frame");
                xSemaphoreGive(video->io_mutex);
                io_locked = false;
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            enc_locked = true;
            ESP_GOTO_ON_FALSE(video->stream_out_buf && video->stream_out_size > 0, ESP_ERR_NO_MEM, fail0, TAG, "stream output buffer not initialized");
            ESP_GOTO_ON_ERROR(catflapcam_encoder_process(video->encoder_handle, video->buffer[buf.index], video->buffer_size,
                              video->stream_out_buf, video->stream_out_size, &jpeg_encoded_size), fail0, TAG, "failed to encode video frame");
        }

        ESP_GOTO_ON_ERROR(ioctl(video->fd, VIDIOC_QBUF, &buf), fail0, TAG, "failed to queue video frame");
        memset(&buf, 0, sizeof(buf));
        if (enc_locked) {
            xSemaphoreGive(video->sem);
            enc_locked = false;
        }
        xSemaphoreGive(video->io_mutex);
        io_locked = false;

        ESP_GOTO_ON_ERROR(httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)), fail0, TAG, "failed to send boundary");
        ESP_GOTO_ON_ERROR(clock_gettime(CLOCK_MONOTONIC, &ts), fail0, TAG, "failed to get time");
        ESP_GOTO_ON_FALSE((hlen = snprintf(http_string, sizeof(http_string), STREAM_PART, jpeg_encoded_size, (long)ts.tv_sec, (long)ts.tv_nsec)) > 0,
                          ESP_FAIL, fail0, TAG, "failed to format part buffer");
        ESP_GOTO_ON_ERROR(httpd_resp_send_chunk(req, http_string, hlen), fail0, TAG, "failed to send boundary");
        ESP_GOTO_ON_ERROR(httpd_resp_send_chunk(req, (char *)video->stream_out_buf, jpeg_encoded_size), fail0, TAG, "failed to send jpeg");

        if (CATFLAPCAM_STREAM_FRAME_INTERVAL_MS > 0) {
            TickType_t now = xTaskGetTickCount();
            if (last_send_tick != 0) {
                TickType_t frame_ticks = pdMS_TO_TICKS(CATFLAPCAM_STREAM_FRAME_INTERVAL_MS);
                TickType_t elapsed = now - last_send_tick;
                if (elapsed < frame_ticks) {
                    vTaskDelay(frame_ticks - elapsed);
                }
            }
            last_send_tick = xTaskGetTickCount();
        }
    }

    return ESP_OK;

fail0:
    if (enc_locked) {
        xSemaphoreGive(video->sem);
    }
    if (io_locked) {
        ioctl(video->fd, VIDIOC_QBUF, &buf);
        xSemaphoreGive(video->io_mutex);
    }
    return ret;
}

static esp_err_t capture_image_handler(httpd_req_t *req)
{
    catflapcam_webcam_t *web_cam = (catflapcam_webcam_t *)req->user_ctx;
    request_desc_t desc;
    ESP_RETURN_ON_ERROR(decode_request(web_cam, req, &desc), TAG, "failed to decode request");

    esp_err_t err = catflapcam_webcam_capture_snapshot(&web_cam->video[desc.index]);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline");
    httpd_resp_set_type(req, "text/plain");

    if (err == ESP_OK) {
        return httpd_resp_send(req, "OK\n", 3);
    }

    httpd_resp_set_status(req, "502 Bad Gateway");
    return httpd_resp_send(req, "FAIL\n", 5);
}

esp_err_t catflapcam_http_server_start(catflapcam_webcam_t *web_cam)
{
    httpd_handle_t stream_httpd = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.send_wait_timeout = CATFLAPCAM_HTTP_SEND_TIMEOUT_S;
    config.recv_wait_timeout = CATFLAPCAM_HTTP_SEND_TIMEOUT_S;
    config.lru_purge_enable = true;

    httpd_uri_t static_file_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = static_file_handler,
        .user_ctx = (void *)web_cam,
    };
    httpd_uri_t capture_image_uri = {
        .uri = "/api/capture_image",
        .method = HTTP_GET,
        .handler = capture_image_handler,
        .user_ctx = (void *)web_cam,
    };
    httpd_uri_t ota_update_uri = {
        .uri = "/api/ota",
        .method = HTTP_POST,
        .handler = ota_update_handler,
        .user_ctx = (void *)web_cam,
    };
    httpd_uri_t camera_info_uri = {
        .uri = "/api/get_camera_info",
        .method = HTTP_GET,
        .handler = camera_info_handler,
        .user_ctx = (void *)web_cam,
    };
    httpd_uri_t snapshots_page_uri = {
        .uri = "/snapshots",
        .method = HTTP_GET,
        .handler = snapshots_page_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t snapshots_list_uri = {
        .uri = "/api/snapshots",
        .method = HTTP_GET,
        .handler = snapshots_list_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t snapshots_delete_uri = {
        .uri = "/api/snapshots/*",
        .method = HTTP_DELETE,
        .handler = snapshots_delete_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t snapshots_file_uri = {
        .uri = "/snapshots/*",
        .method = HTTP_GET,
        .handler = snapshot_file_handler,
        .user_ctx = NULL,
    };
    config.stack_size = CATFLAPCAM_STREAM_SERVER_STACK_SIZE;
    ESP_LOGI(TAG, "Starting stream server on port: '%d'", config.server_port);
    ESP_RETURN_ON_ERROR(httpd_start(&stream_httpd, &config), TAG, "failed to start control http server");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &capture_image_uri), TAG, "failed to register capture handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &ota_update_uri), TAG, "failed to register OTA handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &camera_info_uri), TAG, "failed to register camera info handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &snapshots_page_uri), TAG, "failed to register snapshots page handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &snapshots_list_uri), TAG, "failed to register snapshots list handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &snapshots_delete_uri), TAG, "failed to register snapshots delete handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &snapshots_file_uri), TAG, "failed to register snapshots file handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &static_file_uri), TAG, "failed to register static file handler");

    for (int i = 0; i < web_cam->video_count; i++) {
        if (!catflapcam_webcam_is_valid_video(&web_cam->video[i])) {
            continue;
        }

        httpd_uri_t stream_uri = {
            .uri = "/stream",
            .method = HTTP_GET,
            .handler = image_stream_handler,
            .user_ctx = (void *)&web_cam->video[i],
        };

        config.stack_size = CATFLAPCAM_STREAM_SERVER_STACK_SIZE;
        config.server_port += 1;
        config.ctrl_port += 1;
        ESP_RETURN_ON_ERROR(httpd_start(&stream_httpd, &config), TAG, "failed to start stream http server for source=%d", i);
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &stream_uri), TAG, "failed to register stream handler for source=%d", i);
    }

    return ESP_OK;
}
