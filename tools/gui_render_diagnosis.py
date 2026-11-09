#!/usr/bin/env python3
"""Controlled real-desktop GUI render observations.

This helper launches the existing player with the Conan run environment, fixes
the XWayland window geometry, and records foreground or intentionally
unfocused-visible observations. It never uses offscreen rendering, synthetic
media, a forced process kill, or an alternate audio device. The focus result is
always reported from the observed X11 state; a request that the compositor
rejects is not called a valid unfocused/foreground phase.
"""

import argparse
import ctypes as ct
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]


class X11:
    def __init__(self):
        self.lib = ct.CDLL("libX11.so.6")
        L = self.lib
        L.XOpenDisplay.argtypes = [ct.c_char_p]
        L.XOpenDisplay.restype = ct.c_void_p
        L.XCloseDisplay.argtypes = [ct.c_void_p]
        L.XInternAtom.argtypes = [ct.c_void_p, ct.c_char_p, ct.c_int]
        L.XInternAtom.restype = ct.c_ulong
        L.XDefaultRootWindow.argtypes = [ct.c_void_p]
        L.XDefaultRootWindow.restype = ct.c_ulong
        L.XSendEvent.argtypes = [ct.c_void_p, ct.c_ulong, ct.c_int, ct.c_long, ct.c_void_p]
        L.XSendEvent.restype = ct.c_int
        L.XFlush.argtypes = [ct.c_void_p]
        L.XMapRaised.argtypes = [ct.c_void_p, ct.c_ulong]
        L.XMoveResizeWindow.argtypes = [ct.c_void_p, ct.c_ulong, ct.c_int, ct.c_int, ct.c_uint, ct.c_uint]
        L.XCreateSimpleWindow.argtypes = [ct.c_void_p, ct.c_ulong, ct.c_int, ct.c_int, ct.c_uint, ct.c_uint,
                                          ct.c_uint, ct.c_ulong, ct.c_ulong]
        L.XCreateSimpleWindow.restype = ct.c_ulong
        L.XSetInputFocus.argtypes = [ct.c_void_p, ct.c_ulong, ct.c_int, ct.c_ulong]
        L.XSetInputFocus.restype = ct.c_int
        L.XDestroyWindow.argtypes = [ct.c_void_p, ct.c_ulong]
        L.XStoreName.argtypes = [ct.c_void_p, ct.c_ulong, ct.c_char_p]
        L.XDisplayWidth.argtypes = [ct.c_void_p, ct.c_int]
        L.XDisplayHeight.argtypes = [ct.c_void_p, ct.c_int]
        self.display = L.XOpenDisplay(b":0")
        if not self.display:
            raise RuntimeError("cannot open DISPLAY=:0")

    def close(self):
        if self.display:
            self.lib.XCloseDisplay(self.display)
            self.display = None

    def atom(self, name):
        return self.lib.XInternAtom(self.display, name.encode(), 0)

    def move_resize(self, window, x, y, width, height):
        # A managed/reparented XWayland client must ask the WM to configure it.
        self.message(window, "_NET_MOVERESIZE_WINDOW",
                     [10 | (15 << 8) | (2 << 12), x, y, width, height])
        self.lib.XMapRaised(self.display, int(window, 16))
        self.lib.XFlush(self.display)

    def activate(self, window):
        sent = self.message(window, "_NET_ACTIVE_WINDOW", [2, 0, 0, 0, 0])
        self.lib.XMapRaised(self.display, int(window, 16))
        self.lib.XFlush(self.display)
        return sent

    def keep_above(self, window):
        self.message(window, "_NET_WM_STATE", [1, self.atom("_NET_WM_STATE_ABOVE"), 0, 2, 0])

    def message(self, window, name, values):
        class Data(ct.Union):
            _fields_ = [("b", ct.c_char * 20), ("s", ct.c_short * 10), ("l", ct.c_long * 5)]

        class Client(ct.Structure):
            _fields_ = [("type", ct.c_int), ("serial", ct.c_ulong), ("send_event", ct.c_int),
                        ("display", ct.c_void_p), ("window", ct.c_ulong), ("message_type", ct.c_ulong),
                        ("format", ct.c_int), ("data", Data)]

        class Event(ct.Union):
            _fields_ = [("client", Client), ("pad", ct.c_long * 24)]

        event = Event()
        event.client.type = 33  # ClientMessage
        event.client.display = self.display
        event.client.window = int(window, 16)
        event.client.message_type = self.atom(name)
        event.client.format = 32
        for i, value in enumerate(values):
            event.client.data.l[i] = value
        root = self.lib.XDefaultRootWindow(self.display)
        sent = self.lib.XSendEvent(self.display, root, 0, (1 << 20) | (1 << 19), ct.byref(event))
        self.lib.XFlush(self.display)
        return bool(sent)

    def create_occluder(self):
        root = self.lib.XDefaultRootWindow(self.display)
        width = self.lib.XDisplayWidth(self.display, 0)
        height = self.lib.XDisplayHeight(self.display, 0)
        cover = self.lib.XCreateSimpleWindow(self.display, root, 0, 0, width, height, 0, 0, 0)
        self.lib.XStoreName(self.display, cover, b"GUI render diagnosis: controlled occlusion")
        self.lib.XMapRaised(self.display, cover)
        self.message(hex(cover), "_NET_WM_STATE", [1, self.atom("_NET_WM_STATE_FULLSCREEN"), 0, 2, 0])
        self.keep_above(hex(cover))
        return cover

    def create_focus_sink(self):
        root = self.lib.XDefaultRootWindow(self.display)
        sink = self.lib.XCreateSimpleWindow(self.display, root, 1800, 100, 64, 64, 0, 0, 0)
        self.lib.XStoreName(self.display, sink, b"GUI render diagnosis: focus sink")
        self.lib.XMapRaised(self.display, sink)
        self.activate(hex(sink))
        self.lib.XFlush(self.display)
        return sink

    def destroy(self, window):
        if window:
            self.lib.XDestroyWindow(self.display, window)
            self.lib.XFlush(self.display)


def player_window(pid):
    tree = subprocess.check_output(["xwininfo", "-display", ":0", "-root", "-tree"], text=True)
    for match in re.finditer(r'(0x[0-9a-f]+) "[^"\n]*": \("media_player" "media_player"\)', tree):
        window = match[1]
        prop = subprocess.check_output(["xprop", "-display", ":0", "-id", window, "_NET_WM_PID"], text=True)
        if re.search(rf'=\s*{pid}\s*$', prop):
            return window
    return None


def window_info(window):
    info = subprocess.check_output(["xwininfo", "-display", ":0", "-id", window], text=True)
    values = {}
    for key, pattern in {
        "x": r"Absolute upper-left X:\s*(-?\d+)",
        "y": r"Absolute upper-left Y:\s*(-?\d+)",
        "width": r"Width:\s*(\d+)",
        "height": r"Height:\s*(\d+)",
        "map_state": r"Map State:\s*(\w+)",
    }.items():
        match = re.search(pattern, info)
        values[key] = int(match[1]) if match and key not in ("map_state",) else (match[1] if match else None)
    state = subprocess.check_output(["xprop", "-display", ":0", "-id", window, "_NET_WM_STATE"], text=True).strip()
    root_state = subprocess.check_output(["xprop", "-display", ":0", "-root", "_NET_ACTIVE_WINDOW"], text=True).strip()
    active = bool(re.search(r"#\s*" + re.escape(window) + r"\s*$", root_state))
    return {**values, "net_wm_state": state, "root_active_window": root_state,
            "active_observed": active, "window_info": info}


def process_sample(pid, started, previous=None):
    proc = Path(f"/proc/{pid}")
    fields = (proc / "stat").read_text().rsplit(")", 1)[1].split()
    sample = {"elapsed_sec": time.monotonic() - started,
              "cpu_seconds": (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK"),
              "rss_bytes": int(fields[21]) * os.sysconf("SC_PAGE_SIZE"),
              "threads": int(fields[17]), "fd_count": len(list((proc / "fd").iterdir()))}
    if previous:
        dt = sample["elapsed_sec"] - previous["elapsed_sec"]
        sample["process_cpu_one_core_pct"] = 100 * (sample["cpu_seconds"] - previous["cpu_seconds"]) / dt if dt > 0 else 0
    return sample


def close_window(x11, window):
    protocol = x11.atom("WM_PROTOCOLS")
    delete = x11.atom("WM_DELETE_WINDOW")
    class Data(ct.Union):
        _fields_ = [("b", ct.c_char * 20), ("s", ct.c_short * 10), ("l", ct.c_long * 5)]
    class Client(ct.Structure):
        _fields_ = [("type", ct.c_int), ("serial", ct.c_ulong), ("send_event", ct.c_int),
                    ("display", ct.c_void_p), ("window", ct.c_ulong), ("message_type", ct.c_ulong),
                    ("format", ct.c_int), ("data", Data)]
    class Event(ct.Union):
        _fields_ = [("client", Client), ("pad", ct.c_long * 24)]
    event = Event()
    event.client.type = 33
    event.client.display = x11.display
    event.client.window = int(window, 16)
    event.client.message_type = protocol
    event.client.format = 32
    event.client.data.l[0] = delete
    if not x11.lib.XSendEvent(x11.display, int(window, 16), 0, 0, ct.byref(event)):
        raise RuntimeError("WM_DELETE_WINDOW could not be sent")
    x11.lib.XFlush(x11.display)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("media", type=Path)
    parser.add_argument("--mode", choices=["software", "vaapi", "auto"], required=True)
    parser.add_argument("--phase", choices=["foreground", "unfocused", "occluded"], required=True)
    parser.add_argument("--sample-seconds", type=float, default=30.0)
    parser.add_argument("--geometry", default="100,100,1280,720", help="x,y,width,height in X11 root coordinates")
    args = parser.parse_args()
    media = args.media.resolve(strict=True)
    try:
        x, y, width, height = [int(v) for v in args.geometry.split(",")]
    except ValueError as exc:
        raise SystemExit("--geometry must be x,y,width,height") from exc
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    run = ROOT / "logs/gui-render-diagnosis" / f"{stamp}-{media.stem}-{args.mode}-{args.phase}"
    run.mkdir(parents=True, exist_ok=False)
    (run / "media.json").write_text(subprocess.check_output(["ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", str(media)], text=True))
    env = os.environ.copy()
    env.update(DISPLAY=":0", QT_QPA_PLATFORM="xcb", QT_XCB_GL_INTEGRATION="xcb_egl")
    command = [str(ROOT / "build/RelWithDebInfo/app/media_player"), "--video-decoder", args.mode, str(media)]
    shell = "source " + shlex.quote(str(ROOT / "build/RelWithDebInfo/generators/conanrun.sh")) + "\nexec " + shlex.join(command)
    manifest = {"command": command, "media": str(media), "mode": args.mode, "phase": args.phase,
                "sample_seconds": args.sample_seconds, "geometry": [x, y, width, height],
                "environment": {k: env.get(k) for k in ["DISPLAY", "WAYLAND_DISPLAY", "XDG_SESSION_TYPE", "XDG_RUNTIME_DIR", "QT_QPA_PLATFORM", "QT_XCB_GL_INTEGRATION"]},
                "started_utc": datetime.now(timezone.utc).isoformat()}
    x11 = X11()
    proc = None
    focus_sink = None
    occluder = None
    try:
        with (run / "console.log").open("x") as output:
            proc = subprocess.Popen(["bash", "--noprofile", "--norc", "-c", shell], cwd=run, env=env, stdin=subprocess.DEVNULL,
                                    stdout=output, stderr=subprocess.STDOUT)
            manifest["pid"] = proc.pid
            (run / "manifest.json").write_text(json.dumps(manifest, indent=2))
            started = time.monotonic()
            window = None
            previous = None
            screenshot_done = False
            close_at = None
            close_reason = None
            measurement_started = None
            phase_valid_streak = 0
            valid_samples = 0
            observed_samples = 0
            states = (run / "window-state.jsonl").open("x")
            samples = (run / "process-samples.jsonl").open("x")
            try:
                while proc.poll() is None:
                    now = time.monotonic()
                    if window is None:
                        window = player_window(proc.pid)
                        if window:
                            x11.move_resize(window, x, y, width, height)
                            if args.phase == "foreground":
                                x11.keep_above(window)
                                activation_requested = x11.activate(window)
                            elif args.phase == "unfocused":
                                x11.keep_above(window)
                                focus_sink = x11.create_focus_sink()
                                activation_requested = False
                            else:
                                activation_requested = False
                            (run / "focus-request.json").write_text(json.dumps({"phase": args.phase, "activation_requested": activation_requested, "focus_sink_created": bool(focus_sink)}, indent=2))
                    if window:
                        state = window_info(window)
                        # Allow the WM's placement policy, but require a constant
                        # requested client size and an entirely on-screen client.
                        screen_width = x11.lib.XDisplayWidth(x11.display, 0)
                        screen_height = x11.lib.XDisplayHeight(x11.display, 0)
                        on_screen = (0 <= state["x"] and 0 <= state["y"] and
                                     state["x"] + width <= screen_width and state["y"] + height <= screen_height)
                        if args.phase == "foreground" and not state["active_observed"]:
                            x11.activate(window)
                        if args.phase == "unfocused" and focus_sink and state["active_observed"]:
                            sink_state = window_info(hex(focus_sink))
                            if sink_state["map_state"] == "IsViewable":
                                x11.activate(hex(focus_sink))
                                x11.lib.XSetInputFocus(x11.display, focus_sink, 2, 0)
                                x11.lib.XFlush(x11.display)
                        if not on_screen:
                            x11.move_resize(window, x, y, width, height)
                        if args.phase == "occluded" and occluder is None and now - started >= 3:
                            occluder = x11.create_occluder()
                        cover_state = window_info(hex(occluder)) if occluder else None
                        if cover_state and not cover_state["active_observed"]:
                            x11.activate(hex(occluder))
                        stacking = subprocess.check_output(["xprop", "-display", ":0", "-root", "_NET_CLIENT_LIST_STACKING"], text=True)
                        stack_ids = re.findall(r"0x[0-9a-f]+", stacking)
                        cover_above = bool(occluder and hex(occluder) in stack_ids and window in stack_ids and
                                           stack_ids.index(hex(occluder)) > stack_ids.index(window))
                        covered = bool(cover_state and cover_state["map_state"] == "IsViewable" and cover_above and
                                       cover_state["x"] <= state["x"] and cover_state["y"] <= state["y"] and
                                       cover_state["x"] + cover_state["width"] >= state["x"] + width and
                                       cover_state["y"] + cover_state["height"] >= state["y"] + height)
                        state_ok = (state["map_state"] == "IsViewable" and
                                    "_NET_WM_STATE_HIDDEN" not in state["net_wm_state"] and
                                    [state["width"], state["height"]] == [width, height] and on_screen and
                                    (state["active_observed"] if args.phase == "foreground" else not state["active_observed"]) and
                                    (args.phase != "occluded" or covered))
                        phase_valid_streak = phase_valid_streak + 1 if state_ok else 0
                        if measurement_started is None and phase_valid_streak >= 2:
                            measurement_started = now
                        in_measurement = measurement_started is not None and close_at is None
                        if in_measurement:
                            observed_samples += 1
                            valid_samples += int(state_ok)
                        state.update({"timestamp_monotonic": time.monotonic() - started, "phase": args.phase,
                                      "phase_state_valid": state_ok, "in_measurement": in_measurement,
                                      "occluder": cover_state, "controlled_occlusion": covered,
                                      "client_list_stacking": stacking})
                        states.write(json.dumps(state) + "\n"); states.flush()
                        if not screenshot_done and measurement_started is not None and now - measurement_started >= 5:
                            info = state
                            grab = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "warning", "-f", "x11grab", "-draw_mouse", "0", "-window_id", window,
                                                   "-video_size", f"{info['width']}x{info['height']}", "-i", ":0", "-frames:v", "1", "-update", "1", "-n", str(run / "window.png")], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                            (run / "screenshot.log").write_text(grab.stdout)
                            screenshot_done = grab.returncode == 0 and (run / "window.png").exists()
                            desktop_grab = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "warning", "-f", "x11grab", "-draw_mouse", "0",
                                                          "-video_size", f"{screen_width}x{screen_height}", "-i", ":0", "-frames:v", "1", "-update", "1", "-n", str(run / "desktop.png")],
                                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                            (run / "desktop-screenshot.log").write_text(desktop_grab.stdout)
                    try:
                        sample = process_sample(proc.pid, started, previous)
                        previous = sample
                        samples.write(json.dumps(sample) + "\n"); samples.flush()
                    except FileNotFoundError:
                        break
                    if close_at is None and window and measurement_started is not None and now - measurement_started >= args.sample_seconds:
                        close_reason = "sample_complete"
                        close_window(x11, window); close_at = now
                    elif close_at is None and window and measurement_started is None and now - started >= 15:
                        close_reason = "phase_unavailable"
                        close_window(x11, window); close_at = now
                    if close_at is not None and now - close_at > 20:
                        raise RuntimeError(f"normal close exceeded 20s; leaving PID {proc.pid} for inspection")
                    time.sleep(1)
            finally:
                states.close(); samples.close()
        result = {"exit_code": proc.wait(), "elapsed_sec": time.monotonic() - started,
                  "window_found": bool(window), "screenshot_captured": screenshot_done,
                  "normal_close_requested": close_at is not None, "close_reason": close_reason,
                  "measurement_started_elapsed_sec": measurement_started - started if measurement_started is not None else None,
                  "measurement_valid_samples": valid_samples, "measurement_observed_samples": observed_samples,
                  "phase_valid_fraction": valid_samples / observed_samples if observed_samples else None,
                  "phase_valid": observed_samples > 0 and valid_samples == observed_samples}
        (run / "result.json").write_text(json.dumps(result, indent=2))
        print(json.dumps({"run_dir": str(run), **result}), flush=True)
        return 0 if result["exit_code"] == 0 and result["normal_close_requested"] and result["phase_valid"] else 1
    finally:
        if focus_sink:
            x11.destroy(focus_sink)
        if occluder:
            x11.destroy(occluder)
        x11.close()


if __name__ == "__main__":
    raise SystemExit(main())
