#ifndef ADC_DAC_H
#define ADC_DAC_H

#include <stdint.h>
#include <stddef.h>


typedef struct {
    uint16_t *buffer;       // Mảng lưu dữ liệu
    uint16_t window_size;   // Kích thước cửa sổ
    uint16_t index;         // Chỉ số hiện tại
    uint32_t sum;           // Tổng giá trị trong cửa sổ
    uint8_t filled;         // Cửa sổ có đầy chưa
} SlidingWindowFilter;


typedef struct {
    float alpha;            // Hệ số làm mịn (0-1)
    float previous_value;   // Giá trị lọc trước đó
    uint8_t initialized;    // Đã khởi tạo chưa
} LowpassFilter;


void adc_init(void);


void dac_init(void);


uint16_t adc_read_raw(void);


void sliding_window_init(SlidingWindowFilter *filter, uint16_t window_size);


uint16_t sliding_window_filter(SlidingWindowFilter *filter, uint16_t raw_value);


void lowpass_init(LowpassFilter *filter, float alpha);


uint16_t lowpass_filter(LowpassFilter *filter, uint16_t raw_value);


uint16_t average_filter(const uint16_t *data, uint16_t size);

void dac_write(uint8_t dac_value);


uint8_t adc_to_dac(uint16_t adc_value);


void sliding_window_free(SlidingWindowFilter *filter);

#endif // ADC_DAC_H
