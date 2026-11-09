#!/usr/bin/env python3
"""Extract observed counters from retained real playback logs; no estimated GPU times."""
import argparse
import json
from pathlib import Path
import re


def fields(line):
    result = {}
    for name, value in re.findall(r"\b([A-Za-z_][A-Za-z_0-9]*)=([^\s]+)", line):
        if value in ["true", "false"]:
            result[name] = value == "true"
        else:
            try:
                result[name] = float(value) if "." in value and "0x" not in value else int(value)
            except ValueError:
                result[name] = value
    return result


def summarize(run):
    text = (run / "console.log").read_text()
    output = {"run_dir": str(run.resolve()), "manifest": json.loads((run / "manifest.json").read_text())}
    if (run / "result.json").exists():
        output["result"] = json.loads((run / "result.json").read_text())
    output["decoder"] = []
    output["decoder_api"] = []
    output["sink"] = []
    output["renderer"] = []
    output["render_timings"] = []
    output["clock_fallbacks"] = []
    output["errors"] = []
    output["render_intervals"] = []
    for number, line in enumerate(text.splitlines(), 1):
        data = fields(line)
        data["log_line"] = number
        if "[VideoPerf][Summary] component=decoder " in line:
            output["decoder"].append(data)
        elif "[VideoPerf][Summary] component=decoder_api " in line:
            output["decoder_api"].append(data)
        elif "[VideoSinkPerf][Summary]" in line:
            output["sink"].append(data)
        elif "[VideoRender][Summary]" in line:
            output["renderer"].append(data)
        elif "[VideoRender][Timing] scope=summary" in line:
            output["render_timings"].append(data)
        elif "[VideoRender][Interval]" in line:
            output["render_intervals"].append(data)
        elif "[VideoRender][Resources]" in line:
            output["renderer_resources"] = data
        elif "[HardwareFrame][ReleaseSummary]" in line:
            output["hardware_releases"] = data
        elif "[ALSA][Summary]" in line:
            output["alsa"] = data
        elif "[AVSync][ClockFallback]" in line:
            output["clock_fallbacks"].append(data)
        elif "AVSync scheduled " in line:
            match = re.search(r"scheduled (\d+), droped (\d+), average/max scheduling error (\d+)/(\d+) us, clock fallback (\d+)", line)
            if match:
                output["av_sync"] = dict(zip(["scheduled", "dropped", "average_error_us", "max_error_us", "clock_fallbacks"], map(int, match.groups())))
        if "[error]" in line or "[critical]" in line:
            output["errors"].append({"log_line": number, "text": line})
    samples = [json.loads(line) for line in (run / "process-samples.jsonl").read_text().splitlines()]
    if samples:
        output["process_observations"] = {"fd_min": min(x["fd_count"] for x in samples),
                                          "fd_max": max(x["fd_count"] for x in samples),
                                          "last_fd": samples[-1]["fd_count"],
                                          "rss_max_bytes": max(x["rss_bytes"] for x in samples)}
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    data = [summarize(run) for run in args.runs]
    # Refuse accidental history overwrite; callers choose a new output name.
    with args.output.open("x") as output:
        json.dump(data, output, indent=2)
        output.write("\n")


if __name__ == "__main__":
    main()
