"""
ZW101 Fingerprint Module Tester — FPS / EF01 Protocol
Command set mirrors the Adafruit Fingerprint Sensor Library.
Requires: pip install pyserial
"""
import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox
import serial
import serial.tools.list_ports
import threading
import time
import struct

# ── FPS protocol constants (Adafruit Fingerprint Sensor Library) ───────────────
HEADER  = bytes([0xEF, 0x01])
ADDR    = bytes([0xFF, 0xFF, 0xFF, 0xFF])
PID_CMD = 0x01   # command packet  (host → module)
PID_ACK = 0x07   # acknowledgement (module → host)

FINGERPRINT_GETIMAGE       = 0x01
FINGERPRINT_IMAGE2TZ       = 0x02
FINGERPRINT_SEARCH         = 0x04
FINGERPRINT_REGMODEL       = 0x05
FINGERPRINT_STORE          = 0x06
FINGERPRINT_LOAD           = 0x07
FINGERPRINT_DELETE         = 0x0C
FINGERPRINT_EMPTY          = 0x0D
FINGERPRINT_WRITE_REG      = 0x0E
FINGERPRINT_READSYSPARAM   = 0x0F
FINGERPRINT_SETPASSWORD    = 0x12
FINGERPRINT_VERIFYPASSWORD = 0x13
FINGERPRINT_HISPEEDSEARCH  = 0x1B
FINGERPRINT_TEMPLATECOUNT  = 0x1D
FINGERPRINT_READ_INDEX     = 0x1F
FINGERPRINT_AURALEDCONFIG  = 0x3C   # PS_ControlBLN (breathing/general LED control)
FINGERPRINT_LEDON          = 0x50
FINGERPRINT_LEDOFF         = 0x51

CONFIRM = {
    0x00: "OK",
    0x01: "Packet receive error",
    0x02: "No finger on sensor",
    0x03: "Image capture failed",
    0x06: "Image too messy",
    0x07: "Feature extraction failed",
    0x08: "No match",
    0x09: "Not found in library",
    0x0A: "Enroll mismatch — scans didn't match",
    0x0B: "Bad page ID / location out of range",
    0x0C: "DB read error / template not found",
    0x0D: "Upload feature failed",
    0x0E: "Module cannot accept packets",
    0x0F: "Upload image failed",
    0x10: "Delete failed",
    0x11: "Library clear failed",
    0x13: "Wrong password",
    0x15: "Invalid image",
    0x18: "Flash write error",
    0x1A: "Invalid register number",
    0x21: "Password verify error",
    0xFE: "Bad packet",
    0xFF: "Timeout",
}


# ── Packet helpers ─────────────────────────────────────────────────────────────

def fps_checksum(pid: int, len_bytes: bytes, body: bytes) -> int:
    return (pid + sum(len_bytes) + sum(body)) & 0xFFFF


def build_packet(ins: int, params: bytes = b'') -> bytes:
    body = bytes([ins]) + params
    lb   = struct.pack('>H', len(body) + 2)
    cs   = struct.pack('>H', fps_checksum(PID_CMD, lb, body))
    return HEADER + ADDR + bytes([PID_CMD]) + lb + body + cs


def parse_response(buf: bytes):
    """Return (confirm_code, extra_data) or raise ValueError."""
    if len(buf) < 12:
        raise ValueError(f"Too short ({len(buf)} bytes)")
    if buf[:2] != HEADER:
        raise ValueError(f"Bad header: {buf[:2].hex().upper()}")
    pid    = buf[6]
    length = struct.unpack('>H', buf[7:9])[0]
    if len(buf) < 9 + length:
        raise ValueError("Truncated body")
    body    = buf[9 : 9 + length - 2]
    cs_recv = struct.unpack('>H', buf[9 + length - 2 : 9 + length])[0]
    cs_calc = fps_checksum(pid, buf[7:9], body)
    if cs_calc != cs_recv:
        raise ValueError(f"Checksum: calc=0x{cs_calc:04X} recv=0x{cs_recv:04X}")
    if not body:
        raise ValueError("Empty body")
    return body[0], body[1:]


# ── Application ────────────────────────────────────────────────────────────────

class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("ZW101 Fingerprint Tester  [FPS / EF01]")
        self.root.geometry("980x720")
        self.root.resizable(True, True)
        self.ser: serial.Serial | None = None
        self.lock = threading.Lock()
        self._map_states: list[bool] = [False] * 50
        self._enroll_cancel = threading.Event()
        self._build()

    # ── UI ─────────────────────────────────────────────────────────────────────

    def _build(self):
        self._build_conn_bar()
        self._build_log()           # anchored to bottom first
        nb = ttk.Notebook(self.root)
        nb.pack(fill=tk.BOTH, expand=True, padx=8, pady=4)
        self._build_combined_tab(nb)
        self._build_settings_tab(nb)
        self.refresh_ports()

    def _build_conn_bar(self):
        f = ttk.LabelFrame(self.root, text="Connection", padding=6)
        f.pack(fill=tk.X, padx=8, pady=4)
        ttk.Label(f, text="Port:").grid(row=0, column=0, padx=4)
        self.port_var = tk.StringVar()
        self.port_cb  = ttk.Combobox(f, textvariable=self.port_var, width=14)
        self.port_cb.grid(row=0, column=1, padx=4)
        ttk.Label(f, text="Baud:").grid(row=0, column=2, padx=4)
        self.baud_var = tk.StringVar(value="57600")
        ttk.Combobox(f, textvariable=self.baud_var, width=10,
                     values=["9600", "19200", "38400", "57600", "115200"]
                     ).grid(row=0, column=3, padx=4)
        ttk.Button(f, text="Refresh", command=self.refresh_ports).grid(row=0, column=4, padx=4)
        self.conn_btn = ttk.Button(f, text="Connect", command=self.toggle_connect)
        self.conn_btn.grid(row=0, column=5, padx=4)
        self.conn_lbl = ttk.Label(f, text="● Disconnected", foreground="red")
        self.conn_lbl.grid(row=0, column=6, padx=10)

    def _build_combined_tab(self, nb):
        tab = ttk.Frame(nb, padding=8)
        nb.add(tab, text="  Device & Manage  ")

        tab.columnconfigure(0, weight=1)
        tab.columnconfigure(1, weight=2)
        tab.rowconfigure(0, weight=0)
        tab.rowconfigure(1, weight=0)
        tab.rowconfigure(2, weight=0)

        # ── Row 0, Col 0: Device Info ──────────────────────────────────────
        dev_f = ttk.LabelFrame(tab, text="Device Info", padding=8)
        dev_f.grid(row=0, column=0, sticky=tk.NSEW, padx=(0, 4), pady=(0, 4))

        for i, (label, cmd) in enumerate([
            ("Verify Password",      self.cmd_verify_password),
            ("Read System Params",   self.cmd_read_sys_param),
            ("Get Template Count",   self.cmd_get_count),
            ("Check Finger Present", self.cmd_query_finger),
        ]):
            ttk.Button(dev_f, text=label, command=cmd, width=24).grid(
                row=i, column=0, padx=4, pady=4, sticky=tk.W)

        ttk.Separator(dev_f, orient=tk.VERTICAL).grid(
            row=0, column=1, rowspan=4, sticky=tk.NS, padx=10)

        info_f = ttk.Frame(dev_f)
        info_f.grid(row=0, column=2, rowspan=4, sticky=tk.NW, padx=(0, 8))
        for row, key in enumerate(["Status", "Module", "Templates"]):
            ttk.Label(info_f, text=f"{key}:", foreground="gray"
                      ).grid(row=row, column=0, sticky=tk.W, padx=(0, 8), pady=4)
        self.dev_status_lbl = ttk.Label(info_f, text="—")
        self.dev_status_lbl.grid(row=0, column=1, sticky=tk.W, pady=4)
        self.dev_params_lbl = ttk.Label(info_f, text="—", wraplength=180, justify=tk.LEFT)
        self.dev_params_lbl.grid(row=1, column=1, sticky=tk.W, pady=4)
        self.dev_count_lbl = ttk.Label(info_f, text="—")
        self.dev_count_lbl.grid(row=2, column=1, sticky=tk.W, pady=4)

        # ── Row 0, Col 1: Template Management ─────────────────────────────
        mgmt_f = ttk.LabelFrame(tab, text="Template Management", padding=8)
        mgmt_f.grid(row=0, column=1, sticky=tk.NSEW, padx=(4, 0), pady=(0, 4))
        for col in range(4):
            mgmt_f.columnconfigure(col, weight=1)

        chk_box = ttk.LabelFrame(mgmt_f, text="Check ID", padding=6)
        chk_box.grid(row=0, column=0, sticky=tk.NSEW, padx=4, pady=2)
        ttk.Label(chk_box, text="ID:").grid(row=0, column=0, padx=2)
        self.chk_id = tk.StringVar(value="0")
        ttk.Entry(chk_box, textvariable=self.chk_id, width=5).grid(row=0, column=1, padx=2)
        ttk.Button(chk_box, text="Check Exists",
                   command=self.cmd_check_exists).grid(row=1, column=0, columnspan=2, pady=4)

        del1_box = ttk.LabelFrame(mgmt_f, text="Delete Single", padding=6)
        del1_box.grid(row=0, column=1, sticky=tk.NSEW, padx=4, pady=2)
        ttk.Label(del1_box, text="ID:").grid(row=0, column=0, padx=2)
        self.del_id = tk.StringVar(value="0")
        ttk.Entry(del1_box, textvariable=self.del_id, width=5).grid(row=0, column=1, padx=2)
        ttk.Button(del1_box, text="Delete",
                   command=self.cmd_delete_single).grid(row=1, column=0, columnspan=2, pady=4)

        delr_box = ttk.LabelFrame(mgmt_f, text="Delete Range", padding=6)
        delr_box.grid(row=0, column=2, sticky=tk.NSEW, padx=4, pady=2)
        r_inner = ttk.Frame(delr_box)
        r_inner.pack()
        ttk.Label(r_inner, text="First:").grid(row=0, column=0, padx=2)
        self.del_first = tk.StringVar(value="0")
        ttk.Entry(r_inner, textvariable=self.del_first, width=4).grid(row=0, column=1, padx=2)
        ttk.Label(r_inner, text="Last:").grid(row=0, column=2, padx=2)
        self.del_last = tk.StringVar(value="9")
        ttk.Entry(r_inner, textvariable=self.del_last, width=4).grid(row=0, column=3, padx=2)
        ttk.Button(delr_box, text="Delete Range",
                   command=self.cmd_delete_range).pack(pady=4)

        wipe_box = ttk.LabelFrame(mgmt_f, text="⚠  Wipe All", padding=6)
        wipe_box.grid(row=0, column=3, sticky=tk.NSEW, padx=4, pady=2)
        ttk.Label(wipe_box, text="Permanently erases\nevery stored fingerprint.",
                  foreground="#cc3300", justify=tk.CENTER).pack(pady=2)
        ttk.Button(wipe_box, text="WIPE ALL FINGERPRINTS",
                   command=self.cmd_delete_all).pack(pady=4)

        # ── Row 1, Col 0: Enrollment ───────────────────────────────────────
        enroll_f = ttk.LabelFrame(tab, text="Enrollment", padding=8)
        enroll_f.grid(row=1, column=0, sticky=tk.NSEW, padx=(0, 4), pady=(4, 4))

        opt_row = ttk.Frame(enroll_f)
        opt_row.pack(fill=tk.X, pady=2)
        ttk.Label(opt_row, text="Target ID (0–49):").grid(row=0, column=0, padx=4)
        self.enroll_id = tk.StringVar(value="0")
        ttk.Spinbox(opt_row, from_=0, to=49, textvariable=self.enroll_id,
                    width=6).grid(row=0, column=1, padx=4)
        ttk.Label(opt_row,
                  text="Scan the same finger twice. Lift between scans.",
                  foreground="gray").grid(row=0, column=2, padx=8)

        ebtn_f = ttk.Frame(enroll_f)
        ebtn_f.pack(pady=4)
        self.enroll_btn = ttk.Button(ebtn_f, text="▶  Start Enrollment",
                                     command=self.cmd_enroll, width=22)
        self.enroll_btn.grid(row=0, column=0, padx=4)
        ttk.Button(ebtn_f, text="✕  Cancel",
                   command=self.cmd_cancel_enroll, width=10).grid(row=0, column=1, padx=4)

        self.enroll_prog = ttk.Progressbar(enroll_f, mode="determinate", maximum=10)
        self.enroll_prog.pack(fill=tk.X, pady=4)
        self.enroll_lbl = ttk.Label(enroll_f, text="Ready — click Start Enrollment",
                                    font=("", 9))
        self.enroll_lbl.pack()

        # ── Row 1, Col 1: Storage Map ──────────────────────────────────────
        map_f = ttk.LabelFrame(tab, text="Storage Map  (● = enrolled)", padding=8)
        map_f.grid(row=1, column=1, sticky=tk.NSEW, padx=(4, 0), pady=(4, 4))
        map_f.columnconfigure(0, weight=1)

        ttk.Button(map_f, text="Refresh Map", command=self.cmd_storage_map
                   ).grid(row=0, column=0, sticky=tk.W, pady=(0, 6))
        self.map_canvas = tk.Canvas(map_f, bg="#1e1e1e", height=75)
        self.map_canvas.grid(row=1, column=0, sticky=tk.EW)
        self.map_canvas.bind("<Configure>", lambda _e: self._redraw_map())
        self._redraw_map()

        # ── Row 2, Col 0: Verification ────────────────────────────────────────
        verify_f = ttk.LabelFrame(tab, text="Verification", padding=8)
        verify_f.grid(row=2, column=0, sticky=tk.NSEW, padx=(0, 4), pady=(4, 0))

        
        btn_row = ttk.Frame(verify_f)
        btn_row.pack(anchor=tk.W)
        ttk.Button(btn_row, text="Match",
                   command=self.cmd_match, width=12).grid(row=0, column=0, padx=(0, 8))
        ttk.Label(btn_row, text="Timeout (s):").grid(row=0, column=1, padx=(0, 4))
        self.match_timeout = tk.StringVar(value="10")
        ttk.Spinbox(btn_row, from_=1, to=60, textvariable=self.match_timeout,
                    width=4).grid(row=0, column=2, padx=(0, 12))
        self.match_result = ttk.Label(btn_row, text="—", font=("", 13, "bold"))
        self.match_result.grid(row=0, column=3, padx=(0, 8))
        self.match_detail = ttk.Label(btn_row, text="")
        self.match_detail.grid(row=0, column=4)

        # ── Row 2, Col 1: LED Controls ────────────────────────────────────
        led_f = ttk.LabelFrame(tab, text="LED", padding=8)
        led_f.grid(row=2, column=1, sticky=tk.NSEW, padx=(4, 0), pady=(4, 0))

        opts_row = ttk.Frame(led_f)
        opts_row.pack(anchor=tk.W, pady=(0, 6))
        ttk.Label(opts_row, text="Color:").grid(row=0, column=0, padx=(0, 4))
        self.led_color = tk.StringVar(value="2")
        ttk.Combobox(opts_row, textvariable=self.led_color, state="readonly", width=14,
                     values=["1 (Red)", "2 (Blue)", "3 (Purple)", "4 (Green)",
                             "5 (Cyan)", "6 (Yellow)", "7 (White)"]
                     ).grid(row=0, column=1, padx=(0, 12))
        ttk.Label(opts_row, text="Cycles:").grid(row=0, column=2, padx=(0, 4))
        self.led_cycles = tk.StringVar(value="0")
        ttk.Spinbox(opts_row, from_=0, to=255, textvariable=self.led_cycles,
                    width=4).grid(row=0, column=3)
        ttk.Label(opts_row, text="(0 = ∞, breathing/flash only)",
                  foreground="gray").grid(row=0, column=4, padx=(6, 0))

        btn_row = ttk.Frame(led_f)
        btn_row.pack(anchor=tk.W)
        for col, (label, cmd) in enumerate([
            ("Breathing",    self.cmd_led_breathing),
            ("Flash",        self.cmd_led_flash),
            ("Steady On",    self.cmd_led_steady),
            ("Grad Open",    self.cmd_led_grad_open),
            ("Grad Close",   self.cmd_led_grad_close),
            ("Off",          self.cmd_led_off_simple),
        ]):
            ttk.Button(btn_row, text=label, command=cmd, width=10
                       ).grid(row=0, column=col, padx=(0 if col == 0 else 4, 0))

    def _build_settings_tab(self, nb):
        tab = ttk.Frame(nb, padding=10)
        nb.add(tab, text="  Settings  ")
        ttk.Label(tab, text="Module Settings",
                  font=("", 10, "bold")).pack(anchor=tk.W, pady=(0, 6))

        # Risk warning
        risk_f = ttk.LabelFrame(tab, text="⚠  Risk Warning", padding=8)
        risk_f.pack(fill=tk.X, pady=(0, 10))
        ttk.Label(
            risk_f,
            text=(
                "All settings on this page are written directly to module flash and persist after power cycle.\n"
                "• Baud rate: if you set an incorrect value you must reconnect at the new rate to recover — "
                "your current connection will stop working immediately.\n"
                "• Security level: permanently changes the false-accept / false-reject threshold for all future matches.\n"
                "• Packet size: affects all subsequent communication. Mismatch may break the connection."
            ),
            foreground="#cc3300",
            wraplength=820,
            justify=tk.LEFT,
        ).pack(anchor=tk.W)

        pf = ttk.LabelFrame(tab, text="System Parameters", padding=6)
        pf.pack(fill=tk.X, pady=4)
        self.sys_lbl = ttk.Label(pf, text="(not read yet)")
        self.sys_lbl.pack(anchor=tk.W)
        ttk.Button(pf, text="Read System Params",
                   command=self.cmd_read_sys_param).pack(pady=4)

        sf = ttk.LabelFrame(tab, text="Security Level  (WriteReg 0x05)", padding=6)
        sf.pack(fill=tk.X, pady=4)
        ttk.Label(sf, text="1 = most permissive (more false accepts)  …  "
                           "5 = strictest (fewer false accepts):").pack(anchor=tk.W)
        sf2 = ttk.Frame(sf)
        sf2.pack(pady=4)
        self.sec_level = tk.StringVar(value="3")
        ttk.Spinbox(sf2, from_=1, to=5, textvariable=self.sec_level,
                    width=4).grid(row=0, column=0, padx=6)
        ttk.Button(sf2, text="Write Security Level",
                   command=self.cmd_set_security).grid(row=0, column=1, padx=6)

        bf = ttk.LabelFrame(tab, text="Baud Rate  (WriteReg 0x04)", padding=6)
        bf.pack(fill=tk.X, pady=4)
        ttk.Label(bf, text="Change persists after power cycle. "
                           "Reconnect at new rate after writing.").pack(anchor=tk.W)
        bf2 = ttk.Frame(bf)
        bf2.pack(pady=4)
        self.baud_reg = tk.StringVar(value="6 — 57600")
        ttk.Combobox(bf2, textvariable=self.baud_reg, state="readonly", width=16,
                     values=["1 — 9600", "2 — 19200", "4 — 38400",
                             "6 — 57600", "12 — 115200"]
                     ).grid(row=0, column=0, padx=6)
        ttk.Button(bf2, text="Write Baud Rate",
                   command=self.cmd_set_baud).grid(row=0, column=1, padx=6)

        pktf = ttk.LabelFrame(tab, text="Packet Size  (WriteReg 0x06)", padding=6)
        pktf.pack(fill=tk.X, pady=4)
        pktf2 = ttk.Frame(pktf)
        pktf2.pack(pady=4)
        self.pkt_size = tk.StringVar(value="2 — 128 bytes")
        ttk.Combobox(pktf2, textvariable=self.pkt_size, state="readonly", width=16,
                     values=["0 — 32 bytes", "1 — 64 bytes",
                             "2 — 128 bytes", "3 — 256 bytes"]
                     ).grid(row=0, column=0, padx=6)
        ttk.Button(pktf2, text="Write Packet Size",
                   command=self.cmd_set_packet_size).grid(row=0, column=1, padx=6)

    def _build_log(self):
        lf = ttk.LabelFrame(self.root, text="Log", padding=4)
        lf.pack(side=tk.BOTTOM, fill=tk.BOTH, expand=False, padx=8, pady=10)
        self.log = scrolledtext.ScrolledText(lf, height=9, state=tk.DISABLED,
                                              font=("Consolas", 9),
                                              bg="#1a1a2e", fg="#00ff88")
        self.log.pack(fill=tk.BOTH, expand=True)
        ttk.Button(lf, text="Clear", command=self.clear_log).pack(anchor=tk.E)

    # ── Connection ─────────────────────────────────────────────────────────────

    def refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_cb['values'] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def toggle_connect(self):
        if self.ser and self.ser.is_open:
            self.ser.close()
            self.ser = None
            self.conn_btn.config(text="Connect")
            self.conn_lbl.config(text="● Disconnected", foreground="red")
            self.dev_status_lbl.config(text="—", foreground="")
            self.dev_params_lbl.config(text="—", foreground="")
            self.dev_count_lbl.config(text="—", foreground="")
            self.log_msg("Disconnected.")
        else:
            try:
                self.ser = serial.Serial(
                    self.port_var.get(),
                    int(self.baud_var.get()),
                    timeout=2,
                    dsrdtr=False,
                    rtscts=False,
                )
                self.ser.dtr = False
                self.ser.rts = False
                time.sleep(0.3)
                self.ser.reset_input_buffer()
                self.conn_btn.config(text="Disconnect")
                self.conn_lbl.config(text="● Connected", foreground="green")
                self.log_msg(f"Connected: {self.port_var.get()} @ {self.baud_var.get()}")
                self._auto_connect_query()
            except Exception as e:
                messagebox.showerror("Connection Error", str(e))

    def _auto_connect_query(self):
        def _run():
            time.sleep(0.2)

            # 1. Verify password
            cc, _ = self.send_recv(FINGERPRINT_VERIFYPASSWORD, bytes([0x00, 0x00, 0x00, 0x00]))
            if cc == 0x00:
                self.root.after(0, lambda: self.dev_status_lbl.config(
                    text="● OK", foreground="green"))
            elif cc == 0x13:
                self.root.after(0, lambda: self.dev_status_lbl.config(
                    text="⚠ Wrong password", foreground="#cc3300"))
            elif cc is None:
                self.root.after(0, lambda: self.dev_status_lbl.config(
                    text="⚠ No response", foreground="#cc3300"))
                return

            # 2. System parameters
            cc, data = self.send_recv(FINGERPRINT_READSYSPARAM)
            if cc == 0x00 and data and len(data) >= 16:
                capacity  = struct.unpack('>H', data[4:6])[0]
                sec_level = struct.unpack('>H', data[6:8])[0]
                pkt_idx   = struct.unpack('>H', data[12:14])[0]
                baud_n    = struct.unpack('>H', data[14:16])[0]
                pkt_map   = {0: 32, 1: 64, 2: 128, 3: 256}
                info = (f"cap={capacity}  sec={sec_level}  "
                        f"pkt={pkt_map.get(pkt_idx,'?')}B  baud={baud_n*9600}")
                self.root.after(0, lambda i=info: self.dev_params_lbl.config(text=i))
                self.root.after(0, lambda i=info: self.sys_lbl.config(text=i))

            # 3. Storage map → template count
            cc, data = self.send_recv(FINGERPRINT_READ_INDEX, bytes([0x00]))
            if cc == 0x00 and data and len(data) >= 7:
                states = []
                for i in range(50):
                    byte_i, bit_i = divmod(i, 8)
                    states.append(bool(data[byte_i] & (1 << bit_i)))
                self._map_states = states
                enrolled = sum(states)
                self.root.after(0, lambda e=enrolled: self.dev_count_lbl.config(
                    text=f"{e} enrolled"))
                self.root.after(0, self._redraw_map)
            else:
                # fallback to template count command
                cc, data = self.send_recv(FINGERPRINT_TEMPLATECOUNT)
                if cc == 0x00 and data and len(data) >= 2:
                    count = struct.unpack('>H', data[:2])[0]
                    self.root.after(0, lambda c=count: self.dev_count_lbl.config(
                        text=f"{c} enrolled"))

        threading.Thread(target=_run, daemon=True).start()

    # ── Core send/receive ──────────────────────────────────────────────────────

    def send_recv(self, ins: int, params: bytes = b'',
                  timeout: float = 3.0) -> tuple[int | None, bytes | None]:
        if not (self.ser and self.ser.is_open):
            self.log_msg("ERROR: Not connected")
            return None, None

        pkt = build_packet(ins, params)
        self.log_msg(f"TX [0x{ins:02X}]: {pkt.hex(' ').upper()}")

        with self.lock:
            self.ser.reset_input_buffer()
            self.ser.write(pkt)
            self.ser.flush()

            buf      = bytearray()
            deadline = time.time() + timeout

            # Fixed header = 9 bytes: EF01(2)+ADDR(4)+PID(1)+LEN(2)
            while len(buf) < 9 and time.time() < deadline:
                buf += self.ser.read(9 - len(buf))

            if len(buf) < 9:
                self.log_msg("TIMEOUT: no response header")
                return None, None

            length = struct.unpack('>H', buf[7:9])[0]
            total  = 9 + length

            while len(buf) < total and time.time() < deadline:
                buf += self.ser.read(total - len(buf))

            if len(buf) < total:
                self.log_msg(f"TIMEOUT: got {len(buf)}/{total} bytes")
                return None, None

        rx = bytes(buf)
        self.log_msg(f"RX ({len(rx)}B): {rx.hex(' ').upper()}")

        try:
            cc, data = parse_response(rx)
        except ValueError as e:
            self.log_msg(f"Parse error: {e}")
            return None, None

        self.log_msg(f"→ 0x{cc:02X}  {CONFIRM.get(cc, f'unknown 0x{cc:02X}')}")
        return cc, data

    # ── Device commands ────────────────────────────────────────────────────────

    def cmd_verify_password(self):
        """FINGERPRINT_VERIFYPASSWORD — default password 0x00000000"""
        cc, _ = self.send_recv(FINGERPRINT_VERIFYPASSWORD,
                               bytes([0x00, 0x00, 0x00, 0x00]))
        if cc == 0x00:
            self.log_msg("Password OK — module is alive")
            self.dev_status_lbl.config(text="● OK", foreground="green")
        elif cc == 0x13:
            self.log_msg("Wrong password")
            self.dev_status_lbl.config(text="⚠ Wrong password", foreground="#cc3300")

    def cmd_read_sys_param(self):
        """FINGERPRINT_READSYSPARAM"""
        cc, data = self.send_recv(FINGERPRINT_READSYSPARAM)
        if cc == 0x00 and data and len(data) >= 16:
            capacity   = struct.unpack('>H', data[4:6])[0]
            sec_level  = struct.unpack('>H', data[6:8])[0]
            pkt_idx    = struct.unpack('>H', data[12:14])[0]
            baud_n     = struct.unpack('>H', data[14:16])[0]
            pkt_map    = {0: 32, 1: 64, 2: 128, 3: 256}
            info = (f"cap={capacity}  sec={sec_level}  "
                    f"pkt={pkt_map.get(pkt_idx,'?')}B  baud={baud_n*9600}")
            self.log_msg(f"SysParam: {info}")
            self.sys_lbl.config(text=info)
            self.dev_params_lbl.config(text=info)

    def cmd_get_count(self):
        """FINGERPRINT_TEMPLATECOUNT"""
        cc, data = self.send_recv(FINGERPRINT_TEMPLATECOUNT)
        if cc == 0x00 and data and len(data) >= 2:
            count = struct.unpack('>H', data[:2])[0]
            self.log_msg(f"Template count: {count}")
            self.dev_count_lbl.config(text=f"{count} enrolled")

    def cmd_query_finger(self):
        """FINGERPRINT_GETIMAGE — just checks if finger is present"""
        cc, _ = self.send_recv(FINGERPRINT_GETIMAGE)
        if cc == 0x00:
            self.log_msg("Finger detected ✓ (image captured to buffer)")
        elif cc == 0x02:
            self.log_msg("No finger on sensor")

    def cmd_storage_map(self):
        """FINGERPRINT_READ_INDEX — 32-byte slot bitmap, page 0"""
        cc, data = self.send_recv(FINGERPRINT_READ_INDEX, bytes([0x00]))
        if cc == 0x00 and data and len(data) >= 7:
            states = []
            for i in range(50):
                byte_i, bit_i = divmod(i, 8)
                states.append(bool(data[byte_i] & (1 << bit_i)))
            self._map_states = states
            self._redraw_map()
            enrolled = [i for i, s in enumerate(states) if s]
            self.log_msg(f"Storage map: {len(enrolled)} enrolled — {enrolled}")
            self.dev_count_lbl.config(text=f"{len(enrolled)} enrolled")
        else:
            self.log_msg("ReadIndex not supported; showing count only")
            self.cmd_get_count()

    def _redraw_map(self):
        c = self.map_canvas
        c.delete("all")
        w    = c.winfo_width() or 900
        cols = 25
        cell = min(w // cols, 50)
        pad  = 4
        for i in range(50):
            col   = i % cols
            row_i = i // cols
            x0 = col   * cell + pad
            y0 = row_i * cell + pad
            x1 = x0 + cell - pad
            y1 = y0 + cell - pad
            fill = "#00cc44" if (i < len(self._map_states) and self._map_states[i]) else "#333333"
            c.create_rectangle(x0, y0, x1, y1, fill=fill, outline="#555")
            c.create_text((x0 + x1) // 2, (y0 + y1) // 2,
                          text=str(i), fill="white", font=("", 7))

    # ── Enroll commands ────────────────────────────────────────────────────────

    def cmd_enroll(self):
        try:
            fp_id = int(self.enroll_id.get())
            if not (0 <= fp_id <= 49):
                raise ValueError
        except ValueError:
            messagebox.showerror("Input Error", "ID must be 0–49")
            return

        # Refresh storage map to pick next free slot if needed
        try:
            self.cmd_storage_map()
            free_idx = None
            for i, s in enumerate(self._map_states):
                if not s:
                    free_idx = i
                    break
            if free_idx is None:
                messagebox.showerror("No Space", "No free fingerprint slots available.")
                return
            # If requested ID is occupied, pick next free
            if self._map_states[fp_id]:
                fp_id = free_idx
                self.enroll_id.set(str(fp_id))
                self.log_msg(f"Using next available ID {fp_id}")
        except Exception as e:
            self.log_msg(f"Storage map check failed: {e}")

        self._enroll_cancel.clear()
        self.enroll_btn.config(state=tk.DISABLED)
        self.enroll_prog.config(value=0, maximum=10)
        threading.Thread(target=self._enroll_worker, args=(fp_id,),
                         daemon=True).start()

    def _enroll_worker(self, target_id: int):
        def status(msg):
            self.log_msg(f"[Enroll] {msg}")
            self.root.after(0, lambda: self.enroll_lbl.config(text=msg))

        def progress(v):
            self.root.after(0, lambda: self.enroll_prog.config(value=v))

        def done():
            self.root.after(0, lambda: self.enroll_btn.config(state=tk.NORMAL))

        cancelled = self._enroll_cancel

        try:
            # ── Scan 1 → feature buffer 1 ─────────────────────────────────────
            status("Scan 1/2: Place finger on sensor…")
            while not cancelled.is_set():
                cc, _ = self.send_recv(FINGERPRINT_GETIMAGE)
                if cc == 0x00:
                    break
                if cc == 0x02:
                    time.sleep(0.1)
                    continue
                status(f"Image error: {CONFIRM.get(cc, f'0x{cc:02X}')}")
                return
            if cancelled.is_set():
                status("Cancelled")
                return

            cc, _ = self.send_recv(FINGERPRINT_IMAGE2TZ, bytes([0x01]))
            if cc != 0x00:
                status(f"Feature extraction failed: {CONFIRM.get(cc, f'0x{cc:02X}')}")
                return
            progress(2)

            # ── Wait for finger lift ───────────────────────────────────────────
            status("Lift your finger…")
            while not cancelled.is_set():
                cc, _ = self.send_recv(FINGERPRINT_GETIMAGE)
                if cc == 0x02:
                    break
                time.sleep(0.1)
            if cancelled.is_set():
                status("Cancelled")
                return
            time.sleep(0.3)
            progress(4)

            # ── Scan 2 → feature buffer 2 ─────────────────────────────────────
            status("Scan 2/2: Place same finger on sensor again…")
            while not cancelled.is_set():
                cc, _ = self.send_recv(FINGERPRINT_GETIMAGE)
                if cc == 0x00:
                    break
                if cc == 0x02:
                    time.sleep(0.1)
                    continue
                status(f"Image error: {CONFIRM.get(cc, f'0x{cc:02X}')}")
                return
            if cancelled.is_set():
                status("Cancelled")
                return

            cc, _ = self.send_recv(FINGERPRINT_IMAGE2TZ, bytes([0x02]))
            if cc != 0x00:
                status(f"Feature extraction failed: {CONFIRM.get(cc, f'0x{cc:02X}')}")
                return
            progress(6)

            # ── Merge features into template ───────────────────────────────────
            status("Merging features…")
            cc, _ = self.send_recv(FINGERPRINT_REGMODEL)
            if cc == 0x0A:
                status("ERROR: Scans didn't match — use the same finger for both scans")
                return
            if cc != 0x00:
                status(f"Model error: {CONFIRM.get(cc, f'0x{cc:02X}')}")
                return
            progress(8)

            # ── Store template to flash ────────────────────────────────────────
            status(f"Storing as ID {target_id}…")
            cc, _ = self.send_recv(FINGERPRINT_STORE,
                                   bytes([0x01,
                                          (target_id >> 8) & 0xFF,
                                          target_id & 0xFF]))
            if cc == 0x00:
                status(f"✓ Enrolled successfully as ID {target_id}")
                progress(10)
                self.root.after(0, self.cmd_storage_map)
            else:
                status(f"Store failed: {CONFIRM.get(cc, f'0x{cc:02X}')}")
        finally:
            done()

    def cmd_cancel_enroll(self):
        self._enroll_cancel.set()
        self.enroll_lbl.config(text="Cancelling…")
        self.enroll_btn.config(state=tk.NORMAL)

    # ── Verify commands ────────────────────────────────────────────────────────

    def cmd_match(self):
        self.match_result.config(text="Waiting…", foreground="gray")
        #self.match_detail.config(text="Place finger on sensor")

        def _run():
            try:
                wait = max(1, int(self.match_timeout.get()))
            except ValueError:
                wait = 10
            deadline = time.time() + wait

            # Poll for finger
            while time.time() < deadline:
                cc, _ = self.send_recv(FINGERPRINT_GETIMAGE)
                if cc == 0x00:
                    break
                if cc == 0x02:
                    time.sleep(0.1)
                    continue
                msg = CONFIRM.get(cc, f"0x{cc:02X}")
                self.root.after(0, lambda m=msg: self.match_result.config(
                    text=f"Error: {m}", foreground="red"))
                return
            else:
                self.root.after(0, lambda: self.match_result.config(
                    text="Timeout — no finger", foreground="orange"))
                self.root.after(0, lambda: self.match_detail.config(text=""))
                return

            # Extract features
            cc, _ = self.send_recv(FINGERPRINT_IMAGE2TZ, bytes([0x01]))
            if cc != 0x00:
                msg = CONFIRM.get(cc, f"0x{cc:02X}")
                self.root.after(0, lambda m=msg: self.match_result.config(
                    text=f"Feature error: {m}", foreground="red"))
                return

            # High-speed search: buf=1, start=0, count=163 (covers all 50 slots)
            cc, data = self.send_recv(FINGERPRINT_HISPEEDSEARCH,
                                      bytes([0x01, 0x00, 0x00, 0x00, 0xA3]))
            # Some modules return OK with no payload when there's no match.
            # If we get OK with no payload, try a SEARCH fallback which some
            # sensors implement instead of HISPEEDSEARCH.
            if cc == 0x00 and (not data or len(data) < 4):
                self.log_msg("HISPEEDSEARCH returned OK but no payload — trying SEARCH fallback")
                cc2, data2 = self.send_recv(FINGERPRINT_SEARCH,
                                            bytes([0x01, 0x00, 0x00, 0x00, 0xA3]))
                # Prefer the SEARCH response if it contains a match
                if cc2 == 0x00 and data2 and len(data2) >= 4:
                    cc, data = cc2, data2
                else:
                    cc = cc2
                    data = data2

            if cc == 0x00:
                if data and len(data) >= 4:
                    fp_id = struct.unpack('>H', data[0:2])[0]
                    score = struct.unpack('>H', data[2:4])[0]
                    self.root.after(0, lambda: self.match_result.config(
                        text=f"MATCH  —  ID {fp_id}", foreground="#00aa00"))
                else:
                    self.root.after(0, lambda: self.match_result.config(
                        text="NO MATCH", foreground="red"))
                    self.root.after(0, lambda: self.match_detail.config(text=""))
            elif cc == 0x09:
                self.root.after(0, lambda: self.match_result.config(
                    text="NO MATCH", foreground="red"))
                self.root.after(0, lambda: self.match_detail.config(text=""))
            elif cc == 0x1E:
                self.root.after(0, lambda: self.match_result.config(
                    text="Library is empty", foreground="red"))
                self.root.after(0, lambda: self.match_detail.config(text=""))
            else:
                msg = CONFIRM.get(cc, f"0x{cc:02X}") if cc is not None else "comm error"
                self.root.after(0, lambda m=msg: self.match_result.config(
                    text=f"Error: {m}", foreground="red"))

        threading.Thread(target=_run, daemon=True).start()

    # ── Manage commands ────────────────────────────────────────────────────────

    def cmd_check_exists(self):
        try:
            fp_id = int(self.chk_id.get())
        except ValueError:
            messagebox.showerror("Input Error", "Invalid ID")
            return
        # FINGERPRINT_LOAD to buffer 1; 0x00=loaded(exists), 0x0C=not found
        cc, _ = self.send_recv(FINGERPRINT_LOAD,
                               bytes([0x01,
                                      (fp_id >> 8) & 0xFF,
                                      fp_id & 0xFF]))
        if cc == 0x00:
            self.log_msg(f"ID {fp_id}: EXISTS")
        elif cc == 0x0C:
            self.log_msg(f"ID {fp_id}: NOT FOUND")
        else:
            self.log_msg(f"ID {fp_id}: {CONFIRM.get(cc, f'0x{cc:02X}')}")

    def cmd_delete_single(self):
        try:
            fp_id = int(self.del_id.get())
        except ValueError:
            messagebox.showerror("Input Error", "Invalid ID")
            return
        if not messagebox.askyesno("Confirm", f"Delete fingerprint ID {fp_id}?"):
            return
        # FINGERPRINT_DELETE: [id_hi][id_lo][count_hi][count_lo]  count=1
        cc, _ = self.send_recv(FINGERPRINT_DELETE,
                               bytes([(fp_id >> 8) & 0xFF, fp_id & 0xFF,
                                      0x00, 0x01]))
        if cc == 0x00:
            self.log_msg(f"ID {fp_id} deleted")
            self.cmd_storage_map()

    def cmd_delete_range(self):
        try:
            first = int(self.del_first.get())
            last  = int(self.del_last.get())
        except ValueError:
            messagebox.showerror("Input Error", "Invalid range")
            return
        if not messagebox.askyesno("Confirm", f"Delete IDs {first}–{last}?"):
            return
        count = last - first + 1
        cc, _ = self.send_recv(FINGERPRINT_DELETE,
                               bytes([(first >> 8) & 0xFF, first & 0xFF,
                                      (count >> 8) & 0xFF, count & 0xFF]))
        if cc == 0x00:
            self.log_msg(f"Deleted IDs {first}–{last}")
            self.cmd_storage_map()

    def cmd_delete_all(self):
        if not messagebox.askyesno("WIPE ALL",
                                    "Permanently delete ALL fingerprints?\n\nThis cannot be undone!"):
            return
        cc, _ = self.send_recv(FINGERPRINT_EMPTY)
        if cc == 0x00:
            self.log_msg("ALL fingerprints wiped.")
            self.cmd_storage_map()

    # ── LED commands ───────────────────────────────────────────────────────────

    # color bit-mask: bit0=Blue(0x01), bit1=Green(0x02), bit2=Red(0x04)
    _CMAP = {1: 0x04, 2: 0x01, 3: 0x05, 4: 0x02, 5: 0x03, 6: 0x06, 7: 0x07, 0: 0x00}

    def _led_color_mask(self) -> int:
        try:
            return self._CMAP.get(int(self.led_color.get().split()[0]), 0x00)
        except Exception:
            return 0x01  # default blue

    def _led_cycles(self) -> int:
        try:
            return max(0, min(255, int(self.led_cycles.get())))
        except ValueError:
            return 0

    def _led_send(self, func: int, start: int, end: int, cycles: int, label: str):
        cc, _ = self.send_recv(FINGERPRINT_AURALEDCONFIG,
                               bytes([func, start, end, cycles]))
        if cc == 0x00:
            self.log_msg(f"LED → {label}")

    def cmd_led_breathing(self):
        # func=1: breathing; start=rising phase color, end=falling phase color
        m = self._led_color_mask()
        self._led_send(1, m, m, self._led_cycles(), f"Breathing (cycles={self._led_cycles()})")

    def cmd_led_flash(self):
        # func=2: flash on/off
        m = self._led_color_mask()
        self._led_send(2, m, m, self._led_cycles(), f"Flash (cycles={self._led_cycles()})")

    def cmd_led_steady(self):
        # func=3: normally open (steady on)
        m = self._led_color_mask()
        self._led_send(3, m, m, 0, "Steady On")

    def cmd_led_grad_open(self):
        # func=5: gradually open (fade in)
        m = self._led_color_mask()
        self._led_send(5, m, m, 0, "Gradually Open")

    def cmd_led_grad_close(self):
        # func=6: gradually close (fade out)
        m = self._led_color_mask()
        self._led_send(6, m, m, 0, "Gradually Close")

    def cmd_led_off_simple(self):
        # func=4: normally closed (off)
        self._led_send(4, 0x00, 0x00, 0, "Off")

    # ── Settings commands ──────────────────────────────────────────────────────

    def cmd_set_security(self):
        try:
            level = int(self.sec_level.get())
        except ValueError:
            return
        cc, _ = self.send_recv(FINGERPRINT_WRITE_REG, bytes([0x05, level]))
        if cc == 0x00:
            self.log_msg(f"Security level → {level}")

    def cmd_set_baud(self):
        try:
            n = int(self.baud_reg.get().split()[0])
        except (ValueError, IndexError):
            return
        cc, _ = self.send_recv(FINGERPRINT_WRITE_REG, bytes([0x04, n]))
        if cc == 0x00:
            self.log_msg(f"Baud rate → {n * 9600}  (reconnect at new rate)")

    def cmd_set_packet_size(self):
        try:
            idx = int(self.pkt_size.get().split()[0])
        except (ValueError, IndexError):
            return
        cc, _ = self.send_recv(FINGERPRINT_WRITE_REG, bytes([0x06, idx]))
        if cc == 0x00:
            sizes = {0: 32, 1: 64, 2: 128, 3: 256}
            self.log_msg(f"Packet size → {sizes.get(idx, '?')}B")

    # ── Log helpers ────────────────────────────────────────────────────────────

    def log_msg(self, msg: str):
        ts = time.strftime("%H:%M:%S")

        def _append():
            self.log.config(state=tk.NORMAL)
            self.log.insert(tk.END, f"[{ts}] {msg}\n")
            self.log.see(tk.END)
            self.log.config(state=tk.DISABLED)

        self.root.after(0, _append)

    def clear_log(self):
        self.log.config(state=tk.NORMAL)
        self.log.delete("1.0", tk.END)
        self.log.config(state=tk.DISABLED)


if __name__ == "__main__":
    root = tk.Tk()
    App(root)
    root.mainloop()
