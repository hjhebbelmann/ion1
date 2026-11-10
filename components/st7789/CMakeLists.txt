idf_component_register(
    SRCS "st7789.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd
    # lvgl and esp_lvgl_port come from idf_component.yml (public dependencies).
)
