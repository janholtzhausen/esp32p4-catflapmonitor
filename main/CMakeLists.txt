set(srcs
    "main.c"
    "catflapcam_wifi.c"
    "catflapcam_webcam.c"
    "catflapcam_http_server.c"
    "catflapcam_ultrasonic.c"
    "catflapcam_storage.c")
set(html_files "../frontend/gzipped/index.html.gz"
               "../frontend/gzipped/loading.jpg.gz"
               "../frontend/gzipped/favicon.ico.gz"
               "../frontend/gzipped/assets/index.js.gz"
               "../frontend/gzipped/assets/index.css.gz")

idf_component_register(
    SRCS "${srcs}"
    PRIV_INCLUDE_DIRS ./include
    EMBED_TXTFILES ${html_files}
    PRIV_REQUIRES
        esp_driver_gpio
        esp_wifi
        json
        esp_timer
        nvs_flash
        esp_http_server
        mdns
        app_update
        fatfs
        sdmmc
        esp_driver_sdmmc
        catflapcam_video_common
)

