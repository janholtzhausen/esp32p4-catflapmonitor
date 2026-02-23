#ifndef CATFLAPCAM_HTTP_SERVER_H
#define CATFLAPCAM_HTTP_SERVER_H

#include "esp_err.h"
#include "catflapcam_webcam.h"

esp_err_t catflapcam_http_server_start(catflapcam_webcam_t *web_cam);

#endif
