#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <driver/i2s_std.h>
#include <driver/gpio.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_check.h>
#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_assert.h>

#include <nvs_flash.h>

#include "sdkconfig.h"

#define WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define WIFI_MAX_RETRY CONFIG_ESP_MAXIMUM_RETRY

#define I2S_INMP441_SCK  (GPIO_NUM_26)
#define I2S_INMP441_WS   (GPIO_NUM_22)
#define I2S_INMP441_SD   (GPIO_NUM_21)

#define I2S_SAMPLE_BYTES    (4U)
#define I2S_SAMPLE_COUNT    (16384U)
#define I2S_BUFFER_SAMPLES  (1024U)

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "esp32-i2s-mic-test";

static i2s_chan_handle_t s_rx_handle;
static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_num = 0;
static esp_http_client_handle_t s_http_client;


static void
http_init()
{
    esp_http_client_config_t config = {
        .host = "192.168.1.20",
        .port = 5003,
        .path = "/samples",
        .disable_auto_redirect = true,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .auth_type = HTTP_AUTH_TYPE_NONE,
    };
    s_http_client = esp_http_client_init(&config);

    ESP_ERROR_CHECK(esp_http_client_set_method(s_http_client, HTTP_METHOD_POST));
    ESP_ERROR_CHECK(esp_http_client_set_header(s_http_client, "Content-Type", "application/octet-stream"));
}

static esp_err_t
http_send(const int16_t* const samples, size_t count)
{
    ESP_ERROR_CHECK(esp_http_client_set_post_field(
        s_http_client,
        (const char*) samples,
        sizeof(uint16_t) * count));
    return esp_http_client_perform(s_http_client);
}

static void
wifi_event_handler(void* arg, esp_event_base_t event_base,
                   int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_wifi_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void
wifi_init()
{
    esp_err_t rv = nvs_flash_init();
    if (rv == ESP_ERR_NVS_NO_FREE_PAGES || rv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        rv = nvs_flash_init();
    }
    ESP_ERROR_CHECK(rv);

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL,
        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL,
        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", WIFI_SSID, WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", WIFI_SSID, WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

static void
mic_init()
{
    /* Get the default channel configuration */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);

    /* Allocate a new RX channel and get the handle of this channel */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_handle));

    /* Setting the configurations */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_INMP441_SCK,
            .ws = I2S_INMP441_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_INMP441_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    /* Initialize the channel */
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_handle, &std_cfg));

    /* Before reading data, start the RX channel first */
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_handle));
}

static void
mic_deinit()
{
    /* Have to stop the channel before deleting it */
    i2s_channel_disable(s_rx_handle);
    /* Delete channel to release resources */
    i2s_del_channel(s_rx_handle);
}

static size_t
mic_read(int16_t* samples, size_t count)
{
    static int32_t buffer[I2S_BUFFER_SAMPLES];

    size_t sample_index = 0;
    while (count > 0) {
        size_t bytes_need = count * I2S_SAMPLE_BYTES;
        if (bytes_need > sizeof(buffer)) {
            bytes_need = sizeof(buffer);
        }

        size_t bytes_read = 0;
        if (i2s_channel_read(s_rx_handle,
                (char*) buffer, I2S_BUFFER_SAMPLES * I2S_SAMPLE_BYTES,
                &bytes_read,
                portMAX_DELAY) != ESP_OK) {
            ESP_LOGE(TAG, "Error reading from audio channel");
            return 0;
        }

        size_t samples_read = bytes_read / I2S_SAMPLE_BYTES;
        for (int i = 0; i < samples_read; ++i) {
            samples[sample_index++] = buffer[i] >> 11; // Only 12 bits from 24 bits of INMP1441 sample data
            count--;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return sample_index;
}

static void
data_loop()
{
    int16_t* samples = (int16_t*) malloc(sizeof(uint16_t) * I2S_SAMPLE_COUNT);

    http_init();

    esp_err_t error;
    while (true) {
        size_t sample_read = mic_read(samples, I2S_SAMPLE_COUNT);
        if (sample_read == 0) {
            ESP_LOGE(TAG, "No data is available");
            mic_deinit();
            mic_init();
            continue;
        }

        ESP_LOGI(TAG, "Send samples: %u", sample_read);
        error = http_send(samples, sample_read);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Sending samples has failed: %s", esp_err_to_name(error));
        }
    }
}

void app_main()
{
    wifi_init();
    mic_init();
    data_loop();
}