# ESP32 ADC/DAC Noise Filtering Module

## 📋 Mô tả Project

Project này triển khai module **ADC/DAC với xử lý nhiễu** cho ESP32, bao gồm ba thuật toán lọc khác nhau:

1. **Sliding Window Filter** - Lọc dùng trượt cửa sổ
2. **Lowpass Filter (IIR)** - Lọc thông thấp tuyến tính
3. **Average Filter** - Lọc trung bình đơn giản

---

## 🔌 Kết nối Phần cứng

### Biến Trở (Potentiometer)
```
Biến trở 10kΩ
├─ Pin 1 (GND)     → GND (ESP32)
├─ Pin 2 (Wiper)   → GPIO36 (ADC1_CHANNEL_0)
└─ Pin 3 (VCC)     → 3.3V (ESP32)
```

### DAC Output
```
DAC_CHANNEL_0 → GPIO25
├─ Để đầu ra analog từ DAC
└─ Có thể nối với loa, LED, hay mạch xử lý khác
```

### Serial Debug
```
USB to UART:
├─ TX → GPIO1 (U0TXD)
├─ RX → GPIO3 (U0RXD)
└─ GND → GND
```

---

## 📊 Các Bộ Lọc

### 1. **Sliding Window Filter**
- **Ưu điểm**: Tốc độ phản ứng nhanh, chính xác cao
- **Nhược điểm**: Tiêu tốn bộ nhớ
- **Công thức**: `avg = sum(last N samples) / N`
- **Sử dụng**: Kích thước cửa sổ = 10 mẫu

```c
sliding_window_init(&sliding_filter, 10);
uint16_t filtered_value = sliding_window_filter(&sliding_filter, raw_adc);
```

---

### 2. **Lowpass Filter (IIR Butterworth)**
- **Ưu điểm**: Tiết kiệm bộ nhớ, tốc độ xử lý nhanh
- **Nhược điểm**: Độ trễ, cần calibrate alpha
- **Công thức IIR**: `y(n) = α·x(n) + (1-α)·y(n-1)`
  - `α = 0.1`: Mịn, độ trễ cao
  - `α = 0.3`: Balance (khuyến nghị)
  - `α = 0.5`: Phản ứng nhanh, ít lọc

```c
lowpass_init(&lowpass_filter_obj, 0.3f);
uint16_t filtered_value = lowpass_filter(&lowpass_filter_obj, raw_adc);
```

---

### 3. **Average Filter**
- **Ưu điểm**: Đơn giản, dễ hiểu
- **Nhược điểm**: Độ trễ, chỉ tối ưu cho dữ liệu tĩnh
- **Công thức**: `avg = sum(all samples) / count`

```c
uint16_t average_buffer[10];
uint16_t filtered_value = average_filter(average_buffer, 10);
```

---

## 🔢 Chuyển Đổi ADC ↔ DAC

### ADC Specifications
- **Channel**: ADC1_CHANNEL_0 (GPIO36)
- **Resolution**: 12-bit (0-4095)
- **Voltage Range**: 0-3.3V
- **Attenuation**: 12dB (full range)

### DAC Specifications
- **Channel**: DAC_CHANNEL_0 (GPIO25)
- **Resolution**: 8-bit (0-255)
- **Voltage Range**: 0-3.3V

### Conversion Formula
```
DAC_value = ADC_value / 16
(4095 / 255 ≈ 16)
```

---

## ⚙️ Cách Sử dụng

### 1. Khởi tạo
```c
adc_init();    // Khởi tạo ADC1
dac_init();    // Khởi tạo DAC
```

### 2. Lọc dữ liệu
```c
while (1) {
    // Đọc ADC thô
    uint16_t raw = adc_read_raw();
    
    // Áp dụng các bộ lọc
    uint16_t sliding = sliding_window_filter(&sliding_filter, raw);
    uint16_t lowpass = lowpass_filter(&lowpass_filter_obj, raw);
    uint16_t average = average_filter(buffer, 10);
    
    // Chuyển đổi và ghi DAC
    uint8_t dac_val = adc_to_dac(lowpass);
    dac_write(dac_val);
    
    vTaskDelay(pdMS_TO_TICKS(50));  // 50ms delay
}
```

---

## 📈 Kết quả Output

```
ADC_raw:  512 | Sliding: 510 | Lowpass: 511 | Average: 512 | DAC: 31
ADC_raw:  520 | Sliding: 514 | Lowpass: 513 | Average: 515 | DAC: 32
ADC_raw:  505 | Sliding: 513 | Lowpass: 512 | Average: 512 | DAC: 32
```

**Giải thích:**
- `ADC_raw`: Giá trị thô (không lọc)
- `Sliding`: Giá trị sau Sliding Window Filter
- `Lowpass`: Giá trị sau Lowpass Filter (được dùng cho DAC)
- `Average`: Giá trị sau Average Filter
- `DAC`: Giá trị ghi ra DAC (0-255)

---

## 🛠️ Chỉnh Cấu Hình

### Thay đổi Kích thước Cửa sổ Sliding
```c
// Trong main.c
sliding_window_init(&sliding_filter, 20);  // Thay từ 10 thành 20
```

### Thay đổi Hệ số Lowpass (Alpha)
```c
// Trong main.c
lowpass_init(&lowpass_filter_obj, 0.5f);  // Alpha: 0.1-0.5
```

### Thay đổi Thời gian Sample
```c
// Trong main.c, adc_dac_task()
vTaskDelay(pdMS_TO_TICKS(100));  // 100ms thay vì 50ms
```

---

## 🔧 Build và Flash

### 1. Build Project
```bash
idf.py build
```

### 2. Flash lên ESP32
```bash
idf.py flash
```

### 3. Monitor Output
```bash
idf.py monitor
```

### 4. Build + Flash + Monitor (một lệnh)
```bash
idf.py flash monitor
```

---

## 📝 Cấu trúc File

```
ADC_DAC/
├── CMakeLists.txt          # Project config
├── main/
│   ├── CMakeLists.txt      # Component config
│   ├── main.c              # Entry point + main task
│   ├── adc_dac.h           # Header file
│   └── adc_dac.c           # Implementation
└── README.md               # Documentation
```

---

## 🎯 Các Hàm Chính

| Hàm | Mô tả |
|-----|-------|
| `adc_init()` | Khởi tạo ADC1 |
| `dac_init()` | Khởi tạo DAC |
| `adc_read_raw()` | Đọc ADC thô |
| `sliding_window_init()` | Khởi tạo Sliding Window Filter |
| `sliding_window_filter()` | Lọc dữ liệu |
| `lowpass_init()` | Khởi tạo Lowpass Filter |
| `lowpass_filter()` | Lọc dữ liệu IIR |
| `average_filter()` | Lọc trung bình |
| `adc_to_dac()` | Chuyển đổi 12-bit → 8-bit |
| `dac_write()` | Ghi giá trị DAC |

---

## ⚡ Hiệu Năng

- **ADC Sampling Rate**: ~50ms/sample (có thể tăng)
- **Filter Latency**: <5ms
- **Memory Usage**: ~40KB (hoạt động)
- **Power Consumption**: ~100mA @ 3.3V

---

## 📚 Tài Liệu Tham Khảo

- [ESP-IDF ADC Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/adc.html)
- [ESP-IDF DAC Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/dac.html)
- [FreeRTOS Task](https://www.freertos.org/implementing-a-RTOS-task.html)

---

## ✅ Kiểm Tra

- [x] ADC đọc biến trở
- [x] DAC ghi giá trị
- [x] Sliding Window Filter hoạt động
- [x] Lowpass Filter hoạt động
- [x] Average Filter hoạt động
- [x] Serial output thành công

---

## 📞 Ghi chú

- Biến trở phải được kết nối chính xác để tránh giá trị ADC sai
- Nếu DAC không hoạt động, kiểm tra GPIO25 có bị sử dụng bởi pin khác không
- Có thể điều chỉnh các thông số lọc để phù hợp với ứng dụng cụ thể
