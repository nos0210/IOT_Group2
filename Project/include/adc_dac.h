#ifndef ADC_DAC_H
#define ADC_DAC_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Cấu trúc lưu trữ dữ liệu cho bộ lọc sliding window
 */
typedef struct {
    uint16_t *buffer;       // Mảng lưu dữ liệu
    uint16_t window_size;   // Kích thước cửa sổ
    uint16_t index;         // Chỉ số hiện tại
    uint32_t sum;           // Tổng giá trị trong cửa sổ
    uint8_t filled;         // Cửa sổ có đầy chưa
} SlidingWindowFilter;

/**
 * @brief Cấu trúc lưu trữ dữ liệu cho bộ lọc Lowpass
 */
typedef struct {
    float alpha;            // Hệ số làm mịn (0-1)
    float previous_value;   // Giá trị lọc trước đó
    uint8_t initialized;    // Đã khởi tạo chưa
} LowpassFilter;

/**
 * @brief Khởi tạo ADC1 cho ESP32
 * - ADC1_CHANNEL_0 (GPIO36) cho biến trở
 * - Độ phân giải: 12-bit (0-4095)
 */
void adc_init(void);

/**
 * @brief Khởi tạo DAC cho ESP32
 * - DAC_CHANNEL_1 (GPIO25)
 * - Độ phân giải: 8-bit (0-255)
 */
void dac_init(void);

/**
 * @brief Đọc giá trị ADC từ biến trở (không lọc)
 * @return Giá trị ADC thô (0-4095)
 */
uint16_t adc_read_raw(void);

/**
 * @brief Khởi tạo bộ lọc Sliding Window
 * @param filter Con trỏ tới cấu trúc bộ lọc
 * @param window_size Kích thước cửa sổ (số mẫu)
 */
void sliding_window_init(SlidingWindowFilter *filter, uint16_t window_size);

/**
 * @brief Lọc dữ liệu sử dụng Sliding Window Filter
 * @param filter Con trỏ tới cấu trúc bộ lọc
 * @param raw_value Giá trị ADC thô
 * @return Giá trị trung bình trong cửa sổ
 */
uint16_t sliding_window_filter(SlidingWindowFilter *filter, uint16_t raw_value);

/**
 * @brief Khởi tạo bộ lọc Lowpass IIR
 * @param filter Con trỏ tới cấu trúc bộ lọc
 * @param alpha Hệ số làm mịn (0.1-0.5 khuyến nghị)
 */
void lowpass_init(LowpassFilter *filter, float alpha);

/**
 * @brief Lọc dữ liệu sử dụng Lowpass Filter
 * @param filter Con trỏ tới cấu trúc bộ lọc
 * @param raw_value Giá trị ADC thô
 * @return Giá trị đã lọc
 */
uint16_t lowpass_filter(LowpassFilter *filter, uint16_t raw_value);

/**
 * @brief Tính giá trị trung bình của mảng (Simple Average)
 * @param data Mảng dữ liệu
 * @param size Số phần tử trong mảng
 * @return Giá trị trung bình
 */
uint16_t average_filter(const uint16_t *data, uint16_t size);

/**
 * @brief Ghi giá trị DAC
 * @param dac_value Giá trị DAC (0-255)
 */
void dac_write(uint8_t dac_value);

/**
 * @brief Chuyển đổi giá trị ADC (12-bit) thành DAC (8-bit)
 * @param adc_value Giá trị ADC (0-4095)
 * @return Giá trị DAC (0-255)
 */
uint8_t adc_to_dac(uint16_t adc_value);

/**
 * @brief Giải phóng bộ nhớ bộ lọc Sliding Window
 * @param filter Con trỏ tới cấu trúc bộ lọc
 */
void sliding_window_free(SlidingWindowFilter *filter);

#endif // ADC_DAC_H
