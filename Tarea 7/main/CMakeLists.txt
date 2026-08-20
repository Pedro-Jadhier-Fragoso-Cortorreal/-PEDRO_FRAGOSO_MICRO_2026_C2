idf_component_register(
    SRCS main.c
    INCLUDE_DIRS
    PRIV_INCLUDE_DIRS
    REQUIRES nvs_flash esp_wifi esp_event esp_netif mqtt json driver esp_timer
    PRIV_REQUIRES
)
