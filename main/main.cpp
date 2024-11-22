// TODO
// - sort out bloat (esp wd resets)
// - add schedule config
// - is rtc a power concern?
// - test goertzel etc
// - sort out includes
// - enable CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP

// https://github.com/OpenAcousticDevices/AudioMoth-Firmware-Basic/blob/master/src/main.c
// makeRecording
// digitalFilter https://github.com/OpenAcousticDevices/AudioMoth-Firmware-Basic/blob/61e3f6c97c33bfaea49285bde5fbe41243c4eaa2/inc/digitalFilter.h

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
#include <esp_pm.h>

#include "L76X.h"
#include "mic.h"
#include "cam.h"
#include "wifi.h"


const size_t SAMPLE_RATE = 88200; 
const size_t RECORD_DURATION_SECONDS = 10;
const size_t TOTAL_SAMPLES = SAMPLE_RATE * RECORD_DURATION_SECONDS;
const size_t WARMUP_DURATION_SECONDS = 0.1;
const size_t WARMUP_SAMPLES = SAMPLE_RATE * WARMUP_DURATION_SECONDS;

#define PIR_PIN GPIO_NUM_11
#define SLEEP_DURATION_SECONDS 10 // TODO: make configurable

#define BATTERY_CAPACITY 0
bool inited = false;
bool do_send = true;

TaskHandle_t cam_task_handle = NULL;

#define MAX_TIME_MICROSECONDS (UINT64_MAX)
RTC_DATA_ATTR uint64_t sleep_start_time = 0;

#define UART_NUM UART_NUM_1
#define BUF_SIZE (1024)
#define BAUD_RATE 9600
#define TXD_PIN (GPIO_NUM_42)
#define RXD_PIN (GPIO_NUM_44)
GNRMC GPS1;
Coordinates B_GPS;
char buff_G[800] = {0};

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

// ISR Handler for PIR Interrupt
void IRAM_ATTR gpio_isr_handler(void* arg) 
{
    printf("PIR ISR triggered\n");
    BaseType_t higher_priority_task_woken = pdFALSE;
    xTaskNotifyFromISR(cam_task_handle, 0, eNoAction, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

/*******************************************************************************************************************/

// TODO: set sleep duration here 
void configure_sleep() 
{
    rtc_gpio_init(PIR_PIN);
    rtc_gpio_set_direction_in_sleep(PIR_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    esp_sleep_enable_ext0_wakeup(PIR_PIN, 1);

    // mic - power down
    PowerFeather::Board.setEN(false); 

    // cam - power down
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
    while (buffer == NULL) // TODO: fix chance of getting stuck here
    {
        printf("buffer allocation failed");
        continue;
    }

    mic_init(); // this can setEN (or defined pin)

    // wifi_deinit(); // TODO: needs some way of testing if image/audio in-transmission (easier way is to avoid transmission UNLESS nothing happening)

    // TODO: WHY IS INITIAL 0.7S NOISY?
    // - easy solution = record for +1s and cut
    vTaskDelay(pdMS_TO_TICKS(300));
    total_samples_read = mic_read(buffer + total_samples_read, TOTAL_SAMPLES - total_samples_read);
    printf("samples read: %zu\n", total_samples_read);

    mic_deinit();

    /* note: wifi transmission whilst recording causes noise on power line */
    // TODO add max retries
    esp_err_t err = wifi_init(); 
    if (err != ESP_OK)
    {
        printf("wifi init failed: %s\n", esp_err_to_name(err));
    }
    else
    {
        printf("wifi init\n");
        err = wifi_send_audio(buffer, total_samples_read);
        if (err != ESP_OK) 
        {
            printf("failed to send audio: %s\n", esp_err_to_name(err));
        } 
    }
    err = wifi_deinit();
    if (err != ESP_OK)
    {
        printf("wifi deinit failed: %s\n", esp_err_to_name(err));
    }

    free(buffer);
}

void record_task(void *pvParameters) 
{
    while (1) 
    {
        float_t elapsed_time = (esp_timer_get_time()/1000000.0);

        // capture audio if sleep duration is exceeded or no PIR trigger occurred
        if (elapsed_time >= SLEEP_DURATION_SECONDS || esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT0)
        {
            printf("capturing audio...\n");
            capture_audio();  // Capture and process audio

            // TODO a schedule for this - can't be doing it on every boot
            // also shut off VBAT/VS somehow
            // + have a check for if this doesnt locate
            printf("locating...\n");
            GPS1 = L76X_Gat_GNRMC();
            printf("\r\n");
            printf("time: %02d:%02d:%02d\r\n", GPS1.Time_H, GPS1.Time_M, GPS1.Time_S);
            printf("lat: %.7f\n", GPS1.Lat);
            printf("lon: %.7f\n", GPS1.Lon);
            vTaskDelay(pdMS_TO_TICKS(1000));

            // Configure sleep and reset start time after recording or waking up
            configure_sleep();
            sleep_start_time = esp_timer_get_time()/1000000.0;
            esp_deep_sleep(SLEEP_DURATION_SECONDS * 1000000);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/*******************************************************************************************************************/

void capture_images() 
{
    float_t trigger_time = esp_timer_get_time()/1000000.0;
    // TODO: abstract into function
    PowerFeather::Board.enable3V3(true); 
    rtc_gpio_hold_dis(PowerFeather::Mainboard::Pin::A0);
    rtc_gpio_deinit(PowerFeather::Mainboard::Pin::A0);
    gpio_reset_pin(PowerFeather::Mainboard::Pin::A0);
    gpio_set_direction(PowerFeather::Mainboard::Pin::A0, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(PowerFeather::Mainboard::Pin::A0, 0);

    vTaskDelay(pdMS_TO_TICKS(100));
    printf("in capture func\n");

    esp_err_t err = init_cam();
    vTaskDelay(pdMS_TO_TICKS(100));
    printf("cam init result: %s\n", esp_err_to_name(err));
    // if (err != ESP_OK)
    // {
    //     // do something
    // }

    sensor_t * sensor = esp_camera_sensor_get();
    sensor->set_whitebal(sensor, 1);
    sensor->set_awb_gain(sensor, 1);
    sensor->set_wb_mode(sensor, 0);

    for (int i = 0; i < 1; i++) // TODO: make n_burst configurable
    {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) 
        {
            float_t capture_time = esp_timer_get_time()/1000000.0;
            float_t latency = capture_time - trigger_time;
            printf("captured\n");
            printf("latency: %f\n", latency);

            uint8_t* _jpg_buf = NULL;
            size_t _jpg_buf_len = 0;

            // TODO: process image here
            
            bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
            if(!jpeg_converted)
            {
                printf("JPEGify failed\n");
                esp_camera_fb_return(fb);
                continue;
            }
            if(do_send)
            {
                printf("sending...\n");
                esp_task_wdt_delete(NULL); // probs shouldn't do this as might have to yield here
                err = wifi_init(); 
                if (err != ESP_OK)
                {
                    printf("wifi init failed: %s\n", esp_err_to_name(err));
                }
                printf("wifi init\n");
                err = wifi_send_image(_jpg_buf, _jpg_buf_len);
                if (err != ESP_OK) 
                {
                    printf("failed to send image: %s\n", esp_err_to_name(err));
                } 
                printf("image sent\n");
                esp_task_wdt_add(NULL); // probs shouldn't do this
                err = wifi_deinit();
                if (err != ESP_OK)
                {
                    printf("wifi deinit failed: %s\n", esp_err_to_name(err));
                }
            }
            esp_camera_fb_return(fb);
        }
        else
        {
            printf("image capture failed\n");
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }
    esp_camera_deinit();
}

void pir_task(void *pvParameters) 
{
    while(1)
    {
        // if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) 
        if (gpio_get_level(PIR_PIN)) 
        {   
            while (gpio_get_level(PIR_PIN)) 
            {
                printf("PIR triggered...\n");
                capture_images();  // Capture images
                vTaskDelay(pdMS_TO_TICKS(100));  // Delay to avoid flooding
                esp_task_wdt_reset();  // Reset watchdog
            }

            // TODO: this should only sleep if audio recording is not currently running...(needs a simple check is all)
            float_t elapsed_time = (esp_timer_get_time()/1000000.0);
            printf("elapsed_time: %f\n", elapsed_time);

            float_t remaining_sleep = SLEEP_DURATION_SECONDS - elapsed_time;
            printf("remaining sleep: %f\n", remaining_sleep);

            if (remaining_sleep > 0) 
            {
                configure_sleep();  // Sleep for remaining time
                esp_deep_sleep(remaining_sleep * 1000000);
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
        printf("board init success\n");
        inited = true;
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

    printf("sleep start time: %d\n", (int)sleep_start_time);

    rtc_gpio_init(PIR_PIN);
    rtc_gpio_set_direction(PIR_PIN, RTC_GPIO_MODE_INPUT_ONLY); 
    rtc_gpio_wakeup_enable(PIR_PIN, GPIO_INTR_HIGH_LEVEL);

    // Install ISR for PIR
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIR_PIN, gpio_isr_handler, NULL);
    
    uart_init();
    L76X_Send_Command(SET_NMEA_BAUDRATE_9600);
    L76X_Send_Command(SET_POS_FIX_400MS);
    L76X_Send_Command(SET_NMEA_OUTPUT);
    printf("uart init at baud rate: %d\n", BAUD_RATE);
    vTaskDelay(pdMS_TO_TICKS(10));

    printf("locating...\n");
    GPS1 = L76X_Gat_GNRMC();
    printf("\r\n");
    printf("time: %02d:%02d:%02d\r\n", GPS1.Time_H, GPS1.Time_M, GPS1.Time_S);
    printf("lat: %.7f\n", GPS1.Lat);
    printf("lon: %.7f\n", GPS1.Lon);
    vTaskDelay(pdMS_TO_TICKS(1000));

    // should these be of equal priority?
    // BaseType_t result1 = xTaskCreatePinnedToCore(pir_task, "PIR Task", 8192, NULL, 1, NULL, 0);
    // if (result1 == pdPASS) 
    // {
    //     printf("PIR task created\n");
    // } 
    // else 
    // {
    //     printf("PIR task creation failed\n");
    // }

    BaseType_t result2 = xTaskCreatePinnedToCore(record_task, "Record Task", 8192, NULL, 1, &cam_task_handle, 1); //4096
    if (result2 == pdPASS) 
    {
        printf("record task created\n");
    } 
    else 
    {
        printf("record task creation failed\n");
    }
}