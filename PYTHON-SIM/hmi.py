import json
import time
import queue
import threading
import tkinter as tk
from tkinter import ttk, messagebox
from tkinter.scrolledtext import ScrolledText

import serial
import serial.tools.list_ports


APP_TITLE = "FUVIAIR STM32 HMI UART Simulator V2"
DEFAULT_BAUD = 9600
RELAY_COUNT = 10
ACK_TIMEOUT_S = 6.0

MODE_VALUES = ["MANUAL", "TIMER", "SENSOR"]
FIELD_VALUES = ["temperature", "humidity", "co2", "light"]
LOGIC_VALUES = ["ABOVE", "BELOW"]


class STM32HMIApp:
    def __init__(self, root):
        self.root = root
        self.root.title(APP_TITLE)
        self.root.geometry("1320x820")
        self.root.minsize(1180, 720)

        self.ser = None
        self.running = False
        self.rx_thread = None
        self.rx_queue = queue.Queue()
        self.rx_buffer = ""

        self.msg_counter = 0
        self.pending = {}  # msg_id -> dict(cmd, time, meta)

        self.relay_states = [None] * RELAY_COUNT
        self.relay_gpio = [None] * RELAY_COUNT
        self.relay_pin = [None] * RELAY_COUNT

        self.timer_rows = {}   # (relay, index) -> iid
        self.sensor_rows = {}  # relay -> iid

        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value=str(DEFAULT_BAUD))
        self.status_var = tk.StringVar(value="DISCONNECTED")

        self.mode_var = tk.StringVar(value="--")
        self.cfg_var = tk.StringVar(value="--")
        self.temp_var = tk.StringVar(value="--")
        self.fan_var = tk.StringVar(value="--")
        self.fan_mode_var = tk.StringVar(value="--")
        self.device_count_var = tk.StringVar(value="--")
        self.last_rx_var = tk.StringVar(value="--")
        self.clock_var = tk.StringVar(value="--")

        # Giảm spam: mặc định KHÔNG tự GET_STATUS liên tục, và ẩn log telemetry.
        self.auto_sync_var = tk.IntVar(value=0)
        self.sync_interval_var = tk.StringVar(value="10")
        self.hide_telemetry_log_var = tk.IntVar(value=1)
        self.log_tx_status_var = tk.IntVar(value=0)

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

        self.fan_on_var = tk.StringVar(value="35")
        self.fan_off_var = tk.StringVar(value="30")
        self.fan_hot_var = tk.StringVar(value="60")

        self.build_ui()
        self.refresh_ports()

        self.root.after(50, self.process_rx_queue)
        self.root.after(300, self.check_ack_timeouts)
        self.root.after(1000, self.auto_sync_task)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    # ======================================================
    # UI
    # ======================================================
    def build_ui(self):
        top = ttk.Frame(self.root, padding=8)
        top.pack(side=tk.TOP, fill=tk.X)

        ttk.Label(top, text="COM:").pack(side=tk.LEFT)
        self.port_combo = ttk.Combobox(
            top, textvariable=self.port_var, width=30, state="readonly")
        self.port_combo.pack(side=tk.LEFT, padx=4)
        ttk.Button(top, text="Refresh", command=self.refresh_ports).pack(
            side=tk.LEFT, padx=4)

        ttk.Label(top, text="Baud:").pack(side=tk.LEFT, padx=(16, 4))
        ttk.Entry(top, textvariable=self.baud_var, width=8).pack(side=tk.LEFT)

        ttk.Button(top, text="Connect", command=self.connect_serial).pack(
            side=tk.LEFT, padx=(16, 4))
        ttk.Button(top, text="Disconnect", command=self.disconnect_serial).pack(
            side=tk.LEFT, padx=4)
        ttk.Label(top, textvariable=self.status_var,
                  foreground="blue").pack(side=tk.LEFT, padx=16)

        opts = ttk.Frame(self.root, padding=(8, 0, 8, 6))
        opts.pack(side=tk.TOP, fill=tk.X)
        ttk.Checkbutton(opts, text="Auto sync",
                        variable=self.auto_sync_var).pack(side=tk.LEFT)
        ttk.Label(opts, text="chu kỳ").pack(side=tk.LEFT, padx=(8, 2))
        ttk.Entry(opts, textvariable=self.sync_interval_var,
                  width=5).pack(side=tk.LEFT)
        ttk.Label(opts, text="giây").pack(side=tk.LEFT, padx=(2, 14))
        ttk.Checkbutton(opts, text="Ẩn log định kỳ STATUS/FAN_STATE",
                        variable=self.hide_telemetry_log_var).pack(side=tk.LEFT, padx=8)
        ttk.Checkbutton(opts, text="Hiện lệnh GET_STATUS tự động",
                        variable=self.log_tx_status_var).pack(side=tk.LEFT, padx=8)
        ttk.Button(opts, text="SYNC NOW", command=self.sync_now).pack(
            side=tk.LEFT, padx=12)
        ttk.Button(opts, text="SET_MODE MANUAL", command=lambda: self.cmd_set_mode(
            "MANUAL")).pack(side=tk.LEFT, padx=4)

        main = ttk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        main.pack(fill=tk.BOTH, expand=True, padx=8, pady=8)

        left = ttk.Frame(main)
        center = ttk.Frame(main)
        right = ttk.Frame(main)
        main.add(left, weight=1)
        main.add(center, weight=2)
        main.add(right, weight=3)

        self.build_state_frame(left)
        self.build_system_frame(left)
        self.build_mode_frame(left)
        self.build_fan_frame(left)
        self.build_raw_frame(left)

        self.build_relay_frame(center)
        self.build_timer_frame(center)
        self.build_sensor_frame(center)

        self.build_log_frame(right)

    def build_state_frame(self, parent):
        frame = ttk.LabelFrame(parent, text="Trạng thái STM32", padding=8)
        frame.pack(fill=tk.X, pady=4)

        rows = [
            ("Mode", self.mode_var),
            ("cfgVersion", self.cfg_var),
            ("Cabinet Temp", self.temp_var),
            ("Fan", self.fan_var),
            ("Fan Mode", self.fan_mode_var),
            ("Device Count", self.device_count_var),
            ("Clock STM", self.clock_var),
            ("Last RX", self.last_rx_var),
        ]
        for label, var in rows:
            row = ttk.Frame(frame)
            row.pack(fill=tk.X, pady=1)
            ttk.Label(row, text=label + ":", width=14).pack(side=tk.LEFT)
            ttk.Label(row, textvariable=var,
                      foreground="darkgreen").pack(side=tk.LEFT)

    def build_system_frame(self, parent):
        frame = ttk.LabelFrame(parent, text="Lệnh hệ thống", padding=8)
        frame.pack(fill=tk.X, pady=4)

        ttk.Button(frame, text="GET_STATUS",
                   command=self.cmd_get_status_manual).pack(fill=tk.X, pady=3)
        ttk.Button(frame, text="SET_TIME NOW", command=lambda: self.cmd_set_time_now(
            silent=False)).pack(fill=tk.X, pady=3)
        ttk.Button(frame, text="GET_FULL_STATE",
                   command=self.cmd_get_full_state).pack(fill=tk.X, pady=3)
        ttk.Button(frame, text="GET_DEVICE_LIST",
                   command=self.cmd_get_device_list).pack(fill=tk.X, pady=3)
        ttk.Button(frame, text="SAVE_FLASH",
                   command=self.cmd_save_flash).pack(fill=tk.X, pady=3)
        ttk.Button(frame, text="CONFIG_SYNC_REQUEST",
                   command=self.cmd_config_sync_request).pack(fill=tk.X, pady=3)
        ttk.Button(frame, text="TẮT TẤT CẢ RELAY",
                   command=self.cmd_all_off).pack(fill=tk.X, pady=3)

    def build_mode_frame(self, parent):
        frame = ttk.LabelFrame(parent, text="Mode", padding=8)
        frame.pack(fill=tk.X, pady=4)
        for mode in MODE_VALUES:
            ttk.Button(frame, text=f"SET_MODE {mode}", command=lambda m=mode: self.cmd_set_mode(
                m)).pack(fill=tk.X, pady=3)

    def build_fan_frame(self, parent):
        frame = ttk.LabelFrame(parent, text="LM35 / Quạt", padding=8)
        frame.pack(fill=tk.X, pady=4)

        for label, var in [("ON >=", self.fan_on_var), ("OFF <=", self.fan_off_var), ("HOT >=", self.fan_hot_var)]:
            row = ttk.Frame(frame)
            row.pack(fill=tk.X, pady=2)
            ttk.Label(row, text=label, width=8).pack(side=tk.LEFT)
            ttk.Entry(row, textvariable=var, width=8).pack(
                side=tk.LEFT, padx=4)
            ttk.Label(row, text="°C").pack(side=tk.LEFT)

        ttk.Button(frame, text="SET_FAN_THRESHOLD",
                   command=self.cmd_set_fan_threshold).pack(fill=tk.X, pady=3)
        ttk.Button(frame, text="GET_FAN_STATUS",
                   command=self.cmd_get_fan_status).pack(fill=tk.X, pady=3)

    def build_relay_frame(self, parent):
        frame = ttk.LabelFrame(
            parent, text="Relay - cập nhật theo trạng thái STM32", padding=8)
        frame.pack(fill=tk.X, pady=4)

        self.relay_label_widgets = []
        self.relay_gpio_widgets = []

        for relay in range(1, RELAY_COUNT + 1):
            row = ttk.Frame(frame)
            row.pack(fill=tk.X, pady=2)

            ttk.Label(row, text=f"Relay {relay}", width=9).pack(side=tk.LEFT)

            state_lbl = tk.Label(row, text="?", width=6,
                                 relief=tk.SUNKEN, bg="#d9d9d9")
            state_lbl.pack(side=tk.LEFT, padx=2)
            self.relay_label_widgets.append(state_lbl)

            gpio_lbl = ttk.Label(row, text="pin:-- gpio:--", width=18)
            gpio_lbl.pack(side=tk.LEFT, padx=2)
            self.relay_gpio_widgets.append(gpio_lbl)

            ttk.Button(row, text="ON", width=7, command=lambda r=relay: self.cmd_set_relay(
                r, 1)).pack(side=tk.LEFT, padx=2)
            ttk.Button(row, text="OFF", width=7, command=lambda r=relay: self.cmd_set_relay(
                r, 0)).pack(side=tk.LEFT, padx=2)
            ttk.Button(row, text="TEST", width=7, command=lambda r=relay: self.cmd_test_relay(
                r)).pack(side=tk.LEFT, padx=2)

    def build_timer_frame(self, parent):
        frame = ttk.LabelFrame(parent, text="Timer Rule", padding=8)
        frame.pack(fill=tk.BOTH, expand=True, pady=4)

        editor = ttk.Frame(frame)
        editor.pack(fill=tk.X, pady=2)
        ttk.Label(editor, text="Relay").pack(side=tk.LEFT)
        ttk.Combobox(editor, textvariable=self.timer_relay_var, values=[str(i) for i in range(
            1, RELAY_COUNT + 1)], width=5, state="readonly").pack(side=tk.LEFT, padx=3)
        ttk.Label(editor, text="Index").pack(side=tk.LEFT)
        ttk.Combobox(editor, textvariable=self.timer_index_var, values=[str(
            i) for i in range(1, 11)], width=5, state="readonly").pack(side=tk.LEFT, padx=3)
        ttk.Checkbutton(editor, text="Enable", variable=self.timer_enable_var).pack(
            side=tk.LEFT, padx=3)
        ttk.Label(editor, text="ON").pack(side=tk.LEFT)
        ttk.Entry(editor, textvariable=self.timer_on_var,
                  width=8).pack(side=tk.LEFT, padx=3)
        ttk.Label(editor, text="OFF").pack(side=tk.LEFT)
        ttk.Entry(editor, textvariable=self.timer_off_var,
                  width=8).pack(side=tk.LEFT, padx=3)
        ttk.Button(editor, text="LƯU TIMER", command=self.cmd_set_timer).pack(
            side=tk.LEFT, padx=5)

        cols = ("relay", "index", "enable", "on", "off", "status")
        self.timer_tree = ttk.Treeview(
            frame, columns=cols, show="headings", height=8)
        headers = ["Relay", "Index", "Enable", "ON", "OFF", "Trạng thái"]
        widths = [55, 55, 65, 75, 75, 150]
        for col, head, width in zip(cols, headers, widths):
            self.timer_tree.heading(col, text=head)
            self.timer_tree.column(col, width=width, anchor=tk.CENTER)
        self.timer_tree.pack(fill=tk.BOTH, expand=True, pady=4)

        btns = ttk.Frame(frame)
        btns.pack(fill=tk.X)
        ttk.Button(btns, text="Demo R1", command=lambda: self.cmd_set_timer_demo(
            1)).pack(side=tk.LEFT, padx=3)
        ttk.Button(btns, text="Clear list", command=self.clear_timer_list).pack(
            side=tk.LEFT, padx=3)

    def build_sensor_frame(self, parent):
        frame = ttk.LabelFrame(parent, text="Sensor Rule", padding=8)
        frame.pack(fill=tk.BOTH, expand=True, pady=4)

        editor1 = ttk.Frame(frame)
        editor1.pack(fill=tk.X, pady=2)
        ttk.Label(editor1, text="Relay").pack(side=tk.LEFT)
        ttk.Combobox(editor1, textvariable=self.sensor_relay_var, values=[str(i) for i in range(
            1, RELAY_COUNT + 1)], width=5, state="readonly").pack(side=tk.LEFT, padx=3)
        ttk.Label(editor1, text="ID").pack(side=tk.LEFT)
        ttk.Entry(editor1, textvariable=self.sensor_id_var,
                  width=10).pack(side=tk.LEFT, padx=3)
        ttk.Label(editor1, text="Field").pack(side=tk.LEFT)
        ttk.Combobox(editor1, textvariable=self.sensor_field_var, values=FIELD_VALUES,
                     width=12, state="readonly").pack(side=tk.LEFT, padx=3)
        ttk.Label(editor1, text="Logic").pack(side=tk.LEFT)
        ttk.Combobox(editor1, textvariable=self.sensor_logic_var, values=LOGIC_VALUES,
                     width=8, state="readonly").pack(side=tk.LEFT, padx=3)

        editor2 = ttk.Frame(frame)
        editor2.pack(fill=tk.X, pady=2)
        ttk.Label(editor2, text="ON").pack(side=tk.LEFT)
        ttk.Entry(editor2, textvariable=self.sensor_on_var,
                  width=8).pack(side=tk.LEFT, padx=3)
        ttk.Label(editor2, text="OFF").pack(side=tk.LEFT)
        ttk.Entry(editor2, textvariable=self.sensor_off_var,
                  width=8).pack(side=tk.LEFT, padx=3)
        ttk.Checkbutton(editor2, text="Enable", variable=self.sensor_enable_var).pack(
            side=tk.LEFT, padx=3)
        ttk.Button(editor2, text="LƯU SENSOR",
                   command=self.cmd_set_sensor_rule).pack(side=tk.LEFT, padx=5)

        cols = ("relay", "enable", "device", "field",
                "logic", "on", "off", "status")
        self.sensor_tree = ttk.Treeview(
            frame, columns=cols, show="headings", height=7)
        headers = ["Relay", "Enable", "Device",
                   "Field", "Logic", "ON", "OFF", "Trạng thái"]
        widths = [55, 65, 85, 95, 70, 70, 70, 140]
        for col, head, width in zip(cols, headers, widths):
            self.sensor_tree.heading(col, text=head)
            self.sensor_tree.column(col, width=width, anchor=tk.CENTER)
        self.sensor_tree.pack(fill=tk.BOTH, expand=True, pady=4)

        btns = ttk.Frame(frame)
        btns.pack(fill=tk.X)
        ttk.Button(btns, text="Demo R2 CO2", command=lambda: self.cmd_set_sensor_rule_demo(
            2)).pack(side=tk.LEFT, padx=3)
        ttk.Button(btns, text="Clear list", command=self.clear_sensor_list).pack(
            side=tk.LEFT, padx=3)

    def build_raw_frame(self, parent):
        frame = ttk.LabelFrame(parent, text="Raw JSON", padding=8)
        frame.pack(fill=tk.BOTH, expand=True, pady=4)
        self.raw_text = ScrolledText(frame, height=5, width=45)
        self.raw_text.pack(fill=tk.BOTH, expand=True)
        self.raw_text.insert(
            "1.0", '{"msg_id":"R2ON","type":"CMD","cmd":"SET_RELAY","ack_req":true,"data":{"mode":"MANUAL","relay":2,"state":1}}')
        ttk.Button(frame, text="GỬI RAW JSON",
                   command=self.cmd_send_raw).pack(fill=tk.X, pady=4)

    def build_log_frame(self, parent):
        frame = ttk.LabelFrame(
            parent, text="Log UART - đã lọc spam định kỳ", padding=8)
        frame.pack(fill=tk.BOTH, expand=True)
        self.log_text = ScrolledText(frame, height=34)
        self.log_text.pack(fill=tk.BOTH, expand=True)
        btn = ttk.Frame(frame)
        btn.pack(fill=tk.X, pady=4)
        ttk.Button(btn, text="Clear Log", command=self.clear_log).pack(
            side=tk.LEFT, padx=4)
        ttk.Button(btn, text="SYNC NOW", command=self.sync_now).pack(
            side=tk.LEFT, padx=4)
        ttk.Button(btn, text="SET_MODE MANUAL", command=lambda: self.cmd_set_mode(
            "MANUAL")).pack(side=tk.LEFT, padx=4)

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
            self.ser = serial.Serial(port=port, baudrate=baud, bytesize=serial.EIGHTBITS,
                                     parity=serial.PARITY_NONE, stopbits=serial.STOPBITS_ONE,
                                     timeout=0.05)
            self.running = True
            self.rx_thread = threading.Thread(
                target=self.serial_reader, daemon=True)
            self.rx_thread.start()
            self.status_var.set(f"CONNECTED {port} @ {baud}")
            self.log(f"[CONNECT] {port} @ {baud}")
            # Set giờ STM theo giờ máy tính rồi sync 1 lần, không spam liên tục.
            self.root.after(500, lambda: self.cmd_set_time_now(silent=True))
            self.root.after(900, self.sync_now)
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
                            line, self.rx_buffer = self.rx_buffer.split(
                                "\n", 1)
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

    # ======================================================
    # RX PARSE + SYNC UI
    # ======================================================
    def handle_rx_line(self, line):
        self.last_rx_var.set(time.strftime("%H:%M:%S"))

        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            self.log(f"[RX NON-JSON] {line}")
            return

        msg_type = obj.get("type", "")
        should_log = self.should_log_rx(obj)

        if should_log:
            self.log("")
            self.log("========== STM32 -> HMI ==========")
            self.log(line)

        self.apply_rx_to_ui(obj)

        if should_log:
            self.log_summary(obj)

    def should_log_rx(self, obj):
        msg_type = obj.get("type", "")
        if not self.hide_telemetry_log_var.get():
            return True

        # FAN_STATE gửi chu kỳ 3s từ STM, STATUS cũng có thể do auto sync tạo ra.
        if msg_type in ("FAN_STATE", "CABINET"):
            return False

        if msg_type == "STATUS":
            return False

        # Nếu STM đang ở TIMER/SENSOR mà gửi RELAY_STATE lặp lại cùng trạng thái,
        # ẩn khỏi log để không spam. UI vẫn được cập nhật ở apply_rx_to_ui().
        if msg_type == "RELAY_STATE":
            relay = self.to_int(obj.get("relay"), 0)
            idx = relay - 1
            if 0 <= idx < RELAY_COUNT:
                state = self.to_int(obj.get("state"), None)
                gpio = obj.get("gpioLevel", None)
                mode = str(obj.get("mode", ""))
                same_state = (
                    state is not None and self.relay_states[idx] == state)
                same_gpio = (gpio is None or self.relay_gpio[idx] == gpio)
                if mode in ("TIMER", "SENSOR") and same_state and same_gpio:
                    return False

        if msg_type == "ACK":
            ack_for = obj.get("ack_for", "")
            p = self.pending.get(ack_for)
            if p and p.get("silent"):
                return False

        return True

    def apply_rx_to_ui(self, obj):
        msg_type = obj.get("type", "")

        if "cfgVersion" in obj:
            self.cfg_var.set(str(obj.get("cfgVersion")))

        if msg_type in ("STATUS", "FULL_STATE"):
            self.mode_var.set(str(obj.get("mode", self.mode_var.get())))
            self.device_count_var.set(
                str(obj.get("device_count", obj.get("deviceCount", self.device_count_var.get()))))
            self.update_clock_ui(obj)
            self.update_temp_fan(obj)
            self.update_relays_from_list(obj.get("relays"))
            self.parse_full_config(obj)

        elif msg_type == "RELAY_STATE":
            relay = self.to_int(obj.get("relay"), 0)
            state = self.to_int(obj.get("state"), None)
            pin = obj.get("pin", None)
            gpio = obj.get("gpioLevel", None)
            if 1 <= relay <= RELAY_COUNT:
                self.update_relay_ui(relay, state, pin, gpio)

        elif msg_type in ("FAN_STATE", "CABINET"):
            self.update_temp_fan(obj)

        elif msg_type == "CONFIG_STATE":
            self.mode_var.set(str(obj.get("mode", self.mode_var.get())))
            self.parse_config_state(obj)

        elif msg_type == "ACK":
            self.handle_ack(obj)

    def update_clock_ui(self, obj):
        # STM trả minute_of_day và time_valid trong STATUS/FULL_STATE.
        minute = obj.get("minute_of_day", None)
        valid = obj.get("time_valid", None)
        if minute is not None and minute != "":
            m = self.to_int(minute, 0) % 1440
            hh = m // 60
            mm = m % 60
            suffix = "OK" if self.to_int(valid, 0) else "CHƯA SET"
            self.clock_var.set(f"{hh:02d}:{mm:02d} ({suffix})")

    def update_temp_fan(self, obj):
        temp = obj.get("cabinetTempC", obj.get("cabinetTemp", None))
        if temp is not None and temp != "":
            try:
                self.temp_var.set(f"{float(temp):.2f} °C")
            except (TypeError, ValueError):
                self.temp_var.set(str(temp))

        fan = obj.get("fanState", obj.get("fan", None))
        if fan is not None and fan != "":
            self.fan_var.set("ON" if self.to_int(fan, 0) else "OFF")

        fan_mode = obj.get("fanMode", None)
        if fan_mode is not None and fan_mode != "":
            m = self.to_int(fan_mode, None)
            self.fan_mode_var.set(
                {0: "OFF", 1: "AUTO", 2: "ON"}.get(m, str(fan_mode)))

    def update_relays_from_list(self, relays):
        if not isinstance(relays, list):
            return

        for i, val in enumerate(relays[:RELAY_COUNT]):
            # relays có thể là [0,1] hoặc list object.
            if isinstance(val, dict):
                state = val.get("state", val.get("value", None))
                pin = val.get("pin", None)
                gpio = val.get("gpioLevel", None)
            else:
                state = val
                pin = None
                gpio = None
            self.update_relay_ui(i + 1, self.to_int(state, None), pin, gpio)

    def update_relay_ui(self, relay, state, pin=None, gpio=None):
        idx = relay - 1
        if idx < 0 or idx >= RELAY_COUNT:
            return

        if state is not None:
            self.relay_states[idx] = 1 if self.to_int(state, 0) else 0
        if pin is not None:
            self.relay_pin[idx] = pin
        if gpio is not None:
            self.relay_gpio[idx] = gpio

        s = self.relay_states[idx]
        lbl = self.relay_label_widgets[idx]
        if s is None:
            lbl.config(text="?", bg="#d9d9d9")
        elif s:
            lbl.config(text="ON", bg="#8be28b")
        else:
            lbl.config(text="OFF", bg="#ffb0b0")

        pin_txt = "--" if self.relay_pin[idx] is None else str(
            self.relay_pin[idx])
        gpio_txt = "--" if self.relay_gpio[idx] is None else str(
            self.relay_gpio[idx])
        self.relay_gpio_widgets[idx].config(
            text=f"pin:{pin_txt} gpio:{gpio_txt}")

    def parse_full_config(self, obj):
        # FULL_STATE có thể chứa timer_rules / sensor_rules tùy code STM.
        timers = obj.get("timer_rules", obj.get("timers", None))
        if isinstance(timers, list):
            self.load_timer_rules(timers, status="SYNC")

        sensors = obj.get("sensor_rules", obj.get("sensors", None))
        if isinstance(sensors, list):
            self.load_sensor_rules(sensors, status="SYNC")

    def parse_config_state(self, obj):
        cmd = obj.get("cmd", "")
        relay = self.to_int(obj.get("relay"), 0)
        if cmd == "SET_TIMER" and relay:
            schedules = obj.get("schedules", [])
            if isinstance(schedules, list):
                for s in schedules:
                    self.upsert_timer_row(relay, self.to_int(s.get("index"), 1),
                                          self.to_int(s.get("enable"), 0),
                                          str(s.get("on", "--")), str(s.get("off", "--")), "SYNC")
        elif cmd == "SET_SENSOR_RULE" and relay:
            self.upsert_sensor_row(relay, self.to_int(obj.get("enable"), 0),
                                   str(obj.get("id_device", "")), str(
                                       obj.get("field", "")),
                                   str(obj.get("logic", "")), obj.get(
                                       "onValue", ""),
                                   obj.get("offValue", ""), "SYNC")

    def load_timer_rules(self, timers, status="SYNC"):
        for item in timers:
            if isinstance(item, dict) and "schedules" in item:
                relay = self.to_int(item.get("relay"), 0)
                schedules = item.get("schedules", [])
                for s in schedules:
                    if relay:
                        self.upsert_timer_row(relay, self.to_int(s.get("index"), 1),
                                              self.to_int(s.get("enable"), 0),
                                              str(s.get("on", "--")), str(s.get("off", "--")), status)
            elif isinstance(item, dict):
                relay = self.to_int(item.get("relay"), 0)
                if relay:
                    self.upsert_timer_row(relay, self.to_int(item.get("index"), 1),
                                          self.to_int(item.get("enable"), 0),
                                          str(item.get("on", "--")), str(item.get("off", "--")), status)

    def load_sensor_rules(self, sensors, status="SYNC"):
        for item in sensors:
            if not isinstance(item, dict):
                continue
            relay = self.to_int(item.get("relay"), 0)
            if relay:
                self.upsert_sensor_row(relay, self.to_int(item.get("enable"), 0),
                                       str(item.get("id_device",
                                           item.get("idDevice", ""))),
                                       str(item.get("field", "")), str(
                                           item.get("logic", "")),
                                       item.get("onValue", item.get(
                                           "onAbove", "")),
                                       item.get("offValue", item.get("offBelow", "")), status)

    def handle_ack(self, obj):
        ack_for = obj.get("ack_for", "")
        status = obj.get("status", "")
        message = obj.get("message", "")
        p = self.pending.pop(ack_for, None)
        if not p:
            return

        cmd = p.get("cmd", "")
        meta = p.get("meta", {})

        if cmd == "SET_MODE" and status == "OK":
            mode = meta.get("mode")
            if mode:
                self.mode_var.set(str(mode))

        if cmd == "SET_TIMER":
            key = (meta.get("relay"), meta.get("index"))
            iid = self.timer_rows.get(key)
            if iid:
                values = list(self.timer_tree.item(iid, "values"))
                values[-1] = "OK" if status == "OK" else f"ERR {message}"
                self.timer_tree.item(iid, values=values)
        elif cmd == "SET_SENSOR_RULE":
            key = meta.get("relay")
            iid = self.sensor_rows.get(key)
            if iid:
                values = list(self.sensor_tree.item(iid, "values"))
                values[-1] = "OK" if status == "OK" else f"ERR {message}"
                self.sensor_tree.item(iid, values=values)

    def log_summary(self, obj):
        msg_type = obj.get("type", "")
        if msg_type == "ACK":
            self.log(
                f"[ACK] ack_for={obj.get('ack_for', '')} status={obj.get('status', '')} error_code={obj.get('error_code', '')} message={obj.get('message', '')} cfgVersion={obj.get('cfgVersion', '')}")
        elif msg_type == "RELAY_STATE":
            self.log(
                f"[RELAY_STATE] mode={obj.get('mode', '')} relay={obj.get('relay', '')} state={obj.get('state', '')} pin={obj.get('pin', '')} gpioLevel={obj.get('gpioLevel', '')}")
        elif msg_type == "STATUS":
            self.log(f"[STATUS] mode={obj.get('mode', '')} cfgVersion={obj.get('cfgVersion', '')} relays={obj.get('relays', [])} cabinetTemp={obj.get('cabinetTemp', obj.get('cabinetTempC', ''))} fan={obj.get('fan', obj.get('fanState', ''))}")
        elif msg_type == "FULL_STATE":
            self.log(
                f"[FULL_STATE] mode={obj.get('mode', '')} cfgVersion={obj.get('cfgVersion', '')}")
        elif msg_type == "CONFIG_STATE":
            self.log(
                f"[CONFIG_STATE] cmd={obj.get('cmd', '')} relay={obj.get('relay', '')} cfgVersion={obj.get('cfgVersion', '')}")

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

    def send_json(self, obj, *, silent=False, pending_cmd=None, meta=None):
        if not self.is_connected():
            return None

        if "msg_id" not in obj:
            obj["msg_id"] = self.make_msg_id(obj.get("cmd", "MSG"))
        msg_id = obj["msg_id"]

        if obj.get("ack_req", False):
            self.pending[msg_id] = {
                "cmd": pending_cmd or obj.get("cmd", ""),
                "time": time.time(),
                "silent": silent,
                "meta": meta or {}
            }

        text = json.dumps(obj, ensure_ascii=False, separators=(",", ":"))

        if not silent or (obj.get("cmd") == "GET_STATUS" and self.log_tx_status_var.get()):
            self.log("")
            self.log("========== HMI -> STM32 ==========")
            self.log(text)

        self.ser.write((text + "\n").encode("utf-8"))
        return msg_id

    def send_raw(self, text):
        if not self.is_connected():
            return
        text = text.strip()
        if not text:
            return
        self.log("")
        self.log("========== HMI -> STM32 RAW ==========")
        self.log(text)
        self.ser.write((text + "\n").encode("utf-8"))

    # ======================================================
    # COMMANDS
    # ======================================================
    def sync_now(self):
        # Ít spam: chỉ GET_STATUS. Muốn cấu hình đầy đủ thì bấm GET_FULL_STATE.
        self.cmd_get_status(silent=True)

    def auto_sync_task(self):
        if self.auto_sync_var.get() and self.ser and self.ser.is_open:
            self.cmd_get_status(silent=True)
            try:
                interval = max(3, int(float(self.sync_interval_var.get())))
            except ValueError:
                interval = 10
            self.root.after(interval * 1000, self.auto_sync_task)
        else:
            self.root.after(1000, self.auto_sync_task)

    def cmd_get_status_manual(self):
        self.cmd_get_status(silent=False)

    def cmd_get_status(self, silent=False):
        self.send_json({"msg_id": self.make_msg_id("STATUS"), "type": "CMD", "cmd": "GET_STATUS", "ack_req": True},
                       silent=silent, pending_cmd="GET_STATUS")

    def cmd_get_full_state(self):
        self.send_json({"msg_id": self.make_msg_id("FULL"), "type": "CMD",
                       "cmd": "GET_FULL_STATE", "ack_req": True}, pending_cmd="GET_FULL_STATE")

    def cmd_get_device_list(self):
        self.send_json({"msg_id": self.make_msg_id("DEV"), "type": "CMD",
                       "cmd": "GET_DEVICE_LIST", "ack_req": True}, pending_cmd="GET_DEVICE_LIST")

    def cmd_save_flash(self):
        self.send_json({"msg_id": self.make_msg_id("FLASH"), "type": "CMD",
                       "cmd": "SAVE_FLASH", "ack_req": True}, pending_cmd="SAVE_FLASH")

    def cmd_config_sync_request(self):
        self.send_json({"msg_id": self.make_msg_id("SYNC"), "type": "CMD",
                       "cmd": "CONFIG_SYNC_REQUEST", "ack_req": True}, pending_cmd="CONFIG_SYNC_REQUEST")

    def cmd_set_time_now(self, silent=False):
        now = time.localtime()
        self.clock_var.set(f"{now.tm_hour:02d}:{now.tm_min:02d} (ĐANG GỬI)")
        self.send_json({
            "msg_id": self.make_msg_id("TIME"),
            "type": "CMD",
            "cmd": "SET_TIME",
            "ack_req": True,
            "data": {
                "hour": now.tm_hour,
                "minute": now.tm_min
            }
        }, silent=silent, pending_cmd="SET_TIME")

    def cmd_set_mode(self, mode):
        # Không gửi trùng SET_MODE nếu đang chờ ACK
        for msg_id, p in list(self.pending.items()):
            if p.get("cmd") == "SET_MODE":
                self.log(f"[SKIP] Đang chờ ACK SET_MODE: {msg_id}")
                return

        self.send_json({
            "msg_id": self.make_msg_id("MODE"),
            "type": "CMD",
            "cmd": "SET_MODE",
            "ack_req": True,
            "mode": mode,
            "data": {"mode": mode}
        }, pending_cmd="SET_MODE", meta={"mode": mode})

    def cmd_set_relay(self, relay, state):
        self.send_json({
            "msg_id": self.make_msg_id("RELAY"),
            "type": "CMD",
            "cmd": "SET_RELAY",
            "ack_req": True,
            "data": {"mode": "MANUAL", "relay": relay, "state": state}
        }, pending_cmd="SET_RELAY", meta={"relay": relay, "state": state})

    def cmd_test_relay(self, relay):
        self.send_json({"msg_id": self.make_msg_id("TEST"), "type": "CMD", "cmd": "TEST_RELAY",
                       "ack_req": True, "data": {"relay": relay}}, pending_cmd="TEST_RELAY")

    def cmd_all_off(self):
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

        self.upsert_timer_row(relay, index, enable,
                              on_time, off_time, "PENDING")

        msg_id = f"T{int(time.time() * 1000) % 100000}-{self.msg_counter + 1}"

        self.send_json({
            "msg_id": msg_id,
            "cmd": "SET_TIMER",
            "relay": relay,
            "index": index,
            "enable": enable,
            "on": on_time,
            "off": off_time
        }, pending_cmd="SET_TIMER", meta={"relay": relay, "index": index})

    def cmd_set_timer_demo(self, relay):
        schedules = [
            {"index": 1, "enable": 1, "on": "06:00", "off": "07:00"},
            {"index": 2, "enable": 1, "on": "18:00", "off": "20:00"},
        ]
        self.cmd_set_time_now(silent=True)
        for s in schedules:
            self.upsert_timer_row(
                relay, s["index"], s["enable"], s["on"], s["off"], "PENDING")
        self.send_json({
            "msg_id": self.make_msg_id("TIMER"),
            "type": "CMD",
            "cmd": "SET_TIMER",
            "ack_req": True,
            "data": {"relay": relay, "schedules": schedules}
        }, pending_cmd="SET_TIMER", meta={"relay": relay, "index": 1})

    def cmd_set_sensor_rule(self):
        try:
            relay = int(self.sensor_relay_var.get())
            on_value = float(self.sensor_on_var.get())
            off_value = float(self.sensor_off_var.get())
            enable = 1 if self.sensor_enable_var.get() else 0
        except ValueError:
            messagebox.showerror("Lỗi", "Sensor rule không hợp lệ.")
            return

        dev = self.sensor_id_var.get().strip()
        field = self.sensor_field_var.get().strip()
        logic = self.sensor_logic_var.get().strip()
        self.upsert_sensor_row(relay, enable, dev, field,
                               logic, on_value, off_value, "PENDING")
        self.send_json({
            "msg_id": self.make_msg_id("SENSOR"),
            "type": "CMD",
            "cmd": "SET_SENSOR_RULE",
            "ack_req": True,
            "data": {"relay": relay, "id_device": dev, "field": field, "logic": logic,
                     "onValue": on_value, "offValue": off_value, "enable": enable}
        }, pending_cmd="SET_SENSOR_RULE", meta={"relay": relay})

    def cmd_set_sensor_rule_demo(self, relay):
        self.sensor_relay_var.set(str(relay))
        self.sensor_id_var.set("AIR001")
        self.sensor_field_var.set("co2")
        self.sensor_logic_var.set("ABOVE")
        self.sensor_on_var.set("1000")
        self.sensor_off_var.set("800")
        self.sensor_enable_var.set(1)
        self.cmd_set_sensor_rule()

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
            "data": {"fanOnTempC": fan_on, "fanOffTempC": fan_off, "cabinetHotTempC": fan_hot}
        }, pending_cmd="SET_FAN_THRESHOLD")

    def cmd_get_fan_status(self):
        # Code STM có thể dùng GET_FAN hoặc GET_FAN_STATUS tùy bản. Gửi GET_FAN theo bản STM hiện tại.
        self.send_json({"msg_id": self.make_msg_id("FAN_GET"), "type": "CMD",
                       "cmd": "GET_FAN", "ack_req": True}, pending_cmd="GET_FAN")

    def cmd_send_raw(self):
        text = self.raw_text.get("1.0", tk.END)
        self.send_raw(text)

    # ======================================================
    # LIST HELPERS
    # ======================================================
    def upsert_timer_row(self, relay, index, enable, on_time, off_time, status):
        key = (int(relay), int(index))
        values = (relay, index, enable, on_time, off_time, status)
        iid = self.timer_rows.get(key)
        if iid and self.timer_tree.exists(iid):
            self.timer_tree.item(iid, values=values)
        else:
            iid = self.timer_tree.insert("", tk.END, values=values)
            self.timer_rows[key] = iid

    def upsert_sensor_row(self, relay, enable, device, field, logic, on_value, off_value, status):
        key = int(relay)
        values = (relay, enable, device, field,
                  logic, on_value, off_value, status)
        iid = self.sensor_rows.get(key)
        if iid and self.sensor_tree.exists(iid):
            self.sensor_tree.item(iid, values=values)
        else:
            iid = self.sensor_tree.insert("", tk.END, values=values)
            self.sensor_rows[key] = iid

    def clear_timer_list(self):
        for item in self.timer_tree.get_children():
            self.timer_tree.delete(item)
        self.timer_rows.clear()

    def clear_sensor_list(self):
        for item in self.sensor_tree.get_children():
            self.sensor_tree.delete(item)
        self.sensor_rows.clear()

    # ======================================================
    # LOG + ACK TIMEOUT
    # ======================================================
    def check_ack_timeouts(self):
        now = time.time()
        expired = []
        for msg_id, p in self.pending.items():
            if now - p.get("time", now) > ACK_TIMEOUT_S:
                expired.append((msg_id, p))
        for msg_id, p in expired:
            self.pending.pop(msg_id, None)
            if not p.get("silent"):
                self.log(f"[ACK TIMEOUT] {msg_id} cmd={p.get('cmd', '')}")
        self.root.after(300, self.check_ack_timeouts)

    def log(self, text):
        self.log_text.insert(tk.END, text + "\n")
        self.log_text.see(tk.END)

    def clear_log(self):
        self.log_text.delete("1.0", tk.END)

    def on_close(self):
        self.disconnect_serial()
        self.root.destroy()

    @staticmethod
    def to_int(value, default=0):
        try:
            if value is None:
                return default
            return int(value)
        except (TypeError, ValueError):
            return default


def main():
    root = tk.Tk()
    STM32HMIApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
