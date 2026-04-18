import asyncio
import queue
import threading
import tkinter as tk
import traceback
from dataclasses import dataclass
from tkinter import messagebox, ttk
from typing import Optional

from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice

SERVICE_UUID = "12345678-1234-5678-1234-56789abcdef0"
SSID_CHAR_UUID = "12345678-1234-5678-1234-56789abcdef1"
PASS_CHAR_UUID = "12345678-1234-5678-1234-56789abcdef2"
STATUS_CHAR_UUID = "12345678-1234-5678-1234-56789abcdef3"


@dataclass
class DeviceItem:
    name: str
    address: str


class AsyncBLEWorker:
    def __init__(self, ui_queue: queue.Queue):
        self.ui_queue = ui_queue
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self._run_loop, daemon=True)
        self.thread.start()

        self.client: Optional[BleakClient] = None
        self.connected_device: Optional[BLEDevice] = None

    def _run_loop(self) -> None:
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    def submit(self, coro):
        return asyncio.run_coroutine_threadsafe(coro, self.loop)

    async def unpair_device(self, address: str) -> str:
        """Force-close any stale Windows BLE connection using bleak_winrt in-process."""
        try:
            import bleak_winrt.windows.devices.bluetooth as bt
            addr_int = int(address.replace(":", "").replace("-", ""), 16)
            dev = await bt.BluetoothLEDevice.from_bluetooth_address_async(addr_int)
            if dev is None:
                return f"Không tìm thấy {address} trong Windows."
            pairing = dev.device_information.pairing
            if pairing.is_paired:
                import bleak_winrt.windows.devices.enumeration as en
                result = await pairing.unpair_async()
                dev.close()
                return f"Đã unpair {address}: {result.status}"
            else:
                dev.close()  # Release WinRT reference → Windows closes the BLE connection
                return f"Đã ngắt kết nối Windows với {address}."
        except Exception as exc:
            return f"Unpair lỗi: {repr(exc)}"

    async def clear_gatt_cache(self, address: str) -> str:
        """Xóa GATT cache của Windows cho thiết bị — cần thiết sau reflash firmware."""
        try:
            import bleak_winrt.windows.devices.bluetooth as bt
            import bleak_winrt.windows.devices.bluetooth.genericattributeprofile as gatt

            addr_int = int(address.replace(":", "").replace("-", ""), 16)
            device = await bt.BluetoothLEDevice.from_bluetooth_address_async(addr_int)
            if device is None:
                return "Không tìm thấy device để clear cache."

            session = await gatt.GattSession.from_device_id_async(device.bluetooth_device_id)
            session.maintain_connection = False

            # get_gatt_services_async(UNCACHED) forces Windows to re-query device (clears cache)
            result = await device.get_gatt_services_async(bt.BluetoothCacheMode.UNCACHED)
            session.close()
            device.close()
            return f"Đã clear GATT cache, status={result.status}"
        except Exception as exc:
            return f"Clear cache lỗi (bỏ qua): {repr(exc)}"

    async def scan_devices(self) -> list[BLEDevice]:
        devices = await BleakScanner.discover(timeout=10.0, return_adv=True)
        result = []
        for _, (device, adv_data) in devices.items():
            uuids = [u.lower() for u in (adv_data.service_uuids or [])]
            name = (device.name or "").strip()
            if SERVICE_UUID in uuids or name.startswith("ESP32") or "PROV" in name.upper():
                result.append(device)
        return result

    async def connect(self, device: BLEDevice):
        if self.client and self.client.is_connected:
            await self.disconnect()

        address = device.address
        MAX_ATTEMPTS = 3
        CONNECT_TIMEOUT = 40.0
        last_exc: Optional[Exception] = None
        did_unpair = False
        fresh_device: Optional[BLEDevice] = None

        for attempt in range(MAX_ATTEMPTS):
            target = fresh_device if fresh_device is not None else (device if attempt == 0 else address)
            self.client = BleakClient(target, winrt=dict(use_cached_services=False))
            try:
                await self.client.connect(timeout=CONNECT_TIMEOUT)
                self.connected_device = device
                break
            except Exception as exc:
                last_exc = exc
                try:
                    await self.client.disconnect()
                except Exception:
                    pass
                self.client = None

                err_str = str(exc).lower()
                is_unreachable = "unreachable" in err_str or "not paired" in err_str

                if is_unreachable and not did_unpair:
                    did_unpair = True
                    self.ui_queue.put(("error", "[Fix] Unreachable → unpair..."))

                    unpair_msg = await self.unpair_device(address)
                    self.ui_queue.put(("error", f"[Fix] {unpair_msg}"))
                    await asyncio.sleep(6.0)

                    self.ui_queue.put(("error", "[Fix] Scan lại để lấy WinRT handle mới..."))
                    fresh_device = await BleakScanner.find_device_by_address(address, timeout=20.0)
                    if fresh_device is None:
                        raise RuntimeError(
                            f"Sau khi unpair+clear cache, không tìm thấy {address}.\n"
                            "Hãy Scan → Connect lại thủ công."
                        ) from exc
                    continue

                elif is_unreachable and attempt < MAX_ATTEMPTS - 1:
                    wait = 3.0 * (attempt + 1)
                    self.ui_queue.put(("error", f"[Fix] Retry {attempt+1}/{MAX_ATTEMPTS} sau {wait:.0f}s..."))
                    await asyncio.sleep(wait)
                    continue

                elif attempt >= MAX_ATTEMPTS - 1:
                    raise RuntimeError(
                        f"Không thể connect BLE tới {device.name} ({address}) sau {MAX_ATTEMPTS} lần.\n"
                        f"Nguyên nhân: {repr(last_exc)}\n\n"
                        "Thử thủ công: Settings → Bluetooth → xóa ESP32 → Scan lại"
                    ) from exc

                if attempt < MAX_ATTEMPTS - 1:
                    await asyncio.sleep(1.5 * (attempt + 1))

        if not self.client or not self.client.is_connected:
            raise RuntimeError(f"Kết nối BLE không thành công: {repr(last_exc)}")

        services = self.client.services

        def _has_char(uuid_text: str) -> bool:
            return any(c.uuid.lower() == uuid_text for svc in services for c in svc.characteristics)

        missing = [u for u in [SSID_CHAR_UUID, PASS_CHAR_UUID, STATUS_CHAR_UUID] if not _has_char(u)]
        if missing:
            await self.client.disconnect()
            self.client = None
            self.connected_device = None
            raise RuntimeError("Thiếu characteristics: " + ", ".join(missing))

        def on_status(_: int, data: bytearray):
            try:
                text = data.decode("utf-8", errors="ignore")
            except Exception:
                text = str(bytes(data))
            self.ui_queue.put(("status_notify", text))

        await self.client.start_notify(STATUS_CHAR_UUID, on_status)

    async def send_credentials(self, ssid: str, password: str):
        if not self.client or not self.client.is_connected:
            raise RuntimeError("Chưa kết nối BLE")

        await self.client.write_gatt_char(SSID_CHAR_UUID, ssid.encode("utf-8"), response=True)
        await self.client.write_gatt_char(PASS_CHAR_UUID, password.encode("utf-8"), response=True)

    async def disconnect(self):
        if self.client:
            try:
                if self.client.is_connected:
                    try:
                        await self.client.stop_notify(STATUS_CHAR_UUID)
                    except (KeyError, Exception):
                        pass
                    await self.client.disconnect()
            finally:
                self.client = None
                self.connected_device = None


class ProvisionGUI(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("ESP32 WiFi BLE Provision")
        self.geometry("760x520")
        self.minsize(700, 500)

        self.ui_queue: queue.Queue = queue.Queue()
        self.worker = AsyncBLEWorker(self.ui_queue)

        self.devices: list[BLEDevice] = []

        self._build_ui()
        self.after(120, self._poll_ui_queue)

    def _build_ui(self):
        root = ttk.Frame(self, padding=12)
        root.pack(fill=tk.BOTH, expand=True)

        top = ttk.LabelFrame(root, text="BLE Devices", padding=10)
        top.pack(fill=tk.BOTH, expand=False)

        action_row = ttk.Frame(top)
        action_row.pack(fill=tk.X, pady=(0, 8))

        self.btn_scan = ttk.Button(action_row, text="Scan", command=self.on_scan)
        self.btn_scan.pack(side=tk.LEFT)

        self.btn_connect = ttk.Button(action_row, text="Connect", command=self.on_connect)
        self.btn_connect.pack(side=tk.LEFT, padx=8)

        self.btn_disconnect = ttk.Button(action_row, text="Disconnect", command=self.on_disconnect)
        self.btn_disconnect.pack(side=tk.LEFT)

        self.btn_unpair = ttk.Button(action_row, text="Xóa Pairing", command=self.on_unpair)
        self.btn_unpair.pack(side=tk.LEFT, padx=(8, 0))

        self.btn_clear_cache = ttk.Button(action_row, text="Reset Cache", command=self.on_clear_cache)
        self.btn_clear_cache.pack(side=tk.LEFT, padx=(8, 0))

        self.device_list = tk.Listbox(top, height=8)
        self.device_list.pack(fill=tk.BOTH, expand=True)

        creds = ttk.LabelFrame(root, text="WiFi Credentials", padding=10)
        creds.pack(fill=tk.X, expand=False, pady=(12, 0))

        ttk.Label(creds, text="SSID").grid(row=0, column=0, sticky="w")
        self.entry_ssid = ttk.Entry(creds, width=42)
        self.entry_ssid.grid(row=0, column=1, sticky="ew", padx=(8, 0))

        ttk.Label(creds, text="Password").grid(row=1, column=0, sticky="w", pady=(8, 0))
        self.entry_pass = ttk.Entry(creds, width=42, show="*")
        self.entry_pass.grid(row=1, column=1, sticky="ew", padx=(8, 0), pady=(8, 0))

        self.btn_send = ttk.Button(creds, text="Gửi", command=self.on_send)
        self.btn_send.grid(row=2, column=1, sticky="e", pady=(10, 0))
        creds.columnconfigure(1, weight=1)

        status_frame = ttk.LabelFrame(root, text="Status", padding=10)
        status_frame.pack(fill=tk.BOTH, expand=True, pady=(12, 0))

        self.status_text = tk.Text(status_frame, height=12, state=tk.DISABLED)
        self.status_text.pack(fill=tk.BOTH, expand=True)

        self.lbl_conn = ttk.Label(root, text="Chưa kết nối")
        self.lbl_conn.pack(anchor="w", pady=(8, 0))

    def log(self, text: str):
        self.status_text.config(state=tk.NORMAL)
        self.status_text.insert(tk.END, text + "\n")
        self.status_text.see(tk.END)
        self.status_text.config(state=tk.DISABLED)

    def set_busy(self, busy: bool):
        state = tk.DISABLED if busy else tk.NORMAL
        self.btn_scan.configure(state=state)
        self.btn_connect.configure(state=state)
        self.btn_unpair.configure(state=state)
        self.btn_clear_cache.configure(state=state)
        self.btn_send.configure(state=state)

    def on_scan(self):
        self.log("Đang scan BLE...")
        self.set_busy(True)

        future = self.worker.submit(self.worker.scan_devices())

        def done_callback(fut):
            try:
                devices = fut.result()
                self.ui_queue.put(("scan_done", devices))
            except Exception as exc:
                self.ui_queue.put(("error", f"Scan lỗi: {type(exc).__name__}: {repr(exc)}"))

        future.add_done_callback(done_callback)

    def on_connect(self):
        idxs = self.device_list.curselection()
        if not idxs:
            messagebox.showwarning("Thiếu thiết bị", "Hãy chọn thiết bị BLE trước")
            return

        device = self.devices[idxs[0]]
        self.log(f"Đang connect: {device.name} ({device.address})")
        self.set_busy(True)

        future = self.worker.submit(self.worker.connect(device))

        def done_callback(fut):
            try:
                fut.result()
                self.ui_queue.put(("connected", device))
            except Exception as exc:
                self.ui_queue.put(("error", f"Connect lỗi: {type(exc).__name__}: {repr(exc)}"))
                self.ui_queue.put(("error", traceback.format_exc().strip()))

        future.add_done_callback(done_callback)

    def on_unpair(self):
        idxs = self.device_list.curselection()
        if not idxs:
            messagebox.showwarning("Thiếu thiết bị", "Hãy chọn thiết bị BLE trước")
            return
        device = self.devices[idxs[0]]
        self.log(f"Đang xóa pairing: {device.name} ({device.address})...")
        self.set_busy(True)
        future = self.worker.submit(self.worker.unpair_device(device.address))
        def done_callback(fut):
            try:
                msg = fut.result()
                self.ui_queue.put(("error", f"[Unpair] {msg}"))
            except Exception as exc:
                self.ui_queue.put(("error", f"Unpair lỗi: {repr(exc)}"))
            finally:
                self.ui_queue.put(("unpair_done", None))
        future.add_done_callback(done_callback)

    def on_clear_cache(self):
        idxs = self.device_list.curselection()
        if not idxs:
            messagebox.showwarning("Thiếu thiết bị", "Hãy chọn thiết bị BLE trước")
            return
        device = self.devices[idxs[0]]
        self.log(f"Đang clear GATT cache: {device.address}...")
        self.set_busy(True)
        future = self.worker.submit(self.worker.clear_gatt_cache(device.address))
        def done_callback(fut):
            try:
                msg = fut.result()
                self.ui_queue.put(("error", f"[Cache] {msg}"))
            except Exception as exc:
                self.ui_queue.put(("error", f"Cache lỗi: {repr(exc)}"))
            finally:
                self.ui_queue.put(("cache_done", None))
        future.add_done_callback(done_callback)

    def on_disconnect(self):
        future = self.worker.submit(self.worker.disconnect())

        def done_callback(fut):
            try:
                fut.result()
                self.ui_queue.put(("disconnected", None))
            except Exception as exc:
                self.ui_queue.put(("error", f"Disconnect lỗi: {type(exc).__name__}: {repr(exc)}"))

        future.add_done_callback(done_callback)

    def on_send(self):
        ssid = self.entry_ssid.get().strip()
        password = self.entry_pass.get()

        if not ssid:
            messagebox.showwarning("Thiếu SSID", "SSID không được để trống")
            return
        if not password:
            messagebox.showwarning("Thiếu Password", "Password không được để trống")
            return
        if len(ssid.encode("utf-8")) > 32:
            messagebox.showwarning("SSID quá dài", "SSID tối đa 32 bytes")
            return
        if len(password.encode("utf-8")) > 64:
            messagebox.showwarning("Password quá dài", "Password tối đa 64 bytes")
            return

        self.log("Đang gửi SSID/PASSWORD...")
        self.set_busy(True)

        future = self.worker.submit(self.worker.send_credentials(ssid, password))

        def done_callback(fut):
            try:
                fut.result()
                self.ui_queue.put(("sent", None))
            except Exception as exc:
                self.ui_queue.put(("error", f"Gửi lỗi: {type(exc).__name__}: {repr(exc)}"))

        future.add_done_callback(done_callback)

    def _poll_ui_queue(self):
        while True:
            try:
                event, payload = self.ui_queue.get_nowait()
            except queue.Empty:
                break

            if event == "scan_done":
                self.devices = payload
                self.device_list.delete(0, tk.END)
                for dev in payload:
                    label = f"{dev.name or 'Unknown'} | {dev.address}"
                    self.device_list.insert(tk.END, label)
                self.log(f"Scan xong: tìm thấy {len(payload)} thiết bị")
                self.set_busy(False)

            elif event == "connected":
                dev = payload
                self.lbl_conn.configure(text=f"Đã kết nối: {dev.name} ({dev.address})")
                self.log("BLE connected. Đã subscribe STATUS notify.")
                self.set_busy(False)

            elif event == "disconnected":
                self.lbl_conn.configure(text="Chưa kết nối")
                self.log("Đã ngắt kết nối BLE")
                self.set_busy(False)

            elif event == "sent":
                self.log("Gửi credentials thành công, chờ notify từ ESP32...")
                self.set_busy(False)

            elif event == "status_notify":
                self.log(f"ESP32 notify: {payload}")
                self.set_busy(False)

            elif event == "error":
                self.log(payload)
                self.set_busy(False)

            elif event == "unpair_done":
                self.log("Unpair xong. Hãy nhấn Scan rồi Connect lại.")
                self.set_busy(False)

            elif event == "cache_done":
                self.log("Reset cache xong. Hãy nhấn Connect lại.")
                self.set_busy(False)

        self.after(120, self._poll_ui_queue)

    def destroy(self):
        try:
            future = self.worker.submit(self.worker.disconnect())
            future.result(timeout=3)
        except Exception:
            pass

        try:
            self.worker.loop.call_soon_threadsafe(self.worker.loop.stop)
        except Exception:
            pass

        super().destroy()


if __name__ == "__main__":
    app = ProvisionGUI()
    app.mainloop()
