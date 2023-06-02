#include <freertos/FreeRTOS.h>

#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_event.h>
#include <esp_netif.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_http_client.h>

#include <nvs_flash.h>
#include <driver/i2s.h>

static const char* TAG = "ESP32 I2S Mic Test";

// WiFi: connection retry counter
static int s_wifi_retry_num = 0;
/* WiFi: event group to signal about events */
static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT (BIT0)
#define WIFI_FAIL_BIT      (BIT1)

#define ESP_WIFI_SSID      (CONFIG_ESP_WIFI_SSID)
#define ESP_WIFI_PASS      (CONFIG_ESP_WIFI_PASSWORD)
#define ESP_MAXIMUM_RETRY  (CONFIG_ESP_MAXIMUM_RETRY)

#define I2S_DMA_BUFFER_COUNT (4U)     // The total amount of DMA buffers count
#define I2S_DMA_BUFFER_SIZE  (1024U)  // The size of each DMA buffer in samples
#define I2S_SAMPLE_COUNT     (16384U) // The total amount of samples per data transfer
#define I2S_SAMPLE_RATE      (16000U) // The total amount of samples per second

#define I2S_INMP441_PORT (I2S_NUM_0)
#define I2S_INMP441_SCK  (GPIO_NUM_26)
#define I2S_INMP441_WS   (GPIO_NUM_22)
#define I2S_INMP441_SD   (GPIO_NUM_21)

void wifi_event_handler(void* arg, esp_event_base_t event_base,
                        int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi: connect to SSID:%s", ESP_WIFI_SSID);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_num < ESP_MAXIMUM_RETRY) {
            ESP_LOGI(TAG, "WiFi: retry to connect to the SSID:%s", ESP_WIFI_SSID);
            s_wifi_retry_num++;
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "WiFi: the amount of retry attempts is exhausted");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);    
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi: got ip " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else {
        ESP_LOGI(TAG, "Unexpected event");
    }
}

void mic_init()
{
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2,
        .dma_buf_count = I2S_DMA_BUFFER_COUNT,
        .dma_buf_len = I2S_DMA_BUFFER_SIZE,
        .use_apll = false,
    };

    const i2s_pin_config_t i2s_pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = I2S_INMP441_SCK,
        .ws_io_num = I2S_INMP441_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_INMP441_SD
    };

    ESP_ERROR_CHECK(i2s_driver_install(I2S_INMP441_PORT, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_INMP441_PORT, &i2s_pin_config));
    ESP_ERROR_CHECK(i2s_set_clk(I2S_INMP441_PORT, I2S_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_MONO));
}

size_t mic_read(int16_t* samples, size_t count)
{
    static int32_t buffer[I2S_DMA_BUFFER_SIZE / sizeof(int32_t)];

    size_t sample_index = 0;
    while (count > 0) {
        size_t bytes_need = count * sizeof(int32_t);
        if (bytes_need > sizeof(buffer)) {
            bytes_need = sizeof(buffer);
        }

        size_t bytes_read = 0;
        ESP_ERROR_CHECK(i2s_read(I2S_INMP441_PORT, buffer, bytes_need, &bytes_read, portMAX_DELAY));
        
        size_t samples_read = bytes_read / sizeof(int32_t);
        for (int i = 0; i < samples_read; ++i) {
            samples[sample_index] = buffer[i] >> 12;
            sample_index++;
            count--;
        }
    }
    return sample_index;
}

void mic_loop()
{
    int16_t* samples = (int16_t*) malloc(sizeof(uint16_t) * I2S_SAMPLE_COUNT);

    esp_http_client_config_t config = {
        .host = "192.168.1.20",
        .port = 5003,
        .path = "/samples",
        .disable_auto_redirect = true,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .auth_type = HTTP_AUTH_TYPE_NONE,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    
    // Discard first block (removes microphone clicks)
    mic_read(samples, I2S_SAMPLE_COUNT);

    esp_err_t error;
    while (true) {
        size_t sample_read = mic_read(samples, I2S_SAMPLE_COUNT);
        ESP_LOGI(TAG, "Data is ready: %u", sample_read);
        ESP_ERROR_CHECK(esp_http_client_set_post_field(
            client,
            (const char*) samples,
            sizeof(uint16_t) * sample_read));
        error = esp_http_client_perform(client);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(error));
        }
    }
}

bool wifi_init()
{
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();

    ESP_LOGI(TAG, "WiFi: intialize");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL,
        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL,
        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASS,
            .threshold = {
                .rssi = 0,
                .authmode = WIFI_AUTH_WPA2_PSK
            },
            .pmf_cfg = {
                .capable = true,
                .required = false
            }
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());    

    ESP_LOGI(TAG, "WiFi: initialization is finished");

    // Wait until the connection is establieshed or connection failed
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);

    bool rv = false;
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi: connected to SSID:%s", ESP_WIFI_SSID);
        rv = true;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "WiFi: failed connect to SSID:%s", ESP_WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "WiFi: unexpected event");
    }
    
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        instance_any_id));
    vEventGroupDelete(s_wifi_event_group);

    return rv;
}

void nvsf_init()
{
    esp_err_t rv = nvs_flash_init();
    if (rv == ESP_ERR_NVS_NO_FREE_PAGES || rv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      rv = nvs_flash_init();
    }
    ESP_ERROR_CHECK(rv);
}

void app_main()
{
    nvsf_init();

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    if (wifi_init()) {
        mic_init();
        mic_loop();
    }
}