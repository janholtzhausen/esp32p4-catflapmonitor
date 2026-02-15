set(srcs "main.c" "uploader.c")
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
        esp_http_client
        json
        esp_timer
        nvs_flash
        esp_http_server
        mdns
        example_video_common
)
