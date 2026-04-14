#include <stdio.h>
#include <unistd.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "adc_dac.h"

static const char *TAG = "MAIN";

// Bộ lọc
static SlidingWindowFilter sliding_filter;
static LowpassFilter lowpass_filter_obj;
static uint16_t average_buffer[10];  // Buffer cho average filter


 //Đọc ADC và xử lý dữ liệu từ biến trở

void adc_dac_task(void *pvParameters)
{
    // Khởi tạo ADC và DAC
    adc_init();
    dac_init();

    // Khởi tạo các bộ lọc
    sliding_window_init(&sliding_filter, 10);           
    lowpass_init(&lowpass_filter_obj, 0.3f);           

    int sample_count = 0;
    static uint16_t avg_index = 0;

    while (1) {
        // Đọc giá trị ADC thô từ biến trở 
        uint16_t adc_raw = adc_read_raw();

        // Áp dụng các bộ lọc 
        uint16_t sliding_filtered = sliding_window_filter(&sliding_filter, adc_raw);
        uint16_t lowpass_filtered = lowpass_filter(&lowpass_filter_obj, adc_raw);

        // Lưu vào buffer để tính average
        average_buffer[avg_index] = adc_raw;
        avg_index++;
        if (avg_index >= 10) {
            avg_index = 0;
        }
        uint16_t average_filtered = average_filter(average_buffer, 10);

        // Chuyển đổi ADC thành DAC và ghi giá trị 
        uint8_t dac_value = adc_to_dac(lowpass_filtered);
        dac_write(dac_value);

        //In kết quả  
        sample_count++;
        if (sample_count % 50 == 0) {
            ESP_LOGI(TAG, "ADC_raw: %4d | Sliding: %4d | Lowpass: %4d | Average: %4d | DAC: %3d",
                     adc_raw,
                     sliding_filtered,
                     lowpass_filtered,
                     average_filtered,
                     dac_value);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    sliding_window_free(&sliding_filter);
}
void app_main(void)
{
    // Tạo task để đọc ADC và xử lý
    xTaskCreate(adc_dac_task,           // Hàm task
                "ADC_DAC_Task",         // Tên task
                4096,                   // Stack size
                NULL,                   // Parameters
                5,                      // Priority
                NULL);                  // Task handle
}
