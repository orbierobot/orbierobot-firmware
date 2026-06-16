idf_component_register(
    SRCS "app_main.cpp" "IS31FL3733.c" "led_display.c" "boot_sound.c"
    INCLUDE_DIRS "."
    REQUIRES esp32-camera esp_wifi esp_http_server nvs_flash app_update ESP32_VL53L0X esp_psram json driver esp_timer
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_OPENPAW_BOARD_NAME="$ENV{OPENPAW_BOARD}"
)