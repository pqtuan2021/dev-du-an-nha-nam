import json
import time
import queue
import threading
import tkinter as tk
from tkinter import ttk, messagebox
from tkinter.scrolledtext import ScrolledText

import serial
import serial.tools.list_ports


APP_TITLE = "FUVIAIR STM32 HMI UART Simulator"
DEFAULT_BAUD = 9600
RELAY_COUNT = 10


class STM32HMIApp:
    def __init__(self, root):
        self.root = root
        self.root.title(APP_TITLE)
        self.root.geometry("1200x760")

        self.ser = None
        self.running = False
        self.rx_thread = None
        self.rx_queue = queue.Queue()
        self.rx_buffer = ""

        self.msg_counter = 0

        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value=str(DEFAULT_BAUD))
        self.status_var = tk.StringVar(value="DISCONNECTED")

        self.timer_relay_var = tk.StringVar(value="1")
        self.timer_index_var = tk.StringVar(value="1")
        self.timer_enable_var = tk.IntVar(value=1)
        self.timer_on_var = tk.StringVar(value="06:00")
        self.timer_off_var = tk.StringVar(value="07:00")

        self.sensor_relay_var = tk.StringVar(value="2")
        self.sensor_id_var = tk.StringVar(value="AIR001")
        self.sensor_field_var = tk.StringVar(value="co2")
        self.sensor_logic_var = tk.StringVar(value="ABOVE")
        self.sensor_on_var = tk.StringVar(value="1000")
        self.sensor_off_var = tk.StringVar(value="800")
        self.sensor_enable_var = tk.IntVar(value=1)

        self.fan_on_var = tk.StringVar(value="40")
        self.fan_off_var = tk.StringVar(value="35")
        self.fan_hot_var = tk.StringVar(value="60")

        self.build_ui()
        self.refresh_ports()

        self.root.after(50, self.process_rx_queue)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    # ======================================================
    # UI
    # ======================================================
    def build_ui(self):
        top = ttk.Frame(self.root, padding=8)
        top.pack(side=tk.TOP, fill=tk.X)

        ttk.Label(top, text="COM:").pack(side=tk.LEFT)

        self.port_combo = ttk.Combobox(
            top,
            textvariable=self.port_var,
            width=32,
            state="readonly"
        )
        self.port_combo.pack(side=tk.LEFT, padx=4)

        ttk.Button(top, text="Refresh", command=self.refresh_ports).pack(side=tk.LEFT, padx=4)

        ttk.Label(top, text="Baud:").pack(side=tk.LEFT, padx=(16, 4))
        ttk.Entry(top, textvariable=self.baud_var, width=8).pack(side=tk.LEFT)

        ttk.Button(top, text="Connect", command=self.connect_serial).pack(side=tk.LEFT, padx=(16, 4))
        ttk.Button(top, text="Disconnect", command=self.disconnect_serial).pack(side=tk.LEFT, padx=4)

        ttk.Label(top, textvariable=self.status_var, foreground="blue").pack(side=tk.LEFT, padx=16)

        main = ttk.Frame(self.root, padding=8)
        main.pack(fill=tk.BOTH, expand=True)

        left = ttk.Frame(main)
        left.pack(side=tk.LEFT, fill=tk.Y, padx=(0, 8))

        mid = ttk.Frame(main)
        mid.pack(side=tk.LEFT, fill=tk.Y, padx=(0, 8))

        right = ttk.Frame(main)
        right.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        self.build_system_frame(left)
        self.build_mode_frame(left)
        self.build_fan_frame(left)
        self.build_timer_frame(left)
        self.build_sensor_frame(left)

        self.build_relay_frame(mid)
        self.build_raw_frame(mid)

        self.build_log_frame(right)

    def build_system_frame(self, parent):
        frame = ttk.LabelFrame(parent, text="Lệnh hệ thống", padding=8)
        frame.pack(fill=tk.X, pady=4)

        ttk.Button(frame, text="GET_STATUS", command=self.cmd_get_status).pack(fill=tk.X, pady=3)
        ttk.Button(frame, text="GET_FULL_STATE", command=self.cmd_get_full_state).pack(fill=tk.X, pady=3)
        ttk.Button(frame, text="GET_DEVICE_LIST", command=self.cmd_get_device_list).pack(fill=tk.X, pady=3)
        ttk.Button(frame, text="SAVE_FLASH", command=self.cmd_save_flash).pack(fill=tk.X, pady=3)
        ttk.Button(frame, text="CONFIG_SYNC_REQUEST", command=self.cmd_config_sync_request).pack(fill=tk.X, pady=3)
        ttk.Button(frame, text="TẮT TẤT CẢ RELAY", command=self.cmd_all_off).pack(fill=tk.X, pady=3)

    def build_mode_frame(self, parent):
        frame = ttk.LabelFrame(parent, text="Mode", padding=8)
        frame.pack(fill=tk.X, pady=4)

        ttk.Button(
            frame,
            text="SET_MODE MANUAL",
            command=lambda: self.cmd_set_mode("MANUAL")
        ).pack(fill=tk.X, pady=3)

        ttk.Button(
            frame,
            text="SET_MODE TIMER",
            command=lambda: self.cmd_set_mode("TIMER")
        ).pack(fill=tk.X, pady=3)

        ttk.Button(
            frame,
            text="SET_MODE SENSOR",
            command=lambda: self.cmd_set_mode("SENSOR")
        ).pack(fill=tk.X, pady=3)

    def build_fan_frame(self, parent):
        frame = ttk.LabelFrame(parent, text="LM35 / Quạt", padding=8)
        frame.pack(fill=tk.X, pady=4)

        row1 = ttk.Frame(frame)
        row1.pack(fill=tk.X, pady=2)

        ttk.Label(row1, text="ON >=").pack(side=tk.LEFT)
        ttk.Entry(row1, textvariable=self.fan_on_var, width=8).pack(side=tk.LEFT, padx=4)
        ttk.Label(row1, text="°C").pack(side=tk.LEFT)

        row2 = ttk.Frame(frame)
        row2.pack(fill=tk.X, pady=2)

        ttk.Label(row2, text="OFF <=").pack(side=tk.LEFT)
        ttk.Entry(row2, textvariable=self.fan_off_var, width=8).pack(side=tk.LEFT, padx=4)
        ttk.Label(row2, text="°C").pack(side=tk.LEFT)

        row3 = ttk.Frame(frame)
        row3.pack(fill=tk.X, pady=2)

        ttk.Label(row3, text="HOT >=").pack(side=tk.LEFT)
        ttk.Entry(row3, textvariable=self.fan_hot_var, width=8).pack(side=tk.LEFT, padx=4)
        ttk.Label(row3, text="°C").pack(side=tk.LEFT)

        ttk.Button(frame, text="SET_FAN_THRESHOLD", command=self.cmd_set_fan_threshold).pack(fill=tk.X, pady=3)
        ttk.Button(frame, text="GET_FAN_STATUS", command=self.cmd_get_fan_status).pack(fill=tk.X, pady=3)

    def build_timer_frame(self, parent):
        frame = ttk.LabelFrame(parent, text="Timer Rule", padding=8)
        frame.pack(fill=tk.X, pady=4)

        row1 = ttk.Frame(frame)
        row1.pack(fill=tk.X, pady=2)

        ttk.Label(row1, text="Relay").pack(side=tk.LEFT)
        ttk.Combobox(
            row1,
            textvariable=self.timer_relay_var,
            values=[str(i) for i in range(1, RELAY_COUNT + 1)],
            width=5,
            state="readonly"
        ).pack(side=tk.LEFT, padx=4)

        ttk.Label(row1, text="Index").pack(side=tk.LEFT)
        ttk.Combobox(
            row1,
            textvariable=self.timer_index_var,
            values=[str(i) for i in range(1, 11)],
            width=5,
            state="readonly"
        ).pack(side=tk.LEFT, padx=4)

        ttk.Checkbutton(row1, text="Enable", variable=self.timer_enable_var).pack(side=tk.LEFT, padx=4)

        row2 = ttk.Frame(frame)
        row2.pack(fill=tk.X, pady=2)

        ttk.Label(row2, text="ON").pack(side=tk.LEFT)
        ttk.Entry(row2, textvariable=self.timer_on_var, width=8).pack(side=tk.LEFT, padx=4)

        ttk.Label(row2, text="OFF").pack(side=tk.LEFT)
        ttk.Entry(row2, textvariable=self.timer_off_var, width=8).pack(side=tk.LEFT, padx=4)

        ttk.Button(frame, text="GỬI SET_TIMER", command=self.cmd_set_timer).pack(fill=tk.X, pady=3)
        ttk.Button(frame, text="DEMO TIMER R1", command=lambda: self.cmd_set_timer_demo(1)).pack(fill=tk.X, pady=3)

    def build_sensor_frame(self, parent):
        frame = ttk.LabelFrame(parent, text="Sensor Rule", padding=8)
        frame.pack(fill=tk.X, pady=4)

        row1 = ttk.Frame(frame)
        row1.pack(fill=tk.X, pady=2)

        ttk.Label(row1, text="Relay").pack(side=tk.LEFT)
        ttk.Combobox(
            row1,
            textvariable=self.sensor_relay_var,
            values=[str(i) for i in range(1, RELAY_COUNT + 1)],
            width=5,
            state="readonly"
        ).pack(side=tk.LEFT, padx=4)

        ttk.Label(row1, text="ID").pack(side=tk.LEFT)
        ttk.Entry(row1, textvariable=self.sensor_id_var, width=12).pack(side=tk.LEFT, padx=4)

        row2 = ttk.Frame(frame)
        row2.pack(fill=tk.X, pady=2)

        ttk.Label(row2, text="Field").pack(side=tk.LEFT)
        ttk.Combobox(
            row2,
            textvariable=self.sensor_field_var,
            values=["temperature", "humidity", "co2", "light"],
            width=12,
            state="readonly"
        ).pack(side=tk.LEFT, padx=4)

        ttk.Label(row2, text="Logic").pack(side=tk.LEFT)
        ttk.Combobox(
            row2,
            textvariable=self.sensor_logic_var,
            values=["ABOVE", "BELOW"],
            width=8,
            state="readonly"
        ).pack(side=tk.LEFT, padx=4)

        row3 = ttk.Frame(frame)
        row3.pack(fill=tk.X, pady=2)

        ttk.Label(row3, text="ON").pack(side=tk.LEFT)
        ttk.Entry(row3, textvariable=self.sensor_on_var, width=8).pack(side=tk.LEFT, padx=4)

        ttk.Label(row3, text="OFF").pack(side=tk.LEFT)
        ttk.Entry(row3, textvariable=self.sensor_off_var, width=8).pack(side=tk.LEFT, padx=4)

        ttk.Checkbutton(frame, text="Enable", variable=self.sensor_enable_var).pack(anchor=tk.W)

        ttk.Button(frame, text="GỬI SET_SENSOR_RULE", command=self.cmd_set_sensor_rule).pack(fill=tk.X, pady=3)
        ttk.Button(frame, text="DEMO SENSOR R2 CO2", command=lambda: self.cmd_set_sensor_rule_demo(2)).pack(fill=tk.X, pady=3)

    def build_relay_frame(self, parent):
        frame = ttk.LabelFrame(parent, text="Manual Relay", padding=8)
        frame.pack(fill=tk.X, pady=4)

        ttk.Label(
            frame,
            text="Lưu ý: nút ON/OFF tự gửi mode=MANUAL trong data"
        ).pack(fill=tk.X, pady=(0, 6))

        for relay in range(1, RELAY_COUNT + 1):
            row = ttk.Frame(frame)
            row.pack(fill=tk.X, pady=2)

            ttk.Label(row, text=f"Relay {relay}", width=10).pack(side=tk.LEFT)

            ttk.Button(
                row,
                text="ON",
                width=8,
                command=lambda r=relay: self.cmd_set_relay(r, 1)
            ).pack(side=tk.LEFT, padx=2)

            ttk.Button(
                row,
                text="OFF",
                width=8,
                command=lambda r=relay: self.cmd_set_relay(r, 0)
            ).pack(side=tk.LEFT, padx=2)

            ttk.Button(
                row,
                text="TEST",
                width=8,
                command=lambda r=relay: self.cmd_test_relay(r)
            ).pack(side=tk.LEFT, padx=2)

    def build_raw_frame(self, parent):
        frame = ttk.LabelFrame(parent, text="Raw JSON", padding=8)
        frame.pack(fill=tk.X, pady=4)

        self.raw_text = ScrolledText(frame, height=8, width=56)
        self.raw_text.pack(fill=tk.X)

        self.raw_text.insert(
            "1.0",
            '{"msg_id":"R2ON","type":"CMD","cmd":"SET_RELAY","ack_req":true,"data":{"mode":"MANUAL","relay":2,"state":1}}'
        )

        ttk.Button(frame, text="GỬI RAW JSON", command=self.cmd_send_raw).pack(fill=tk.X, pady=4)

    def build_log_frame(self, parent):
        frame = ttk.LabelFrame(parent, text="Log UART", padding=8)
        frame.pack(fill=tk.BOTH, expand=True)

        self.log_text = ScrolledText(frame, height=34)
        self.log_text.pack(fill=tk.BOTH, expand=True)

        btn = ttk.Frame(frame)
        btn.pack(fill=tk.X, pady=4)

        ttk.Button(btn, text="Clear Log", command=self.clear_log).pack(side=tk.LEFT, padx=4)
        ttk.Button(btn, text="GET_STATUS", command=self.cmd_get_status).pack(side=tk.LEFT, padx=4)
        ttk.Button(btn, text="SET_MODE MANUAL", command=lambda: self.cmd_set_mode("MANUAL")).pack(side=tk.LEFT, padx=4)

    # ======================================================
    # SERIAL
    # ======================================================
    def refresh_ports(self):
        ports = serial.tools.list_ports.comports()
        port_list = [p.device for p in ports]

        self.port_combo["values"] = port_list

        if port_list and not self.port_var.get():
            self.port_var.set(port_list[0])

    def connect_serial(self):
        if self.ser and self.ser.is_open:
            messagebox.showinfo("Thông báo", "Đã kết nối rồi.")
            return

        port = self.port_var.get().strip()

        if not port:
            messagebox.showerror("Lỗi", "Chưa chọn COM.")
            return

        try:
            baud = int(self.baud_var.get())
        except ValueError:
            messagebox.showerror("Lỗi", "Baud không hợp lệ.")
            return

        try:
            self.ser = serial.Serial(
                port=port,
                baudrate=baud,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=0.05
            )

            self.running = True
            self.rx_thread = threading.Thread(target=self.serial_reader, daemon=True)
            self.rx_thread.start()

            self.status_var.set(f"CONNECTED {port} @ {baud}")
            self.log(f"[CONNECT] {port} @ {baud}")

        except serial.SerialException as e:
            messagebox.showerror("Lỗi mở COM", str(e))

    def disconnect_serial(self):
        self.running = False

        try:
            if self.ser and self.ser.is_open:
                self.ser.close()
        except Exception:
            pass

        self.status_var.set("DISCONNECTED")
        self.log("[DISCONNECT]")

    def serial_reader(self):
        while self.running:
            try:
                if self.ser and self.ser.is_open:
                    data = self.ser.read(self.ser.in_waiting or 1)

                    if data:
                        text = data.decode("utf-8", errors="ignore")
                        self.rx_buffer += text

                        while "\n" in self.rx_buffer:
                            line, self.rx_buffer = self.rx_buffer.split("\n", 1)
                            line = line.strip()

                            if line:
                                self.rx_queue.put(line)

                time.sleep(0.01)

            except Exception as e:
                self.rx_queue.put(f"[SERIAL ERROR] {e}")
                self.running = False
                break

    def process_rx_queue(self):
        try:
            while True:
                line = self.rx_queue.get_nowait()
                self.handle_rx_line(line)
        except queue.Empty:
            pass

        self.root.after(50, self.process_rx_queue)

    def handle_rx_line(self, line):
        self.log("")
        self.log("========== STM32 -> HMI ==========")
        self.log(line)

        try:
            obj = json.loads(line)
            self.show_parsed(obj)
        except json.JSONDecodeError:
            pass

    def show_parsed(self, obj):
        msg_type = obj.get("type", "")

        if msg_type == "BOOT":
            self.log(
                f"[BOOT] {obj.get('message', '')} "
                f"baud={obj.get('baud', '')} "
                f"relay_count={obj.get('relay_count', '')} "
                f"activeHigh={obj.get('activeHigh', '')}"
            )

        elif msg_type == "ACK":
            self.log(
                f"[ACK] ack_for={obj.get('ack_for', '')} "
                f"status={obj.get('status', '')} "
                f"error_code={obj.get('error_code', '')} "
                f"message={obj.get('message', '')} "
                f"cfgVersion={obj.get('cfgVersion', '')}"
            )

        elif msg_type == "RELAY_STATE":
            self.log(
                f"[RELAY_STATE] mode={obj.get('mode', '')} "
                f"relay={obj.get('relay', '')} "
                f"state={obj.get('state', '')} "
                f"pin={obj.get('pin', '')} "
                f"gpioLevel={obj.get('gpioLevel', '')} "
                f"activeHigh={obj.get('activeHigh', '')} "
                f"cfgVersion={obj.get('cfgVersion', '')}"
            )

        elif msg_type == "STATUS":
            self.log(
                f"[STATUS] mode={obj.get('mode', '')} "
                f"cfgVersion={obj.get('cfgVersion', '')} "
                f"relays={obj.get('relays', [])} "
                f"cabinetTempC={obj.get('cabinetTempC', obj.get('cabinetTemp', ''))} "
                f"fanState={obj.get('fanState', obj.get('fan', ''))}"
            )

        elif msg_type == "FULL_STATE":
            self.log(
                f"[FULL_STATE] mode={obj.get('mode', '')} "
                f"cfgVersion={obj.get('cfgVersion', '')} "
                f"relays={obj.get('relays', [])} "
                f"cabinetTempC={obj.get('cabinetTempC', obj.get('cabinetTemp', ''))} "
                f"fanState={obj.get('fanState', obj.get('fan', ''))}"
            )

        elif msg_type == "FAN_STATE":
            self.log(
                f"[FAN_STATE] fan={obj.get('fan', obj.get('fanState', ''))} "
                f"fanMode={obj.get('fanMode', '')} "
                f"cabinetTemp={obj.get('cabinetTemp', obj.get('cabinetTempC', ''))} "
                f"fanOnTemp={obj.get('fanOnTemp', obj.get('fanOnTempC', ''))} "
                f"fanOffTemp={obj.get('fanOffTemp', obj.get('fanOffTempC', ''))}"
            )

        elif msg_type == "CABINET":
            self.log(
                f"[CABINET] temp={obj.get('cabinetTempC', '')} "
                f"sensorOk={obj.get('cabinetSensorOk', '')} "
                f"fan={obj.get('fanState', '')} "
                f"hot={obj.get('cabinetHot', '')}"
            )

    # ======================================================
    # SEND
    # ======================================================
    def is_connected(self):
        if not self.ser or not self.ser.is_open:
            messagebox.showwarning("Chưa kết nối", "Bạn chưa Connect COM.")
            return False
        return True

    def make_msg_id(self, prefix):
        self.msg_counter += 1
        return f"HMI_{prefix}-{int(time.time() * 1000)}-{self.msg_counter}"

    def send_json(self, obj):
        if not self.is_connected():
            return

        text = json.dumps(obj, ensure_ascii=False, separators=(",", ":"))

        self.log("")
        self.log("========== HMI -> STM32 ==========")
        self.log(text)

        # Quan trọng: luôn thêm \n để STM32 biết hết 1 dòng JSON
        self.ser.write((text + "\n").encode("utf-8"))

    def send_raw(self, text):
        if not self.is_connected():
            return

        text = text.strip()

        if not text:
            return

        self.log("")
        self.log("========== HMI -> STM32 RAW ==========")
        self.log(text)

        # Quan trọng: luôn thêm \n
        self.ser.write((text + "\n").encode("utf-8"))

    # ======================================================
    # COMMANDS
    # ======================================================
    def cmd_get_status(self):
        self.send_json({
            "msg_id": self.make_msg_id("STATUS"),
            "type": "CMD",
            "cmd": "GET_STATUS",
            "ack_req": True
        })

    def cmd_get_full_state(self):
        self.send_json({
            "msg_id": self.make_msg_id("FULL"),
            "type": "CMD",
            "cmd": "GET_FULL_STATE",
            "ack_req": True
        })

    def cmd_get_device_list(self):
        self.send_json({
            "msg_id": self.make_msg_id("DEV"),
            "type": "CMD",
            "cmd": "GET_DEVICE_LIST",
            "ack_req": True
        })

    def cmd_save_flash(self):
        self.send_json({
            "msg_id": self.make_msg_id("FLASH"),
            "type": "CMD",
            "cmd": "SAVE_FLASH",
            "ack_req": True
        })

    def cmd_config_sync_request(self):
        self.send_json({
            "msg_id": self.make_msg_id("SYNC"),
            "type": "CMD",
            "cmd": "CONFIG_SYNC_REQUEST",
            "ack_req": True
        })

    def cmd_set_mode(self, mode):
        self.send_json({
            "msg_id": self.make_msg_id("MODE"),
            "type": "CMD",
            "cmd": "SET_MODE",
            "ack_req": True,
            "data": {
                "mode": mode
            }
        })

    def cmd_set_relay(self, relay, state):
        # Đã thêm mode=MANUAL để phù hợp code STM32 full
        self.send_json({
            "msg_id": self.make_msg_id("RELAY"),
            "type": "CMD",
            "cmd": "SET_RELAY",
            "ack_req": True,
            "data": {
                "mode": "MANUAL",
                "relay": relay,
                "state": state
            }
        })

    def cmd_test_relay(self, relay):
        self.send_json({
            "msg_id": self.make_msg_id("TEST"),
            "type": "CMD",
            "cmd": "TEST_RELAY",
            "ack_req": True,
            "data": {
                "relay": relay
            }
        })

    def cmd_all_off(self):
        # Đưa về MANUAL trước rồi tắt lần lượt
        self.cmd_set_mode("MANUAL")
        self.root.after(120, self._all_off_step, 1)

    def _all_off_step(self, relay):
        if relay > RELAY_COUNT:
            return

        self.cmd_set_relay(relay, 0)
        self.root.after(80, self._all_off_step, relay + 1)

    def cmd_set_timer(self):
        try:
            relay = int(self.timer_relay_var.get())
            index = int(self.timer_index_var.get())
            enable = 1 if self.timer_enable_var.get() else 0
            on_time = self.timer_on_var.get().strip()
            off_time = self.timer_off_var.get().strip()
        except ValueError:
            messagebox.showerror("Lỗi", "Timer không hợp lệ.")
            return

        self.send_json({
            "msg_id": self.make_msg_id("TIMER"),
            "type": "CONFIG",
            "cmd": "SET_TIMER",
            "ack_req": True,
            "data": {
                "relay": relay,
                "schedules": [
                    {
                        "index": index,
                        "enable": enable,
                        "on": on_time,
                        "off": off_time
                    }
                ]
            }
        })

    def cmd_set_timer_demo(self, relay):
        self.send_json({
            "msg_id": self.make_msg_id("TIMER"),
            "type": "CONFIG",
            "cmd": "SET_TIMER",
            "ack_req": True,
            "data": {
                "relay": relay,
                "schedules": [
                    {
                        "index": 1,
                        "enable": 1,
                        "on": "06:00",
                        "off": "07:00"
                    },
                    {
                        "index": 2,
                        "enable": 1,
                        "on": "18:00",
                        "off": "20:00"
                    }
                ]
            }
        })

    def cmd_set_sensor_rule(self):
        try:
            relay = int(self.sensor_relay_var.get())
            on_value = float(self.sensor_on_var.get())
            off_value = float(self.sensor_off_var.get())
            enable = 1 if self.sensor_enable_var.get() else 0
        except ValueError:
            messagebox.showerror("Lỗi", "Sensor rule không hợp lệ.")
            return

        self.send_json({
            "msg_id": self.make_msg_id("SENSOR"),
            "type": "CONFIG",
            "cmd": "SET_SENSOR_RULE",
            "ack_req": True,
            "data": {
                "relay": relay,
                "id_device": self.sensor_id_var.get().strip(),
                "field": self.sensor_field_var.get().strip(),
                "logic": self.sensor_logic_var.get().strip(),
                "onValue": on_value,
                "offValue": off_value,
                "enable": enable
            }
        })

    def cmd_set_sensor_rule_demo(self, relay):
        self.send_json({
            "msg_id": self.make_msg_id("SENSOR"),
            "type": "CONFIG",
            "cmd": "SET_SENSOR_RULE",
            "ack_req": True,
            "data": {
                "relay": relay,
                "id_device": "AIR001",
                "field": "co2",
                "logic": "ABOVE",
                "onAbove": 1000,
                "offBelow": 800,
                "enable": 1
            }
        })

    def cmd_set_fan_threshold(self):
        try:
            fan_on = float(self.fan_on_var.get())
            fan_off = float(self.fan_off_var.get())
            fan_hot = float(self.fan_hot_var.get())
        except ValueError:
            messagebox.showerror("Lỗi", "Ngưỡng quạt không hợp lệ.")
            return

        self.send_json({
            "msg_id": self.make_msg_id("FAN_TH"),
            "type": "CMD",
            "cmd": "SET_FAN_THRESHOLD",
            "ack_req": True,
            "data": {
                "fanOnTempC": fan_on,
                "fanOffTempC": fan_off,
                "cabinetHotTempC": fan_hot
            }
        })

    def cmd_get_fan_status(self):
        self.send_json({
            "msg_id": self.make_msg_id("FAN_GET"),
            "type": "CMD",
            "cmd": "GET_FAN_STATUS",
            "ack_req": True
        })

    def cmd_send_raw(self):
        text = self.raw_text.get("1.0", tk.END)
        self.send_raw(text)

    # ======================================================
    # LOG
    # ======================================================
    def log(self, text):
        self.log_text.insert(tk.END, text + "\n")
        self.log_text.see(tk.END)

    def clear_log(self):
        self.log_text.delete("1.0", tk.END)

    def on_close(self):
        self.disconnect_serial()
        self.root.destroy()


def main():
    root = tk.Tk()
    app = STM32HMIApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()