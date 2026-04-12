# ESP32 ADC/DAC Implementation - Best Practices & Notes

## 📌 Giới Thiệu

Đây là tài liệu tham khảo chi tiết cho module ADC/DAC với xử lý nhiễu trên ESP32.

---

## 🎯 Chiến Lược Lọc Dữ Liệu

### 1. **Sliding Window Filter** (Cửa sổ Trượt)

#### Nguyên Lý
- Duyệt qua N mẫu gần nhất
- Tính trung bình của chúng
- "Trượt" cửa sổ sang mẫu mới

#### Công Thức
```
y(n) = [x(n) + x(n-1) + ... + x(n-N+1)] / N
```

#### Ưu/Nhược
✅ Chính xác cao  
✅ Không độ trễ lớn  
❌ Tiêu tốn bộ nhớ động  
❌ Phải cấp phát/giải phóng bộ nhớ  

#### Khi nào dùng
- Dữ liệu biến đổi nhanh
- Có bộ nhớ dư thừa
- Cần độ chính xác cao

#### Cấu hình
```c
SlidingWindowFilter filter;
sliding_window_init(&filter, 10);       // 10-sample window
uint16_t filtered = sliding_window_filter(&filter, raw_value);
```

#### Ví dụ
```
Input:  100  200  150  180  220  190
Output: 100  150  150  176  183  187
(Trung bình trượt của 3 mẫu)
```

---

### 2. **Lowpass Filter (IIR)** - **KHUYẾN NGHỊ**

#### Nguyên Lý - Công Thức Butterworth
```
y(n) = α·x(n) + (1-α)·y(n-1)
```

Các thành phần:
- `x(n)` = Giá trị ADC hiện tại
- `y(n-1)` = Giá trị lọc trước đó
- `α` = Hệ số độ nhạy (0.0-1.0)

#### Alpha (α) Sensitivity

| α | Tên | Lọc | Độ Trễ | Sử Dụng |
|---|-----|-----|--------|---------|
| 0.05 | Very Strong | Rất mạnh | Cao | Nhiễu cực lớn |
| 0.1 | Strong | Mạnh | Cao | Nhiễu lớn |
| **0.3** | **Medium** | **Vừa** | **Vừa** | **Khuyến nghị** |
| 0.5 | Weak | Yếu | Thấp | Phản ứng nhanh |
| 0.7 | Very Weak | Rất yếu | Rất thấp | Dữ liệu sạch |

#### Ưu/Nhược
✅ Tiết kiệm bộ nhớ  
✅ Tốc độ xử lý nhanh  
✅ Không cần bộ nhớ động  
❌ Độ trễ cao (tuỳ α)  
❌ Cần calibrate tham số  

#### Khi nào dùng
- Thiết bị có bộ nhớ hạn chế
- Cần xử lý real-time
- Dữ liệu thay đổi từng chút
- **MỨC ĐỘ KHUYẾN NGHỊ: TẤT CẢ CÁC TRƯỜNG HỢP**

#### Cấu hình
```c
LowpassFilter filter;
lowpass_init(&filter, 0.3f);            // α = 0.3
uint16_t filtered = lowpass_filter(&filter, raw_value);
```

#### Giải Thích Chi Tiết

Ví dụ: α = 0.3

```
Bước 1:
  Raw Input: 100
  Previous Output: 0
  y = 0.3 × 100 + 0.7 × 0 = 30

Bước 2:
  Raw Input: 200
  Previous Output: 30
  y = 0.3 × 200 + 0.7 × 30 = 60 + 21 = 81

Bước 3:
  Raw Input: 150
  Previous Output: 81
  y = 0.3 × 150 + 0.7 × 81 = 45 + 56.7 = 101.7 ≈ 102

Chuỗi: 100 → 200 → 150 becomes 30 → 81 → 102 (smooth)
```

---

### 3. **Average Filter** (Trung Bình)

#### Nguyên Lý
```
y = (x₁ + x₂ + ... + xₙ) / n
```

#### Ưu/Nhược
✅ Đơn giản, dễ hiểu  
✅ Nhanh  
❌ Độ trễ cao  
❌ Chỉ tốt cho dữ liệu tĩnh  

#### Khi nào dùng
- Dữ liệu ổn định
- Sensor đo lường tĩnh

#### Cấu hình
```c
uint16_t buffer[10];
uint16_t filtered = average_filter(buffer, 10);
```

---

## 📊 So Sánh Các Bộ Lọc

```
              Tốc độ   Bộ nhớ   Độ trễ   Chính xác
Sliding        ⭐⭐    ⭐      ⭐⭐⭐   ⭐⭐⭐⭐
Lowpass        ⭐⭐⭐  ⭐⭐⭐  ⭐⭐    ⭐⭐⭐
Average        ⭐⭐⭐  ⭐⭐⭐  ⭐      ⭐⭐

Khuyến nghị: Lowpass (Best overall)
```

---

## 🔌 ADC Hardware Configuration

### GPIO Map
| Chức năng | GPIO | Kênh ADC |
|-----------|------|---------|
| Potentiometer Input | GPIO36 | ADC1_CHANNEL_0 |
| DAC Output | GPIO25 | DAC_CHANNEL_0 |

### ADC Specifications
- **Unit**: ADC1
- **Resolution**: 12-bit (0-4095)
- **Voltage Range**: 0-3.3V
- **Attenuation**: 12dB (full range 0-3.3V)
- **Sampling Speed**: ~40 µs/sample

### Công Thức Chuyển Đổi Điện Áp
```
Voltage (V) = ADC_value × 3.3 / 4095
              
Ví dụ:
ADC = 2048 → V = 2048 × 3.3 / 4095 ≈ 1.65V
```

---

## 🛠️ ADC/DAC Conversion

### ADC → DAC
```
DAC_value = ADC_value / 16

Ví dụ:
ADC = 4095 (3.3V)  → DAC = 255 (3.3V)
ADC = 2048 (1.65V) → DAC = 128 (1.65V)
ADC = 0 (0V)       → DAC = 0 (0V)
```

### Cơ Sở Toán Học
```
Tỷ số = ADC_max / DAC_max = 4095 / 255 ≈ 16.05
DAC_value = uint8_t(ADC_value / 16)
```

---

## 🚀 Task & Scheduling

### FreeRTOS Task Configuration

```c
xTaskCreate(adc_dac_task,      // Hàm entry
            "ADC_DAC_Task",    // Tên
            4096,              // Stack (bytes)
            NULL,              // Parameters
            5,                 // Priority (0-24)
            NULL);             // Task handle
```

### Ưu Tiên Ưa Thích
```
Priority 5-10: ADC/DAC processing
Priority 1-4: Low priority tasks (logging, UI)
```

---

## 📈 Performance Tuning

### Tối Ưu Hóa Tốc Độ

1. **Tăng ADC Sample Rate**
   ```c
   vTaskDelay(pdMS_TO_TICKS(10));  // 10ms thay vì 50ms
   ```

2. **Giảm Filter Complexity**
   ```c
   sliding_window_init(&filter, 5);  // 5 mẫu thay vì 20
   ```

3. **Tối Ưu Hoá Lowpass**
   ```c
   lowpass_init(&filter, 0.5f);  // α cao = ít lọc, nhanh hơn
   ```

### Giảm Bộ Nhớ

1. **Không sử dụng Sliding Window**
   - Dùng Lowpass IIR thay vào
   - Tiết kiệm ~50 bytes mỗi filter

2. **Giảm Buffer Size**
   ```c
   average_buffer[5];  // 5 mẫu thay vì 10
   ```

---

## 🐛 Debugging & Troubleshooting

### ADC Không Đọc Được

**Symptoms**: ADC luôn trả về 0 hoặc 4095

**Solutions**:
```c
// 1. Kiểm tra GPIO36 có bị sử dụng không
// 2. Kiểm tra biến trở có kết nối đúng không
// 3. Kiểm tra attenuation:
lowpass_init(&filter, 0.3f);

// 4. Thử đọc ADC thô
uint16_t raw = adc_read_raw();
ESP_LOGI(TAG, "ADC raw: %d", raw);
```

### DAC Không Có Output

**Symptoms**: GPIO25 không có điện áp

**Solutions**:
```c
// 1. Kiểm tra DAC đã khởi tạo:
dac_init();

// 2. Thử ghi giá trị cố định:
dac_write(128);  // 1.65V

// 3. Kiểm tra GPIO25 không bị xung đột:
// GPIO25 không nên dùng ở chỗ khác
```

### Dữ Liệu Lọc Không Thay Đổi

**Symptoms**: Output filter luôn là giá trị cố định

**Solutions**:
```c
// 1. Kiểm tra α (alpha) quá nhỏ:
lowpass_init(&filter, 0.5f);  // Tăng từ 0.1 lên 0.5

// 2. Kiểm tra window size:
sliding_window_init(&filter, 5);  // Giảm từ 20 xuống 5

// 3. Kiểm tra ADC input thay đổi:
uint16_t raw = adc_read_raw();
ESP_LOGI(TAG, "Raw: %d", raw);  // Phải thay đổi
```

---

## 📚 API Reference Quick

| Function | Purpose | Return |
|----------|---------|--------|
| `adc_init()` | Khởi tạo ADC | void |
| `dac_init()` | Khởi tạo DAC | void |
| `adc_read_raw()` | Đọc ADC thô | uint16_t |
| `sliding_window_init()` | Khởi tạo SW filter | void |
| `sliding_window_filter()` | Lọc dữ liệu | uint16_t |
| `lowpass_init()` | Khởi tạo LP filter | void |
| `lowpass_filter()` | Lọc dữ liệu | uint16_t |
| `average_filter()` | Lọc trung bình | uint16_t |
| `adc_to_dac()` | Chuyển đổi ADC→DAC | uint8_t |
| `dac_write()` | Ghi DAC | void |
| `sliding_window_free()` | Giải phóng bộ nhớ | void |

---

## ✅ Checklist Triển Khai

- [ ] Kiểm tra kết nối phần cứng
- [ ] Biên dịch test units
- [ ] Chạy `run_all_tests()`
- [ ] Kiểm tra ADC đọc được giá trị
- [ ] Kiểm tra DAC ghi được giá trị
- [ ] So sánh các filter
- [ ] Lựa chọn filter tốt nhất
- [ ] Calibrate tham số
- [ ] Test trên thiết bị thực

---

## 🔗 Tài Liệu Tham Khảo

- ESP-IDF ADC: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/adc.html
- ESP-IDF DAC: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/dac.html
- Digital Signal Processing Filters
- FreeRTOS Documentation

---

## 📝 Ghi chú cuối cùng

> **Khuyến nghị chung**: Sử dụng **Lowpass Filter với α=0.3** cho hầu hết các ứng dụng. Nó cung cấp cân bằng tốt giữa độ mịn, tốc độ phản ứng và hiệu suất.

