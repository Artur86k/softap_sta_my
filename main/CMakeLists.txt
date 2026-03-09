idf_component_register(SRCS "softap_sta.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_netif esp_http_server nvs_flash esp_event lwip)
