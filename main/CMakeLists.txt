idf_component_register(SRCS "scan.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_wifi nvs_flash myuart)
