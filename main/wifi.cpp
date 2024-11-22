#include "wifi.h"

#include <freertos/task.h>
#include <nvs_flash.h>

#include <esp_http_client.h>
#include <esp_wifi.h>
#include <esp_log.h>

static esp_http_client_handle_t s_http_client = NULL;  

static const char* TAG = "wifi";
EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_num = 0;

static bool wifi_initialized = false;
static SemaphoreHandle_t wifi_mutex;

void http_init() {
    esp_http_client_config_t config = 
    {
        // .host = "172.20.10.7",
        .host = "192.168.1.144",
        .port = 5003,
        .auth_type = HTTP_AUTH_TYPE_NONE,
        .path = "/samples",
        .disable_auto_redirect = true,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
    };
    s_http_client = esp_http_client_init(&config);

    if (s_http_client == NULL) 
    {
        ESP_LOGE(TAG, "failed to initialize HTTP client");
        return;
    }

    ESP_ERROR_CHECK(esp_http_client_set_method(s_http_client, HTTP_METHOD_POST));
    ESP_ERROR_CHECK(esp_http_client_set_header(s_http_client, "Content-Type", "application/octet-stream"));
}

esp_err_t wifi_init() 
{
    if (wifi_initialized) {
        ESP_LOGI(TAG, "WiFi already initialized");
        return ESP_OK;
    }
    
    // // TODO: is this necessary?
    // if (wifi_mutex == NULL) {
    //     wifi_mutex = xSemaphoreCreateMutex();
    // }

    // if (xSemaphoreTake(wifi_mutex, portMAX_DELAY) == pdTRUE) {
    //     // Ensure that wifi_init() is only called once
    //     esp_err_t err = wifi_init_internal();
    //     xSemaphoreGive(wifi_mutex);
    //     return err;
    // }

    esp_err_t rv = nvs_flash_init();
    if (rv == ESP_ERR_NVS_NO_FREE_PAGES || rv == ESP_ERR_NVS_NEW_VERSION_FOUND) 
    {
        rv = nvs_flash_erase();
        if (rv != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase NVS flash");
            return rv;
        }

        rv = nvs_flash_init();
        if (rv != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize NVS flash");
            return rv;
        }
    }

    if (rv != ESP_OK) {
        return rv;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_FAIL;
    }

    rv = esp_netif_init();
    if (rv != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize esp-netif");
        return rv;
    }

    rv = esp_event_loop_create_default();
    if (rv != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create default event loop");
        return rv;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    rv = esp_wifi_init(&cfg);
    if (rv != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi");
        return rv;
    }

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    rv = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    if (rv != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi event handler");
        return rv;
    }

    rv = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);
    if (rv != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler");
        return rv;
    }

    wifi_config_t wifi_config = 
    {
        .sta = 
        {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    rv = esp_wifi_set_mode(WIFI_MODE_STA);
    if (rv != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi mode");
        return rv;
    }

    rv = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (rv != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi configuration");
        return rv;
    }

    rv = esp_wifi_start();
    if (rv != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi");
        return rv;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to SSID: <%s>", WIFI_SSID);
        http_init();
        wifi_initialized = true;
        return ESP_OK;
    } 
    else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID: <%s>", WIFI_SSID);
        return ESP_FAIL;
    } 
    else {
        ESP_LOGE(TAG, "Unexpected error occurred");
        return ESP_FAIL;
    }
}

esp_err_t wifi_deinit()
{
    if (!wifi_initialized) 
    {
        ESP_LOGI(TAG, "WiFi not initialized");
        return ESP_OK;
    }

    esp_err_t rv;

    // Stop the WiFi driver
    rv = esp_wifi_stop();
    if (rv != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop WiFi");
        return rv;
    }

    // Deinit the WiFi driver
    rv = esp_wifi_deinit();
    if (rv != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinitialize WiFi");
        return rv;
    }

    // Destroy the default event loop
    rv = esp_event_loop_delete_default();
    if (rv != ESP_OK) {
        ESP_LOGE(TAG, "Failed to delete default event loop");
        return rv;
    }

    // Deinitialize the esp-netif
    esp_netif_deinit();

    // Free the event group
    if (s_wifi_event_group != NULL) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    // Erase NVS flash if necessary
    rv = nvs_flash_deinit();
    if (rv != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinitialize NVS flash");
        return rv;
    }

    ESP_LOGI(TAG, "WiFi deinitialized successfully");
    wifi_initialized = false;

    return ESP_OK;
}

void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) 
    {
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) 
    {
        if (s_wifi_retry_num < WIFI_MAX_RETRY) 
        {
            esp_wifi_connect();
            s_wifi_retry_num++;
            ESP_LOGI(TAG, "connecting...");
        } else 
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connecting failed");
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) 
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_send_audio(const int16_t* const samples, size_t count) 
{
    // Set the Content-Type header to "audio/raw" to indicate raw PCM data
    ESP_ERROR_CHECK(esp_http_client_set_header(s_http_client, "Content-Type", "audio/raw"));
    
    // Set the POST field with audio data
    ESP_ERROR_CHECK(esp_http_client_set_post_field(s_http_client, (const char*) samples, sizeof(int16_t) * count));
    
    // Perform the POST request
    return esp_http_client_perform(s_http_client);
}

esp_err_t wifi_send_image(const uint8_t *image, size_t length) 
{
    if (s_http_client == NULL) 
    {
        http_init();
    }

    // Set the Content-Type header to "image/jpeg"
    ESP_ERROR_CHECK(esp_http_client_set_header(s_http_client, "Content-Type", "image/jpeg"));

    // Set the POST field with image data
    esp_http_client_set_post_field(s_http_client, (const char *)image, length);

    // Perform the POST request
    esp_err_t err = esp_http_client_perform(s_http_client);

    if (err == ESP_OK) 
    {
        ESP_LOGI(TAG, "image sent");
    } 
    else 
    {
        ESP_LOGE(TAG, "image send failed: %s", esp_err_to_name(err));
    }

    return err;
}