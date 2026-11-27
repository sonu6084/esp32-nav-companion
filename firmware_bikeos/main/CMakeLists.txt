# bike-os. Dependencies come from the component manager (main/idf_component.yml),
# as for firmware_v2 and firmware_watcher.
file(GLOB NAV_SRCS "nav/*.c")
file(GLOB APP_SRCS "apps/*.c" "fonts/*.c" "icons/*.c")

idf_component_register(
  SRCS
    "main.c" "nav_core.c" "sys.c" "rtc.c" "cfg.c" "audio.c" "alert.c"
    "ride.c" "bos_faces.c" "bos_intro.c" "intro_data.c" "home.c" "app_manager.c" "ui_theme.c" "ui_settime.c" "weather_store.c" "weather_net.c"
    ${NAV_SRCS} ${APP_SRCS}
  INCLUDE_DIRS "." "nav" "apps" "fonts" "icons"
  EMBED_FILES "assets/intro.gif"
  REQUIRES
    esp_driver_i2c esp_driver_spi esp_driver_gpio esp_driver_ledc esp_driver_sdmmc esp_driver_i2s
    esp_lcd esp_lcd_touch esp_lcd_spd2010 esp_lcd_touch_spd2010
    fatfs wear_levelling sdmmc nvs_flash bt esp_timer esp_psram esp_adc
    esp_wifi esp_netif esp_event esp_http_client esp-tls mbedtls json
    lvgl esp_jpeg
)

# Hot per-pixel loop of the rotated (heading-up) map renderer (as in firmware_ws146).
set_source_files_properties(nav/map_view.c PROPERTIES COMPILE_OPTIONS "-O3")
