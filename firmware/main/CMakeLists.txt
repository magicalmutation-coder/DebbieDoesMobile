execute_process(COMMAND git rev-parse --short HEAD
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE DEBBIE_GIT_HASH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(NOT DEBBIE_GIT_HASH)
    set(DEBBIE_GIT_HASH "unknown")
endif()

set(DEBBIE_BUILD_HEADER "${CMAKE_CURRENT_BINARY_DIR}/build_info.h")
file(WRITE ${DEBBIE_BUILD_HEADER} "/* Auto-generated build info */\n#ifndef BUILD_INFO_H\n#define BUILD_INFO_H\n#define DEBBIE_GIT_HASH \"${DEBBIE_GIT_HASH}\"\n#endif\n")

idf_component_register(PRIV_REQUIRES esp_driver_i2s 
    SRCS
        "main.c"
        "storage_manager.c"
        "wifi_manager.c"
        "bluetooth_manager.c"
        "audio_manager.c"
        "camera_manager.c"
        "display_manager.c"
        "openai_client.c"
        "notification_client.c"
        "web_server.c"
        "network_scanner.c"
        "vuln_reporter.c"
        "self_agent.c"
        "memory_manager.c"
    INCLUDE_DIRS
        "."
        "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        esp_https_ota
        esp_websocket_client
        esp_http_server
        esp_adc
        esp_camera
        esp_lcd
        driver
        lvgl
        cjson
        mbedtls
        esp_timer
        freertos
        log
        lwip
        bt
)
