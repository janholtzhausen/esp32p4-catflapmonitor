#ifndef MAIN_H
#define MAIN_H

#include <inttypes.h>
#include <stdint.h>

#define CATFLAPCAM_CAMERA_VIDEO_BUFFER_NUMBER  CONFIG_CATFLAPCAM_CAMERA_VIDEO_BUFFER_NUMBER

#define CATFLAPCAM_JPEG_ENC_QUALITY            CONFIG_CATFLAPCAM_JPEG_COMPRESSION_QUALITY

#define CATFLAPCAM_MDNS_INSTANCE               CONFIG_CATFLAPCAM_MDNS_INSTANCE
#define CATFLAPCAM_MDNS_HOST_NAME              CONFIG_CATFLAPCAM_MDNS_HOST_NAME

#define CATFLAPCAM_PART_BOUNDARY               CONFIG_CATFLAPCAM_HTTP_PART_BOUNDARY
#define CATFLAPCAM_ULTRASONIC_TRIGGER_ENABLE   CONFIG_CATFLAPCAM_ULTRASONIC_TRIGGER_ENABLE
#define CATFLAPCAM_ULTRASONIC_TRIG_GPIO        CONFIG_CATFLAPCAM_ULTRASONIC_TRIG_GPIO
#define CATFLAPCAM_ULTRASONIC_ECHO_GPIO        CONFIG_CATFLAPCAM_ULTRASONIC_ECHO_GPIO
#define CATFLAPCAM_ULTRASONIC_DISTANCE_CM      CONFIG_CATFLAPCAM_ULTRASONIC_TRIGGER_DISTANCE_CM
#define CATFLAPCAM_ULTRASONIC_MIN_INTERVAL_MS  CONFIG_CATFLAPCAM_ULTRASONIC_MIN_INTERVAL_MS
#define CATFLAPCAM_ULTRASONIC_SOURCE_INDEX     CONFIG_CATFLAPCAM_ULTRASONIC_SOURCE_INDEX
#define CATFLAPCAM_HTTP_MAX_BODY_SIZE          2048
#define CATFLAPCAM_STREAM_ENC_WAIT_MS          20
#define CATFLAPCAM_CAPTURE_ENC_WAIT_MS         300
#define CATFLAPCAM_STREAM_IO_WAIT_MS           2
#define CATFLAPCAM_CAPTURE_IO_WAIT_MS          200
#define CATFLAPCAM_STREAM_SERVER_STACK_SIZE    (1024 * 7)
#define CATFLAPCAM_STREAM_FRAME_INTERVAL_MS    50
#define CATFLAPCAM_HTTP_SEND_TIMEOUT_S         4

#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=" CATFLAPCAM_PART_BOUNDARY
#define STREAM_BOUNDARY "\r\n--" CATFLAPCAM_PART_BOUNDARY "\r\n"
#define STREAM_PART "Content-Type: image/jpeg\r\nContent-Length: %" PRIu32 "\r\nX-Timestamp: %ld.%09ld\r\n\r\n"

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const uint8_t loading_jpg_gz_start[] asm("_binary_loading_jpg_gz_start");
extern const uint8_t loading_jpg_gz_end[] asm("_binary_loading_jpg_gz_end");
extern const uint8_t favicon_ico_gz_start[] asm("_binary_favicon_ico_gz_start");
extern const uint8_t favicon_ico_gz_end[] asm("_binary_favicon_ico_gz_end");
extern const uint8_t assets_index_js_gz_start[] asm("_binary_index_js_gz_start");
extern const uint8_t assets_index_js_gz_end[] asm("_binary_index_js_gz_end");
extern const uint8_t assets_index_css_gz_start[] asm("_binary_index_css_gz_start");
extern const uint8_t assets_index_css_gz_end[] asm("_binary_index_css_gz_end");


#define TAG "catflapcam"

#endif

