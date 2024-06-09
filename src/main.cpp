#include "Wifi.h"
#include "I2S.h"
#include "Http.h"

#include <PowerFeather.h>
#include <freertos/FreeRTOS.h>

#include <esp_log.h>
#include <esp_sleep.h>

#define BATTERY_CAPACITY 0
const gpio_num_t PIR_PIN = GPIO_NUM_15;

using namespace PowerFeather;
static const char* TAG = "esp32-test";

bool inited = false;
int warm_up;

void data_loop() {
    int16_t* samples = (int16_t*) malloc(sizeof(uint16_t) * I2S_SAMPLE_COUNT);
    http_init();
    esp_err_t error;
    while (true) {
        gpio_set_level(Mainboard::Pin::LED, !gpio_get_level(Mainboard::Pin::LED));

        Board.enableBatteryCharging(gpio_get_level(Mainboard::Pin::BTN) == 0);
        /* this gets + prints supply/batteery current + voltage */
        // uint16_t supplyVoltage = 0, batteryVoltage = 0;
        // int16_t supplyCurrent = 0, batteryCurrent = 0;
        // uint8_t batteryCharge = 0;
        // Board.getSupplyVoltage(supplyVoltage);
        // Board.getSupplyCurrent(supplyCurrent);
        // Board.getBatteryVoltage(batteryVoltage);
        // Board.getBatteryCurrent(batteryCurrent);
        // printf("[Supply]  Voltage: %d mV    Current: %d mA\n", supplyVoltage, supplyCurrent);
        // printf("[Battery] Voltage: %d mV    Current: %d mA    ", batteryVoltage, batteryCurrent);
        // Result res = Board.getBatteryCharge(batteryCharge);
        // if (res == Result::Ok) {
        //     printf("Charge: %d %%\n", batteryCharge);
        // } else if (res == Result::InvalidState) {
        //     printf("Charge: <BATTERY_CAPACITY set to 0>\n");
        // } else {
        //     printf("Charge: <battery not detected>\n");
        // }

        /*mic read*/
        size_t sample_read = mic_read(samples);
        if (sample_read == 0) {
            ESP_LOGE(TAG, "No data available");
            mic_deinit();
            mic_init();
            continue;
        }
        
        /*send mic data*/
        ESP_LOGI(TAG, "Send: %u", sample_read);
        error = http_send(samples, sample_read);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Sending has failed: %s", esp_err_to_name(error));
        }
        
        /*pir*/
        int sensor_output = gpio_get_level(PIR_PIN);
        if (sensor_output == 1) {
            if (warm_up == 1) {
                printf("Warming up\n\n");
                warm_up = 0;
                vTaskDelay(pdMS_TO_TICKS(30));
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            printf("Object detected\n\n");
            warm_up = 1;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

extern "C" void app_main() {
    gpio_reset_pin(Mainboard::Pin::BTN);
    gpio_set_direction(Mainboard::Pin::BTN, GPIO_MODE_INPUT);

    gpio_reset_pin(Mainboard::Pin::LED);
    gpio_set_direction(Mainboard::Pin::LED, GPIO_MODE_INPUT_OUTPUT);

    gpio_reset_pin(PIR_PIN);
    gpio_set_direction(PIR_PIN, GPIO_MODE_INPUT);

    if (Board.init(BATTERY_CAPACITY) == Result::Ok) {
        printf("Board initialized successfully\n\n");
        Board.setBatteryChargingMaxCurrent(100);
        inited = true;
    }

    esp_sleep_enable_ext0_wakeup(PIR_PIN, 1);
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
        printf("Wake up from PIR sensor\n");
    }

    wifi_init();
    mic_init();
    if (inited) {
        data_loop();
    } else {
        printf("Board not initialized\n");
    }

    printf("Entering deep sleep\n");
    esp_deep_sleep_start();
}