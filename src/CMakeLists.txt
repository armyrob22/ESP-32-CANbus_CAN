idf_component_register(
    SRCS "main.c" "ring.c" "can_capture.c" "net.c" "sender.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi esp_netif esp_event esp_timer nvs_flash lwip
)
