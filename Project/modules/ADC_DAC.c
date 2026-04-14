#include "adc_dac.h"
#include <string.h>
#include <stdlib.h>
#include "esp_adc/adc_oneshot.h"
#include "driver/dac_oneshot.h"
#include "esp_log.h"

static const char *TAG = "ADC_DAC";

// ADC handle
static adc_oneshot_unit_handle_t adc_handle = NULL;

// DAC handle
static dac_oneshot_handle_t dac_handle = NULL;

void adc_init(void)
{
    if (adc_handle != NULL) {
        return;
    }

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    // Cấu hình channel 0 (GPIO36)
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_12,           // 12-bit (0-4095)
        .atten = ADC_ATTEN_DB_12,              // Attenuation 12dB (0-3.3V)
    };

    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_0, &config));
}

void dac_init(void)
{
    if (dac_handle != NULL) {
        return;
    }

    dac_oneshot_config_t config = {
        .chan_id = DAC_CHAN_0,  // DAC Channel 0 (GPIO25)
    };

    ESP_ERROR_CHECK(dac_oneshot_new_channel(&config, &dac_handle));
}

uint16_t adc_read_raw(void)
{
    if (adc_handle == NULL) {
        return 0;
    }

    int adc_value = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, ADC_CHANNEL_0, &adc_value));
    return (uint16_t)adc_value;
}

void sliding_window_init(SlidingWindowFilter *filter, uint16_t window_size)
{
    if (filter == NULL) {
        return;
    }

    filter->buffer = (uint16_t *)malloc(window_size * sizeof(uint16_t));
    if (filter->buffer == NULL) {
        return;
    }

    filter->window_size = window_size;
    filter->index = 0;
    filter->sum = 0;
    filter->filled = 0;

    memset(filter->buffer, 0, window_size * sizeof(uint16_t));
}

uint16_t sliding_window_filter(SlidingWindowFilter *filter, uint16_t raw_value)
{
    if (filter == NULL || filter->buffer == NULL) {
        return raw_value;
    }

    // Nếu cửa sổ chưa đầy, cộng giá trị mới
    if (!filter->filled) {
        filter->sum += raw_value;
        filter->buffer[filter->index] = raw_value;
        filter->index++;

        if (filter->index >= filter->window_size) {
            filter->filled = 1;
            filter->index = 0;
        }

        return filter->sum / filter->index;
    }

    filter->sum -= filter->buffer[filter->index];  // Trừ giá trị cũ
    filter->sum += raw_value;                      // Cộng giá trị mới
    filter->buffer[filter->index] = raw_value;     // Cập nhật buffer
    filter->index++;

    if (filter->index >= filter->window_size) {
        filter->index = 0;
    }

    return filter->sum / filter->window_size;
}

void lowpass_init(LowpassFilter *filter, float alpha)
{
    if (filter == NULL) {
        return;
    }

    if (alpha < 0.0f || alpha > 1.0f) {
        alpha = (alpha < 0.0f) ? 0.0f : 1.0f;
    }

    filter->alpha = alpha;
    filter->previous_value = 0.0f;
    filter->initialized = 0;

}

uint16_t lowpass_filter(LowpassFilter *filter, uint16_t raw_value)
{
    if (filter == NULL) {
        return raw_value;
    }

    if (!filter->initialized) {
        filter->previous_value = (float)raw_value;
        filter->initialized = 1;
        return raw_value;
    }

    // Áp dụng công thức IIR
    float filtered = filter->alpha * (float)raw_value + 
                    (1.0f - filter->alpha) * filter->previous_value;
    filter->previous_value = filtered;

    return (uint16_t)filtered;
}
// Tinh giá trị trung bình
uint16_t average_filter(const uint16_t *data, uint16_t size)
{
    if (data == NULL || size == 0) {
        return 0;
    }

    uint32_t sum = 0;
    for (uint16_t i = 0; i < size; i++) {
        sum += data[i];
    }

    return (uint16_t)(sum / size);
}
// Ghi giá trị DAC
void dac_write(uint8_t dac_value)
{
    if (dac_handle == NULL) {
        return;
    }

}

uint8_t adc_to_dac(uint16_t adc_value)
{
    //12 bit
    if (adc_value > 4095) {
        adc_value = 4095;
    }

    // Chuyển đổi: 
    uint8_t dac_value = (uint8_t)(adc_value / 16);
    return dac_value;
}

void sliding_window_free(SlidingWindowFilter *filter)
{
    if (filter != NULL && filter->buffer != NULL) {
        free(filter->buffer);
        filter->buffer = NULL;
        filter->window_size = 0;
        filter->index = 0;
        filter->sum = 0;
        filter->filled = 0;
    }
}
