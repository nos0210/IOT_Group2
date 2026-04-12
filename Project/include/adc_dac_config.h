/**
 * @file adc_dac_config.h
 * @brief Tệp cấu hình chung cho module ADC/DAC
 * 
 * Sửa các giá trị ở đây để tùy chỉnh hành vi của module
 */

#ifndef ADC_DAC_CONFIG_H
#define ADC_DAC_CONFIG_H

// ============================================================
// SLIDING WINDOW FILTER CONFIGURATION
// ============================================================

/** Kích thước cửa sổ cho Sliding Window Filter (số mẫu) */
#define SLIDING_WINDOW_SIZE     10

// ============================================================
// LOWPASS FILTER CONFIGURATION  
// ============================================================

/** 
 * Hệ số Alpha cho Lowpass Filter IIR
 * Range: 0.0 - 1.0
 * 
 * α = 0.1   : Lọc rất mạnh, độ trễ cao, khó phản ứng nhanh
 * α = 0.3   : Balance (khuyến nghị mặc định)
 * α = 0.5   : Lọc vừa phải, phản ứng nhanh
 * α = 0.7   : Lọc yếu, phản ứng rất nhanh
 * 
 * Công thức: y(n) = α·x(n) + (1-α)·y(n-1)
 */
#define LOWPASS_ALPHA           0.3f

// ============================================================
// AVERAGE FILTER CONFIGURATION
// ============================================================

/** Kích thước buffer cho Average Filter */
#define AVERAGE_FILTER_SIZE     10

// ============================================================
// ADC CONFIGURATION
// ============================================================

/** ADC Channel (GPIO36 = ADC1_CHANNEL_0) */
#define ADC_CHANNEL             ADC_CHANNEL_0

/** ADC Resolution (12-bit = 0-4095) */
#define ADC_BITWIDTH            ADC_BITWIDTH_12

/** ADC Attenuation (12dB = 0-3.3V full range) */
#define ADC_ATTEN               ADC_ATTEN_DB_12

// ============================================================
// DAC CONFIGURATION
// ============================================================

/** DAC Channel (GPIO25 = DAC_CHAN_0) */
#define DAC_CHANNEL             DAC_CHAN_0

// ============================================================
// TASK CONFIGURATION
// ============================================================

/** Delay giữa các lần lấy mẫu ADC (milliseconds) */
#define ADC_SAMPLE_INTERVAL_MS  50

/** Stack size cho ADC_DAC_Task (bytes) */
#define ADC_TASK_STACK_SIZE     4096

/** Priority của ADC_DAC_Task */
#define ADC_TASK_PRIORITY       5

// ============================================================
// LOGGING CONFIGURATION
// ============================================================

/** Loại mẫu nào mới in ra log (mỗi N mẫu sẽ in 1) */
#define LOG_PRINT_INTERVAL      50

// ============================================================
// FILTER SELECTION
// ============================================================

/** Sử dụng loại filter nào cho DAC output 
 * 0 = Sliding Window
 * 1 = Lowpass
 * 2 = Average
 */
#define SELECTED_FILTER         1  // Mặc định: Lowpass

// ============================================================
// CONVERSION CONFIGURATION
// ============================================================

/** 
 * Tỷ số chuyển đổi ADC → DAC
 * ADC: 12-bit (0-4095)
 * DAC: 8-bit (0-255)
 * Tỷ số = 4095 / 255 ≈ 16
 */
#define ADC_TO_DAC_RATIO        16

// ============================================================
// DEBUG FLAGS
// ============================================================

/** In ra tất cả các giá trị hoặc chỉ theo INTERVAL */
#define DEBUG_PRINT_ALL         0

/** In chi tiết thông tin khởi tạo */
#define DEBUG_INIT_INFO         1

/** Bật/Tắt verification cho dữ liệu */
#define DEBUG_VERIFY_DATA       1

#endif // ADC_DAC_CONFIG_H
