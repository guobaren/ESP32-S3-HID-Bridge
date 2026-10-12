idf_component_register(SRCS "main.c" "vendor_descriptors.c" INCLUDE_DIRS "."
    REQUIRES espressif__esp_tinyusb esp_timer esp_rom)
