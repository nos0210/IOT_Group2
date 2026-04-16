import asyncio
import queue
import threading
import tkinter as tk
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

    async def scan_devices(self) -> list[BLEDevice]:
        devices = await BleakScanner.discover(timeout=6.0, return_adv=True)
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

        self.client = BleakClient(device)
        await self.client.connect(timeout=15.0)
        self.connected_device = device

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
                    await self.client.stop_notify(STATUS_CHAR_UUID)
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
                self.ui_queue.put(("error", f"Scan lỗi: {exc}"))

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
                self.ui_queue.put(("error", f"Connect lỗi: {exc}"))

        future.add_done_callback(done_callback)

    def on_disconnect(self):
        future = self.worker.submit(self.worker.disconnect())

        def done_callback(fut):
            try:
                fut.result()
                self.ui_queue.put(("disconnected", None))
            except Exception as exc:
                self.ui_queue.put(("error", f"Disconnect lỗi: {exc}"))

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
                self.ui_queue.put(("error", f"Gửi lỗi: {exc}"))

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
