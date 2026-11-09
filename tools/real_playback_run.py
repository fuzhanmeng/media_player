#!/usr/bin/env python3
"""Run the real Qt player, retain logs, sample /proc, and close via WM_DELETE_WINDOW.

No synthetic media, forced process termination, or playback algorithm changes.
--sample-seconds requests a normal window close after a bounded observation;
the default waits for the player's own EOS/error state.
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


def player_window(pid):
    tree = subprocess.check_output(["xwininfo", "-display", ":0", "-root", "-tree"], text=True)
    for match in re.finditer(r'(0x[0-9a-f]+) "[^"\n]*": \("media_player" "media_player"\)', tree):
        window = match[1]
        prop = subprocess.check_output(["xprop", "-display", ":0", "-id", window, "_NET_WM_PID"], text=True)
        if re.search(rf'=\s*{pid}\s*$', prop):
            return window
    return None


def window_message(window, activate=False):
    x = ct.CDLL("libX11.so.6")
    x.XOpenDisplay.argtypes = [ct.c_char_p]
    x.XOpenDisplay.restype = ct.c_void_p
    x.XInternAtom.argtypes = [ct.c_void_p, ct.c_char_p, ct.c_int]
    x.XInternAtom.restype = ct.c_ulong

    class Data(ct.Union):
        _fields_ = [("b", ct.c_char * 20), ("s", ct.c_short * 10), ("l", ct.c_long * 5)]

    class Client(ct.Structure):
        _fields_ = [("type", ct.c_int), ("serial", ct.c_ulong), ("send_event", ct.c_int),
                    ("display", ct.c_void_p), ("window", ct.c_ulong), ("message_type", ct.c_ulong),
                    ("format", ct.c_int), ("data", Data)]

    class Event(ct.Union):
        _fields_ = [("client", Client), ("pad", ct.c_long * 24)]

    x.XSendEvent.argtypes = [ct.c_void_p, ct.c_ulong, ct.c_int, ct.c_long, ct.POINTER(Event)]
    x.XSendEvent.restype = ct.c_int
    x.XFlush.argtypes = [ct.c_void_p]
    x.XCloseDisplay.argtypes = [ct.c_void_p]
    x.XDefaultRootWindow.argtypes = [ct.c_void_p]
    x.XDefaultRootWindow.restype = ct.c_ulong
    x.XMapRaised.argtypes = [ct.c_void_p, ct.c_ulong]
    display = x.XOpenDisplay(b":0")
    if not display:
        raise RuntimeError("Cannot open real display for normal close")
    event = Event()
    event.client.type = 33  # ClientMessage
    event.client.display = display
    event.client.window = int(window, 16)
    event.client.message_type = x.XInternAtom(display, b"_NET_ACTIVE_WINDOW" if activate else b"WM_PROTOCOLS", 0)
    event.client.format = 32
    event.client.data.l[0] = 2 if activate else x.XInternAtom(display, b"WM_DELETE_WINDOW", 0)
    target = x.XDefaultRootWindow(display) if activate else int(window, 16)
    mask = (1 << 20) | (1 << 19) if activate else 0
    if activate:
        x.XMapRaised(display, int(window, 16))
    sent = x.XSendEvent(display, target, 0, mask, ct.byref(event))
    x.XFlush(display)
    x.XCloseDisplay(display)
    if not sent:
        raise RuntimeError("Window event could not be sent")


def close_window(window):
    window_message(window)


def process_sample(pid, started):
    proc = Path(f"/proc/{pid}")
    fields = (proc / "stat").read_text().rsplit(")", 1)[1].split()
    return {"elapsed_sec": time.monotonic() - started,
            "cpu_seconds": (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK"),
            "rss_bytes": int(fields[21]) * os.sysconf("SC_PAGE_SIZE"),
            "threads": int(fields[17]), "fd_count": len(list((proc / "fd").iterdir()))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("media", type=Path)
    parser.add_argument("--mode", choices=["software", "vaapi", "auto"], required=True)
    parser.add_argument("--integration", choices=["xcb_egl", "xcb_glx"], default="xcb_egl")
    parser.add_argument("--sample-seconds", type=float, default=0)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    media = args.media.resolve(strict=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    run = root / "logs/real-playback" / f"{stamp}-{media.stem}-{args.mode}-{args.integration}"
    run.mkdir(parents=True, exist_ok=False)
    probe = subprocess.check_output(["ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", str(media)], text=True)
    (run / "media.json").write_text(probe)
    env = os.environ.copy()
    env.update(DISPLAY=":0", QT_QPA_PLATFORM="xcb", QT_XCB_GL_INTEGRATION=args.integration)
    command = [str(root / "build/RelWithDebInfo/app/media_player"), "--video-decoder", args.mode, str(media)]
    shell_command = "source " + shlex.quote(str(root / "build/RelWithDebInfo/generators/conanrun.sh")) + "\nexec " + shlex.join(command)
    manifest = {"command": command, "media": str(media), "mode": args.mode,
                "integration": args.integration, "sample_seconds": args.sample_seconds,
                "logical_cpus": os.cpu_count(), "started_utc": datetime.now(timezone.utc).isoformat(),
                "environment": {k: env.get(k) for k in ["DISPLAY", "WAYLAND_DISPLAY", "XDG_SESSION_TYPE", "XDG_RUNTIME_DIR", "QT_QPA_PLATFORM", "QT_XCB_GL_INTEGRATION", "QT_OPENGL", "LIBGL_ALWAYS_SOFTWARE"]}}
    with (run / "console.log").open("x") as output:
        proc = subprocess.Popen(["bash", "--noprofile", "--norc", "-c", shell_command], cwd=run,
                                env=env, stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT)
        manifest["pid"] = proc.pid
        (run / "manifest.json").write_text(json.dumps(manifest, indent=2))
        print(f"RUN_DIR={run}\nPID={proc.pid}", flush=True)
        started = time.monotonic()
        last_progress = started
        window = None
        captured = False
        capture_attempted = False
        close_at = None
        close_reason = None
        eos_seen = False
        error_seen = False
        incomplete_line = ""
        last_sample = None
        with (run / "process-samples.jsonl").open("x") as samples, (run / "console.log").open() as reader:
            while proc.poll() is None:
                now = time.monotonic()
                try:
                    sample = process_sample(proc.pid, started)
                    if last_sample:
                        sample["process_cpu_one_core_pct"] = 100 * (sample["cpu_seconds"] - last_sample["cpu_seconds"]) / (sample["elapsed_sec"] - last_sample["elapsed_sec"])
                    samples.write(json.dumps(sample) + "\n")
                    samples.flush()
                    last_sample = sample
                except FileNotFoundError:
                    continue
                available = incomplete_line + reader.read()
                complete_lines = available.rsplit("\n", 1)
                new_log = complete_lines[0] if len(complete_lines) == 2 else ""
                incomplete_line = complete_lines[-1]
                eos_seen = eos_seen or bool(re.search(r"Player state \w+ -> Ended", new_log))
                error_seen = error_seen or bool(re.search(r"Player state \w+ -> Error", new_log))
                if window is None:
                    window = player_window(proc.pid)
                    if window:
                        window_message(window, activate=True)
                        (run / "activation.txt").write_text("Requested raise and _NET_ACTIVE_WINDOW once; compositor acceptance is not assumed.\n")
                if window and not capture_attempted and now - started >= 10:
                    window_info = subprocess.check_output(["xwininfo", "-display", ":0", "-id", window], text=True)
                    (run / "window.txt").write_text(window_info)
                    (run / "window-state.txt").write_text(subprocess.check_output(["xprop", "-display", ":0", "-id", window, "_NET_WM_STATE", "_NET_WM_DESKTOP"], text=True)
                        + subprocess.check_output(["xprop", "-display", ":0", "-root", "_NET_ACTIVE_WINDOW", "_NET_CURRENT_DESKTOP"], text=True))
                    width = re.search(r"Width: (\d+)", window_info)[1]
                    height = re.search(r"Height: (\d+)", window_info)[1]
                    (run / "runtime-maps.txt").write_text(Path(f"/proc/{proc.pid}/maps").read_text())
                    with (run / "screenshot.log").open("x") as capture_log:
                        capture = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "warning", "-f", "x11grab", "-draw_mouse", "0", "-window_id", window,
                                                  "-video_size", f"{width}x{height}", "-i", ":0", "-frames:v", "1", "-update", "1", "-n", str(run / "window.png")], stdout=capture_log, stderr=subprocess.STDOUT)
                    capture_attempted = True
                    captured = capture.returncode == 0 and (run / "window.png").exists()
                    print(f"Screenshot exit={capture.returncode}", flush=True)
                if close_at is None and (eos_seen or error_seen or (args.sample_seconds > 0 and now - started >= args.sample_seconds)):
                    if window:
                        close_reason = "eos" if eos_seen else "player_error" if error_seen else "sample_complete"
                        close_window(window)
                        close_at = now
                        print(f"Normal close requested: {close_reason}", flush=True)
                if close_at is not None and now - close_at > 20:
                    raise RuntimeError(f"Player did not finish normal close; left PID {proc.pid} running for inspection")
                if now - last_progress >= 30:
                    print(f"Progress elapsed={now-started:.1f}s fd={sample['fd_count']} rss_mb={sample['rss_bytes']/1e6:.1f}", flush=True)
                    last_progress = now
                time.sleep(1)
        code = proc.wait()
        result = {"exit_code": code, "eos_seen": eos_seen, "close_reason": close_reason,
                  "elapsed_sec": time.monotonic() - started, "last_process_sample": last_sample,
                  "screenshot_captured": captured}
        (run / "result.json").write_text(json.dumps(result, indent=2))
        print(json.dumps(result), flush=True)
        return 0 if code == 0 and close_reason in ["eos", "sample_complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
