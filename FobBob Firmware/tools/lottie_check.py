#!/usr/bin/env python3
"""
lottie_check.py - pre-flight validator for Lottie JSON destined for ThorVG/LVGL
on the FobBob ESP32-S3.

Usage:
    python tools/lottie_check.py                   # opens GUI file picker
    python tools/lottie_check.py "path/to/file.json"  # CLI mode, exit 0=OK 1=FAIL

Flags the features known to hang or fail ThorVG's bundled Lottie engine.
Known-good reference: the bundled LVGL approve.json (trim-path checkmark) passes clean.
"""
import json
import sys
import collections
import os


# ── Core analysis ─────────────────────────────────────────────────────────────

def check(path):
    """Analyse a Lottie JSON file. Returns (blockers, warnings, meta dict)."""
    with open(path, encoding="utf-8") as f:
        d = json.load(f)

    blockers = []
    warnings = []
    info = collections.Counter()

    # Embedded raster images -> unsupported
    for a in d.get("assets", []):
        p = a.get("p", "")
        if isinstance(p, str) and p.startswith("data:image"):
            blockers.append(
                "Embedded raster image asset (data:image/...) - ThorVG is "
                "vector-only and hangs on these (e.g. lfvideo2lottie exports)."
            )
            break

    def walk(o):
        if isinstance(o, dict):
            # Stroke dash array (d/g/o entries) - confirmed to hang ThorVG
            # whether static or animated
            if o.get("ty") == "st" and "d" in o and isinstance(o["d"], list):
                info["stroke_dash_array"] += 1
            # Fill shape (ty:fl) - suspected to hang; absent in all working files
            if o.get("ty") == "fl":
                info["fill_shape"] += 1
            # Empty shape layers - suspected to hang ThorVG's layer iterator
            if "shapes" in o and o.get("ty") == 4 and isinstance(o["shapes"], list) and len(o["shapes"]) == 0:
                info["empty_shape_layer"] += 1
            # Merge paths, polystar, rounded corners - unsupported or untested
            if o.get("ty") in ("mm", "sr", "rd"):
                info[f'ty:{o["ty"]}'] += 1
            # Expressions (ThorVG support is partial)
            if "x" in o and isinstance(o.get("x"), str):
                info["expression"] += 1
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for x in o:
                walk(x)

    walk(d)

    # Size / complexity limits derived from on-device testing:
    # working ceiling: ~6.5KB, 4 layers. Failing: 48KB, 23 layers.
    # Conservative safe threshold: <10KB and <=8 layers.
    file_size = os.path.getsize(path)
    if file_size > 10000:
        blockers.append(
            f"File size {file_size//1024}KB exceeds safe threshold (~10KB). "
            "ThorVG on this device reliably handles 4-6 layer animations up to ~7KB. "
            "Large scene graphs exhaust the parser stack and hang."
        )
    elif len(d.get("layers", [])) > 8:
        warnings.append(
            f"Layer count {len(d.get('layers', []))} is above the tested safe range (<=6). "
            "May hang depending on scene graph complexity."
        )

    if info["stroke_dash_array"]:
        blockers.append(
            f"Stroke dash array (d/g/o on ty:st) x{info['stroke_dash_array']} - "
            "confirmed to hang ThorVG on this device, even when static. "
            "Use trim-path (tm) for draw-on line effects instead."
        )
    if info["fill_shape"]:
        blockers.append(
            f"Fill shape (ty:fl) x{info['fill_shape']} - "
            "suspected to hang ThorVG on this device (absent in all known-working files)."
        )
    if info["empty_shape_layer"]:
        blockers.append(
            f"Empty shape layer x{info['empty_shape_layer']} - "
            "shape layer with no shapes, suspected to hang ThorVG's layer iterator."
        )
    for ty in ("mm", "sr", "rd"):
        if info[f"ty:{ty}"]:
            warnings.append(
                f"Shape type '{ty}' x{info[f'ty:{ty}']} - untested with this ThorVG build; "
                "may render incorrectly or hang."
            )
    if info["expression"]:
        warnings.append(
            f"Expressions x{info['expression']} - ThorVG support is partial; "
            "may not evaluate correctly."
        )

    meta = {
        "version": d.get("v", "?"),
        "fps":     d.get("fr", "?"),
        "frames":  d.get("op", "?"),
        "width":   d.get("w", "?"),
        "height":  d.get("h", "?"),
        "layers":  len(d.get("layers", [])),
        "assets":  len(d.get("assets", [])),
    }
    return blockers, warnings, meta


# ── CLI mode ──────────────────────────────────────────────────────────────────

def cli_report(path):
    blockers, warnings, meta = check(path)
    print(f"File: {path}")
    print(f"  version: {meta['version']}  fps: {meta['fps']}  "
          f"frames: {meta['frames']}  size: {meta['width']}x{meta['height']}  "
          f"layers: {meta['layers']}")

    if blockers:
        print("\n  [FAIL] WILL LIKELY HANG / FAIL:")
        for b in blockers:
            print(f"     - {b}")
    if warnings:
        print("\n  [WARN] WARNINGS:")
        for w in warnings:
            print(f"     - {w}")
    if not blockers and not warnings:
        print("\n  [OK] No known-hostile features. Safe to convert.")
    return 1 if blockers else 0


# ── GUI mode ──────────────────────────────────────────────────────────────────

def run_gui():
    import tkinter as tk
    from tkinter import filedialog, font as tkfont

    COLOR_BG      = "#1e1e2e"
    COLOR_SURFACE = "#2a2a3e"
    COLOR_BORDER  = "#3a3a5a"
    COLOR_TEXT    = "#cdd6f4"
    COLOR_SUBTLE  = "#6c7086"
    COLOR_OK      = "#a6e3a1"
    COLOR_FAIL    = "#f38ba8"
    COLOR_WARN    = "#fab387"
    COLOR_INFO    = "#89b4fa"
    COLOR_BTN     = "#585b70"
    COLOR_BTN_H   = "#7f849c"

    root = tk.Tk()
    root.title("Lottie Validator - FobBob")
    root.configure(bg=COLOR_BG)
    root.resizable(False, False)

    MONO = tkfont.Font(family="Consolas", size=10)
    BOLD = tkfont.Font(family="Consolas", size=10, weight="bold")
    BIG  = tkfont.Font(family="Consolas", size=13, weight="bold")

    # ── Header ────────────────────────────────────────────────────────────────
    tk.Label(root, text="Lottie ThorVG Validator", font=BIG,
             bg=COLOR_BG, fg=COLOR_INFO).pack(pady=(18, 2))
    tk.Label(root, text="Check a .json file before flashing to ESP32-S3",
             font=MONO, bg=COLOR_BG, fg=COLOR_SUBTLE).pack(pady=(0, 14))

    # ── File path row ─────────────────────────────────────────────────────────
    path_frame = tk.Frame(root, bg=COLOR_BG)
    path_frame.pack(padx=20, fill="x")

    path_var = tk.StringVar()
    path_entry = tk.Entry(path_frame, textvariable=path_var, font=MONO,
                          bg=COLOR_SURFACE, fg=COLOR_TEXT, insertbackground=COLOR_TEXT,
                          relief="flat", bd=6, width=52)
    path_entry.pack(side="left", ipady=4, fill="x", expand=True)

    def browse():
        initial = os.path.expanduser("~/Downloads")
        if not os.path.isdir(initial):
            initial = os.path.expanduser("~")
        p = filedialog.askopenfilename(
            title="Select Lottie JSON",
            initialdir=initial,
            filetypes=[("Lottie JSON", "*.json"), ("All files", "*.*")],
        )
        if p:
            path_var.set(p)
            run_check()

    browse_btn = tk.Button(path_frame, text="  Browse...  ", font=MONO,
                           bg=COLOR_BTN, fg=COLOR_TEXT, activebackground=COLOR_BTN_H,
                           activeforeground=COLOR_TEXT, relief="flat", bd=0, cursor="hand2",
                           command=browse)
    browse_btn.pack(side="left", padx=(8, 0), ipady=4)

    # ── Result panel ─────────────────────────────────────────────────────────
    result_frame = tk.Frame(root, bg=COLOR_SURFACE, bd=0,
                            highlightbackground=COLOR_BORDER, highlightthickness=1)
    result_frame.pack(padx=20, pady=14, fill="both")

    result_text = tk.Text(result_frame, font=MONO, bg=COLOR_SURFACE, fg=COLOR_TEXT,
                          relief="flat", bd=8, width=62, height=14,
                          state="disabled", cursor="arrow", wrap="word",
                          selectbackground=COLOR_BORDER)
    result_text.pack(padx=2, pady=2)

    # Configure colour tags
    result_text.tag_configure("ok",      foreground=COLOR_OK)
    result_text.tag_configure("fail",    foreground=COLOR_FAIL)
    result_text.tag_configure("warn",    foreground=COLOR_WARN)
    result_text.tag_configure("info",    foreground=COLOR_INFO)
    result_text.tag_configure("subtle",  foreground=COLOR_SUBTLE)
    result_text.tag_configure("bold",    font=BOLD)

    # ── Status bar ────────────────────────────────────────────────────────────
    status_var = tk.StringVar(value="Select a Lottie JSON file to begin.")
    status_bar = tk.Label(root, textvariable=status_var, font=MONO,
                          bg=COLOR_SURFACE, fg=COLOR_SUBTLE,
                          anchor="w", padx=10, pady=4)
    status_bar.pack(fill="x", side="bottom")

    def write(text, tag=None):
        result_text.configure(state="normal")
        if tag:
            result_text.insert("end", text, tag)
        else:
            result_text.insert("end", text)
        result_text.configure(state="disabled")

    def clear():
        result_text.configure(state="normal")
        result_text.delete("1.0", "end")
        result_text.configure(state="disabled")

    def run_check(*_):
        path = path_var.get().strip()
        if not path:
            return
        clear()

        if not os.path.isfile(path):
            write("File not found:\n", "fail")
            write(f"  {path}\n", "subtle")
            status_var.set("Error: file not found.")
            status_bar.configure(fg=COLOR_FAIL)
            return

        try:
            blockers, warnings, meta = check(path)
        except (json.JSONDecodeError, UnicodeDecodeError) as e:
            write("Could not parse file - is it a valid Lottie JSON?\n", "fail")
            write(f"  {e}\n", "subtle")
            status_var.set("Error: invalid JSON.")
            status_bar.configure(fg=COLOR_FAIL)
            return

        # Metadata block
        write(f"{os.path.basename(path)}\n", ("info", "bold"))
        write(f"  Lottie {meta['version']}   "
              f"{meta['fps']} fps   "
              f"{meta['frames']} frames   "
              f"{meta['width']}x{meta['height']}   "
              f"{meta['layers']} layers   "
              f"{meta['assets']} assets\n", "subtle")
        write("\n")

        if not blockers and not warnings:
            write("  [OK]  No known-hostile features.\n", ("ok", "bold"))
            write("        Safe to convert and flash.\n", "ok")
            status_var.set("[OK]  Safe to convert.")
            status_bar.configure(fg=COLOR_OK)
        else:
            if blockers:
                write("  [FAIL]  Will likely HANG the device:\n", ("fail", "bold"))
                for b in blockers:
                    # wrap long lines manually at ~58 chars
                    words = b.split()
                    line = "    - "
                    for w in words:
                        if len(line) + len(w) + 1 > 60:
                            write(line.rstrip() + "\n", "fail")
                            line = "      " + w + " "
                        else:
                            line += w + " "
                    if line.strip():
                        write(line.rstrip() + "\n", "fail")
                write("\n")

            if warnings:
                write("  [WARN]  May not render correctly:\n", ("warn", "bold"))
                for w in warnings:
                    write(f"    - {w}\n", "warn")
                write("\n")

            if blockers:
                status_var.set("[FAIL]  This file will hang the device.")
                status_bar.configure(fg=COLOR_FAIL)
            else:
                status_var.set("[WARN]  Warnings present - may render wrong.")
                status_bar.configure(fg=COLOR_WARN)



        # Advice footer
        if blockers:
            write("\n  Tip: look for animations using trim-path (not dashed-stroke)\n"
                  "  for draw-on line effects. Run the checker before converting.\n", "subtle")

    # Allow dragging a file onto the entry to auto-check
    path_entry.bind("<Return>", run_check)
    path_var.trace_add("write", lambda *_: None)   # no auto-run on type, only on Enter/Browse

    # ── Check button ──────────────────────────────────────────────────────────
    check_btn = tk.Button(root, text="  Check  ", font=BOLD,
                          bg=COLOR_INFO, fg=COLOR_BG,
                          activebackground=COLOR_BTN_H, activeforeground=COLOR_BG,
                          relief="flat", bd=0, cursor="hand2",
                          command=run_check)
    check_btn.pack(pady=(0, 16), ipadx=10, ipady=5)

    root.mainloop()


# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    if len(sys.argv) == 2:
        sys.exit(cli_report(sys.argv[1]))
    else:
        run_gui()
