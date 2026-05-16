
# deploy_ota.py — Ký, upload firmware lên S3 và trigger OTA qua AWS IoT Core MQTT.
# Cách dùng:
#     python deploy_ota.py <version> <firmware.bin>
#     python deploy_ota.py 1.2.0 build/ADC_DAC.bin
#
# Yêu cầu:
#     pip install boto3
#     AWS CLI đã cấu hình (aws configure) với quyền S3 + IoT
#     ESP-IDF environment đã được activate (để dùng espsecure)
import hashlib
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

import boto3
from botocore.exceptions import BotoCoreError, ClientError

# ── Cấu hình — ĐIỀN VÀO ĐÂY ──────────────────────────────────────────────────
BUCKET      = "esp32-firmware-2025"           # Tên S3 bucket
DEVICE_ID   = "esp32-001"                    # AWS IoT Thing name
REGION      = "ap-southeast-1"               # AWS region
URL_TTL     = 3600                            # Presigned URL hiệu lực (giây)
# Đường dẫn tới signing key (tạo bởi: python -m espsecure generate-signing-key --version 1)
SIGNING_KEY = Path("secure_boot_signing_key.pem")
# ─────────────────────────────────────────────────────────────────────────────


def md5_of_file(path: Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def is_firmware_signed(firmware: Path, key: Path) -> bool:
    """Kiểm tra firmware đã được ký bằng signing key chưa."""
    result = subprocess.run(
        [sys.executable, "-m", "espsecure", "verify-signature",
         "--version", "1",
         "--keyfile", str(key),
         str(firmware)],
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def sign_firmware(firmware: Path, key: Path) -> Path:
    """Ký firmware bằng Secure Boot V1 signing key. Trả về path của signed binary."""
    if not key.exists():
        print(f"Lỗi: Không tìm thấy signing key: {key}")
        print(f"      Tạo key bằng lệnh:")
        print(f"      python -m espsecure generate-signing-key --version 1 {key}")
        sys.exit(1)

    # Nếu build system (idf.py build với CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y)
    # đã ký sẵn, chỉ cần verify và dùng nguyên bản.
    if is_firmware_signed(firmware, key):
        print(f"[0/5] Firmware đã được ký sẵn (build system) — bỏ qua bước ký thủ công")
        return firmware

    signed = firmware.with_suffix(".signed.bin")
    print(f"[0/5] Ký firmware: {firmware.name}  →  {signed.name}")
    result = subprocess.run(
        [sys.executable, "-m", "espsecure", "sign-data",
         "--version", "1",
         "--keyfile", str(key),
         "--output", str(signed),
         str(firmware)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"Lỗi ký firmware:\n{result.stderr}")
        sys.exit(1)
    print(f"      Ký thành công ({signed.stat().st_size:,} bytes)")
    return signed


def upload_firmware(s3, path: Path, version: str) -> str:
    """Upload firmware đã ký lên S3, trả về S3 key."""
    s3_key = f"firmware/{version}/firmware.bin"
    print(f"[1/5] Uploading  {path}  →  s3://{BUCKET}/{s3_key}")
    s3.upload_file(
        str(path), BUCKET, s3_key,
        ExtraArgs={"ContentType": "application/octet-stream"},
    )
    print(f"      Upload hoàn thành ({path.stat().st_size:,} bytes)")
    return s3_key


def generate_presigned_url(s3, s3_key: str) -> str:
    """Tạo Presigned URL GET có thời hạn URL_TTL giây."""
    url = s3.generate_presigned_url(
        "get_object",
        Params={"Bucket": BUCKET, "Key": s3_key},
        ExpiresIn=URL_TTL,
    )
    print(f"[2/5] Presigned URL (expires {URL_TTL}s): {url[:80]}...")
    return url


def publish_ota_command(iot_data, version: str, url: str, md5: str) -> None:
    """Publish lệnh OTA lên topic devices/<id>/ota/command với QoS 1."""
    topic = f"devices/{DEVICE_ID}/ota/command"
    payload = json.dumps(
        {
            "version": version,
            "url": url,
            "md5": md5,
            "secure_boot": True,
            "issued": datetime.now(tz=timezone.utc).isoformat(),
        },
        ensure_ascii=False,
    )
    print(f"[3/5] Publishing OTA command  →  {topic}")
    iot_data.publish(topic=topic, qos=1, payload=payload.encode())
    print(f"      version={version}  md5={md5}")


def subscribe_status(iot_data) -> None:
    """Hiển thị hướng dẫn theo dõi trạng thái OTA."""
    status_topic = f"devices/{DEVICE_ID}/ota/status"
    print(f"\n[4/5] Theo dõi trạng thái OTA:")
    print(f"      AWS IoT Console  →  MQTT test client  →  Subscribe: {status_topic}")
    print(f"      hoặc dùng CLI:")
    print(f"      aws iot-data subscribe --topic \"{status_topic}\" --region {REGION}")
    print()
    print(f"[5/5] Kiểm tra Secure Boot trên thiết bị (serial log):")
    print(f"      Boot thành công  →  'secure boot verification succeeded'")
    print(f"      Firmware lạ      →  'secure boot verification failed' → rollback tự động\n")


def main() -> None:
    if len(sys.argv) < 3:
        print("Cách dùng: python deploy_ota.py <version> <firmware.bin>")
        print("Ví dụ   : python deploy_ota.py 1.2.0 build/ADC_DAC.bin")
        sys.exit(1)

    version  = sys.argv[1]
    firmware = Path(sys.argv[2])

    if not firmware.exists():
        print(f"Lỗi: file firmware không tồn tại: {firmware}")
        sys.exit(1)

    if not version.replace(".", "").isdigit():
        print(f"Cảnh báo: version '{version}' không đúng định dạng x.y.z")

    print(f"\n=== OTA Deploy (Secure Boot)  version={version}  firmware={firmware} ===")

    # Bước 0: Ký firmware (hoặc xác nhận đã ký bởi build system)
    signed_firmware = sign_firmware(firmware, SIGNING_KEY)

    # MD5 tính trên signed binary — đây là file thực sự được flash vào ESP32
    md5 = md5_of_file(signed_firmware)
    print(f"      Signed binary: {signed_firmware.name}")
    print(f"      MD5:           {md5}\n")

    try:
        s3       = boto3.client("s3",        region_name=REGION)
        iot_data = boto3.client("iot-data",  region_name=REGION)
    except (BotoCoreError, ClientError) as e:
        print(f"Lỗi kết nối AWS: {e}")
        sys.exit(1)

    try:
        s3_key = upload_firmware(s3, signed_firmware, version)
        url    = generate_presigned_url(s3, s3_key)
        publish_ota_command(iot_data, version, url, md5)
        subscribe_status(iot_data)
    except ClientError as e:
        print(f"\nAWS Error: {e.response['Error']['Code']} — {e.response['Error']['Message']}")
        sys.exit(1)
    except BotoCoreError as e:
        print(f"\nBotoCore Error: {e}")
        sys.exit(1)

    print("✅ OTA command đã được gửi thành công. Kiểm tra serial log trên ESP32.")


if __name__ == "__main__":
    main()
