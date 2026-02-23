set(srcs "catflapcam_init_video.c")
set(inc_dirs "include")

if(NOT CONFIG_IDF_TARGET_ESP32C61)
    list(APPEND srcs "catflapcam_encoder.c")
endif()

if(CONFIG_CATFLAPCAM_SELECT_CUSTOMIZED_DEV_BOARD)
    list(APPEND inc_dirs "include/boards/customized")
endif()

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "${inc_dirs}"
                       REQUIRES "esp_video")

if(CONFIG_CATFLAPCAM_SELECT_JPEG_HW_DRIVER)
    idf_component_optional_requires(PRIVATE "esp_driver_jpeg")
endif()


