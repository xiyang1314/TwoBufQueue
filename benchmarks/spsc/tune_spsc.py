#!/usr/bin/env python3
"""Search measured configurations independently for throughput and write p50/p99/p99.9."""
import argparse
import csv
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import sys
import time

TRANSPORTS = ("two_buf_queue", "rigtorp", "readerwriterqueue", "atomic_queue")
OBJECTIVES = ("throughput", "p50", "p99", "p999")
PUBLISH_COUNTS = tuple(1 << n for n in range(11)) + (2**32-1,)
FROZEN = {"transport", "mode", "messages", "warmup_messages", "payload_bytes", "callback_iterations",
          "verify_payload", "producer_interval_ns", "timer", "sample_every", "max_samples",
          "timer_calibration_samples", "random_seed", "timeout_seconds", "buffer_budget_bytes", "call_batch_size", "latency_batch_size",
          "round_storage_bytes", "producer_qos", "producer_cpu", "consumer_cpu", "prefault_producer_buffers"}
FROZEN.add("measure_callback_time")
FROZEN.update(("prepared_input", "variable_payload", "prepared_input_limit_bytes"))
FROZEN.update(("is_scan_current", "fixed_round_boundaries"))
SPSC_KEYS = {"readerwriterqueue_copy_after_zero", "rigtorp_write_prefetch", "spsc_capacity", "spsc_idle_spins", "spsc_idle_sleep_ns", "retry_yield_every", "producer_cpu", "consumer_cpu"}
ALIASES = {"block_bytes": ("source_block_size", "output_block_size"),
           "cache_block_bytes": ("source_block_size", "output_block_size", "source_pool_min", "source_pool_max",
                                 "output_pool_min", "output_pool_max", "source_queue_capacity", "output_queue_capacity"),
           "source_pool_blocks": ("source_pool_min", "source_pool_max"),
           "output_pool_blocks": ("output_pool_min", "output_pool_max")}


def validate_calibration(settings):
    for key in ("target_ratio", "min_ratio", "max_ratio"):
        value = settings[key]
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
            raise ValueError("invalid callback ratio: " + key)
    if not settings["min_ratio"] <= settings["target_ratio"] <= settings["max_ratio"]:
        raise ValueError("callback target must be within min/max ratio")
    for key in ("repetitions", "messages", "max_iterations", "max_candidates"):
        if isinstance(settings[key], bool) or not isinstance(settings[key], int) or settings[key] < 1:
            raise ValueError("invalid callback calibration: " + key)
    if isinstance(settings["min_rounds"],bool) or not isinstance(settings["min_rounds"],int) or settings["min_rounds"]<0:
        raise ValueError("min_rounds must be nonnegative; zero keeps the requested calibration count")
    if settings["max_iterations"] > 2**32-1 or settings["reference_publish_count"] not in PUBLISH_COUNTS:
        raise ValueError("callback loop/publish count exceeds supported range")


def choose_callback_iterations(probe, settings):
    """Positive integer loop search; never pretend an unreachable target was reached."""
    seen = {}
    def evaluate(value):
        if value not in seen:
            seen[value] = probe(value)
            if not math.isfinite(seen[value]) or seen[value] <= 0:
                raise ValueError("invalid callback ratio observation")
        return seen[value]
    lower, upper = settings["min_ratio"], settings["max_ratio"]
    low = high = 1
    ratio = evaluate(1)
    while ratio < lower and high < settings["max_iterations"] and len(seen) < settings["max_candidates"]:
        low, high = high, min(2*high, settings["max_iterations"])
        ratio = evaluate(high)
    while not lower <= ratio <= upper and high-low > 1 and len(seen) < settings["max_candidates"]:
        middle = (low+high)//2
        ratio = evaluate(middle)
        if ratio < lower:
            low = middle
        else:
            high = middle
    matching = [n for n, ratio in seen.items() if lower <= ratio <= upper]
    candidates = matching or list(seen)
    return min(candidates, key=lambda n: (abs(seen[n]-settings["target_ratio"]), n))


def effective_config(base, changes):
    result = dict(base)
    for key, value in changes.items():
        if key == "cache_block_bytes":
            if isinstance(value,bool) or not isinstance(value,int) or value<1:
                raise ValueError("cache_block_bytes must be positive")
            for prefix in ("source","output"):
                block_key="source_block_size" if prefix=="source" else "output_block_size"
                old_blocks=base[prefix+"_pool_max"]
                budget=base[block_key]*old_blocks
                if base[prefix+"_pool_min"]!=old_blocks or budget%value:
                    raise ValueError("cache_block_bytes requires a fixed pool and an exactly divisible byte budget")
                result[block_key]=value
                result[prefix+"_pool_min"]=result[prefix+"_pool_max"]=budget//value
                result[prefix+"_queue_capacity"]=max(2,base[prefix+"_queue_capacity"]*(budget//value)//old_blocks)
            continue
        for target in ALIASES.get(key, (key,)):
            result[target] = value
    return result


def expected_round_count(config, result):
    """Validate natural publication boundaries before accepting a measurement."""
    record_bytes = result["source_record_bytes"]
    alignment = result["publication_alignment"]
    warmup = result["effective_warmup_messages"]
    if (config.get("fixed_round_boundaries", False) or
        not config.get("is_scan_current", True) or config["publish_count"] == 2**32-1):
        limit = config["round_storage_bytes"]//config["payload_bytes"]
        if (record_bytes != config["payload_bytes"] or alignment != 1 or limit < 1 or
            warmup != max(1, config["warmup_messages"]) or
            result["round_message_limit"] != limit or result["round_message_minimum"] != limit):
            raise ValueError("invalid completed-block publication metadata")
        return (config["messages"]+limit-1)//limit
    valid_alignments = {n - n % config["publish_count"] for n in PUBLISH_COUNTS if n >= config["publish_count"]}
    if record_bytes != config["payload_bytes"] or alignment not in valid_alignments:
        raise ValueError("invalid publication metadata")
    records = config["source_block_size"]//record_bytes
    requested = max(1, config["warmup_messages"])
    position = (requested-1)%records+1
    rounded = (position+alignment-1)//alignment*alignment
    expected_warmup = requested-position+(rounded if rounded<=records else records+alignment)
    limit = config["round_storage_bytes"]//config["payload_bytes"]
    minimum = limit-2*(alignment-1)
    if (config["publish_count"]>alignment or records<alignment or minimum<1 or
        warmup!=expected_warmup or result["round_message_limit"]!=limit or
        result["round_message_minimum"]!=minimum):
        raise ValueError("invalid publication boundary/warmup")
    sent, rounds = 0, 0
    while sent<config["messages"]:
        if config["messages"]-sent<=limit:
            end = config["messages"]
        else:
            desired = warmup+sent+limit
            position = (desired-1)%records+1
            block_begin = desired-position
            published = (block_begin+position//alignment*alignment if position>=alignment
                         else block_begin-records%alignment)
            end = published-warmup
        if end<=sent or end-sent>limit or (end<config["messages"] and end-sent<minimum):
            raise ValueError("invalid round progress")
        sent, rounds = end, rounds+1
    return rounds


def candidate_id(config):
    data = {k: v for k, v in config.items() if k not in {"mode", "messages", "random_seed", "verify_payload"}}
    return hashlib.sha256(json.dumps(data, sort_keys=True).encode()).hexdigest()[:12]


def validate_space(space, defaults, transport):
    assigned = set()
    for key, values in space.items():
        if key in FROZEN or key not in defaults and key not in ALIASES:
            raise ValueError("Not a tunable queue parameter: " + key)
        if transport != "two_buf_queue" and key not in SPSC_KEYS:
            raise ValueError("Unused SPSC tuning parameter: " + key)
        if transport == "two_buf_queue" and key.startswith("spsc_"):
            raise ValueError("Unused TwoBufQueue tuning parameter: " + key)
        if key == "rigtorp_write_prefetch" and transport != "rigtorp":
            raise ValueError("Unused rigtorp tuning parameter: " + key)
        if key == "readerwriterqueue_copy_after_zero" and transport != "readerwriterqueue":
            raise ValueError("Unused ReaderWriterQueue tuning parameter: " + key)
        if not isinstance(values, list) or not values:
            raise ValueError("Search values must be nonempty lists: " + key)
        targets = set(ALIASES.get(key, (key,)))
        if assigned & targets:
            raise ValueError("Overlapping search fields/aliases: " + key)
        assigned.update(targets)


def validate_length_reference(changes, defaults):
    # A reference configuration can specify block sizes and native queue capacity for each payload length.
    # Validate them separately to preserve ownership constraints and detect overlapping block aliases.
    validate_space({k: [v] for k, v in changes.items() if not k.startswith("spsc_") and k not in {"rigtorp_write_prefetch","readerwriterqueue_copy_after_zero"}},
                   defaults, "two_buf_queue")
    validate_space({k: [v] for k, v in changes.items() if k.startswith("spsc_")},
                   defaults, "readerwriterqueue")
    validate_space({k: [v] for k, v in changes.items() if k == "rigtorp_write_prefetch"},
                   defaults, "rigtorp")
    validate_space({k: [v] for k, v in changes.items() if k == "readerwriterqueue_copy_after_zero"},
                   defaults, "readerwriterqueue")


def initial_candidates(base, space, strategy, limit, rng):
    result, seen = [], set()
    def add(config):
        key = candidate_id(config)
        if key not in seen:
            seen.add(key)
            result.append(config)
    add(base)
    if strategy == "grid":
        count = math.prod(len(v) for v in space.values())
        if limit and count > limit:
            # Sample indices without materializing an enormous Cartesian product.
            indices = sorted(rng.sample(range(count), min(limit - 1, count)))
            for index in indices:
                changes = {}
                for key, values in reversed(list(space.items())):
                    index, remainder = divmod(index, len(values))
                    changes[key] = values[remainder]
                add(effective_config(base, changes))
        else:
            for values in itertools.product(*space.values()):
                add(effective_config(base, dict(zip(space, values))))
    else:
        # First visit each dimension around the same baseline, before spending
        # the remaining budget on combinations of metric-specific incumbents.
        for key, values in space.items():
            for value in values:
                add(effective_config(base, {key: value}))
    return result[:limit] if limit else result


def aggregate(rows):
    throughput = [r["result"] for r in rows if r["result"]["config"]["mode"] == "throughput"]
    latency = [r["result"] for r in rows if r["result"]["config"]["mode"] == "write_latency"]
    if not throughput or not latency:
        return None
    kinds = {r.get("latency_kind", "single_call") for r in latency}
    if len(kinds) != 1:
        raise ValueError("cannot mix single-call and batch-average samples")
    kind = kinds.pop()
    sample_key = "batch_average_write_ns" if kind == "batch_average" else "successful_write_ns"
    divisor = latency[0]["config"].get("latency_batch_size", 1) if kind == "batch_average" else 1
    if any((r["config"].get("latency_batch_size", 1) if kind == "batch_average" else 1) != divisor for r in latency):
        raise ValueError("cannot mix different latency batch sizes")
    values = [r["m_messages_per_second"] for r in throughput]
    result = {"throughput": statistics.fmean(values), "throughput_min": min(values), "throughput_max": max(values),
              "cpu_cores": statistics.fmean(r["cpu_cores"] for r in throughput),
              "peak_rss_mib": max(r["peak_rss_mib"] for r in throughput),
              "failed_write_calls": statistics.fmean(r["failed_write_calls"] for r in throughput),
              "actual_spsc_capacity": throughput[0]["actual_spsc_capacity"],
              "nominal_buffer_bytes": throughput[0]["nominal_buffer_bytes"],
              "latency_samples_min": min(r["latency_samples"] for r in latency),
              "timer_quantum_ns": max(r["timer_quantum_ns"] for r in latency),
              "latency_kind": kind, "latency_batch_size": divisor,
              "latency_quantum_ns": max(r.get("latency_quantum_ns", r["timer_quantum_ns"]) for r in latency),
              "latency_overhead_p50_ns": statistics.fmean(r["timer_overhead_ns"]["p50"]/divisor for r in latency),
              "timer_overhead_p50_ns": statistics.fmean(r["timer_overhead_ns"]["p50"] for r in latency),
              "resolution_limited": any(r["resolution_limited"] for r in latency),
              "p999_sample_count_warning": any(r["p999_sample_count_warning"] for r in latency)}
    if any(not r[sample_key] for r in latency):
        return None
    for key in OBJECTIVES[1:]:
        result[key] = statistics.fmean(r[sample_key][key] for r in latency)
        result[key+"_status"] = "unresolved" if any(
            r[sample_key][key] <= 0 or r[sample_key][key] <
            max(5*r.get("latency_quantum_ns", r["timer_quantum_ns"]),
                2*r["timer_overhead_ns"]["p50"]/divisor) for r in latency) else "observed"
    if all(r["config"].get("measure_callback_time", False) for r in throughput):
        for key in ("producer_seconds", "callback_processing_seconds", "callback_to_producer_ratio", "drain_seconds"):
            result[key] = statistics.fmean(r[key] for r in throughput)
        result["callback_ratio_min"] = min(r["callback_to_producer_ratio"] for r in throughput)
        result["callback_ratio_max"] = max(r["callback_to_producer_ratio"] for r in throughput)
    if all("backpressured_messages" in r for r in throughput):
        for key in ("backpressured_messages", "producer_retry_seconds", "producer_retry_max_seconds"):
            result[key] = statistics.fmean(r[key] for r in throughput)
        result["backpressured_message_fraction"] = statistics.fmean(r["backpressured_messages"]/r["config"]["messages"] for r in throughput)
        result["failed_attempt_fraction"] = statistics.fmean(r["failed_write_calls"]/(r["config"]["messages"]+r["failed_write_calls"]) for r in throughput)
        pressure = [r.get("library_backpressure") for r in throughput]
        result["library_backpressure"] = None if any(p is None for p in pressure) else {
            key: statistics.fmean(p[key] for p in pressure) for key in (
                "writer_pressure_events", "writer_rejected_attempts", "writer_blocked_events", "writer_blocked_ticks",
                "copy_memory_full_events", "copy_backpressured_frames")}
    return result


def latency_status(metrics, objective):
    # Conservative reporting guard, not a confidence interval or an overhead correction.
    limit = max(5*metrics.get("latency_quantum_ns", metrics.get("timer_quantum_ns", 0)),
                2*metrics.get("latency_overhead_p50_ns", metrics.get("timer_overhead_p50_ns", 0)))
    return metrics.get(objective+"_status", "unresolved" if metrics[objective] <= 0 or metrics[objective] < limit else "observed")


def select_best(candidates, objective):
    if objective not in OBJECTIVES:
        raise ValueError("Unknown objective")
    if not candidates:
        raise ValueError("No complete successful measurements")
    if objective == "throughput":
        return max(candidates, key=lambda c: c["metrics"][objective])
    unresolved = [c for c in candidates if latency_status(c["metrics"], objective) == "unresolved"]
    if unresolved:
        # Values below the measurement floor cannot establish a latency winner.
        # Continue the search with a throughput-selected representative of this group.
        return max(unresolved, key=lambda c: c["metrics"]["throughput"])
    return min(candidates, key=lambda c: (c["metrics"][objective], -c["metrics"]["throughput"]))


def objective_result(candidates, objective, length, iterations):
    best = select_best(candidates, objective)
    unresolved = objective != "throughput" and latency_status(best["metrics"], objective) == "unresolved"
    result = dict(best, objective=objective, payload_bytes=length, callback_iterations=iterations,
                  ranking_status="unresolved" if unresolved else "observed",
                  objective_value=None if unresolved else best["metrics"][objective],
                  selection_basis="throughput_representative" if unresolved else objective)
    if unresolved:
        result["resolution_limited_candidates"] = [c["candidate_id"] for c in candidates
            if latency_status(c["metrics"], objective) == "unresolved"]
    elif objective != "throughput":
        floor = best["metrics"][objective]
        result["within_one_timer_tick"] = [c["candidate_id"] for c in candidates
            if c["metrics"][objective] <= floor+max(c["metrics"]["latency_quantum_ns"], best["metrics"]["latency_quantum_ns"])]
    return result


class ParameterSearch:
    def __init__(self, executable, profile, output, allow_debug=False):
        self.executable = Path(executable).resolve()
        self.profile = profile
        if set(profile) - {"payload_bytes", "callback_iterations", "base", "search", "spaces", "transports", "callback_calibration", "base_by_payload"}:
            raise ValueError("unknown profile field")
        self.output = Path(output)
        if self.output.exists() and any(self.output.iterdir()):
            raise ValueError("output directory must be empty; existing measurements are preserved")
        self.output.mkdir(parents=True, exist_ok=True)
        self.defaults = json.loads(subprocess.check_output([str(self.executable), "--defaults"], text=True))
        self.build_info = json.loads(subprocess.check_output([str(self.executable), "--build-info"], text=True))
        if not allow_debug and (not self.build_info["optimized"] or self.build_info["sanitized"]):
            raise ValueError("Performance search requires an optimized non-sanitized binary")
        self.settings = {"strategy": "coordinate", "max_candidates_per_transport": 32, "passes": 2,
                         "screen_messages": 1000000, "final_messages": 20000000,
                         "screen_repetitions": 3, "final_repetitions": 3,
                         "screen_min_rounds": 0, "final_min_rounds": 0, "validation_messages": 10000,
                         "batch_reference": True}
        self.settings.update(profile.get("search", {}))
        if set(self.settings) - {"strategy", "max_candidates_per_transport", "passes", "screen_messages",
            "final_messages", "screen_repetitions", "final_repetitions", "validation_messages", "batch_reference",
            "screen_min_rounds", "final_min_rounds"}:
            raise ValueError("unknown search setting")
        if self.settings["strategy"] not in ("coordinate", "grid"):
            raise ValueError("strategy must be coordinate or grid")
        for key in ("screen_messages", "final_messages", "screen_repetitions", "final_repetitions", "validation_messages", "passes"):
            if not isinstance(self.settings[key], int) or self.settings[key] < 1:
                raise ValueError(key + " must be positive")
        for key in ("screen_min_rounds", "final_min_rounds"):
            if not isinstance(self.settings[key], int) or self.settings[key] < 0:
                raise ValueError(key + " must be nonnegative")
        if not isinstance(self.settings["max_candidates_per_transport"], int) or self.settings["max_candidates_per_transport"] < 0:
            raise ValueError("candidate limit must be nonnegative (0 means unlimited)")
        if not isinstance(self.settings["batch_reference"], bool):
            raise ValueError("batch_reference must be boolean")
        self.transports = profile.get("transports", list(TRANSPORTS))
        if not self.transports or len(set(self.transports)) != len(self.transports) or any(t not in TRANSPORTS for t in self.transports):
            raise ValueError("invalid transport list")
        self.base = dict(self.defaults)
        if set(profile.get("base", {})) - set(self.defaults):
            raise ValueError("unknown base parameter")
        self.base.update(profile.get("base", {}))
        self.base_by_payload = profile.get("base_by_payload", {})
        if not isinstance(self.base_by_payload,dict) or any(str(k) not in ("8","16","32","64") for k in self.base_by_payload):
            raise ValueError("invalid base_by_payload lengths")
        for changes in self.base_by_payload.values():
            if not isinstance(changes,dict): raise ValueError("base_by_payload entries must be mappings")
            validate_length_reference(changes,self.defaults)
        self.calibration = profile.get("callback_calibration")
        self.calibration_results = []
        if self.calibration is not None:
            defaults = dict(target_ratio=2.5, min_ratio=2.0, max_ratio=3.0, repetitions=3,
                            messages=1000000, min_rounds=2, max_iterations=1024,
                            max_candidates=10, reference_publish_count=16)
            if not isinstance(self.calibration, dict) or set(self.calibration) - set(defaults):
                raise ValueError("invalid callback calibration settings")
            defaults.update(self.calibration)
            self.calibration = defaults
            validate_calibration(defaults)
            if "callback_iterations" in profile or not self.base.get("measure_callback_time") or not self.base.get("round_storage_bytes"):
                raise ValueError("calibration requires callback timing/rounds and replaces callback_iterations")
        if any(k in profile.get("base", {}) for k in ("payload_bytes", "messages", "mode", "transport", "callback_iterations")):
            raise ValueError("put payload_bytes / callback_iterations at profile top level; counts belong in search")
        self.spaces = profile.get("spaces", {})
        if set(self.spaces) - set(TRANSPORTS) - {"spsc"}:
            raise ValueError("unknown search-space transport")
        for transport in self.transports:
            validate_space(self.space(transport), self.defaults, transport)
        self.rng = random.Random(self.base["random_seed"])
        self.rows, self.failures, self.checksums = [], [], {}
        self.raw = (self.output / "raw.jsonl").open("w", encoding="utf-8")
        self.errors = (self.output / "failures.jsonl").open("w", encoding="utf-8")
        self.runs = 0
        self.started = time.monotonic()

    def length_base(self, length):
        return effective_config(self.base,self.base_by_payload.get(str(length),{}))

    def space(self, transport):
        return self.spaces.get(transport, self.spaces.get("spsc", {}) if transport != "two_buf_queue" else {})

    def run_one(self, config, stage, trial, identity):
        command = [str(self.executable)]
        for key, value in config.items():
            command += ["--" + key, str(value).lower() if isinstance(value, bool) else str(value)]
        try:
            process = subprocess.run(command, capture_output=True, text=True,
                                     timeout=config["timeout_seconds"] + 10, check=True)
            result = json.loads(process.stdout)
            if result["consumed"] != config["messages"]:
                raise ValueError("wrong consumed message count")
            if config.get("round_storage_bytes", 0):
                if (result["timing_scope"] != "producer_rounds" or
                    result["completed_rounds"] != expected_round_count(config, result) or
                    result["seconds"] != result["producer_seconds"]):
                    raise ValueError("invalid producer round timing/completion")
            if config.get("producer_qos") == "user_interactive" and not result["producer_qos_applied"]:
                raise ValueError("producer high QoS was not applied")
            if config.get("measure_callback_time") and (result["timed_callback_messages"] != config["messages"] or
                                                       result["callback_processing_seconds"] <= 0):
                raise ValueError("callback timing count/elapsed validation failed")
            signature = (stage, config["payload_bytes"], config["callback_iterations"], config["messages"],
                         result["effective_warmup_messages"])
            checksum = result["callback_work_checksum"]
            if signature in self.checksums and self.checksums[signature] != checksum:
                raise ValueError("callback work differs between configurations/libraries")
            self.checksums[signature] = checksum
            row = {"stage": stage, "trial": trial, "candidate_id": identity, "command": command, "result": result}
            self.rows.append(row)
            self.raw.write(json.dumps(row, ensure_ascii=False) + "\n")
            self.raw.flush()
            return row
        except (subprocess.SubprocessError, ValueError) as error:
            row = {"stage": stage, "trial": trial, "candidate_id": identity, "command": command,
                   "error": str(error), "stderr": (getattr(error, "stderr", "") or "")[-8192:]}
            self.failures.append(row)
            self.errors.write(json.dumps(row, ensure_ascii=False) + "\n")
            self.errors.flush()
            return None
        finally:
            self.runs += 1
            if self.runs % 20 == 0:
                print("{} runs, {:.1f}s elapsed".format(self.runs, time.monotonic()-self.started), file=sys.stderr, flush=True)

    def measurement_count(self, config, stage):
        count = self.settings["screen_messages" if stage == "screen" else "final_messages"]
        if config.get("round_storage_bytes", 0):
            count = max(count, (config["round_storage_bytes"]//config["payload_bytes"]) *
                        self.settings.get(stage+"_min_rounds", 0))
        return count

    def evaluate(self, configs, stage):
        count_key = "screen_messages" if stage == "screen" else "final_messages"
        repetition_key = "screen_repetitions" if stage == "screen" else "final_repetitions"
        valid, jobs = [], []
        if stage == "screen":
            validations = list(configs)
            self.rng.shuffle(validations)
            for config in validations:
                checked = dict(config, mode="throughput", verify_payload=True,
                               messages=self.settings["validation_messages"],
                               warmup_messages=min(config["warmup_messages"], self.settings["validation_messages"]))
                if self.run_one(checked, "validation", 0, candidate_id(config)):
                    valid.append(config)
        else:
            valid = list(configs)
        for trial in range(self.settings[repetition_key]):
            round_jobs = []
            for config in valid:
                for mode in ("throughput", "write_latency"):
                    checked = dict(config, mode=mode, messages=self.measurement_count(config, stage),
                                   random_seed=config["random_seed"]+trial)
                    round_jobs.append((checked, trial, candidate_id(config)))
            self.rng.shuffle(round_jobs)
            jobs.extend(round_jobs)
        start_index = len(self.rows)
        for checked, trial, identity in jobs:
            self.run_one(checked, stage, trial, identity)
        result = []
        for config in valid:
            identity = candidate_id(config)
            rows = [r for r in self.rows[start_index:] if r["candidate_id"] == identity and r["stage"] == stage]
            # A candidate must finish every scheduled run; do not select partial successes.
            if len(rows) != self.settings[repetition_key]*2:
                continue
            metrics = aggregate(rows)
            if metrics:
                result.append({"candidate_id": identity, "parameters": dict(config, messages=self.measurement_count(config, stage)),
                               "stage": stage, "metrics": metrics})
        return result

    def calibrate_callback(self, length):
        settings = self.calibration
        observations = {}
        def probe(iterations):
            length_base = self.length_base(length)
            reference_publish = (length_base["publish_count"] if "publish_count" in self.base_by_payload.get(str(length),{})
                                 else settings["reference_publish_count"])
            config = dict(length_base, transport="two_buf_queue", payload_bytes=length,
                          callback_iterations=iterations, publish_count=reference_publish,
                          mode="throughput", verify_payload=False)
            config["messages"] = max(settings["messages"], settings["min_rounds"] *
                                     (config["round_storage_bytes"]//length))
            measured = []
            for trial in range(settings["repetitions"]):
                row = self.run_one(dict(config, random_seed=config["random_seed"]+trial),
                                   "callback_calibration", trial, candidate_id(config))
                if row is None:
                    raise RuntimeError("callback calibration run failed; raw failure retained")
                measured.append(row["result"])
            observation = dict(iterations=iterations,
                ratio=statistics.fmean(r["callback_to_producer_ratio"] for r in measured),
                producer_seconds=statistics.fmean(r["producer_seconds"] for r in measured),
                callback_processing_seconds=statistics.fmean(r["callback_processing_seconds"] for r in measured),
                ratios=[r["callback_to_producer_ratio"] for r in measured])
            observations[iterations] = observation
            print("callback calibration {} B: {} loops, {:.2f}x".format(length, iterations, observation["ratio"]),
                  file=sys.stderr, flush=True)
            return observation["ratio"]
        selected = choose_callback_iterations(probe, settings)
        record = dict(payload_bytes=length, reference_transport="two_buf_queue",
                      reference_publish_count=(self.length_base(length)["publish_count"] if "publish_count" in self.base_by_payload.get(str(length),{}) else settings["reference_publish_count"]),
                      reference_parameters=self.length_base(length),
                      selected_iterations=selected, selected_ratio=observations[selected]["ratio"],
                      target_met=settings["min_ratio"] <= observations[selected]["ratio"] <= settings["max_ratio"],
                      observations=list(observations.values()), settings=settings,
                      timing_scope="round first-to-last callback including dispatch and in-round waits; excluding between-round idle")
        self.calibration_results.append(record)
        (self.output/"callback-calibration.json").write_text(json.dumps(self.calibration_results, indent=2)+"\n")
        return selected

    def run(self):
        lengths = self.profile.get("payload_bytes", [8, 16, 32, 64])
        loops = self.profile.get("callback_iterations", [0])
        if not lengths or any(n not in (8, 16, 32, 64) for n in lengths):
            raise ValueError("invalid payload sizes")
        if not loops or any(not isinstance(n, int) or n < 0 for n in loops):
            raise ValueError("invalid callback loop values")
        all_candidates, groups, winners = [], [], []
        limit = self.settings["max_candidates_per_transport"]
        workloads = ((length, self.calibrate_callback(length)) for length in lengths) if self.calibration is not None else itertools.product(lengths, loops)
        for length, iterations in workloads:
            configurations, initial, seen = {}, [], set()
            for transport in self.transports:
                base = dict(self.length_base(length), transport=transport, payload_bytes=length, callback_iterations=iterations)
                candidates = initial_candidates(base, self.space(transport), self.settings["strategy"], limit, self.rng)
                configurations[transport] = candidates
                initial.extend(candidates)
                seen.update(candidate_id(c) for c in candidates)
            screened = self.evaluate(initial, "screen")
            if self.settings["strategy"] == "coordinate":
                for unused_pass in range(self.settings["passes"]):
                    additions = []
                    for transport in self.transports:
                        current = [c for c in screened if c["parameters"]["transport"] == transport]
                        if not current:
                            continue
                        parents = {select_best(current, o)["candidate_id"]: select_best(current, o)["parameters"] for o in OBJECTIVES}
                        possibilities = []
                        for parent in parents.values():
                            for key, values in self.space(transport).items():
                                for value in values:
                                    candidate = effective_config(parent, {key: value})
                                    if candidate_id(candidate) not in seen:
                                        possibilities.append(candidate)
                        self.rng.shuffle(possibilities)
                        for candidate in possibilities:
                            if limit and len(configurations[transport]) >= limit:
                                break
                            identity = candidate_id(candidate)
                            if identity in seen:
                                continue
                            seen.add(identity)
                            configurations[transport].append(candidate)
                            additions.append(candidate)
                    if not additions:
                        break
                    screened.extend(self.evaluate(additions, "screen"))
            all_candidates.extend(screened)
            finalists = {}
            for transport in self.transports:
                candidates = [c for c in screened if c["parameters"]["transport"] == transport]
                if not candidates:
                    raise RuntimeError("No valid candidates for {} {} bytes".format(transport, length))
                for objective in OBJECTIVES:
                    best = select_best(candidates, objective)
                    finalists[best["candidate_id"]] = best["parameters"]
            confirmed = self.evaluate(list(finalists.values()), "final")
            all_candidates.extend(confirmed)
            selected_configs = {}
            for transport in self.transports:
                candidates = [c for c in confirmed if c["parameters"]["transport"] == transport]
                for objective in OBJECTIVES:
                    winner = objective_result(candidates, objective, length, iterations)
                    winners.append(winner)
                    selected_configs[winner["candidate_id"]] = winner["parameters"]
                groups.append({"payload_bytes": length, "callback_iterations": iterations, "transport": transport,
                               "tested_candidates": len(configurations[transport]),
                               "successful_screen_candidates": len([c for c in screened if c["parameters"]["transport"] == transport]),
                               "search_space_product": math.prod(len(v) for v in self.space(transport).values()),
                               "finalists": len(candidates)})
            if self.settings["batch_reference"]:
                for identity, config in selected_configs.items():
                    checked = dict(config, mode="call_batch", producer_interval_ns=0, messages=self.settings["final_messages"])
                    self.run_one(checked, "batch_reference", 0, identity)
            self.write_reports(all_candidates, groups, winners)
        self.raw.close()
        self.errors.close()
        return winners

    def write_reports(self, candidates, groups, winners):
        ranked = [w for w in winners if w["ranking_status"] != "unresolved"]
        unresolved = [w for w in winners if w["ranking_status"] == "unresolved"]
        summary = {"scope": "best measured configurations within the declared search space and budget; not a global optimum",
                   "aggregation": "arithmetic mean of per-run throughput and per-run quantiles; no pooled quantiles",
                   "ranking_policy": "Latency below 5 effective ticks or twice scaled median empty-bracket overhead is unresolved; zero is never a latency winner. For batch averages both tick and overhead are divided by batch size. This guard is not a confidence interval.",
                   "settings": self.settings, "groups": groups, "winners": ranked,
                   "unresolved": unresolved, "failed_runs": len(self.failures)}
        (self.output/"best.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
        self.write_csv(self.output/"candidates.csv", candidates)
        self.write_csv(self.output/"best.csv", ranked)
        self.write_csv(self.output/"unresolved.csv", unresolved)
        lines = ["# 可调性能测试结果", "", "最优仅指本次候选集合；各指标独立选择，再用独立正式轮次复测。",
                 "低于测量下限的延时指标显示为精度不足，不评选最优；对应行仅保留按吞吐选择的代表配置。",
                 "延时为每组平均写入耗时的分位数：组总耗时 / 组内成功写入次数，包含循环、重试与计时开销，不主动 flush。",
                 "这不是单次 write 调用的 p50/p99/p99.9；预生成负载在计时区之外。组大小见完整参数 latency_batch_size。",
                 "", "| 字节 | 回调循环 | 实现 | 优化目标 | 排名状态 | M msg/s | 组均值 p50 ns | p99 ns | p99.9 ns | CPU 核 | 配置 |",
                 "|---:|---:|---|---|---|---:|---:|---:|---:|---:|---|"]
        for w in winners:
            m = w["metrics"]
            shown = ["精度不足" if latency_status(m, key) == "unresolved" else "{:.3f}".format(m[key]) for key in OBJECTIVES[1:]]
            status = "无法可靠排名；代表配置" if w["ranking_status"] == "unresolved" else "按观测值选择"
            lines.append("| {} | {} | {} | {} | {} | {:.3f} | {} | {} | {} | {:.2f} | {} |".format(
                w["payload_bytes"], w["callback_iterations"], w["parameters"]["transport"], w["objective"],
                status, m["throughput"], *shown, m["cpu_cores"], w["candidate_id"]))
        lines += ["", "各指标为独立运行的算术平均；分位数先逐次计算，再平均。重复次数见 settings。"]
        pressure_winners = [w for w in winners if w["objective"] == "throughput" and
                            "backpressured_messages" in w["metrics"]]
        if pressure_winners:
            lines += ["", "## 吞吐配置的时间比与背压", "",
                      "下表各项均为吞吐正式轮次的算术平均。回调时间为各轮首条入口至末条业务完成的区间之和，含轮内分发、等待和调度；比例大于 1 本身不能证明写端背压。",
                      "写入失败后重试直到成功，故失败次数不等于最终丢失条数。重试计时仅在失败路径启用。", "",
                      "| 字节 | 实现 | 写线程 ms | 回调区间 ms | 回调/写线程 | 受压消息 | 失败尝试 | 重试 ms | Writer压力 | Writer阻塞 | 二层拒绝 |",
                      "|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
            for w in pressure_winners:
                m = w["metrics"]
                ms = lambda key: "{:.3f}".format(m[key]*1000) if key in m else "—"
                ratio = "{:.3f}×".format(m["callback_to_producer_ratio"]) if "callback_to_producer_ratio" in m else "—"
                p = m.get("library_backpressure")
                internal = ["{:.1f}".format(p[key]) if p is not None else "—" for key in
                            ("writer_pressure_events", "writer_blocked_events", "copy_memory_full_events")]
                lines.append("| {} | {} | {} | {} | {} | {:.1f} | {:.1f} | {} | {} | {} | {} |".format(
                    w["payload_bytes"], w["parameters"]["transport"], ms("producer_seconds"),
                    ms("callback_processing_seconds"), ratio, m["backpressured_messages"],
                    m["failed_write_calls"], ms("producer_retry_seconds"), *internal))
            lines += ["", "受压消息指至少一次 try 写入失败的消息；失败尝试含同一条消息的多次重试。TwoBufQueue Block 模式内部等待可能最终成功，不体现在 try 失败数中，另见 Writer阻塞计数及原始 blocked_ticks。",
                      "TwoBufQueue 内部统计扣除预热，覆盖正式发送、排空和停止；二层拒绝按复制片段尝试计数，不能与逐条写失败等同。第三方未提供同口径的内部计数，显示 —；其 try 失败和重试耗时仍可直接统计。"]
        if getattr(self, "calibration_results", None):
            lines += ["回调循环先以 TwoBufQueue 固定 reference_publish_count 校准，各库使用相同次数和计算逻辑。",
                      "回调计时为每轮首条到末条的处理区间，含分发/轮内等待，排除轮间等待；不是逐条纯函数耗时之和。",
                      "比例以 TwoBufQueue 为基准，其他库写入速度不同，不强行调整各自循环次数。校准观测见 callback-calibration.json。"]
        if any(w["parameters"].get("round_storage_bytes", 0) for w in winners):
            lines += ["吞吐按各轮写线程发送耗时之和计算；轮末排空回调和最终 finish 在计时外，不主动 flush。复制和回调仍会并发竞争资源。",
                      "每轮条数上限 = round_storage_bytes / payload_bytes 向下取整；按源块内自然发布位置略微向下调整。相同源配置的四种实现使用相同边界。",
                      "本机高 QoS 如启用仅是调度偏好；producer_cpu=-1 表示未硬绑核。实际状态见 raw.jsonl。"]
        lines += ["", "已排名的最优参数见 best.json 的 winners / best.csv；无法排名的代表配置见 unresolved / unresolved.csv。",
                  "所有候选及原始观测值见 candidates.csv，逐轮命令与数据见 raw.jsonl。",
                  "回调循环 0 表示无模拟业务计算；测试框架仍检查序号和计数。TwoBufQueue 包含第二层复制。",
                  "SPSC 使用原生轮询消费，TwoBufQueue 使用自身自旋/事件策略；CPU 和名义缓冲容量同时报告。"]
        if any(w["metrics"]["resolution_limited"] for w in winners):
            lines += ["", "**计时分辨率提示：部分 write 调用接近计时器粒度或空测量开销。**",
                      "使用保守阈值：任一轮的分位数低于 5 个 tick 或两倍空计时中位数时，不评选该项最优。阈值不是置信区间。",
                      "resolution_limited_candidates 列出无法分辨的候选；原始的 0 ns 观测仍保留供审计。",
                      "本机 p50/p99/p99.9 是组均值的原始观测值，不能解释为单次函数分位数；没有机械扣减计时开销。",
                      "batch_reference 给出批次均值参考，不能替代单次调用分位数。"]
        if any(w["metrics"]["p999_sample_count_warning"] for w in winners):
            lines += ["", "部分正式轮次少于 100,000 个有效样本，p99.9 尾部样本较少；应增加 messages 或减小 latency_batch_size 后复测（组大小改变了统计口径）。"]
        if self.settings["screen_repetitions"] < 2 or self.settings["final_repetitions"] < 3:
            lines += ["", "本次重复次数较少，只适合流程验证或初筛，不能据此宣布稳定性能排名。"]
        if self.failures:
            lines += ["", "失败的配置/轮次保留在 failures.jsonl，不参与最优选择。"]
        (self.output/"SUMMARY.md").write_text("\n".join(lines)+"\n", encoding="utf-8")

    @staticmethod
    def write_csv(path, candidates):
        rows = []
        for c in candidates:
            row = {"stage": c["stage"], "candidate_id": c["candidate_id"], "objective": c.get("objective", ""),
                   "ranking_status": c.get("ranking_status", ""), "objective_value": c.get("objective_value", ""),
                   "selection_basis": c.get("selection_basis", "")}
            row.update(c["parameters"])
            row.update(c["metrics"])
            rows.append(row)
        if not rows:
            return
        with path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)


def write_environment(search, profile_path):
    root = Path(__file__).resolve().parents[2]
    files = [p for folder in (root/"src", root/"include", root/"benchmarks/tunable") for p in folder.rglob("*") if p.is_file()]
    files += [root/"CMakeLists.txt", Path(__file__), root/"benchmarks/spsc/spsc_dependencies.json", profile_path]
    manifest = {str(p.relative_to(root)) if str(p).startswith(str(root)+os.sep) else str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    cache = search.executable.parent/"CMakeCache.txt"
    if not cache.exists():
        cache = search.executable.parent.parent/"CMakeCache.txt"
    build_flags = {}
    if cache.exists():
        for line in cache.read_text(encoding="utf-8").splitlines():
            if line.startswith(("CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS", "CMAKE_BUILD_TYPE:",
                                "CMAKE_EXE_LINKER_FLAGS", "CMAKE_OSX_DEPLOYMENT_TARGET:", "CMAKE_GENERATOR:")) and "=" in line:
                key, value = line.split("=", 1)
                build_flags[key.split(":", 1)[0]] = value
    cpu_model = platform.processor()
    if sys.platform == "darwin":
        try:
            cpu_model = subprocess.check_output(["/usr/sbin/sysctl", "-n", "machdep.cpu.brand_string"],
                                               text=True, stderr=subprocess.DEVNULL).strip()
        except (OSError, subprocess.SubprocessError):
            pass
    info = {"platform": platform.platform(), "machine": platform.machine(), "cpu_model": cpu_model,
            "logical_cpus": os.cpu_count(), "cmake_flags": build_flags,
            "python": sys.version, "build": search.build_info, "executable": str(search.executable),
            "executable_sha256": hashlib.sha256(search.executable.read_bytes()).hexdigest(), "profile": search.profile,
            "resolved_search": search.settings, "source_sha256": manifest,
            "dependencies": json.loads((root/"benchmarks/spsc/spsc_dependencies.json").read_text()),
            "note": "Record power policy, CPU model and background load when publishing results; no affinity claimed unless successful."}
    (search.output/"environment.json").write_text(json.dumps(info, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--profile", type=Path, default=Path(__file__).resolve().parent.parent/"profiles"/"spsc_quick.json")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--allow-debug", action="store_true", help="Contract testing only; never publish these as performance results")
    args = parser.parse_args()
    profile = json.loads(args.profile.read_text(encoding="utf-8"))
    if args.output.exists() and any(args.output.iterdir()):
        parser.error("output directory must be empty to preserve previous measurements")
    search = ParameterSearch(args.executable, profile, args.output, args.allow_debug)
    write_environment(search, args.profile.resolve())
    search.run()
    print("Results:", (args.output/"SUMMARY.md").resolve())


if __name__ == "__main__":
    main()
