idf_component_register(
    SRCS "main.c" "display_p4.c" "lvgl_mem.c" "view.c" "panel.c" "lesson.c" "touch.c" "usb_mirror.c" "mirror_decode.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl esp_lcd esp_lcd_jd9165 esp_driver_ledc esp_driver_ppa esp_driver_gpio esp_hw_support esp_mm
             esp_timer esp_psram esp_driver_i2c usb)

# display_p4.c's frame copies use esp_lcd's 2D-DMA copy (esp_async_fbcpy), whose header is esp_lcd's own
target_include_directories(${COMPONENT_LIB} PRIVATE "$ENV{IDF_PATH}/components/esp_lcd/priv_include")
# display_p4.c: PSRAM's largest free block without walking the heap (MeteoBus: the panel's white flashes)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=heap_caps_get_largest_free_block")
# lvgl_mem.c provides LVGL's allocator (CONFIG_LV_USE_CUSTOM_MALLOC). Nothing in main references it,
# so force the linker to keep it (liblvgl is scanned before libmain resolves these).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init" "-u lv_malloc_core" "-u lv_realloc_core" "-u lv_free_core")
