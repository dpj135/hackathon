#!/usr/bin/env python3
"""Score an nds_player binary on a trace suite.

The automated score follows the contest rules:

  feasibility   = 20 * valid_cases / case_count
  optimization  = weighted mean of per-trace 60 * min(1, J_reference / J_player)
  performance   = 10 * cases_within_limits / case_count
  automated     = feasibility + optimization + performance     (max 90)

Design and reproducibility (10 points) are left to human review.
A trace that crashes, times out, exceeds the memory limit, or fails
validation scores 0 on all three automated components.
J_reference / J_player above 1 is reported as a tie-break and does not
raise the 60-point optimization cap.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def load_json(path: Path):
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def run_limited(command, timeout_sec: float, memory_limit_mb: int):
    timer = "/usr/bin/time"
    timed = [timer, "-f", "NDS_MAXRSS_KB %M", "--", *command] if Path(timer).exists() else command
    proc = subprocess.Popen(
        timed,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    started = time.monotonic()
    status = "ok"
    peak_kb = 0
    try:
        while proc.poll() is None:
            elapsed = time.monotonic() - started
            try:
                with open(f"/proc/{proc.pid}/status", encoding="utf-8") as handle:
                    for line in handle:
                        if line.startswith("VmHWM:"):
                            peak_kb = max(peak_kb, int(line.split()[1]))
            except OSError:
                pass
            if memory_limit_mb and peak_kb > memory_limit_mb * 1024:
                status = "memory"
                os.killpg(proc.pid, signal.SIGKILL)
                break
            if elapsed > timeout_sec:
                status = "timeout"
                os.killpg(proc.pid, signal.SIGKILL)
                break
            time.sleep(0.02)
    finally:
        stdout, stderr = proc.communicate()
    text = stderr.decode("utf-8", "replace")
    for line in text.splitlines():
        if line.startswith("NDS_MAXRSS_KB "):
            try:
                peak_kb = max(peak_kb, int(line.split()[1]))
            except ValueError:
                pass
    if status == "ok" and proc.returncode != 0:
        status = "error"
    return {
        "status": status,
        "code": proc.returncode,
        "seconds": time.monotonic() - started,
        "peak_mb": peak_kb / 1024.0,
        "stdout": stdout.decode("utf-8", "replace")[-2000:],
        "stderr": text[-2000:],
    }


def check_frozen(trace, frozen: dict, case_id: str):
    costs = trace["instance"]["fixed_parameters"]
    for key, expected in frozen.items():
        if key not in costs:
            raise SystemExit(f"{case_id}: trace is missing frozen cost {key}")
        if int(costs[key]) != int(expected):
            raise SystemExit(
                f"{case_id}: {key} is {costs[key]}, frozen value is {expected}"
            )
    if int(costs["CPU_D2H_CALL_COST"]) != int(costs["CPU_H2D_CALL_COST"]):
        raise SystemExit(f"{case_id}: CPU_D2H_CALL_COST must equal CPU_H2D_CALL_COST")
    if int(costs["NPU_D2H_CALL_COST"]) != int(costs["NPU_H2D_CALL_COST"]):
        raise SystemExit(f"{case_id}: NPU_D2H_CALL_COST must equal NPU_H2D_CALL_COST")


def score_case(player: Path, validate: Path | None, trace_path: Path, timeout_sec: float,
               memory_limit_mb: int, work: Path):
    output = work / "solution.json"
    played = run_limited([str(player), str(trace_path), str(output)], timeout_sec, memory_limit_mb)
    if played["status"] != "ok":
        return played, None
    try:
        solution = load_json(output)
        cost = int(solution["metrics"]["total_cost"])
        metrics = solution["metrics"]
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError) as error:
        played["status"] = "error"
        played["stderr"] = (played["stderr"] + f"\nbad solution json: {error}")[-2000:]
        return played, None
    if cost <= 0:
        played["status"] = "invalid"
        played["stderr"] = (played["stderr"] + "\nnon-positive total_cost")[-2000:]
        return played, metrics
    if validate is not None:
        checked = run_limited([str(validate), str(trace_path), str(output)], timeout_sec, memory_limit_mb)
        played["seconds"] += checked["seconds"]
        played["peak_mb"] = max(played["peak_mb"], checked["peak_mb"])
        if checked["status"] != "ok":
            played["status"] = "timeout" if checked["status"] == "timeout" else "invalid"
            played["stderr"] = checked["stderr"]
            return played, metrics
    return played, metrics


def main():
    parser = argparse.ArgumentParser(description="Score an NDS player on a trace suite")
    parser.add_argument("--player", required=True, type=Path)
    parser.add_argument("--validate", type=Path)
    parser.add_argument("--suite", required=True, type=Path)
    parser.add_argument("--data-dir", required=True, type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--write-baseline", type=Path)
    parser.add_argument("--frozen", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--label", default="suite")
    args = parser.parse_args()

    suite = load_json(args.suite)
    frozen = load_json(args.frozen) if args.frozen else {}
    baseline = load_json(args.baseline)["cases"] if args.baseline else {}
    memory_limit_mb = int(suite.get("memory_limit_mb", 16384))
    rows = []
    weight_total = 0.0
    opt_total = 0.0
    valid = 0
    within = 0
    tie_break = 0.0

    for case in suite["cases"]:
        case_id = case["id"]
        weight = float(case["weight"])
        weight_total += weight
        trace_path = args.data_dir / case["file"]
        trace = load_json(trace_path)
        if frozen:
            check_frozen(trace, frozen, case_id)
        with tempfile.TemporaryDirectory(prefix="nds-score-") as temp:
            played, metrics = score_case(
                args.player, args.validate, trace_path, float(case["timeout_sec"]),
                memory_limit_mb, Path(temp),
            )
        record = {
            "id": case_id,
            "file": case["file"],
            "weight": weight,
            "tags": case.get("tags", []),
            "status": played["status"],
            "seconds": round(played["seconds"], 3),
            "peak_mb": round(played["peak_mb"], 2),
            "trace_opt": 0.0,
            "ratio": None,
        }
        if played["status"] == "ok" and metrics is not None:
            cost = int(metrics["total_cost"])
            record["total_cost"] = cost
            record["metrics"] = metrics
            if args.write_baseline:
                valid += 1
                within += 1
                record["trace_opt"] = 60.0
                record["ratio"] = 1.0
            else:
                if case_id not in baseline:
                    raise SystemExit(f"baseline file has no cost for {case_id}")
                reference = int(baseline[case_id]["total_cost"])
                if reference <= 0:
                    raise SystemExit(f"baseline cost for {case_id} is not positive")
                ratio = reference / cost
                record["baseline_cost"] = reference
                record["ratio"] = ratio
                record["trace_opt"] = 60.0 * min(1.0, ratio)
                record["trace_time_us"] = metrics.get("trace_time_us")
                record["read_amplification"] = metrics.get("read_amplification")
                record["cache_hits"] = metrics.get("cache_hits")
                valid += 1
                within += 1
                tie_break += ratio
                opt_total += weight * record["trace_opt"]
        else:
            details = [
                line.strip() for line in played["stderr"].splitlines()
                if line.strip() and not line.startswith("NDS_MAXRSS_KB ")
            ]
            record["error"] = (details[-1] if details else "failed")[:400]
        if args.write_baseline and played["status"] == "ok":
            opt_total += weight * 60.0
        rows.append(record)
        print(
            f"{case_id:24} {record['status']:8} "
            f"J={record.get('total_cost', '-'):>12} "
            f"ratio={record['ratio'] if record['ratio'] is not None else '-'} "
            f"opt={record['trace_opt']:.2f} {record['seconds']:.2f}s",
            flush=True,
        )

    if weight_total <= 0:
        raise SystemExit("suite weights must be positive")
    count = len(rows)
    if args.write_baseline:
        payload = {"suite": suite.get("name", args.label), "cases": {}}
        for record in rows:
            if record["status"] != "ok":
                raise SystemExit(f"cannot freeze baseline; {record['id']} is {record['status']}: {record.get('error')}")
            payload["cases"][record["id"]] = {"total_cost": record["total_cost"]}
        args.write_baseline.parent.mkdir(parents=True, exist_ok=True)
        args.write_baseline.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        print(f"wrote baseline costs to {args.write_baseline}")

    feasibility = 20.0 * valid / count
    optimization = opt_total / weight_total
    performance = 10.0 * within / count
    automated = feasibility + optimization + performance
    summary = {
        "label": args.label,
        "suite": suite.get("name", args.label),
        "cases": rows,
        "case_count": count,
        "valid_cases": valid,
        "feasibility": round(feasibility, 4),
        "optimization": round(optimization, 4),
        "performance": round(performance, 4),
        "automated": round(automated, 4),
        "automated_max": 90,
        "design": None,
        "design_max": 10,
        "tie_break_ratio_sum": None if args.write_baseline else round(tie_break, 6),
        "memory_limit_mb": memory_limit_mb,
    }
    text = json.dumps(summary, indent=2, ensure_ascii=False)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n", encoding="utf-8")
    print()
    print(f"{args.label}: feasibility {summary['feasibility']:.2f}/20")
    print(f"{args.label}: optimization {summary['optimization']:.2f}/60")
    print(f"{args.label}: performance {summary['performance']:.2f}/10")
    print(f"{args.label}: automated {summary['automated']:.2f}/90")
    print(f"{args.label}: design is manual /10")
    if summary["tie_break_ratio_sum"] is not None:
        print(f"{args.label}: tie-break ratio sum {summary['tie_break_ratio_sum']:.6f}")
    return 0 if valid == count or args.write_baseline else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
