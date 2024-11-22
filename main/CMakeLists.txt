idf_component_register(SRCS main.cpp mic.h mic.cpp cam.h cam.cpp wifi.h wifi.cpp L76X.h L76X.cpp
                        PRIV_INCLUDE_DIRS .
                        PRIV_REQUIRES tflite-lib nvs_flash esp_event esp_http_client esp_wifi esp_timer esp_adc esp_psram esp_pm)