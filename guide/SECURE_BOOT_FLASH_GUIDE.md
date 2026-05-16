# Hướng dẫn Flash Secure Boot — ESP32 Group 2

## Trạng thái hiện tại ✅

| Bước | Trạng thái |
|------|-----------|
| Phase 1: Tạo signing key | ✅ Hoàn thành |
| Phase 2: Cấu hình sdkconfig | ✅ Hoàn thành |
| Phase 3: OTA integration | ✅ Hoàn thành |
| Phase 4: Build & Signature | ✅ **XONG** — Signature hợp lệ |
| Phase 4: Flash lên board | ⏳ Chờ thực hiện |
| Phase 5: Hardening sản xuất | ⏳ Chưa thực hiện |

**Binary đã ký**: `Project/build/ADC_DAC.bin` — 1,245,172 bytes — 16/05/2026 19:03 ✅  
**Kết quả verify**: `Signature is valid` ✅

---

## ⚠️ QUAN TRỌNG — Trước khi flash

### 1. Backup signing key (BẮT BUỘC)
```
Project/secure_boot_signing_key.pem
```
Copy file này ra USB/ổ cứng ngoài. **Mất key = không thể ký firmware mới**.

### 2. Kiểm tra COM port
Trong PowerShell:
```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```
Hoặc kiểm tra **Device Manager > Ports (COM & LPT)** khi cắm ESP32.

---

## Bước Flash (lần đầu — burn eFuse)

> ⚠️ **LƯU Ý QUAN TRỌNG — Board không tự reset qua DTR/RTS**  
> Board này KHÔNG hỗ trợ auto-reset. Mọi lệnh esptool/espefuse đều cần vào **Download Mode thủ công**:  
> **Giữ BOOT → nhấn RESET → thả cả hai → chạy lệnh ngay**

> ⚠️ **`idf.py flash` KHÔNG flash bootloader khi Secure Boot enabled**  
> Output thực tế: `"Secure boot enabled, so bootloader not flashed automatically"`  
> Phải dùng `esptool` thủ công để flash bootloader.

### Kích hoạt môi trường IDF
```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
& "C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1"
cd "C:\Users\Admin\Desktop\esp\IOT_PROJECT_GROUP2\Project"
```

---

### BƯỚC 1 — Burn Secure Boot key vào eFuse (CHỈ LÀM 1 LẦN)

> Nếu đã burn key rồi (từ session trước), **bỏ qua bước này**, chuyển sang Bước 2.  
> Để kiểm tra xem key đã burn chưa: xem mục "Xác minh eFuse" bên dưới.

1. Vào Download Mode: **Giữ BOOT → nhấn RESET → thả cả hai**
2. Chạy ngay:
```powershell
python -m espefuse -p COM7 burn-key secure_boot_v1 build\bootloader\secure-bootloader-key-256.bin
```
3. Khi hỏi xác nhận, nhập `YES`

---

### BƯỚC 2 — Flash bootloader + partition table + app

> **Đây là bước bắt buộc** — phải flash thủ công vì `idf.py flash` bỏ qua bootloader.

1. Vào Download Mode: **Giữ BOOT → nhấn RESET → thả cả hai**
2. Chạy ngay:
```powershell
python -m esptool --chip esp32 -p COM7 -b 460800 --before no-reset --after no-reset write-flash --flash-mode dio --flash-freq 40m --flash-size 4MB 0x1000 build\bootloader\bootloader.bin 0xd000 build\partition_table\partition-table.bin 0x13000 build\ota_data_initial.bin 0x20000 build\ADC_DAC.bin
```
3. Đợi flash hoàn thành (`Hash of data verified`)

---

### BƯỚC 3 — Boot lần đầu (kích hoạt ABS_DONE_0)

1. Mở monitor TRƯỚC khi reset (để bắt được log bootloader):
```powershell
idf.py -p COM7 monitor
```
2. Nhấn RESET trên board (KHÔNG giữ BOOT)
3. Quan sát log trong **3 giây đầu tiên** sau reset

### Log cần thấy sau khi flash

**Lần đầu boot (burn ABS_DONE_0) — chỉ xuất hiện 1 lần:**
```
I (xxx) secure_boot_v1: Generating secure boot digest...
I (xxx) secure_boot_v1: Digest generation complete.
I (xxx) secure_boot_v1: blowing secure boot efuse...
I (xxx) secure_boot_v1: secure boot is now enabled for bootloader image
```

**Các lần boot sau (eFuse đã burn):**
```
I (xxx) secure_boot_v1: bootloader secure boot is already enabled.
```

**Log từ app (thêm vào main.c):**
```
I (xxx) MAIN: === SECURE BOOT: ACTIVE (eFuse burned) ===
```

> **Lưu ý**: Log xác minh signature app chạy ở mức DEBUG — không hiển thị mặc định.
Tìm các dòng (ESP-IDF 6, Secure Boot V1):

**Lần đầu flash (burn eFuse) — chỉ xuất hiện 1 lần:**
```
I (xxx) secure_boot_v1: Generating secure boot digest...
I (xxx) secure_boot_v1: Digest generation complete.
I (xxx) secure_boot_v1: blowing secure boot efuse...
I (xxx) secure_boot_v1: secure boot is now enabled for bootloader image
```

**Các lần boot sau (eFuse đã burn):**
```
I (xxx) secure_boot_v1: bootloader secure boot is already enabled.
```

> **Lưu ý**: Log xác minh signature app chạy ở mức DEBUG — không hiển thị mặc định.
> Để xem thêm chi tiết, thêm `CONFIG_BOOTLOADER_LOG_LEVEL_DEBUG=y` vào sdkconfig.defaults.

---

## Sau khi flash — Xác minh eFuse

> Cần vào Download Mode: **Giữ BOOT → nhấn RESET → thả cả hai**, rồi chạy:

```powershell
python -m espefuse -p COM7 summary
```

Phải thấy:
```
SECURE_BOOT_KEY (BLOCK1) = [32 bytes of key data]  # key đã burn
ABS_DONE_0 = 0                                      # 0 = Reflashable mode
ABS_DONE_0 = 1 (sau lần boot đầu)                  # 1 = Secure Boot hoàn toàn active
```

---

## Xác minh Secure Boot hoạt động

### Thử flash firmware KHÔNG được ký (phải bị từ chối)
```powershell
# Firmware cũ chưa ký sẽ bị reject
idf.py -p COM5 flash   # Firmware mới luôn được ký tự động bởi build system
```

### Upload OTA thủ công (test)
```powershell
cd "C:\Users\Admin\Desktop\esp\IOT_PROJECT_GROUP2\Project"
python deploy_ota.py --firmware build/ADC_DAC.bin --version 1.1.0
```

---

## Phase 5 — Hardening sản xuất (CHƯA thực hiện)

Chỉ thực hiện khi sẵn sàng deploy thực tế (không thể hoàn tác):

### Chuyển Flash Encryption sang RELEASE mode
Sửa `Project/sdkconfig.defaults`:
```kconfig
# Xóa dòng này:
# CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT=y

# Thêm dòng này:
CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y
CONFIG_SECURE_DISABLE_ROM_DL_MODE=y
```

Sau đó rebuild và flash:
```powershell
idf.py reconfigure
idf.py build
idf.py -p COM5 flash
```

### Disable JTAG (tùy chọn)
```powershell
python -m espefuse -p COM5 burn_efuse JTAG_DISABLE
```

---

## Checklist bảo mật

- [x] `secure_boot_signing_key.pem` trong `.gitignore`
- [x] OTA firmware được ký trong `deploy_ota.py`
- [x] MD5 verification trong `aws_mqtt_ota_task.c`
- [x] Secure Boot V1 Reflashable cấu hình đúng
- [ ] Backup signing key offline
- [ ] Flash lên board và verify eFuse
- [ ] Test OTA với Secure Boot enabled
- [ ] (Production) Chuyển sang Flash Encryption RELEASE mode

---

## Files đã thay đổi trong session này

| File | Thay đổi |
|------|---------|
| `Project/sdkconfig.defaults` | Thêm Secure Boot + Flash Encryption configs |
| `Project/sdkconfig` | Regenerated với Secure Boot |
| `Project/partitions_ota.csv` | Di chuyển nvs/otadata lên 0xe000+ (tránh overlap 0xd000) |
| `Project/modules/aws_mqtt_ota_task.c` | Thêm MD5 verify, dùng `esp_https_ota` advanced API |
| `Project/deploy_ota.py` | Thêm signing step trước khi upload S3 |
| `.gitignore` | Bảo vệ key files và build output |
| `Project/secure_boot_signing_key.pem` | **MỚI** — ECDSA NIST-P256 signing key |
