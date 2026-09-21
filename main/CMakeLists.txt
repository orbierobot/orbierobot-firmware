idf_component_register(
    SRCS "app_main.cpp" "IS31FL3733.c" "led_display.c" "boot_sound.c" "wifi_portal.c"
    INCLUDE_DIRS "."
    REQUIRES esp32-camera esp_wifi esp_http_server nvs_flash app_update ESP32_VL53L0X esp_psram json driver esp_timer
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_OPENPAW_BOARD_NAME="$ENV{OPENPAW_BOARD}"
)

# Dev builds keep the USB Serial/JTAG console alive so idf.py monitor works.
# Battery/demo builds leave this OFF so app_main can disable the USB PHY —
# floating D+/D- lines reset the board every ~5s when untethered.
#   idf.py -DORBIE_KEEP_USB_CONSOLE=1 build
if(ORBIE_KEEP_USB_CONSOLE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ORBIE_KEEP_USB_CONSOLE=1)
endif()
# Dev convenience: leave the robot's own AP open (no WPA2 password).
# Bench only. The captive portal posts the user's HOME Wi-Fi password over
# plain HTTP, which an open AP leaves readable to anyone in radio range.
#   idf.py -DORBIE_OPEN_AP=1 build
if(ORBIE_OPEN_AP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ORBIE_OPEN_AP=1)
endif()
