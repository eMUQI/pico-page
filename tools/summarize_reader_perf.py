#!/usr/bin/env python3
"""Summarize reader serial timing logs without mixing software and panel time."""
import argparse
import json
import math
import re
from collections import defaultdict
from pathlib import Path


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def summarize(paths):
    samples = defaultdict(list)
    thresholds = defaultdict(set)
    resources = defaultdict(list)
    overflow_reports = []
    overflow_lines = 0
    for path in paths:
        for line in path.read_text(errors="replace").splitlines():
            fields = dict(re.findall(r"([a-z_]+)=(-?\d+)", line))
            if "Input dispatch:" in line and {"queued", "waited", "prepare"} <= fields.keys():
                group = "queued" if int(fields["queued"]) else "unqueued"
                for source, name in (("waited", "input_wait"), ("prepare", "input_handler")):
                    samples[f"{name}/{group}"].append(int(fields[source]))
            elif "Frame complete:" in line and fields.get("ok") == "1":
                for field in ("copy", "power", "draw", "complete", "save"):
                    if field in fields:
                        name = "frame_complete" if field in ("complete", "save") else f"panel_{field}"
                        samples[name].append(int(fields[field]))
            elif "reader_perf:" in line:
                stage = re.search(r"stage=([a-z_]+) us=(\d+)", line)
                if stage:
                    name = "stage/" + stage[1]
                    samples[name].append(int(stage[2]) / 1000)
                    thresholds[name].add(int(fields.get("min_us", "20000")) / 1000)
                if "zip_stage" in line:
                    for field in ("read_us", "read_max_us", "xml_us", "xml_max_us", "other_us", "checkpoint_max_us"):
                        if field in fields:
                            samples["zip/" + field.removesuffix("_us")].append(int(fields[field]) / 1000)
            if "PP " in line and " OK view=" in line:
                for field in ("internal", "largest_internal", "psram", "largest_psram", "stack_bytes", "input_stack_bytes"):
                    if field in fields:
                        resources[field].append(int(fields[field]))
                if "input_overflows" in fields:
                    overflow_reports.append(int(fields["input_overflows"]))
            for worker in ("content_worker", "network_worker"):
                if worker + ":" in line and "stack_free" in fields:
                    resources[worker + "_stack_bytes"].append(int(fields["stack_free"]))
            overflow = re.search(r"Input queue overflow: count=(\d+)", line)
            if overflow:
                overflow_lines += 1
                overflow_reports.append(int(overflow[1]))
    return {
        "milliseconds": {name: {"n": len(values), "p50": percentile(values, .5),
                                 "p95": percentile(values, .95), "p99": percentile(values, .99),
                                 "max": max(values), "logged_min_ms": sorted(thresholds[name])}
                         for name, values in sorted(samples.items())},
        "minimum_bytes": {name: min(values) for name, values in sorted(resources.items())},
        "overflow_log_lines": overflow_lines,
        "maximum_reported_overflow_count": max(overflow_reports, default=None),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    report = summarize(args.logs)
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return
    print("单位：毫秒。输入等待按是否跨刷新排队分组；屏幕耗时不计入软件准备。")
    print("带最小记录阈值的阶段只有慢样本，不能据此判断完整 P95/P99。小样本尾分位数仅供排查。\n")
    print("| 指标 | 样本 | P50 | P95 | P99 | 最长 | 记录阈值 |")
    print("| --- | ---: | ---: | ---: | ---: | ---: | --- |")
    for name, row in report["milliseconds"].items():
        threshold = ", ".join(f"≥ {v:g}" for v in row["logged_min_ms"] if v)
        print(f"| {name} | {row['n']} | {row['p50']:.3f} | {row['p95']:.3f} | {row['p99']:.3f} | {row['max']:.3f} | {threshold or '无'} |")
    print("\n内存与任务栈的最小观测值（字节）：" + json.dumps(report["minimum_bytes"], ensure_ascii=False))
    print(f"队列溢出日志 {report['overflow_log_lines']} 条，最大已报告计数 {report['maximum_reported_overflow_count']}。缺少日志不代表没有溢出。")
    print("ZIP other 包含解压及回调调度；XML 时间包含解析回调。各阶段不可与父阶段相加。")


if __name__ == "__main__":
    main()
