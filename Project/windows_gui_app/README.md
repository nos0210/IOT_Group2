# ESP32 WiFi Provisioning GUI

```
pip install -r requirements.txt
```

3. Start app:

```
python app.py
```

## BLE UUIDs

- Service: `12345678-1234-5678-1234-56789abcdef0`
- SSID write: `12345678-1234-5678-1234-56789abcdef1`
- PASS write: `12345678-1234-5678-1234-56789abcdef2`
- STATUS notify/read: `12345678-1234-5678-1234-56789abcdef3`
```
Chạy firmware ESP32

Mở ESP-IDF PowerShell (đúng môi trường IDF 6.0).

Vào thư mục project:
C:/Users/Admin/Desktop/esp/IOT_PROJECT_GROUP2/Project

Build:
idf.py build

Flash + monitor (đổi COM5 theo máy bạn):
idf.py -p COM5 flash monitor

Chạy app Windows BLE

Mở PowerShell mới.

Vào thư mục app:
C:/Users/Admin/Desktop/esp/IOT_PROJECT_GROUP2/Project/windows_gui_app

Cài thư viện:
pip install -r requirements.txt

Chạy app:
python app.py

Test end-to-end

ESP32 đang chạy và advertising BLE.

Trong app bấm Scan.

Chọn thiết bị ESP32 rồi Connect.

Nhập SSID + Password.

Bấm Gửi.

Quan sát status notify trả về (WIFI_CONNECTING, WIFI_CONNECTED hoặc lỗi).

Nếu gặp lỗi Permission denied của Ninja khi build

Đóng tất cả terminal đang build.

Xóa thư mục build trong project.

Mở lại ESP-IDF PowerShell và chạy lại:
idf.py build
idf.py -p COM7 monitor
```
python -m esptool --chip esp32 -p COM7 -b 115200 --before no-reset --after hard-reset write-flash 0x1000 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/ADC_DAC.bin

```
idf.py -p COM7 -b 115200 -D ESPTOOLPY_BEFORE=no-reset -D ESPTOOLPY_AFTER=hard-reset flash
```
idf.py -p COM7 -b 115200 flash
