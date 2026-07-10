idf_component_register(
    SRCS "st7789.c" "font5x7.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd
)
