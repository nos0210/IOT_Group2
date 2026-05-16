```
Topic
    - Subcribe: devices/esp32-001/ota/status
    - Publish: devices/esp32-001/ota/command
Payload:
{
  "version": "test-1.0",
  "url": "https://example.com/test.bin",
  "md5": "d41d8cd98f00b204e9800998ecf8427e"
}
```
```
Security config:
AWS IoT Core -> Security -> Policy
Json config:
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "iot:Connect",
      "Resource": "arn:aws:iot:ap-southeast-1:*:client/esp32-*"
    },
    {
      "Effect": "Allow",
      "Action": "iot:Subscribe",
      "Resource": [
        "arn:aws:iot:ap-southeast-1:*:topicfilter/devices/esp32-*/ota/command",
        "arn:aws:iot:ap-southeast-1:*:topicfilter/devices/esp32-*/ota/status"
      ]
    },
    {
      "Effect": "Allow",
      "Action": "iot:Receive",
      "Resource": [
        "arn:aws:iot:ap-southeast-1:*:topic/devices/esp32-*/ota/command",
        "arn:aws:iot:ap-southeast-1:*:topic/devices/esp32-*/ota/status"
      ]
    },
    {
      "Effect": "Allow",
      "Action": "iot:Publish",
      "Resource": [
        "arn:aws:iot:ap-southeast-1:*:topic/devices/esp32-*/ota/status",
        "arn:aws:iot:ap-southeast-1:*:topic/devices/esp32-*/ota/command"
      ]
    }
  ]
}
```
```
Hướng dẫn sử setup AWS IOT core :
Hướng dẫn cấu hình AWS IoT Core + S3 trên PowerShell (Windows)

💡 Tất cả lệnh viết 1 dòng duy nhất, không dùng \ xuống dòng.


Bước 1 — Tạo S3 Bucket
powershell# Tạo bucket
aws s3 mb s3://esp32-firmware-2025 --region ap-southeast-1

# Tắt public access
aws s3api put-public-access-block --bucket esp32-firmware-2025 --public-access-block-configuration "BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true"

# Kiểm tra
aws s3 ls | findstr esp32
aws s3api get-public-access-block --bucket esp32-firmware-2025
Kết quả thành công:
2025-xx-xx xx:xx:xx esp32-firmware-2025

Bước 2 — Tạo IoT Thing
powershell# Tạo Thing
aws iot create-thing --thing-name "esp32-001" --region ap-southeast-1

# Kiểm tra
aws iot describe-thing --thing-name esp32-001 --region ap-southeast-1
Kết quả thành công:
json{
    "thingName": "esp32-001",
    "thingArn": "arn:aws:iot:ap-southeast-1:123456789012:thing/esp32-001"
}

Bước 3 — Tạo Certificate & Private Key

⚠️ File device.key chỉ tải được 1 lần duy nhất — lưu cẩn thận!

powershell# Tạo thư mục certs
mkdir certs
cd certs

# Tạo certificate + keys
aws iot create-keys-and-certificate --set-as-active --certificate-pem-outfile device.crt --public-key-outfile device.pub --private-key-outfile device.key --region ap-southeast-1

# Tải Amazon Root CA
Invoke-WebRequest -Uri "https://www.amazontrust.com/repository/AmazonRootCA1.pem" -OutFile "AmazonRootCA1.pem"

# Kiểm tra 3 file đã có
dir
Kết quả thành công — thấy 3 file:
device.crt
device.key
AmazonRootCA1.pem

📋 Quan trọng: Copy certificateArn từ output lệnh trên, dạng:
arn:aws:iot:ap-southeast-1:123456789012:cert/abc123...
Dùng ở Bước 5.


Bước 4 — Tạo IoT Policy
powershell# Quay về thư mục project
cd ..

# Tạo file policy.json
$policy = '{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"iot:Connect","Resource":"arn:aws:iot:ap-southeast-1:*:client/esp32-*"},{"Effect":"Allow","Action":["iot:Subscribe","iot:Receive"],"Resource":["arn:aws:iot:ap-southeast-1:*:topicfilter/devices/esp32-*/ota/command","arn:aws:iot:ap-southeast-1:*:topicfilter/devices/esp32-*/ota/status"]},{"Effect":"Allow","Action":"iot:Publish","Resource":"arn:aws:iot:ap-southeast-1:*:topic/devices/esp32-*/ota/status"}]}'
$policy | Out-File -FilePath "iot-policy.json" -Encoding utf8

# Tạo policy trên AWS
aws iot create-policy --policy-name "ESP32-OTA-Policy" --policy-document file://iot-policy.json --region ap-southeast-1
Kết quả thành công:
json{
    "policyName": "ESP32-OTA-Policy",
    "policyArn": "arn:aws:iot:ap-southeast-1:123456789012:policy/ESP32-OTA-Policy"
}

Bước 5 — Gắn Policy và Certificate vào Thing

📋 Thay <CERT_ARN> bằng certificateArn đã copy ở Bước 3.

powershell# Gắn Policy vào Certificate
aws iot attach-policy --policy-name "ESP32-OTA-Policy" --target "<CERT_ARN>" --region ap-southeast-1

# Gắn Certificate vào Thing
aws iot attach-thing-principal --thing-name "esp32-001" --principal "<CERT_ARN>" --region ap-southeast-1

# Kiểm tra
aws iot list-thing-principals --thing-name esp32-001 --region ap-southeast-1
Kết quả thành công:
json{
    "principals": [
        "arn:aws:iot:ap-southeast-1:123456789012:cert/abc123..."
    ]
}

Bước 6 — Lấy IoT Endpoint
powershellaws iot describe-endpoint --endpoint-type iot:Data-ATS --region ap-southeast-1
Kết quả — lưu lại địa chỉ này:
json{
    "endpointAddress": "xxxxxxxxxxxxxx-ats.iot.ap-southeast-1.amazonaws.com"
}

Bước 7 — Test kết nối MQTT
Cài mosquitto trước:

Tải tại: 👉 https://mosquitto.org/download/ → chọn Windows installer
Cài xong, thêm vào PATH: C:\Program Files\mosquitto

powershell# Kiểm tra mosquitto đã cài
mosquitto_sub --help
Mở 2 cửa sổ PowerShell:
Cửa sổ 1 — Subscribe (lắng nghe):
powershellmosquitto_sub --host xxxxxxxxxxxxxx-ats.iot.ap-southeast-1.amazonaws.com --port 8883 --cafile certs/AmazonRootCA1.pem --cert certs/device.crt --key certs/device.key --topic "devices/esp32-001/ota/status" --id "esp32-001" -v
Cửa sổ 2 — Publish (gửi test):
powershellmosquitto_pub --host xxxxxxxxxxxxxx-ats.iot.ap-southeast-1.amazonaws.com --port 8883 --cafile certs/AmazonRootCA1.pem --cert certs/device.crt --key certs/device.key --topic "devices/esp32-001/ota/status" --id "esp32-001-pub" --message "{\"status\":\"test\",\"device_id\":\"esp32-001\"}"
Kết quả thành công — Cửa sổ 1 hiển thị:
devices/esp32-001/ota/status {"status":"test","device_id":"esp32-001"}

✅ Checklist toàn bộ
□ Bước 1  Tạo S3 Bucket + tắt public access
□ Bước 2  Tạo IoT Thing (esp32-001)
□ Bước 3  Tạo Certificate → lưu device.crt, device.key, AmazonRootCA1.pem
□ Bước 4  Tạo IoT Policy
□ Bước 5  Gắn Policy + Certificate vào Thing
□ Bước 6  Lưu IoT Endpoint URL
□ Bước 7  Test MQTT → nhận message thành công ✅
```