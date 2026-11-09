#!/usr/bin/env python3
"""Summarize retained real-desktop GUI diagnostics without overwriting history.

Rates use counts divided by elapsed time, and means use their event counts.
The first periodic interval of each component and all shutdown summaries are
excluded from stable metrics. Long intervals remain included: a GUI stall must
not disappear from the result simply because it delayed the reporting timer.
"""

import argparse
from datetime import datetime
import json
from pathlib import Path
import re

from summarize_playback import fields


UNAVAILABLE = "unavailable"
TIMESTAMP = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\]")


def json_file(path, default):
    return json.loads(path.read_text()) if path.exists() else default


def json_lines(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()] if path.exists() else []


def numeric(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def total(rows, key):
    return sum(row[key] for row in rows) if rows and all(numeric(row.get(key)) for row in rows) else UNAVAILABLE


def rate(count, seconds):
    return count / seconds if numeric(count) and numeric(seconds) and seconds > 0 else UNAVAILABLE


def maximum(rows, key):
    values = [row[key] for row in rows if numeric(row.get(key))]
    return max(values) if values else UNAVAILABLE


def weighted_mean(rows, key, weight):
    selected = [row for row in rows if numeric(row.get(key)) and numeric(row.get(weight))]
    weight_sum = sum(row[weight] for row in selected)
    return sum(row[key] * row[weight] for row in selected) / weight_sum if weight_sum else UNAVAILABLE


def stable(rows, duration):
    # The logs reset their counters each interval. Do not mix in cumulative
    # destructor summaries or duplicate them by combining both log files.
    return [row for row in rows[1:] if numeric(row.get(duration)) and row[duration] >= 4.5]


def window(rows, duration):
    if not rows:
        return {"interval_count": 0, "elapsed_sec": UNAVAILABLE}
    output = {"interval_count": len(rows), "elapsed_sec": total(rows, duration),
              "first_log_line": rows[0]["log_line"], "last_log_line": rows[-1]["log_line"]}
    if numeric(rows[0].get("process_elapsed_sec")):
        output["start_process_elapsed_sec"] = rows[0]["process_elapsed_sec"] - rows[0][duration]
        output["end_process_elapsed_sec"] = rows[-1]["process_elapsed_sec"]
    return output


def cpu_metrics(samples, gui_rows):
    bounds = [(row["process_elapsed_sec"] - row["interval_sec"], row["process_elapsed_sec"])
              for row in gui_rows if numeric(row.get("process_elapsed_sec"))]
    pairs = [(a, b) for a, b in zip(samples, samples[1:])
             if any(a["elapsed_sec"] >= start and b["elapsed_sec"] <= end for start, end in bounds)
             and b["elapsed_sec"] > a["elapsed_sec"]]
    if not pairs:
        return {"process_cpu_one_core_pct": UNAVAILABLE, "sample_pair_count": 0,
                "basis": "sample pairs wholly inside stable GUI intervals"}
    elapsed = sum(b["elapsed_sec"] - a["elapsed_sec"] for a, b in pairs)
    cpu = sum(b["cpu_seconds"] - a["cpu_seconds"] for a, b in pairs)
    return {"process_cpu_one_core_pct": 100 * cpu / elapsed,
            "process_cpu_one_core_peak_pct": max(100 * (b["cpu_seconds"] - a["cpu_seconds"]) /
                                                  (b["elapsed_sec"] - a["elapsed_sec"]) for a, b in pairs),
            "sample_pair_count": len(pairs), "elapsed_sec": elapsed, "cpu_seconds": cpu,
            "start_process_elapsed_sec": pairs[0][0]["elapsed_sec"],
            "end_process_elapsed_sec": pairs[-1][1]["elapsed_sec"],
            "basis": "sample pairs wholly inside stable GUI intervals; 100% is one core"}


def summarize(run):
    run = run.resolve()
    manifest = json_file(run / "manifest.json", {})
    result = json_file(run / "result.json", {})
    log = run / "console.log"
    if not log.exists():
        log = run / "logs/media_player.log"
    started = datetime.fromisoformat(manifest["started_utc"]).timestamp() if manifest.get("started_utc") else None
    gui_all, decoder_all, renderer_all = [], [], []
    errors, external_warnings = [], []
    for number, line in enumerate(log.read_text().splitlines(), 1):
        row = fields(line)
        row["log_line"] = number
        stamp = TIMESTAMP.match(line)
        if stamp and started is not None:
            # The player's logger uses the host's local time, as does astimezone
            # for a naive datetime. Preserve the offset for review in the JSON.
            local = datetime.strptime(stamp[1], "%Y-%m-%d %H:%M:%S.%f").astimezone()
            row["timestamp"] = local.isoformat()
            row["process_elapsed_sec"] = local.timestamp() - started
        if "[GuiRender][Diagnostics]" in line and row.get("reason") == "interval":
            gui_all.append(row)
        elif "[VideoPerf][Interval] component=decoder " in line:
            decoder_all.append(row)
        elif "[VideoRender][Interval]" in line:
            renderer_all.append(row)
        if "[error]" in line or "[critical]" in line:
            errors.append({"log_line": number, "text": line})
        elif "Fontconfig error:" in line or "QXcb" in line or "QOpenGL" in line and not stamp:
            external_warnings.append({"log_line": number, "text": line})

    gui = stable(gui_all, "interval_sec")
    decoder = stable(decoder_all, "elapsed_sec")
    renderer = stable(renderer_all, "elapsed_sec")
    gui_output = window(gui, "interval_sec")
    seconds = gui_output["elapsed_sec"]
    counters = ["video_dispatch_frames", "gui_mailbox_received_frames", "gui_mailbox_overwritten_frames",
                "gui_update_requests", "gui_update_queued", "gui_update_while_pending", "paint_events",
                "paintGL_calls", "paintGL_new_frame_calls", "paintGL_duplicate_frame_calls", "paintGL_no_frame_calls",
                "frameSwapped_count", "aboutToCompose_count", "top_level_update_request_events",
                "view_update_request_events", "update_request_events"]
    gui_output.update({key: total(gui, key) for key in counters})
    for counter, key in [("gui_mailbox_received_frames", "mailbox_receive_fps"),
                         ("gui_update_queued", "update_request_fps"), ("paintGL_calls", "paintGL_fps"),
                         ("paintGL_new_frame_calls", "paintGL_new_frame_fps"), ("frameSwapped_count", "frameSwapped_fps")]:
        gui_output[key] = rate(gui_output[counter], seconds)
    for name, count in [("paintGL_duration", "paintGL_calls"), ("update_to_paint", "paintGL_calls"),
                        ("mailbox_to_paint", "paintGL_new_frame_calls"),
                        ("latest_mailbox_to_gui", "gui_update_queued"), ("compose_duration", "frameSwapped_count")]:
        gui_output[name + "_avg_ms"] = weighted_mean(gui, name + "_avg_ms", count)
        gui_output[name + "_max_ms"] = maximum(gui, name + "_max_ms")
    for key in ["heartbeat_delay_max_ms", "heartbeat_interval_ms", "paintGL_interval_max_ms", "frameSwapped_interval_max_ms"]:
        gui_output[key] = maximum(gui, key)
    gui_output["active_interval_count"] = sum(row.get("active") is True for row in gui)
    gui_output["visible_interval_count"] = sum(row.get("visible") is True for row in gui)
    gui_output["exposed_interval_count"] = sum(row.get("exposed") is True for row in gui)

    decoder_output = window(decoder, "elapsed_sec")
    decoder_output.update(decoded_frames=total(decoder, "decoded_frames"), decode_errors=total(decoder, "decode_errors"))
    decoder_output["decode_fps"] = rate(decoder_output["decoded_frames"], decoder_output["elapsed_sec"])
    decoder_output["source_fps"] = decoder[-1].get("source_fps", UNAVAILABLE) if decoder else UNAVAILABLE
    renderer_output = window(renderer, "elapsed_sec")
    for key in ["unique_draw_submitted_frames", "gl_draw_submitted_frames", "gl_draw_failed", "egl_image_create_failed",
                "gl_egl_image_bind_failed", "software_upload_failed", "cpu_download_failed", "vaapi_dmabuf_render_frames",
                "software_texture_upload_frames", "vaapi_cpu_download_frames"]:
        renderer_output[key] = total(renderer, key)
    renderer_output["unique_draw_fps"] = rate(renderer_output["unique_draw_submitted_frames"], renderer_output["elapsed_sec"])
    renderer_output["displayed_frames"] = "not_observable"

    states = json_lines(run / "window-state.jsonl")
    measured_states = [state for state in states if state.get("in_measurement")]
    phase = {key: result.get(key, UNAVAILABLE) for key in ["phase_valid", "phase_valid_fraction",
             "measurement_started_elapsed_sec", "measurement_valid_samples", "measurement_observed_samples"]}
    phase["requested"] = manifest.get("phase", UNAVAILABLE)
    phase["observed_measurement_samples"] = len(measured_states)
    phase["active_measurement_samples"] = sum(state.get("active_observed") is True for state in measured_states)
    phase["covered_measurement_samples"] = sum(state.get("controlled_occlusion") is True for state in measured_states)
    phase["observed_geometries"] = [list(geometry) for geometry in sorted({tuple(state.get(key) for key in
                                  ["x", "y", "width", "height"]) for state in measured_states})]
    return {"run_dir": str(run), "manifest": manifest, "result": result,
            "selection_policy": "per component: exclude first periodic interval and shutdown summaries; include remaining intervals >=4.5s, including delayed reports",
            "timing_basis": "CPU wall time; component interval boundaries are independent",
            "mean_weight_basis": {"paintGL_duration": "exact: paintGL_calls",
                "update_to_paint": "approximate across intervals: paintGL_calls; actual timing sample count not logged",
                "mailbox_to_paint": "approximate across intervals: paintGL_new_frame_calls; valid duplicate paints also contribute timing samples",
                "latest_mailbox_to_gui": "gui_update_queued; no sample only before a first receive or if producer advances timestamp past slot entry",
                "compose_duration": "frameSwapped_count; exact when every swap pairs with aboutToCompose",
                "single_interval_means": "reported directly by renderer and exact for that interval's timing samples"},
            "decoder": decoder_output, "renderer": renderer_output, "gui": gui_output,
            "cpu": cpu_metrics(json_lines(run / "process-samples.jsonl"), gui), "phase": phase,
            "errors": errors, "external_warnings": external_warnings,
            "gui_intervals": gui, "decoder_intervals": decoder, "renderer_intervals": renderer}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, help="new JSON file; existing paths are refused")
    args = parser.parse_args()
    data = [summarize(run) for run in args.runs]
    rendered = json.dumps(data, indent=2) + "\n"
    if args.output:
        with args.output.open("x") as output:
            output.write(rendered)
    print(rendered, end="")


if __name__ == "__main__":
    main()
