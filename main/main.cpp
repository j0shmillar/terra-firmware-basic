// TODO
// - disable VBAT...somehow?
// - add cam PWDN
// - remove all pf
// - add schedule config
// - sort out includes
// - skip bootloader validate on wake

#include <PowerFeather.h>

#include <freertos/FreeRTOS.h>

#include <driver/rtc_io.h>
#include <driver/gpio.h>
#include <driver/uart.h>

#include <sdkconfig.h>

#include <esp_task_wdt.h>
#include <esp_camera.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_pm.h>

#include "L76X.h"
#include "mic.h"
#include "cam.h"

#include "wifi.h" // TODO: add lte 

/*******************************************************************************************************************/

static const char* TAG = "main";

// TODO: make all configurable from menuconfig + get rid

#define BATTERY_CAPACITY 0
#define SLEEP_DURATION_SECONDS 10
RTC_DATA_ATTR uint64_t sleep_start_time = 0;

bool do_send = true;
bool wifi_initialized = false;
SemaphoreHandle_t wifi_mutex;

#define PIR_PIN GPIO_NUM_11

#define UART_NUM UART_NUM_1
#define BUF_SIZE (1024)
#define BAUD_RATE 9600
#define TXD_PIN (GPIO_NUM_44)
#define RXD_PIN (GPIO_NUM_42)
GNRMC GPS1;
Coordinates B_GPS;
char buff_G[800] = {0};

bool capturing_m = false;
const size_t SAMPLE_RATE = 88200; 
const size_t RECORD_DURATION_SECONDS = 10;
const size_t TOTAL_SAMPLES = SAMPLE_RATE * RECORD_DURATION_SECONDS;

bool capturing_i = false;
TaskHandle_t cam_task_handle = NULL;

/*******************************************************************************************************************/

void uart_init()
{
    uart_config_t uart_config = 
    {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 1024 * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

// ISR Handler for PIR
void IRAM_ATTR gpio_isr_handler(void* arg) 
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    xTaskNotifyFromISR(cam_task_handle, 0, eNoAction, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

esp_err_t wifi_safe_init() 
{
    esp_err_t result = ESP_FAIL;
    xSemaphoreTake(wifi_mutex, portMAX_DELAY);

    if (!wifi_initialized) 
    {
        ESP_LOGI(TAG, "Initializing WiFi...\n");
        result = wifi_init();
        if (result == ESP_OK) 
        {
            wifi_initialized = true;
            ESP_LOGI(TAG, "WiFi initialized successfully\n");
        } 
        else 
        {
            ESP_LOGI(TAG, "WiFi initialization failed\n");
        }
    } 
    else 
    {
        result = ESP_OK;
    }

    xSemaphoreGive(wifi_mutex);
    return result;
}

esp_err_t wifi_safe_deinit() 
{
    esp_err_t result = ESP_FAIL;

    xSemaphoreTake(wifi_mutex, portMAX_DELAY);

    if (wifi_initialized && !capturing_m && !capturing_i) 
    {
        ESP_LOGI(TAG, "Deinitializing WiFi...\n");
        result = wifi_deinit();
        if (result == ESP_OK) {
            wifi_initialized = false; 
            ESP_LOGI(TAG, "WiFi deinitialized successfully\n");
        } 
        else 
        {
            ESP_LOGI(TAG, "WiFi deinitialization failed\n");
        }
    } 
    else if(wifi_initialized) 
    {
        ESP_LOGI(TAG,"WiFi deinit skipped: another task is running\n");
        result = ESP_ERR_INVALID_STATE; 
    } 
    else 
    {
        ESP_LOGI(TAG,"WiFi deinit skipped: WiFi not initialized\n");
        result = ESP_ERR_INVALID_STATE; 
    }

    xSemaphoreGive(wifi_mutex);
    return result;
}

/*******************************************************************************************************************/

// TODO: set sleep duration here 
void configure_sleep() 
{
    // TODO: disable VBAT for GPS

    rtc_gpio_init(PIR_PIN);
    rtc_gpio_set_direction_in_sleep(PIR_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    esp_sleep_enable_ext0_wakeup(PIR_PIN, 1);

    PowerFeather::Board.setEN(false); 

    PowerFeather::Board.enable3V3(false);

    rtc_gpio_init(PowerFeather::Mainboard::Pin::A0);
    rtc_gpio_set_direction_in_sleep(PowerFeather::Mainboard::Pin::A0, RTC_GPIO_MODE_INPUT_ONLY); 
    rtc_gpio_set_level(PowerFeather::Mainboard::Pin::A0, 1);
    rtc_gpio_hold_en(PowerFeather::Mainboard::Pin::A0);

    // other v sources
    PowerFeather::Board.enableVSQT(false);

}

/*******************************************************************************************************************/

void capture_audio() 
{
    PowerFeather::Board.setEN(true);
    
    size_t total_samples_read = 0;
    int16_t* buffer = (int16_t*)malloc(sizeof(int16_t) * TOTAL_SAMPLES);
    
    if(buffer != NULL) // TODO: fix chance of getting stuck here
    {
        mic_init(); // can setEN here (or defined pin)

        vTaskDelay(pdMS_TO_TICKS(300));
        total_samples_read = mic_read(buffer + total_samples_read, TOTAL_SAMPLES - total_samples_read);

        mic_deinit();
        capturing_m = false; //Task_m critical ends
 
        // TODO: add max retries
        esp_err_t err = wifi_safe_init(); 
        if (err == ESP_OK)
        {
            err = wifi_send_audio(buffer, total_samples_read);
            if (err != ESP_OK) 
            {
                ESP_LOGE(TAG, "Failed sending audio: %s\n", esp_err_to_name(err));
            } 
        }
        err = wifi_safe_deinit();

        free(buffer);
    }
}

void record_task(void *pvParameters) 
{
    while (1) 
    {
        float_t elapsed_time = (esp_timer_get_time()/1000000.0);

        if (elapsed_time >= SLEEP_DURATION_SECONDS || esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT0)
        {
            xSemaphoreTake(wifi_mutex, portMAX_DELAY);
            capturing_m = true; // Task_m critical starts
            xSemaphoreGive(wifi_mutex);

            capture_audio(); 

            // TODO: if sending fails, continue
            if (!capturing_i) 
            {
                configure_sleep();
                sleep_start_time = esp_timer_get_time() / 1000000.0;
                esp_deep_sleep(SLEEP_DURATION_SECONDS * 1000000);
            }

            xSemaphoreTake(wifi_mutex, portMAX_DELAY);
            xSemaphoreGive(wifi_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/*******************************************************************************************************************/

void capture_images() 
{
    float_t trigger_time = esp_timer_get_time()/1000000.0;

    // TODO: abstract into function (+ actually add)
    PowerFeather::Board.enable3V3(true); 
    rtc_gpio_hold_dis(PowerFeather::Mainboard::Pin::A0);
    rtc_gpio_deinit(PowerFeather::Mainboard::Pin::A0);
    gpio_reset_pin(PowerFeather::Mainboard::Pin::A0);
    gpio_set_direction(PowerFeather::Mainboard::Pin::A0, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(PowerFeather::Mainboard::Pin::A0, 0);

    vTaskDelay(pdMS_TO_TICKS(100));
    esp_err_t err = init_cam();
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Cam init result: %s\n", esp_err_to_name(err));
    if (err == ESP_OK)
    {
        for (int i = 0; i < 1; i++) // TODO: make n_burst configurable
        {

            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) 
            {
                float_t capture_time = esp_timer_get_time()/1000000.0;
                float_t latency = capture_time - trigger_time;
                ESP_LOGI(TAG, "Latency: %f\n", latency);

                uint8_t* _jpg_buf = NULL;
                size_t _jpg_buf_len = 0;
                
                bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
                if(!jpeg_converted)
                {
                    ESP_LOGI(TAG, "JPEGify failed\n");
                    esp_camera_fb_return(fb);
                    continue;
                }

                capturing_i = false;

                if(do_send)
                {
                    err = wifi_safe_init();
                    if (err == ESP_OK)
                    {
                        err = wifi_send_image(_jpg_buf, _jpg_buf_len);
                        if (err != ESP_OK) 
                        {
                            ESP_LOGE(TAG, "Failed to send image: %s\n", esp_err_to_name(err));
                        } 
                    }
                    err = wifi_safe_deinit();
                }
                esp_camera_fb_return(fb);
            }
            else
            {
                ESP_LOGE(TAG, "Capture failed\n");
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(100)); 
        }
        esp_camera_deinit();
    }
    capturing_i = false;
}

void pir_task(void *pvParameters) 
{
    while(1)
    {
        if (gpio_get_level(PIR_PIN)) 
        {   
            ESP_LOGI(TAG, "PIR triggered...\n");
            
            xSemaphoreTake(wifi_mutex, portMAX_DELAY);
            capturing_i = true; // Task_i critical starts
            xSemaphoreGive(wifi_mutex);
            
            while (gpio_get_level(PIR_PIN)) 
            {   
                capture_images(); 
                vTaskDelay(pdMS_TO_TICKS(100)); 
            }

            xSemaphoreTake(wifi_mutex, portMAX_DELAY);
            xSemaphoreGive(wifi_mutex);

            if (!capturing_m)
            {
                float_t elapsed_time = (esp_timer_get_time()/1000000.0);
                float_t remaining_sleep = SLEEP_DURATION_SECONDS - elapsed_time;

                if (remaining_sleep > 0) 
                {
                    configure_sleep();
                    esp_deep_sleep(remaining_sleep * 1000000);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/*******************************************************************************************************************/

extern "C" void app_main()
{

    if (PowerFeather::Board.init(BATTERY_CAPACITY) == PowerFeather::Result::Ok)
    {
        PowerFeather::Board.enableBatteryCharging(false);
        PowerFeather::Board.enableBatteryFuelGauge(false);
        PowerFeather::Board.enableBatteryTempSense(false);
    }

    static bool wdt_initialized = false;
    if (!wdt_initialized) 
    {
        esp_task_wdt_config_t wdt_config = {
            .timeout_ms = 30000,
            .idle_core_mask = (1 << 0) | (1 << 1),
            .trigger_panic = true
        };
        esp_task_wdt_init(&wdt_config);
        wdt_initialized = true; 
    }

    wifi_mutex = xSemaphoreCreateMutex();

    rtc_gpio_init(PIR_PIN);
    rtc_gpio_set_direction(PIR_PIN, RTC_GPIO_MODE_INPUT_ONLY); 
    rtc_gpio_wakeup_enable(PIR_PIN, GPIO_INTR_HIGH_LEVEL);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIR_PIN, gpio_isr_handler, NULL);
    
    uart_init();
    L76X_Send_Command(SET_NMEA_BAUDRATE_9600);
    L76X_Send_Command(SET_POS_FIX_400MS);
    L76X_Send_Command(SET_NMEA_OUTPUT);
    vTaskDelay(pdMS_TO_TICKS(10));

    // TODO: write a schedule
    // - also shut off VBAT/VS somehow
    // + have a check for if GPS doesnt locate
    PowerFeather::Board.enable3V3(true);
    DEV_Uart_SendString("$PQGLP,W,1,1*21");
    GPS1 = L76X_Gat_GNRMC();
    PowerFeather::Board.enable3V3(false);

    BaseType_t result1 = xTaskCreatePinnedToCore(pir_task, "Task_i", 8192, NULL, 1, NULL, 0);
    if (result1 == pdPASS) 
    {
        ESP_LOGI(TAG, "Task i created\n");
    } 
    else 
    {
        ESP_LOGI(TAG, "Task i creation failed\n");
    }

    BaseType_t result2 = xTaskCreatePinnedToCore(record_task, "Task_m", 8192, NULL, 1, &cam_task_handle, 1); //4096
    if (result2 == pdPASS) 
    {
        ESP_LOGI(TAG, "Task m created\n");
    } 
    else 
    {
        ESP_LOGI(TAG, "Task m creation failed\n");
    }
}