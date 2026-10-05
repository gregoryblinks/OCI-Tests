#!/usr/bin/env python3
"""ES MBO microstructure research add-on for Bookmap.

Python 3.7+; standard library only. No order submission, account access, or
profitability claim. Live bars use LOCAL_ARRIVAL time, NOT exchange time.
Load this file in Bookmap's Python API, or run --demo / --replay offline.
See README.md and docs/METHODOLOGY.md before using the signals.
"""
import argparse
import csv
import hashlib
import heapq
import json
import math
import os
import queue
import random
import re
import statistics
import subprocess
import sys
import threading
import time
import traceback
import uuid
from collections import Counter, defaultdict, deque
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from functools import lru_cache
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

VERSION = "1.2.0"
SECOND = 1_000_000_000
MINUTE = 60 * SECOND
SIDES = ("bid", "ask")
SOURCES = {
    "bookmap_python": "https://github.com/BookmapAPI/python-api",
    "bookmap_python_source": "https://github.com/BookmapAPI/python-api/blob/master/client-rpc/src/bookmap/bookmap.py",
    "bookmap_python_requirements": "https://bookmap.com/knowledgebase/docs/Addons-Python-API",
    "rithmic_setup": "https://bookmap.com/knowledgebase/docs/KB-Help-MBO-Bundle-Installation-Guide",
    "cme_mbo": "https://www.cmegroup.com/articles/faqs/market-by-order-mbo.html",
    "cme_es": "https://www.cmegroup.com/markets/equities/sp/e-mini-sandp500.contractSpecifications.html",
}


@lru_cache(maxsize=512)
def _utc_second(sec):
    return datetime.fromtimestamp(sec, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")


@lru_cache(maxsize=8)
def _utc_day(day):
    return datetime.fromtimestamp(day * 86400, timezone.utc).strftime("%Y-%m-%d")


def utc(ns):
    # Do not pass nanoseconds through a float: Excel-safe ISO text preserves them.
    sec, nano = divmod(int(ns), SECOND)
    return _utc_second(sec) + ".%09dZ" % nano


def clean_id(value):
    if value is None or str(value).strip().lower() in ("", "none", "null", "nan", "0", "-1"):
        return ""
    return str(value)


def ratio(a, b, default=0.0):
    return a / b if b else default


def finite(x):
    return isinstance(x, (int, float)) and math.isfinite(x)


def quantile(values, probability):
    if not values:
        return None
    a = sorted(values)
    k = (len(a) - 1) * probability
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    return a[lo] + (a[hi] - a[lo]) * (k - lo)


def percentile(value, values):
    """Midrank percentile: ties, including an all-zero history, score 50."""
    if not values or not finite(value):
        return None
    return 100.0 * (sum(x < value for x in values) + 0.5 * sum(x == value for x in values)) / len(values)


@dataclass
class Config:
    lookback: int = 15
    min_baseline: int = 15
    context_bars: int = 3
    confirmation_bars: int = 2
    episode_ttl_bars: int = 5
    high_percentile: float = 80.0
    low_percentile: float = 20.0
    effort_result_gap: float = 40.0
    near_ticks: int = 8
    bootstrap_seconds: float = 5.0
    min_book_orders: int = 20
    stale_quote_seconds: float = 5.0
    min_quote_coverage: float = 0.98
    max_spread_ticks: int = 6
    max_wide_spread_fraction: float = 0.01
    max_processing_lag_ms: float = 1000.0
    match_window_ms: float = 250.0
    refill_window_ms: float = 1500.0
    allow_price_time_matching: bool = False
    min_passive_id_coverage: float = 0.90
    min_execution_match_coverage: float = 0.80
    max_boundary_reduction_share: float = 0.05
    acceptance_fraction: float = 0.60
    absorption_range_fraction: float = 0.25
    trigger_buffer_ticks: int = 1
    invalidation_buffer_ticks: int = 1
    max_events_per_bar: int = 1000000
    event_queue_capacity: int = 200000
    log_full_book_levels: bool = True
    async_csv_logging: bool = True
    csv_queue_capacity: int = 200000
    csv_batch_size: int = 512
    fsync_at_bar_close: bool = False
    assumed_round_trip_cost_ticks: float = 2.0
    symbol_regex: str = r"^ES[HMUZ][0-9]{1,2}(?:[.@].*)?$"

    # AUDIT_SKIP does not assume why a zero-size callback was sent. It is
    # excluded from executions, counted, and fences execution-batch grouping.
    # STRICT retains the prior stop-on-zero policy for diagnostic comparison.
    zero_size_trade_policy: str = "AUDIT_SKIP"
    auto_open_panel: bool = True
    panel_topmost: bool = True
    panel_geometry: str = "900x310+60+60"
    panel_refresh_ms: int = 500

    def validate(self):
        if self.zero_size_trade_policy not in ("AUDIT_SKIP", "STRICT"):
            raise ValueError("zero_size_trade_policy must be AUDIT_SKIP or STRICT")
        for name in ("auto_open_panel", "panel_topmost", "async_csv_logging"):
            if not isinstance(getattr(self, name), bool):
                raise ValueError(name + " must be true or false")
        if not isinstance(self.panel_refresh_ms, int) or isinstance(self.panel_refresh_ms, bool) or not 100 <= self.panel_refresh_ms <= 2000:
            raise ValueError("panel_refresh_ms must be an integer in [100, 2000]")
        if not isinstance(self.panel_geometry, str) or not re.fullmatch(r"[0-9]+x[0-9]+(?:[+-][0-9]+[+-][0-9]+)?", self.panel_geometry):
            raise ValueError("panel_geometry must be WIDTHxHEIGHT or WIDTHxHEIGHT+X+Y")
        if self.lookback < 3 or not 3 <= self.min_baseline <= self.lookback:
            raise ValueError("Require 3 <= min_baseline <= lookback")
        if not 0 < self.low_percentile < 50 < self.high_percentile < 100:
            raise ValueError("Require 0 < low_percentile < 50 < high_percentile < 100")
        for name in ("near_ticks", "context_bars", "confirmation_bars", "episode_ttl_bars",
                     "max_spread_ticks", "max_events_per_bar", "event_queue_capacity", "min_book_orders",
                     "csv_queue_capacity", "csv_batch_size"):
            if isinstance(getattr(self, name), bool) or not isinstance(getattr(self, name), int) or getattr(self, name) <= 0:
                raise ValueError(name + " must be a positive integer")
        for name in ("min_quote_coverage", "min_passive_id_coverage", "min_execution_match_coverage",
                     "max_boundary_reduction_share", "acceptance_fraction", "absorption_range_fraction", "max_wide_spread_fraction"):
            if not 0 <= getattr(self, name) <= 1:
                raise ValueError(name + " must be in [0, 1]")
        for name in ("bootstrap_seconds", "stale_quote_seconds", "match_window_ms", "refill_window_ms",
                     "max_processing_lag_ms", "assumed_round_trip_cost_ticks", "effort_result_gap"):
            if not finite(getattr(self, name)) or getattr(self, name) < 0:
                raise ValueError(name + " must be non-negative and finite")
        for name in ("trigger_buffer_ticks", "invalidation_buffer_ticks"):
            if not isinstance(getattr(self, name), int) or getattr(self, name) < 1:
                raise ValueError(name + " must be a positive integer")
        if self.csv_batch_size > self.csv_queue_capacity:
            raise ValueError("csv_batch_size must not exceed csv_queue_capacity")
        re.compile(self.symbol_regex)
        return self

    @classmethod
    def load(cls, path=None):
        if not path:
            return cls().validate()
        with open(path, encoding="utf-8-sig") as f:
            data = json.load(f)
        return cls(**data).validate()


@dataclass
class Event:
    ts_ns: int
    seq: int
    kind: str
    data: dict = field(default_factory=dict)
    processing_lag_ms: float = 0.0


@dataclass
class Order:
    side: str
    price: int
    qty: int
    born_ns: int
    born_in_bootstrap: bool


class BookError(RuntimeError):
    pass


def whole_level(value, context, field_name, minimum, units="integer level"):
    """Normalize exactly whole numeric values without truncating or rescaling.

    bool is deliberately rejected although it is a subclass of int in Python.
    Bookmap prices are already tick levels. ES size_multiplier is checked at
    subscription; this helper must not divide by pips or guess a size scale.
    """
    if isinstance(value, bool):
        number = None
    elif isinstance(value, int):
        number = value
    elif isinstance(value, float) and math.isfinite(value) and value.is_integer():
        number = int(value)
    else:
        number = None
    if number is None or number < minimum:
        required = "positive" if minimum else "non-negative"
        raise BookError("%s: %s=%r (%s); expected %s %s" % (
            context, field_name, value, type(value).__name__, required, units))
    return number


def whole_mbo_level(value, event_type, field_name, minimum):
    """CANCEL never calls this helper: only its stored order is meaningful."""
    return whole_level(value, "MBO " + event_type, field_name, minimum)


def trade_flag(value, field_name):
    """Reject ambiguous truthy strings/numbers; the published callback uses bool."""
    if not isinstance(value, bool):
        raise BookError("TRADE: %s=%r (%s); expected bool" % (
            field_name, value, type(value).__name__))
    return value


def audit_json_value(value):
    """Losslessly tag nonfinite callback floats for strict JSON/CSV diagnostics.

    Only the exceptional logging path needs this traversal. Ordinary callback
    dictionaries are serialized unchanged. Replay decodes the reserved tag.
    """
    if isinstance(value, float) and not math.isfinite(value):
        return {"__es_nonfinite_float__": repr(value)}
    if isinstance(value, dict):
        return {k: audit_json_value(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [audit_json_value(v) for v in value]
    return value


def decode_audit_json(value):
    if isinstance(value, dict):
        if set(value) == {"__es_nonfinite_float__"}:
            text = value["__es_nonfinite_float__"]
            if text not in ("nan", "inf", "-inf"):
                raise ValueError("Invalid nonfinite callback encoding")
            return float(text)
        return {k: decode_audit_json(v) for k, v in value.items()}
    if isinstance(value, list):
        return [decode_audit_json(v) for v in value]
    return value


class OrderBook:
    """Order membership and aggregate depth, not an exchange-priority queue."""
    def __init__(self):
        self.orders = {}  # type: Dict[str, Order]
        self.levels = {s: {} for s in SIDES}
        self.counts = {s: {} for s in SIDES}
        self.heaps = {s: [] for s in SIDES}
        self.near_cache = {s: {} for s in SIDES}

    def _level(self, side, price, qty_delta, count_delta):
        levels, counts = self.levels[side], self.counts[side]
        old = levels.get(price, 0)
        new = old + qty_delta
        count = counts.get(price, 0) + count_delta
        if new < 0 or count < 0 or ((new == 0) != (count == 0)):
            raise BookError("Aggregate book invariant violated")
        if new:
            levels[price], counts[price] = new, count
            if not old:
                heapq.heappush(self.heaps[side], -price if side == "bid" else price)
        else:
            levels.pop(price, None)
            counts.pop(price, None)
        # Cached near-touch totals are exact, not sampled. Invalidate when the
        # best changes; otherwise a single level delta updates each cached width.
        best = self.best(side)
        for width, (cached_best, total) in list(self.near_cache[side].items()):
            if best != cached_best:
                del self.near_cache[side][width]
            elif best is not None:
                distance = best - price if side == "bid" else price - best
                if 0 <= distance < width:
                    self.near_cache[side][width] = (best, total + qty_delta)
        if len(self.heaps[side]) > 4 * len(levels) + 1024:
            self.heaps[side] = [-p if side == "bid" else p for p in levels]
            heapq.heapify(self.heaps[side])

    def best(self, side):
        heap = self.heaps[side]
        while heap:
            p = -heap[0] if side == "bid" else heap[0]
            if p in self.levels[side]:
                return p
            heapq.heappop(heap)
        return None

    def near(self, side, width):
        best = self.best(side)
        if best is None:
            return 0
        cached = self.near_cache[side].get(width)
        if cached is not None and cached[0] == best:
            return cached[1]
        step = -1 if side == "bid" else 1
        total = sum(self.levels[side].get(best + step * i, 0) for i in range(width))
        self.near_cache[side][width] = (best, total)
        return total

    def is_near(self, side, price, width):
        best = self.best(side)
        if best is None:
            return False
        distance = best - price if side == "bid" else price - best
        # Price improvements are near-touch as well.
        return distance < width

    def snapshot(self):
        return {(s, p): (q, self.counts[s][p]) for s in SIDES for p, q in self.levels[s].items()}

    def apply(self, event_type, oid, price, qty, ts_ns, bootstrap=False):
        if not oid:
            raise BookError("Missing MBO order ID")
        old = self.orders.get(oid)
        if event_type == "CANCEL":
            # Removal is by ID. Callback price/size can be placeholders (e.g.
            # -1 or None); neither determines the removed price or quantity.
            # Use the last reconstructed order, as Bookmap's on_remove_order
            # helper does. Unknown IDs still mean an incomplete/damaged book.
            if old is None:
                raise BookError("Unknown order ID on CANCEL; resnapshot required")
            new = None
        elif event_type in ("BID_NEW", "ASK_NEW", "REPLACE"):
            if event_type == "REPLACE" and old is None:
                raise BookError("Unknown order ID on REPLACE; resnapshot required")
            price = whole_mbo_level(price, event_type, "price_level", 1)
            qty = whole_mbo_level(qty, event_type, "size_level", 0)
            if event_type in ("BID_NEW", "ASK_NEW"):
                if qty == 0:
                    raise BookError("MBO %s: NEW with zero size" % event_type)
                side = "bid" if event_type == "BID_NEW" else "ask"
                if old is not None:
                    raise BookError("Duplicate NEW for a live order ID")
                new = Order(side, price, qty, ts_ns, bootstrap)
            else:
                new = None if qty == 0 else Order(
                    old.side, price, qty, old.born_ns, old.born_in_bootstrap)
        else:
            raise BookError("Unsupported MBO event: " + str(event_type))
        if old and new and old.price == new.price:
            self._level(old.side, old.price, new.qty - old.qty, 0)
            # Preserve the original membership iteration order. This is NOT
            # exchange priority, but deterministic demo/replay tooling uses it.
            del self.orders[oid]
            self.orders[oid] = new
        else:
            if old:
                self._level(old.side, old.price, -old.qty, -1)
                del self.orders[oid]
            if new:
                self._level(new.side, new.price, new.qty, 1)
                self.orders[oid] = new
        moved = bool(old and new and old.price != new.price)
        reduced = old.qty if old and (new is None or moved) else max(0, old.qty - new.qty) if old else 0
        added = new.qty if new and (old is None or moved) else max(0, new.qty - old.qty) if new else 0
        return old, new, reduced, added, moved

    def assert_consistent(self):
        totals, counts = defaultdict(int), defaultdict(int)
        for order in self.orders.values():
            totals[(order.side, order.price)] += order.qty
            counts[(order.side, order.price)] += 1
        assert self.snapshot() == {k: (q, counts[k]) for k, q in totals.items()}


class MemoryStore:
    """Test sink; the live sink never retains raw event history in memory."""
    def __init__(self):
        self.rows = defaultdict(list)
        self.latest_value = None
        self.rejected_value = None
        self.zero_trade_value = None
        self.path = None

    def write(self, table, row, ts_ns):
        self.rows[table].append(dict(row))

    def latest(self, data):
        self.latest_value = dict(data)

    def rejected(self, data):
        self.rejected_value = dict(data)

    def zero_trade(self, data):
        self.zero_trade_value = dict(data)

    def heartbeat(self, data):
        pass

    def flush(self, durable=False):
        pass

    def close(self):
        pass


def safe_cell(value):
    if value is None:
        return ""
    if isinstance(value, float) and not math.isfinite(value):
        value = audit_json_value(value)
    if isinstance(value, (dict, list, tuple)):
        try:
            value = json.dumps(value, separators=(",", ":"), sort_keys=True, allow_nan=False)
        except ValueError:
            # Invalid NaN/infinity must be logged, not cause a second worker
            # failure while attempting to record the original validation fault.
            value = json.dumps(audit_json_value(value), separators=(",", ":"),
                               sort_keys=True, allow_nan=False)
    if isinstance(value, str) and value.lstrip().startswith(("=", "+", "-", "@")):
        return "'" + value
    return value


class CsvStore:
    """Append-only, fixed-schema, daily partitions; UTF-8 BOM for Excel.

    Each process run uses a unique directory. Atomic JSON files are display
    snapshots, not a substitute for the append-only decision audit.
    """
    def __init__(self, root, alias, config, metadata=None):
        safe_alias = re.sub(r"[^A-Za-z0-9_.-]", "_", alias)
        run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "_" + uuid.uuid4().hex[:8]
        self.path = Path(root).expanduser().resolve() / safe_alias / run_id
        self.path.mkdir(parents=True, exist_ok=False)
        self.handles, self.schemas = {}, {}
        manifest = {
            "version": VERSION, "run_id": run_id, "alias": alias, "config": asdict(config),
            "config_sha256": hashlib.sha256(json.dumps(asdict(config), sort_keys=True).encode()).hexdigest(),
            "sources": SOURCES, "source_verification_date": "2026-10-05", "metadata": metadata or {},
            "timestamp_basis": "LOCAL_ARRIVAL unless explicitly replay/synthetic",
            "warnings": ["Research signals, not validated trading performance", "No exchange sequence or PriorityID",
                         "Snapshot completion is heuristic", "Removals are not automatically cancellations",
                         "AUDIT_SKIP excludes zero-size trade callbacks without certifying their upstream meaning"],
        }
        self._atomic("manifest.json", manifest)

    def _atomic(self, name, value):
        target = self.path / name
        temp = target.with_suffix(target.suffix + ".tmp")
        with open(str(temp), "w", encoding="utf-8") as f:
            json.dump(value, f, indent=2, sort_keys=True, allow_nan=False)
        os.replace(str(temp), str(target))

    def write(self, table, row, ts_ns):
        day = _utc_day(int(ts_ns) // (86400 * SECOND))
        keys = tuple(row.keys())
        if table in self.schemas and self.schemas[table] != keys:
            raise RuntimeError("CSV schema drift in " + table)
        self.schemas[table] = keys
        key = (day, table)
        if key not in self.handles:
            folder = self.path / day
            folder.mkdir(exist_ok=True)
            filename = folder / (table + ".csv")
            exists = filename.exists() and filename.stat().st_size > 0
            f = open(str(filename), "a" if exists else "w", newline="", encoding="utf-8" if exists else "utf-8-sig")
            writer = csv.writer(f)
            if not exists:
                writer.writerow(keys)
            self.handles[key] = (f, writer)
            if len(self.handles) > 32:
                oldest = next(iter(self.handles))
                old_file, _ = self.handles.pop(oldest)
                old_file.flush()
                old_file.close()
        self.handles[key][1].writerow([safe_cell(v) for v in row.values()])

    def latest(self, data):
        self._atomic("latest.json", data)

    def rejected(self, data):
        self._atomic("rejected_event.json", data)

    def zero_trade(self, data):
        self._atomic("zero_size_trade_event.json", data)

    def heartbeat(self, data):
        self._atomic("health.json", data)

    def flush(self, durable=False):
        for f, _ in self.handles.values():
            f.flush()
            if durable:
                os.fsync(f.fileno())

    def close(self):
        for f, _ in self.handles.values():
            f.flush()
            f.close()
        self.handles.clear()


class AsyncCsvStore(CsvStore):
    """Bounded, ordered CSV batches on a dedicated writer thread in live mode.

    JSON health/display files stay on the engine thread. CSV formatting and disk
    writes do not stop MBO reconstruction. A full queue or write failure is fatal,
    never an instruction to silently discard audit rows. flush() schedules an
    ordered flush; drain()/close() wait and verify it. RAM buffering is NOT a
    crash-durable journal. fsync_at_bar_close uses an explicit blocking barrier.
    """
    def __init__(self, root, alias, config, metadata=None):
        super().__init__(root, alias, config, metadata)
        self.capacity, self.batch_size = config.csv_queue_capacity, config.csv_batch_size
        self._jobs = queue.Queue(maxsize=max(4, self.capacity // self.batch_size + 8))
        self._state_lock = threading.Lock()
        self._pending = []
        self._outstanding, self._highwater, self._written = 0, 0, 0
        self._oldest_ns, self._writer_error = 0, ''
        self._closing = False
        self._writer = threading.Thread(target=self._write_loop, name='ES-CSV-' + alias, daemon=True)
        self._writer.start()

    def _fail(self, detail):
        with self._state_lock:
            if not self._writer_error:
                self._writer_error = detail
        try:
            (self.path / 'fatal_error.txt').write_text(detail, encoding='utf-8')
        except OSError:
            pass

    def _check(self):
        if self._writer_error:
            raise RuntimeError(self._writer_error)

    def _submit(self, job):
        self._check()
        try:
            self._jobs.put_nowait(job)
        except queue.Full:
            self._fail('CSV_QUEUE_OVERFLOW; audit cannot keep up; run is unsafe')
            self._check()

    def write(self, table, row, ts_ns):
        self._check()
        if self._closing:
            raise RuntimeError('CSV writer is closed')
        with self._state_lock:
            overflow = self._outstanding >= self.capacity
            if not overflow:
                self._outstanding += 1
                self._highwater = max(self._highwater, self._outstanding)
                if self._outstanding == 1:
                    self._oldest_ns = time.monotonic_ns()
        if overflow:
            self._fail('CSV_QUEUE_OVERFLOW; audit cannot keep up; run is unsafe')
            self._check()
        self._pending.append((table, row, ts_ns))
        if len(self._pending) >= self.batch_size:
            self._send_batch()

    def _send_batch(self):
        if self._pending:
            batch, self._pending = self._pending, []
            self._submit(('ROWS', batch, None))

    def _write_loop(self):
        try:
            while True:
                kind, payload, barrier = self._jobs.get()
                try:
                    if kind == 'ROWS':
                        for table, row, ts_ns in payload:
                            CsvStore.write(self, table, row, ts_ns)
                        with self._state_lock:
                            self._outstanding -= len(payload)
                            self._written += len(payload)
                            if not self._outstanding:
                                self._oldest_ns = 0
                    elif kind == 'FLUSH':
                        CsvStore.flush(self, payload)
                    elif kind == 'CLOSE':
                        CsvStore.close(self)
                        return
                except Exception as exc:
                    # Publish failure before waking a drain/close waiter.
                    self._fail('CSV_WRITE_FAILURE: ' + str(exc) + '\n' + traceback.format_exc())
                    raise
                finally:
                    if barrier is not None:
                        barrier.set()
                    self._jobs.task_done()
        except Exception as exc:
            self._fail('CSV_WRITE_FAILURE: ' + str(exc) + '\n' + traceback.format_exc())
            try:
                CsvStore.close(self)
            except Exception:
                pass

    def diagnostics(self):
        with self._state_lock:
            return {'csv_queue_rows': self._outstanding, 'csv_queue_capacity': self.capacity,
                    'csv_queue_highwater': self._highwater, 'csv_rows_written': self._written,
                    # Busy duration is NOT the age of the oldest row: the writer
                    # can be draining continuously. Do not misuse it as latency.
                    'csv_busy_ms': ((time.monotonic_ns() - self._oldest_ns) / 1e6 if self._oldest_ns else 0),
                    'csv_writer_error': self._writer_error, 'csv_async': True}

    def _barrier(self, kind, payload=None, timeout=30.0):
        self._send_batch()
        barrier = threading.Event()
        self._submit((kind, payload, barrier))
        deadline = time.monotonic() + timeout
        while not barrier.wait(.02):
            self._check()
            if time.monotonic() >= deadline:
                self._fail('CSV_DRAIN_TIMEOUT; trailing audit may be incomplete')
                self._check()
        self._check()

    def flush(self, durable=False):
        if self._closing:
            return
        if durable:
            self._barrier('FLUSH', True)
        else:
            self._send_batch()
            self._submit(('FLUSH', False, None))

    def drain(self, timeout=30.0):
        self._barrier('FLUSH', False, timeout)

    def close(self):
        if self._closing:
            return
        if self._writer_error:
            self._closing = True
            # The writer exits itself on IO failure. On producer-side overflow,
            # do not race its handles; ask it to drain already accepted batches.
            try:
                self._jobs.put(('CLOSE', None, None), timeout=1)
            except queue.Full:
                pass
            self._writer.join(timeout=2)
            return
        self._barrier('CLOSE')
        self._closing = True
        self._writer.join(timeout=2)


FLOW_KEYS = ("added", "reduced", "snapshot_added", "snapshot_reduced", "reprice_in", "reprice_out",
             "exec_id", "exec_price", "withdrawal_est", "refill", "same_id_refill", "traded", "trade_count",
             "near_added", "near_reduced", "near_withdrawal_est")


@dataclass
class Bar:
    start: int
    opening_levels: dict
    prev_close: Optional[int]
    reference_up: Optional[int]
    reference_down: Optional[int]
    partial: bool = False
    bootstrap: bool = False
    trades: list = field(default_factory=list)
    adds: list = field(default_factory=list)
    reductions: list = field(default_factory=list)
    levels: dict = field(default_factory=lambda: defaultdict(Counter))
    counts: Counter = field(default_factory=Counter)
    quote: Counter = field(default_factory=Counter)
    flags: set = field(default_factory=set)
    open_tick: Optional[int] = None
    high_tick: Optional[int] = None
    low_tick: Optional[int] = None
    close_tick: Optional[int] = None
    max_lag_ms: float = 0.0
    flow_tracker: Any = None
    batch_tracker: Any = None

    @property
    def end(self):
        return self.start + MINUTE


# Percentiles are calculated for each of these metrics against the previous
# lookback calendar bars, using only valid observations INSIDE that window.
BENCHMARK_METRICS = (
    "volume", "buy_volume", "sell_volume", "abs_delta", "abs_delta_ratio", "range_ticks", "abs_net_ticks",
    "buy_progress_ticks", "sell_progress_ticks", "buy_impact", "sell_impact", "path_efficiency",
    "bid_depth_twa", "ask_depth_twa", "spread_twa", "bid_added", "ask_added",
    "bid_near_added", "ask_near_added", "bid_refill", "ask_refill", "bid_refill_ratio", "ask_refill_ratio",
    "bid_near_withdrawal_est", "ask_near_withdrawal_est", "bid_turnover", "ask_turnover",
    "buy_sweep_count", "sell_sweep_count", "buy_max_batch_qty", "sell_max_batch_qty",
)


class Benchmarks:
    def __init__(self, cfg):
        self.cfg = cfg
        self.history = deque(maxlen=cfg.lookback)

    def compute(self, metrics):
        valid = [x for x in self.history if x["data_valid"]]
        result = {"baseline_n": len(valid), "baseline_window_bars": len(self.history),
                  "baseline_first_utc": valid[0]["bar_start_utc"] if valid else "",
                  "baseline_last_utc": valid[-1]["bar_start_utc"] if valid else ""}
        for name in BENCHMARK_METRICS:
            values = [x[name] for x in valid if finite(x.get(name))]
            result[name + "_n"] = len(values)
            result[name + "_pct"] = percentile(metrics.get(name), values)
            result[name + "_q_low"] = quantile(values, self.cfg.low_percentile / 100)
            result[name + "_q50"] = quantile(values, 0.5)
            result[name + "_q_high"] = quantile(values, self.cfg.high_percentile / 100)
        result["baseline_ready"] = len(valid) >= self.cfg.min_baseline
        return result

    def append(self, metrics):
        self.history.append(dict(metrics))


def attribute_flows(bar, cfg, store, alias):
    """Bounded-time, quantity-conserving correlation, in either callback order.

    Matching NEVER crosses a candle boundary. A removal's residual is only an
    estimate of withdrawal, not a confirmed cancellation. Price-only matching
    is optional and never upgrades the passive-ID coverage quality gate.
    """
    if bar.flow_tracker is not None:
        return bar.flow_tracker.finish()
    groups = defaultdict(deque)
    for t in bar.trades:
        key = (t["side"], t["price"], t["passive_id"])
        groups[key].append({"ts": t["ts"], "remaining": t["qty"], "seq": t["seq"]})
    window = int(cfg.match_window_ms * 1_000_000)
    boundary_qty = 0
    for r in bar.reductions:
        level = bar.levels[(r["side"], r["price"])]
        remaining, by_id, by_price = r["qty"], 0, 0
        if not r["bootstrap"] and not r["moved"]:
            candidate_keys = [(r["side"], r["price"], r["oid"])]
            if cfg.allow_price_time_matching:
                candidate_keys.append((r["side"], r["price"], ""))
            for index, key in enumerate(candidate_keys):
                credits = groups.get(key, deque())
                while credits and credits[0]["ts"] < r["ts"] - window:
                    credits.popleft()
                for credit in credits:
                    if credit["ts"] > r["ts"] + window or remaining <= 0:
                        break
                    matched = min(remaining, credit["remaining"])
                    credit["remaining"] -= matched
                    remaining -= matched
                    if index == 0:
                        by_id += matched
                    else:
                        by_price += matched
                while credits and credits[0]["remaining"] <= 0:
                    credits.popleft()
            level["exec_id"] += by_id
            level["exec_price"] += by_price
            level["withdrawal_est"] += remaining
            if r["near"]:
                level["near_withdrawal_est"] += remaining
            if r["ts"] - bar.start < window or bar.end - r["ts"] <= window:
                boundary_qty += remaining
        else:
            remaining = 0  # Snapshot reconstruction / known price relocation, not cancellation evidence.
        store.write("attribution", {
            "alias": alias, "bar_start_utc": utc(bar.start), "event_utc": utc(r["ts"]),
            "ingest_seq": r["seq"], "order_id_text": "id:" + r["oid"], "side": r["side"],
            "price_tick": r["price"], "reduced_qty": r["qty"], "id_correlated_execution_qty": by_id,
            "price_time_correlated_execution_qty": by_price, "unexplained_withdrawal_est_qty": remaining,
            "known_reprice_out_qty": r["qty"] if r["moved"] else 0,
            "bootstrap": r["bootstrap"], "near_touch": r["near"],
            "observed_order_age_ms": r["age_ms"], "age_is_lower_bound": r["age_lower_bound"],
            "match_policy": "same_bar_id_side_price_time_quantity; optional missing_id_price_time",
        }, bar.start)

    # Refills consume already-observed trade-volume credits once. An add is
    # never called a refill solely because it shares a price with a trade.
    credits = defaultdict(deque)
    ti = 0
    refill_window = int(cfg.refill_window_ms * 1_000_000)
    for a in bar.adds:
        while ti < len(bar.trades) and (bar.trades[ti]["ts"], bar.trades[ti]["seq"]) <= (a["ts"], a["seq"]):
            t = bar.trades[ti]
            credits[(t["side"], t["price"])].append({
                "ts": t["ts"], "qty": t["qty"], "oid": t["passive_id"], "seq": t["seq"]})
            ti += 1
        if a["bootstrap"] or a["moved"]:
            continue
        cs = credits[(a["side"], a["price"])]
        while cs and cs[0]["ts"] < a["ts"] - refill_window:
            cs.popleft()
        left, refill, same = a["qty"], 0, 0
        for credit in cs:
            q = min(left, credit["qty"])
            if q:
                left -= q
                credit["qty"] -= q
                refill += q
                if credit["oid"] and credit["oid"] == a["oid"]:
                    same += q
            if not left:
                break
        while cs and cs[0]["qty"] == 0:
            cs.popleft()
        level = bar.levels[(a["side"], a["price"])]
        level["refill"] += refill
        level["same_id_refill"] += same
        if refill:
            store.write("replenishment", {
                "alias": alias, "bar_start_utc": utc(bar.start), "event_utc": utc(a["ts"]),
                "ingest_seq": a["seq"], "order_id_text": "id:" + a["oid"], "side": a["side"],
                "price_tick": a["price"], "added_qty": a["qty"], "execution_following_refill_qty": refill,
                "same_id_refill_qty": same, "window_ms": cfg.refill_window_ms,
                "interpretation": "displayed replenishment evidence; not participant or iceberg identification",
            }, bar.start)
    return boundary_qty


def execution_batches(bar, cfg, store, alias):
    if bar.batch_tracker is not None:
        return bar.batch_tracker.finish()
    result = {"buy_sweep_count": 0, "sell_sweep_count": 0,
              "buy_max_batch_qty": 0, "sell_max_batch_qty": 0, "incomplete_batches": 0}
    active = None
    for t in bar.trades:
        # A skipped zero-size callback is NOT interpreted as a batch start/end.
        # Do not join positive-size prints across an unexplained observation.
        segment = t.get("batch_segment", 0)
        if active and segment != active["batch_segment"]:
            result["incomplete_batches"] += 1
            active = None
        if t["start"]:
            if active:
                result["incomplete_batches"] += 1
            active = {"start": t["ts"], "is_buy": t["is_buy"], "low": t["price"], "high": t["price"],
                      "qty": 0, "prints": 0, "aggressor_id": t["aggressor_id"], "batch_segment": segment}
        if active:
            if t["is_buy"] != active["is_buy"]:
                result["incomplete_batches"] += 1
                active = None
                continue
            active["qty"] += t["qty"]
            active["prints"] += 1
            active["low"] = min(active["low"], t["price"])
            active["high"] = max(active["high"], t["price"])
            if t["end"]:
                label = "buy" if active["is_buy"] else "sell"
                span = active["high"] - active["low"]
                result[label + "_sweep_count"] += int(span >= 1 and active["prints"] >= 2)
                result[label + "_max_batch_qty"] = max(result[label + "_max_batch_qty"], active["qty"])
                store.write("execution_batches", {
                    "alias": alias, "bar_start_utc": utc(bar.start), "start_utc": utc(active["start"]),
                    "end_utc": utc(t["ts"]), "aggressor": label, "aggressor_id_text": "id:" + active["aggressor_id"],
                    "quantity": active["qty"], "prints": active["prints"], "price_span_ticks": span,
                    "complete_flagged_batch": True,
                }, bar.start)
                active = None
    if active:
        result["incomplete_batches"] += 1
    return result


class FlowTracker:
    """Same-bar correlation spread across events, instead of a close-time burst.

    Reductions mature ONLY after their full forward match window. At an exact
    deadline all equal-timestamp trades are admitted before matching. At close,
    only the already observed trades in that candle are eligible. This preserves
    the original batch algorithm, including removal-before-trade delivery.
    """
    def __init__(self, bar, cfg, store, alias):
        self.bar, self.cfg, self.store, self.alias = bar, cfg, store, alias
        self.bar_utc = utc(bar.start)
        self.window = int(cfg.match_window_ms * 1_000_000)
        self.refill_window = int(cfg.refill_window_ms * 1_000_000)
        self.groups, self.refills = defaultdict(deque), defaultdict(deque)
        self.pending = deque()
        self.boundary_qty = 0
        self.finished = False

    def trade(self, t):
        self.groups[(t['side'], t['price'], t['passive_id'])].append(
            {'ts': t['ts'], 'remaining': t['qty'], 'seq': t['seq']})
        self.refills[(t['side'], t['price'])].append(
            {'ts': t['ts'], 'qty': t['qty'], 'oid': t['passive_id']})

    def reduction(self, r):
        self.pending.append(r)

    def advance(self, ts):
        while self.pending and self.pending[0]['ts'] + self.window < ts:
            self._match(self.pending.popleft())

    def _match(self, r):
        level = self.bar.levels[(r['side'], r['price'])]
        remaining, by_id, by_price = r['qty'], 0, 0
        if not r['bootstrap'] and not r['moved']:
            keys = [(r['side'], r['price'], r['oid'])]
            if self.cfg.allow_price_time_matching:
                keys.append((r['side'], r['price'], ''))
            for index, key in enumerate(keys):
                credits = self.groups.get(key)
                if not credits:
                    continue
                while credits and credits[0]['ts'] < r['ts'] - self.window:
                    credits.popleft()
                for credit in credits:
                    if credit['ts'] > r['ts'] + self.window or remaining <= 0:
                        break
                    matched = min(remaining, credit['remaining'])
                    credit['remaining'] -= matched
                    remaining -= matched
                    if index == 0:
                        by_id += matched
                    else:
                        by_price += matched
                while credits and credits[0]['remaining'] <= 0:
                    credits.popleft()
                if not credits:
                    self.groups.pop(key, None)
            level['exec_id'] += by_id
            level['exec_price'] += by_price
            level['withdrawal_est'] += remaining
            if r['near']:
                level['near_withdrawal_est'] += remaining
            if r['ts'] - self.bar.start < self.window or self.bar.end - r['ts'] <= self.window:
                self.boundary_qty += remaining
        else:
            remaining = 0
        self.store.write('attribution', {
            'alias': self.alias, 'bar_start_utc': self.bar_utc, 'event_utc': utc(r['ts']),
            'ingest_seq': r['seq'], 'order_id_text': 'id:' + r['oid'], 'side': r['side'],
            'price_tick': r['price'], 'reduced_qty': r['qty'], 'id_correlated_execution_qty': by_id,
            'price_time_correlated_execution_qty': by_price, 'unexplained_withdrawal_est_qty': remaining,
            'known_reprice_out_qty': r['qty'] if r['moved'] else 0,
            'bootstrap': r['bootstrap'], 'near_touch': r['near'],
            'observed_order_age_ms': r['age_ms'], 'age_is_lower_bound': r['age_lower_bound'],
            'match_policy': 'same_bar_id_side_price_time_quantity; optional missing_id_price_time',
        }, self.bar.start)

    def add(self, a):
        if a['bootstrap'] or a['moved']:
            return
        cs = self.refills.get((a['side'], a['price']))
        if not cs:
            return
        while cs and cs[0]['ts'] < a['ts'] - self.refill_window:
            cs.popleft()
        left, refill, same = a['qty'], 0, 0
        while cs and left:
            credit = cs[0]
            q = min(left, credit['qty'])
            left -= q
            credit['qty'] -= q
            refill += q
            if credit['oid'] and credit['oid'] == a['oid']:
                same += q
            if credit['qty'] <= 0:
                cs.popleft()
        if not cs:
            self.refills.pop((a['side'], a['price']), None)
        level = self.bar.levels[(a['side'], a['price'])]
        level['refill'] += refill
        level['same_id_refill'] += same
        if refill:
            self.store.write('replenishment', {
                'alias': self.alias, 'bar_start_utc': self.bar_utc, 'event_utc': utc(a['ts']),
                'ingest_seq': a['seq'], 'order_id_text': 'id:' + a['oid'], 'side': a['side'],
                'price_tick': a['price'], 'added_qty': a['qty'], 'execution_following_refill_qty': refill,
                'same_id_refill_qty': same, 'window_ms': self.cfg.refill_window_ms,
                'interpretation': 'displayed replenishment evidence; not participant or iceberg identification',
            }, self.bar.start)

    def finish(self):
        if not self.finished:
            while self.pending:
                self._match(self.pending.popleft())
            self.finished = True
            self.groups.clear()
            self.refills.clear()
        return self.boundary_qty


class BatchTracker:
    """Accumulate completed execution batches on arrival, preserving zero fences."""
    def __init__(self, bar, store, alias):
        self.bar, self.store, self.alias = bar, store, alias
        self.bar_utc = utc(bar.start)
        self.result = {'buy_sweep_count': 0, 'sell_sweep_count': 0,
                       'buy_max_batch_qty': 0, 'sell_max_batch_qty': 0, 'incomplete_batches': 0}
        self.active = None
        self.finished = False

    def trade(self, t):
        a = self.active
        segment = t.get('batch_segment', 0)
        if a and segment != a['batch_segment']:
            self.result['incomplete_batches'] += 1
            a = None
        if t['start']:
            if a:
                self.result['incomplete_batches'] += 1
            a = {'start': t['ts'], 'is_buy': t['is_buy'], 'low': t['price'], 'high': t['price'],
                 'qty': 0, 'prints': 0, 'aggressor_id': t['aggressor_id'], 'batch_segment': segment}
        if a:
            if t['is_buy'] != a['is_buy']:
                self.result['incomplete_batches'] += 1
                self.active = None
                return
            a['qty'] += t['qty']
            a['prints'] += 1
            a['low'], a['high'] = min(a['low'], t['price']), max(a['high'], t['price'])
            if t['end']:
                label = 'buy' if a['is_buy'] else 'sell'
                span = a['high'] - a['low']
                self.result[label + '_sweep_count'] += int(span >= 1 and a['prints'] >= 2)
                self.result[label + '_max_batch_qty'] = max(self.result[label + '_max_batch_qty'], a['qty'])
                self.store.write('execution_batches', {
                    'alias': self.alias, 'bar_start_utc': self.bar_utc, 'start_utc': utc(a['start']),
                    'end_utc': utc(t['ts']), 'aggressor': label, 'aggressor_id_text': 'id:' + a['aggressor_id'],
                    'quantity': a['qty'], 'prints': a['prints'], 'price_span_ticks': span,
                    'complete_flagged_batch': True,
                }, self.bar.start)
                a = None
        self.active = a

    def finish(self):
        if not self.finished:
            if self.active:
                self.result['incomplete_batches'] += 1
                self.active = None
            self.finished = True
        return dict(self.result)


def measure_bar(bar, book, cfg, store, alias, tick_size, timestamp_basis):
    boundary_qty = attribute_flows(bar, cfg, store, alias)
    o = bar.open_tick if bar.open_tick is not None else bar.prev_close
    h = bar.high_tick if bar.high_tick is not None else o
    l = bar.low_tick if bar.low_tick is not None else o
    c = bar.close_tick if bar.close_tick is not None else o
    buy = sum(t["qty"] for t in bar.trades if t["is_buy"])
    sell = sum(t["qty"] for t in bar.trades if not t["is_buy"])
    volume, delta = buy + sell, buy - sell
    net = (c - bar.prev_close) if c is not None and bar.prev_close is not None else ((c - o) if c is not None else 0)
    body, price_range = (c - o if c is not None else 0), (h - l if h is not None else 0)
    path, last = 0, bar.prev_close if bar.prev_close is not None else o
    for t in bar.trades:
        if last is not None:
            path += abs(t["price"] - last)
        last = t["price"]
    valid_ns = bar.quote["valid_ns"]
    coverage = valid_ns / MINUTE
    reasons = set(bar.flags)
    if bar.partial:
        reasons.add("PARTIAL_ATTACH_BAR")
    if bar.bootstrap:
        reasons.add("SNAPSHOT_GUARD")
    if not volume:
        reasons.add("NO_TRADES")
        if bar.counts["zero_size_trade"]:
            reasons.add("NO_POSITIVE_SIZE_TRADES")
    if coverage < cfg.min_quote_coverage:
        reasons.add("QUOTE_COVERAGE_LOW")
    end_bid, end_ask = book.best("bid"), book.best("ask")
    if end_bid is None or end_ask is None or end_bid >= end_ask:
        reasons.add("END_BOOK_INVALID")
    if (bar.quote["wide_spread_ns"] / MINUTE > cfg.max_wide_spread_fraction
            or (end_bid is not None and end_ask is not None and end_ask - end_bid > cfg.max_spread_ticks)):
        reasons.add("SPREAD_GUARD")
    if bar.max_lag_ms > cfg.max_processing_lag_ms:
        reasons.add("PROCESSING_LAG")
    if len(book.orders) < cfg.min_book_orders:
        reasons.add("BOOK_TOO_SMALL")
    m = {
        "alias": alias, "bar_start_utc": utc(bar.start), "bar_end_utc": utc(bar.end),
        "bar_start_ns_text": "ns:" + str(bar.start), "timestamp_basis": timestamp_basis,
        "data_valid": not reasons, "quality_flags": ";".join(sorted(reasons)),
        "snapshot_completion_verified": False, "exchange_sequence_available": False,
        "open_tick": o, "high_tick": h, "low_tick": l, "close_tick": c,
        "open": o * tick_size if o is not None else None, "high": h * tick_size if h is not None else None,
        "low": l * tick_size if l is not None else None, "close": c * tick_size if c is not None else None,
        "volume": volume, "buy_volume": buy, "sell_volume": sell, "delta": delta,
        "delta_ratio": ratio(delta, volume), "abs_delta": abs(delta), "abs_delta_ratio": ratio(abs(delta), volume),
        "trade_count": len(bar.trades), "otc_ignored_count": bar.counts["otc"],
        "zero_size_trade_count": bar.counts["zero_size_trade"],
        "onbook_trade_callback_count": len(bar.trades) + bar.counts["zero_size_trade"],
        "zero_size_trade_fraction": ratio(bar.counts["zero_size_trade"], len(bar.trades) + bar.counts["zero_size_trade"]),
        "zero_size_trade_policy": cfg.zero_size_trade_policy,
        "data_warnings": "ZERO_SIZE_TRADES_SKIPPED_UNVERIFIED" if bar.counts["zero_size_trade"] else "",
        "mbo_event_count": bar.counts["mbo"], "new_count": bar.counts["new"],
        "replace_count": bar.counts["replace"], "cancel_event_count": bar.counts["cancel"],
        "unique_known_aggressor_order_ids": len(set(t["aggressor_id"] for t in bar.trades if t["aggressor_id"])),
        "unique_known_passive_order_ids": len(set(t["passive_id"] for t in bar.trades if t["passive_id"])),
        "range_ticks": price_range, "body_ticks": body, "net_ticks": net, "abs_net_ticks": abs(net),
        "close_location": ratio(c - l, price_range, 0.5) if c is not None else 0.5,
        "price_path_ticks": path, "path_efficiency": ratio(abs(net), path),
        "buy_progress_ticks": max(net, 0), "sell_progress_ticks": max(-net, 0),
        "buy_impact": ratio(100.0 * max(net, 0), buy), "sell_impact": ratio(100.0 * max(-net, 0), sell),
        "quote_coverage": coverage, "min_quote_coverage_required": cfg.min_quote_coverage,
        "empty_book_fraction": bar.quote["empty_ns"] / MINUTE,
        "unobserved_quote_fraction": max(0, MINUTE - valid_ns - bar.quote["empty_ns"]
                                        - bar.quote["crossed_ns"] - bar.quote["stale_ns"]) / MINUTE,
        "crossed_or_locked_fraction": bar.quote["crossed_ns"] / MINUTE,
        "stale_quote_fraction": bar.quote["stale_ns"] / MINUTE,
        "spread_twa": ratio(bar.quote["spread_ns"], valid_ns), "spread_max_ticks": bar.quote["max_spread"],
        "wide_spread_fraction": bar.quote["wide_spread_ns"] / MINUTE,
        "bid_depth_twa": ratio(bar.quote["bid_depth_ns"], valid_ns),
        "ask_depth_twa": ratio(bar.quote["ask_depth_ns"], valid_ns),
        "end_bid_tick": book.best("bid"), "end_ask_tick": book.best("ask"),
        "end_order_count": len(book.orders), "max_processing_lag_ms": bar.max_lag_ms,
        "reference_up_tick": bar.reference_up, "reference_down_tick": bar.reference_down,
        "volume_above_reference": ratio(sum(t["qty"] for t in bar.trades if bar.reference_up is not None and t["price"] > bar.reference_up), volume),
        "volume_below_reference": ratio(sum(t["qty"] for t in bar.trades if bar.reference_down is not None and t["price"] < bar.reference_down), volume),
        "time_above_reference": ratio(bar.quote["above_ns"], valid_ns),
        "time_below_reference": ratio(bar.quote["below_ns"], valid_ns),
        "boundary_unexplained_reduction_qty": boundary_qty,
    }
    for side in SIDES:
        for key in FLOW_KEYS:
            m[side + "_" + key] = sum(level[key] for (s, p), level in bar.levels.items() if s == side)
        executed = sell if side == "bid" else buy
        m[side + "_refill_ratio"] = ratio(m[side + "_refill"], executed)
        m[side + "_turnover"] = ratio(executed, m[side + "_depth_twa"])
        candidates = [(p, v) for (s, p), v in bar.levels.items()
                      if s == side and v["refill"] > 0 and v["traded"] > 0
                      and h is not None and ((p <= l + max(1, price_range * cfg.absorption_range_fraction))
                                            if side == "bid" else (p >= h - max(1, price_range * cfg.absorption_range_fraction)))]
        zone = max(candidates, key=lambda item: min(item[1]["refill"], item[1]["traded"])) if candidates else None
        m[side + "_zone_tick"] = zone[0] if zone else None
        m[side + "_zone_traded_qty"] = zone[1]["traded"] if zone else 0
        m[side + "_zone_refill_qty"] = zone[1]["refill"] if zone else 0
    id_qty = sum(t["qty"] for t in bar.trades if t["passive_id"])
    matched = sum(m[s + "_exec_id"] for s in SIDES)
    removable = sum(m[s + "_reduced"] - m[s + "_reprice_out"] for s in SIDES)
    m["passive_id_coverage"] = ratio(id_qty, volume)
    m["id_execution_match_coverage"] = ratio(matched, volume)
    m["boundary_unexplained_reduction_share"] = ratio(boundary_qty, removable)
    m["withdrawal_signal_usable"] = (m["passive_id_coverage"] >= cfg.min_passive_id_coverage
        and m["id_execution_match_coverage"] >= cfg.min_execution_match_coverage
        and m["boundary_unexplained_reduction_share"] <= cfg.max_boundary_reduction_share)
    m.update(execution_batches(bar, cfg, store, alias))
    closing = book.snapshot()
    keys = set(bar.levels)
    if cfg.log_full_book_levels:
        keys |= set(bar.opening_levels) | set(closing)
    for side, price in sorted(keys):
        before, after = bar.opening_levels.get((side, price), (0, 0)), closing.get((side, price), (0, 0))
        level = bar.levels[(side, price)]
        residual = after[0] - (before[0] + level["added"] + level["snapshot_added"]
                               - level["reduced"] - level["snapshot_reduced"])
        if residual:
            raise BookError("Per-level quantity conservation failed")
        row = {"alias": alias, "bar_start_utc": utc(bar.start), "side": side, "price_tick": price,
               "price": price * tick_size, "opening_qty": before[0], "closing_qty": after[0],
               "opening_order_count": before[1], "closing_order_count": after[1]}
        row.update({k: level[k] for k in FLOW_KEYS})
        row["book_conservation_residual"] = residual
        store.write("levels", row, bar.start)
    return m


@dataclass
class Decision:
    state: str
    direction: int
    action: str
    reason: str
    trigger_tick: Optional[int] = None
    invalidation_tick: Optional[int] = None
    raw_candidate: str = "NEUTRAL"
    candidate_count: int = 0
    state_age: int = 0
    previous_state: str = "NEUTRAL"
    rules: dict = field(default_factory=dict)
    episode: dict = field(default_factory=dict)

    def display(self, tick_size=0.25):
        bias = "BULLISH" if self.direction > 0 else "BEARISH" if self.direction < 0 else "NEUTRAL"
        if self.trigger_tick is None:
            levels = "Wait for a confirmed setup; no active trigger"
        else:
            sign, inverse = (">=", "<=") if self.direction > 0 else ("<=", ">=")
            levels = "Trade %s %.2f / Trade %s %.2f; expires in 5 min" % (
                sign, self.trigger_tick * tick_size, inverse, self.invalidation_tick * tick_size)
        return {"STATE": self.state, "BIAS": bias, "ACTION": self.action,
                "REASON": self.reason, "TRIGGER / INVALIDATION": levels}


class StateMachine:
    """Transparent heuristic states, not estimated probabilities or identities."""
    def __init__(self, cfg):
        self.cfg = cfg
        self.state, self.direction, self.age = "NEUTRAL", 0, 0
        self.pending, self.pending_count = None, 0
        self.episode, self.absorption = None, None

    def references(self, history):
        valid = [m for m in list(history)[-self.cfg.context_bars:] if m["data_valid"]]
        up = max(m["high_tick"] for m in valid) if len(valid) == self.cfg.context_bars else None
        down = min(m["low_tick"] for m in valid) if len(valid) == self.cfg.context_bars else None
        if self.episode:
            if self.episode["direction"] > 0:
                up = self.episode["anchor"]
            else:
                down = self.episode["anchor"]
        return up, down

    def _safe(self, state, reason):
        previous = self.state
        self.state, self.direction, self.age = state, 0, 1
        self.pending, self.pending_count = None, 0
        self.episode, self.absorption = None, None
        return Decision(state, 0, "STAND ASIDE" if state == "DATA_QUALITY" else "WAIT", reason,
                        previous_state=previous, raw_candidate=state, state_age=1)

    def decide(self, m, b):
        if not m["data_valid"]:
            return self._safe("DATA_QUALITY", m["quality_flags"] or "Book integrity unavailable")
        if not b["baseline_ready"]:
            return self._safe("WARMUP", "Prior valid minutes %d/%d; collecting baseline" % (b["baseline_n"], self.cfg.min_baseline))
        previous_health = self.state if self.state in ("WARMUP", "DATA_QUALITY") else None
        if previous_health:
            self.state, self.direction, self.age = "NEUTRAL", 0, 0
            self.pending, self.pending_count = None, 0
        cfg = self.cfg
        p = lambda name: b.get(name + "_pct") if b.get(name + "_pct") is not None else 50.0
        high = lambda name: m.get(name, 0) > 0 and p(name) >= cfg.high_percentile
        median_range = max(1.0, b.get("range_ticks_q50") or 1.0)
        weak_limit = max(1.0, cfg.absorption_range_fraction * median_range)
        c, h, l = m["close_tick"], m["high_tick"], m["low_tick"]
        buy_effort, sell_effort = high("buy_volume"), high("sell_volume")
        buy_weak = m["buy_progress_ticks"] <= weak_limit and p("buy_volume") - p("buy_impact") >= cfg.effort_result_gap
        sell_weak = m["sell_progress_ticks"] <= weak_limit and p("sell_volume") - p("sell_impact") >= cfg.effort_result_gap
        ask_zone, bid_zone = m["ask_zone_tick"], m["bid_zone_tick"]
        ask_defended = ask_zone is not None and c <= ask_zone + 1 and h <= ask_zone + weak_limit
        bid_defended = bid_zone is not None and c >= bid_zone - 1 and l >= bid_zone - weak_limit
        ask_abs = buy_effort and buy_weak and m["delta"] > 0 and high("ask_refill") and ask_defended
        bid_abs = sell_effort and sell_weak and m["delta"] < 0 and high("bid_refill") and bid_defended
        up_accept = (m["reference_up_tick"] is not None and c > m["reference_up_tick"]
                     and m["volume_above_reference"] >= cfg.acceptance_fraction
                     and m["time_above_reference"] >= cfg.acceptance_fraction)
        dn_accept = (m["reference_down_tick"] is not None and c < m["reference_down_tick"]
                     and m["volume_below_reference"] >= cfg.acceptance_fraction
                     and m["time_below_reference"] >= cfg.acceptance_fraction)
        up_cont = (m["delta"] > 0 and m["net_ticks"] > 0 and m["close_location"] >= 0.7
                   and (buy_effort or high("abs_delta")) and high("buy_progress_ticks")
                   and p("buy_impact") >= 50 and not ask_abs)
        dn_cont = (m["delta"] < 0 and m["net_ticks"] < 0 and m["close_location"] <= 0.3
                   and (sell_effort or high("abs_delta")) and high("sell_progress_ticks")
                   and p("sell_impact") >= 50 and not bid_abs)
        expansion = (high("bid_depth_twa") and high("ask_depth_twa")
                     and high("bid_near_added") and high("ask_near_added")
                     and p("range_ticks") <= 50)
        vacuum_up = (m["withdrawal_signal_usable"] and high("ask_near_withdrawal_est")
                     and high("buy_progress_ticks") and not buy_effort and m["net_ticks"] > 0)
        vacuum_dn = (m["withdrawal_signal_usable"] and high("bid_near_withdrawal_est")
                     and high("sell_progress_ticks") and not sell_effort and m["net_ticks"] < 0)
        rules = {"buy_effort_high": buy_effort, "sell_effort_high": sell_effort,
                 "buy_effort_result_divergence": buy_weak, "sell_effort_result_divergence": sell_weak,
                 "ask_absorption": ask_abs, "bid_absorption": bid_abs,
                 "up_acceptance_evidence": up_accept, "down_acceptance_evidence": dn_accept,
                 "up_continuation": up_cont, "down_continuation": dn_cont,
                 "liquidity_expansion": expansion, "withdrawal_drive_up": vacuum_up,
                 "withdrawal_drive_down": vacuum_dn, "withdrawal_attribution_usable": m["withdrawal_signal_usable"]}
        failure_dir, failure_anchor, failure_reason = 0, None, ""
        if self.episode:
            ep = self.episode
            ep["age"] += 1
            d, anchor = ep["direction"], ep["anchor"]
            if (c - anchor) * d < 0 and m["delta"] * d < 0:
                failure_dir, failure_anchor = -d, anchor
                failure_reason = "Prior %s breakout rejected; close crossed frozen anchor with opposing aggression" % ("up" if d > 0 else "down")
                self.episode = None
            else:
                held = up_accept if d > 0 else dn_accept
                ep["holds"] = ep["holds"] + 1 if held else 0
                if ep["age"] > cfg.episode_ttl_bars:
                    self.episode = None
        if self.absorption:
            a = self.absorption
            a["age"] += 1
            d = a["direction"]  # Expected reversal, opposite the absorbed aggression.
            broke = c > a["high"] if d > 0 else c < a["low"]
            invalid = c < a["zone"] - 1 if d > 0 else c > a["zone"] + 1
            if broke and m["delta"] * d > 0 and not failure_dir:
                failure_dir, failure_anchor = d, a["zone"]
                failure_reason = "Previously absorbed %s failed; opposite range break confirmed" % ("sellers" if d > 0 else "buyers")
                self.absorption = None
            elif invalid or a["age"] > cfg.context_bars:
                self.absorption = None
        if self.episode is None and not failure_dir:
            if up_accept and m["net_ticks"] > 0:
                self.episode = {"direction": 1, "anchor": m["reference_up_tick"], "holds": 1, "age": 1}
            elif dn_accept and m["net_ticks"] < 0:
                self.episode = {"direction": -1, "anchor": m["reference_down_tick"], "holds": 1, "age": 1}
        accepted = bool(self.episode and self.episode["holds"] >= cfg.confirmation_bars)
        rules["structurally_accepted"] = accepted
        rules["confirmed_failure"] = bool(failure_dir)
        candidate, direction, anchor = "NEUTRAL", 0, None
        reason = "No aligned effort, liquidity and price-response evidence"
        if failure_dir:
            candidate, direction, anchor, reason = "FAILURE", failure_dir, failure_anchor, failure_reason
        elif ask_abs != bid_abs:
            direction = -1 if ask_abs else 1
            candidate, anchor = "ABSORPTION", ask_zone if ask_abs else bid_zone
            side, passive = ("buy", "ask") if ask_abs else ("sell", "bid")
            reason = "%s effort P%.0f; impact P%.0f; %s refill P%.0f; price held" % (
                side.capitalize(), p(side + "_volume"), p(side + "_impact"), passive, p(passive + "_refill"))
            self.absorption = {"direction": direction, "zone": anchor, "high": h, "low": l, "age": 0}
        elif accepted:
            ep = self.episode
            direction, anchor = ep["direction"], ep["anchor"]
            # Once accepted, efficient additional directional response is continuation.
            aligned = up_cont if direction > 0 else dn_cont
            candidate = "CONTINUATION" if self.state in ("ACCEPTANCE", "CONTINUATION") and self.direction == direction and aligned else "ACCEPTANCE"
            reason = "%d closes hold frozen local range; volume/time acceptance confirmed" % ep["holds"]
        elif up_cont != dn_cont:
            candidate, direction = "CONTINUATION", 1 if up_cont else -1
            side = "buy" if direction > 0 else "sell"
            reason = "%s effort P%.0f; progress P%.0f; impact P%.0f; close supports move" % (
                side.capitalize(), p(side + "_volume"), p(side + "_progress_ticks"), p(side + "_impact"))
        elif vacuum_up != vacuum_dn:
            candidate, direction = "WITHDRAWAL_DRIVE", 1 if vacuum_up else -1
            reason = "%s near-touch withdrawal estimate elevated; price moved on modest aggression" % ("Ask" if direction > 0 else "Bid")
        elif expansion:
            candidate = "LIQUIDITY_EXPANSION"
            reason = "Both sides added elevated near-touch depth; price range remained contained"
        previous = previous_health or self.state
        key = (candidate, direction)
        if self.pending == key:
            self.pending_count += 1
        else:
            self.pending, self.pending_count = key, 1
        structural = candidate in ("FAILURE", "ACCEPTANCE")
        confirmed = key == (self.state, self.direction) or structural or self.pending_count >= cfg.confirmation_bars
        if confirmed:
            self.age = self.age + 1 if key == (self.state, self.direction) else 1
            self.state, self.direction = candidate, direction
        else:
            self.age += 1
        if not confirmed:
            return Decision(self.state, self.direction, "WAIT", "%s candidate %d/%d; await confirmation" % (
                candidate, self.pending_count, cfg.confirmation_bars), raw_candidate=candidate,
                candidate_count=self.pending_count, state_age=self.age, previous_state=previous,
                rules=rules, episode=dict(self.episode or {}))
        trigger, invalidation = None, None
        action = "WAIT"
        if direction:
            trigger = h + cfg.trigger_buffer_ticks if direction > 0 else l - cfg.trigger_buffer_ticks
            protection = anchor if anchor is not None else (l if direction > 0 else h)
            invalidation = protection - cfg.invalidation_buffer_ticks if direction > 0 else protection + cfg.invalidation_buffer_ticks
            action = "WATCH LONG" if direction > 0 else "WATCH SHORT"
            if (trigger - invalidation) * direction <= 0:
                trigger, invalidation, action = None, None, "WAIT"
        return Decision(self.state, self.direction, action, reason, trigger, invalidation, candidate,
                        self.pending_count, self.age, previous, rules, dict(self.episode or {}))


@dataclass
class PendingOutcome:
    signal_id: str
    signal_end: int
    available: int
    close: Optional[int]
    direction: int
    state: str
    action: str
    trigger: Optional[int]
    invalidation: Optional[int]
    signal_data_valid: bool
    bars: int = 0
    valid: bool = True
    high: Optional[int] = None
    low: Optional[int] = None
    triggered_ns: Optional[int] = None
    trigger_price: Optional[int] = None
    invalidated_ns: Optional[int] = None
    invalidated_before_trigger: bool = False
    post_trigger_high: Optional[int] = None
    post_trigger_low: Optional[int] = None
    emitted: set = field(default_factory=set)


class OutcomeTracker:
    HORIZONS = (1, 3, 5)

    def __init__(self, cfg, store, alias):
        self.cfg, self.store, self.alias = cfg, store, alias
        self.pending = []

    def add(self, signal_id, end, available, m, d):
        self.pending.append(PendingOutcome(signal_id, end, available, m["close_tick"], d.direction,
            d.state, d.action, d.trigger_tick, d.invalidation_tick, m["data_valid"]))

    def trade(self, ts, price):
        for p in self.pending:
            if ts <= p.available or p.direction == 0 or p.trigger is None:
                continue
            invalid = (price - p.invalidation) * p.direction <= 0
            if p.triggered_ns is None and not p.invalidated_before_trigger:
                if invalid:
                    p.invalidated_ns, p.invalidated_before_trigger = ts, True
                elif (price - p.trigger) * p.direction >= 0:
                    p.triggered_ns, p.trigger_price = ts, price
            if p.triggered_ns is not None:
                p.post_trigger_high = price if p.post_trigger_high is None else max(p.post_trigger_high, price)
                p.post_trigger_low = price if p.post_trigger_low is None else min(p.post_trigger_low, price)
                if invalid and p.invalidated_ns is None:
                    p.invalidated_ns = ts

    def _row(self, p, horizon, endpoint, status):
        complete = status == "COMPLETE"
        raw = endpoint - p.close if complete and endpoint is not None and p.close is not None else None
        signed = raw * p.direction if raw is not None and p.direction else None
        mfe = mae = trigger_return = trigger_mfe = trigger_mae = None
        if complete and p.close is not None and p.high is not None and p.direction:
            mfe = max(0, p.high - p.close) if p.direction > 0 else max(0, p.close - p.low)
            mae = max(0, p.close - p.low) if p.direction > 0 else max(0, p.high - p.close)
        if complete and p.trigger_price is not None and endpoint is not None:
            trigger_return = (endpoint - p.trigger_price) * p.direction
            trigger_mfe = max(0, p.post_trigger_high - p.trigger_price) if p.direction > 0 else max(0, p.trigger_price - p.post_trigger_low)
            trigger_mae = max(0, p.trigger_price - p.post_trigger_low) if p.direction > 0 else max(0, p.post_trigger_high - p.trigger_price)
        return {
            "alias": self.alias, "signal_id": p.signal_id, "signal_end_utc": utc(p.signal_end),
            "decision_available_utc": utc(p.available), "horizon_minutes": horizon,
            "scheduled_endpoint_utc": utc(p.signal_end + horizon * MINUTE), "status": status,
            "state": p.state, "direction": p.direction, "action": p.action,
            "signal_data_valid": p.signal_data_valid, "future_data_valid": p.valid and complete,
            "reference_close_tick": p.close, "endpoint_close_tick": endpoint if complete else None,
            "forward_return_ticks": raw, "signed_forward_return_ticks": signed,
            "close_reference_mfe_ticks": mfe, "close_reference_mae_ticks": mae,
            "trigger_tick": p.trigger, "invalidation_tick": p.invalidation,
            "triggered_utc": utc(p.triggered_ns) if p.triggered_ns else "",
            "first_trade_through_trigger_tick": p.trigger_price,
            "invalidated_utc": utc(p.invalidated_ns) if p.invalidated_ns else "",
            "invalidated_before_trigger": p.invalidated_before_trigger,
            "trigger_reference_markout_ticks": trigger_return,
            "assumed_round_trip_cost_ticks": self.cfg.assumed_round_trip_cost_ticks,
            "illustrative_cost_adjusted_markout_ticks": trigger_return - self.cfg.assumed_round_trip_cost_ticks if trigger_return is not None else None,
            "trigger_reference_mfe_ticks": trigger_mfe, "trigger_reference_mae_ticks": trigger_mae,
            "evaluation_basis": "diagnostic markout; trade-through is not a fill; no stop-exit PnL",
        }

    def bar(self, m, end):
        remaining = []
        for p in self.pending:
            if end <= p.signal_end:
                remaining.append(p)
                continue
            expected = p.signal_end + (p.bars + 1) * MINUTE
            p.valid = p.valid and m["data_valid"] and end == expected
            p.bars += 1
            if m["high_tick"] is not None:
                p.high = m["high_tick"] if p.high is None else max(p.high, m["high_tick"])
                p.low = m["low_tick"] if p.low is None else min(p.low, m["low_tick"])
            elapsed = (end - p.signal_end) // MINUTE
            if elapsed in self.HORIZONS:
                self.store.write("outcomes", self._row(p, elapsed, m["close_tick"], "COMPLETE"), end)
                p.emitted.add(elapsed)
            if elapsed < max(self.HORIZONS):
                remaining.append(p)
        self.pending = remaining

    def censor(self, ts):
        for p in self.pending:
            for h in self.HORIZONS:
                if h not in p.emitted:
                    self.store.write("outcomes", self._row(p, h, None, "CENSORED"), ts)
        self.pending = []


class Engine:
    def __init__(self, alias, start_ns, cfg=None, store=None, emit=None, tick_size=0.25,
                 timestamp_basis="LOCAL_ARRIVAL", now_ns=None, safety_check=None, metadata=None, diagnostics=None):
        self.cfg = (cfg or Config()).validate()
        if not math.isclose(tick_size, 0.25, abs_tol=1e-12):
            raise ValueError("This build is for outright ES with 0.25-point ticks")
        self.alias, self.tick_size, self.timestamp_basis = alias, tick_size, timestamp_basis
        self.metadata = dict(metadata or {})
        self.store, self.emit = store or MemoryStore(), emit
        self.now_ns, self.safety_check = now_ns, safety_check
        self.runtime_diagnostics = diagnostics
        self.last_metrics = None
        self.book, self.benchmarks = OrderBook(), Benchmarks(self.cfg)
        self.fsm = StateMachine(self.cfg)
        self.outcomes = OutcomeTracker(self.cfg, self.store, alias)
        self.start_ns, self.last_ns, self.last_seq = start_ns, start_ns, 0
        self.bootstrap_until = start_ns + int(self.cfg.bootstrap_seconds * SECOND)
        self.last_mbo_ns = None
        self.last_positive_trade_ns = None
        self.last_zero_size_trade_ns = None
        self.positive_trade_count_total = 0
        self.zero_size_trade_count_total = 0
        self.last_heartbeat_ns = 0
        self.fault_reason, self.closed, self.health_guarded = "", False, False
        self.previous_close = None
        self.bar = self._new_bar(start_ns // MINUTE * MINUTE, partial=start_ns % MINUTE != 0)
        self.last_decision = None
        start = Event(start_ns, 0, "START", {"alias": alias, "config": asdict(self.cfg),
            "tick_size": tick_size, "timestamp_basis": timestamp_basis, "metadata": metadata or {}})
        self._raw(start, {})
        self._publish(Decision("WARMUP", 0, "WAIT", "Reconstructing snapshot; waiting for full prior-minute baseline"), start_ns, "")

    def _new_bar(self, start, partial=False):
        upper, lower = self.fsm.references(self.benchmarks.history)
        bar = Bar(start, self.book.snapshot(), self.previous_close, upper, lower,
                  partial=partial, bootstrap=start < self.bootstrap_until)
        bar.flow_tracker = FlowTracker(bar, self.cfg, self.store, self.alias)
        bar.batch_tracker = BatchTracker(bar, self.store, self.alias)
        return bar

    def _publish(self, decision, available, signal_id):
        display = decision.display(self.tick_size)
        self.store.latest({"display": display, "updated_utc": utc(available),
                           "updated_ns_text": "ns:" + str(available), "signal_id": signal_id,
                           "timestamp_basis": self.timestamp_basis})
        if self.emit:
            self.emit(display)
        self.last_decision = decision

    def fault(self, reason, ts, sticky=True):
        if sticky and self.fault_reason:
            return
        if sticky:
            self.fault_reason = reason
        self.bar.flags.add("BOOK_OR_STREAM_FAULT")
        self.store.write("quality", {"alias": self.alias, "utc": utc(ts), "code": "FAULT",
            "detail": reason, "sticky": sticky, "action": "Disable/re-enable add-on for a fresh snapshot"}, ts)
        self._publish(Decision("DATA_QUALITY", 0, "STAND ASIDE", reason[:160]), ts, "")
        self.store.flush()

    def _trade_health(self):
        return {
            "positive_trade_count_total": self.positive_trade_count_total,
            "zero_size_trade_count_total": self.zero_size_trade_count_total,
            "last_positive_trade_utc": utc(self.last_positive_trade_ns) if self.last_positive_trade_ns is not None else "",
            "last_zero_size_trade_utc": utc(self.last_zero_size_trade_ns) if self.last_zero_size_trade_ns is not None else "",
            "zero_size_trade_policy": self.cfg.zero_size_trade_policy,
        }

    def diagnostics(self, now):
        bid, ask = self.book.best("bid"), self.book.best("ask")
        elapsed = max(1, min(MINUTE, self.last_ns - self.bar.start))
        status = ("EMPTY" if bid is None or ask is None else
                  "CROSSED_OR_LOCKED" if bid >= ask else "OK")
        last = self.last_metrics or {}
        runtime = self.runtime_diagnostics() if self.runtime_diagnostics else {}
        row = {
            "alias": self.alias, "utc": utc(now), "engine_version": VERSION,
            "book_status": status, "best_bid_tick": bid, "best_ask_tick": ask,
            "book_orders": len(self.book.orders), "bar_start_utc": utc(self.bar.start),
            "partial_quote_coverage": self.bar.quote["valid_ns"] / elapsed,
            "partial_empty_fraction": self.bar.quote["empty_ns"] / elapsed,
            "partial_crossed_fraction": self.bar.quote["crossed_ns"] / elapsed,
            "partial_stale_fraction": self.bar.quote["stale_ns"] / elapsed,
            "min_quote_coverage_required": self.cfg.min_quote_coverage,
            "last_completed_bar_end_utc": last.get("bar_end_utc", ""),
            "last_completed_quote_coverage": last.get("quote_coverage"),
            "last_completed_quality_flags": last.get("quality_flags", ""),
            "last_completed_empty_fraction": last.get("empty_book_fraction"),
            "last_completed_crossed_fraction": last.get("crossed_or_locked_fraction"),
            "last_completed_stale_fraction": last.get("stale_quote_fraction"),
            "last_completed_data_valid": last.get("data_valid"),
            "mbo_age_ms": (now - self.last_mbo_ns) / 1e6 if self.last_mbo_ns is not None else None,
            "bar_max_processing_lag_ms": self.bar.max_lag_ms,
            "processed_mbo_this_bar": self.bar.counts["mbo"],
            "positive_trades_this_bar": len(self.bar.trades),
            "zero_size_callbacks_this_bar": self.bar.counts["zero_size_trade"],
            "input_queue_depth": runtime.get("input_queue_depth", 0),
            "input_queue_capacity": self.cfg.event_queue_capacity,
            "input_queue_highwater": runtime.get("input_queue_highwater", 0),
            "input_queue_oldest_age_ms": runtime.get("input_queue_oldest_age_ms", 0.0),
            "last_processing_lag_ms": runtime.get("last_processing_lag_ms", 0.0),
            "max_allowed_processing_lag_ms": self.cfg.max_processing_lag_ms,
            "input_events_per_second": runtime.get("input_events_per_second", 0.0),
            "received_events": runtime.get("received_events", self.last_seq),
            "processed_events": self.last_seq,
            "dropped_events": runtime.get("dropped_events", 0),
            "csv_async": runtime.get("csv_async", False),
            "csv_queue_rows": runtime.get("csv_queue_rows", 0),
            "csv_queue_capacity": self.cfg.csv_queue_capacity,
            "csv_queue_highwater": runtime.get("csv_queue_highwater", 0),
            "csv_rows_written": runtime.get("csv_rows_written", 0),
            "csv_writer_error": runtime.get("csv_writer_error", ""),
            "fault": self.fault_reason,
        }
        return row

    def heartbeat(self, now):
        if now - self.last_heartbeat_ns < SECOND:
            return
        self.last_heartbeat_ns = now
        stale = self.last_mbo_ns is None or now - self.last_mbo_ns > self.cfg.stale_quote_seconds * SECOND
        diagnostic = self.diagnostics(now)
        self.store.heartbeat({"updated_utc": utc(now), "updated_ns_text": "ns:" + str(now),
            "last_callback_utc": utc(self.last_ns), "last_mbo_utc": utc(self.last_mbo_ns) if self.last_mbo_ns else "",
            "fault": self.fault_reason, "stale_mbo": stale, "closed": self.closed,
            "bootstrap_until_ns_text": "ns:" + str(self.bootstrap_until),
            **self._trade_health(), "diagnostics": diagnostic})
        if self.store.path is not None:
            # Compact support bundle, not a full multi-GB event capture.
            self.store._atomic("diagnostics.json", diagnostic)
        self.store.write("diagnostics", diagnostic, now)
        if stale and not self.health_guarded and now >= self.bootstrap_until:
            self.health_guarded = True
            self._publish(Decision("DATA_QUALITY", 0, "STAND ASIDE", "MBO stale or absent; wait for fresh data and a valid completed candle"), now, "")
        self.store.flush()

    def _integrate(self, begin, end):
        if end <= begin:
            return
        bid, ask = self.book.best("bid"), self.book.best("ask")
        if bid is None or ask is None:
            self.bar.quote["empty_ns"] += end - begin
            return
        if bid >= ask:
            self.bar.quote["crossed_ns"] += end - begin
            return
        expiry = self.last_mbo_ns + int(self.cfg.stale_quote_seconds * SECOND) if self.last_mbo_ns is not None else begin
        valid_end = max(begin, min(end, expiry))
        dt = valid_end - begin
        self.bar.quote["stale_ns"] += end - valid_end
        if not dt:
            return
        q = self.bar.quote
        q["valid_ns"] += dt
        spread = ask - bid
        q["spread_ns"] += spread * dt
        q["max_spread"] = max(q["max_spread"], spread)
        if spread > self.cfg.max_spread_ticks:
            q["wide_spread_ns"] += dt
        q["bid_depth_ns"] += self.book.near("bid", self.cfg.near_ticks) * dt
        q["ask_depth_ns"] += self.book.near("ask", self.cfg.near_ticks) * dt
        mid = (bid + ask) / 2
        if self.bar.reference_up is not None and mid > self.bar.reference_up:
            q["above_ns"] += dt
        if self.bar.reference_down is not None and mid < self.bar.reference_down:
            q["below_ns"] += dt

    def _finalize(self, event):
        bar = self.bar
        if self.safety_check:
            message = self.safety_check()
            if message and not self.fault_reason:
                event.data["_adapter_fault"] = message
                self.fault(message, event.ts_ns)
        if self.fault_reason:
            bar.flags.add("BOOK_OR_STREAM_FAULT")
        if self.last_mbo_ns is None or bar.end - self.last_mbo_ns > self.cfg.stale_quote_seconds * SECOND:
            bar.flags.add("STALE_AT_CLOSE")
        m = measure_bar(bar, self.book, self.cfg, self.store, self.alias, self.tick_size, self.timestamp_basis)
        recorded = event.data.setdefault("_decision_availability", {})
        key = str(bar.end)
        if key in recorded:
            available = int(recorded[key])
        else:
            available = max(event.ts_ns, bar.end, self.now_ns() if self.now_ns else event.ts_ns)
            recorded[key] = available
        delay_ms = (available - bar.end) / 1_000_000
        if delay_ms > self.cfg.max_processing_lag_ms:
            m["data_valid"] = False
            m["quality_flags"] = ";".join(filter(None, (m["quality_flags"], "DECISION_DELAY")))
        m["decision_available_utc"], m["decision_latency_ms"] = utc(available), delay_ms
        b = self.benchmarks.compute(m)  # Deliberately BEFORE append(m).
        d = self.fsm.decide(m, b)
        self.last_metrics = m
        if not m["data_valid"]:
            d.reason += "; quotes %.1f%%/%.1f%%; empty %.1f%%, crossed %.1f%%, stale %.1f%%; lag %.0fms" % (
                100 * m["quote_coverage"], 100 * self.cfg.min_quote_coverage,
                100 * m["empty_book_fraction"], 100 * m["crossed_or_locked_fraction"],
                100 * m["stale_quote_fraction"], m["max_processing_lag_ms"])
            self.store.write("quality", {"alias": self.alias, "utc": utc(bar.end),
                "code": "INVALID_CANDLE", "detail": d.reason,
                "sticky": False, "action": "Inspect diagnostics.json and diagnostics.csv; do not lower coverage to mask missing/crossed quotes"}, bar.start)
        if m["zero_size_trade_count"]:
            n = m["zero_size_trade_count"]
            d.reason += "; %d zero-size callbacks skipped" % n
            d.rules = dict(d.rules, zero_size_trade_policy=self.cfg.zero_size_trade_policy,
                           zero_size_trade_count=n, data_warnings=m["data_warnings"])
            self.store.write("quality", {"alias": self.alias, "utc": utc(bar.end),
                "code": "ZERO_SIZE_TRADE_SUMMARY",
                "detail": "%d zero-size callbacks excluded; %d positive-size trades; zero share %.6f; upstream meaning unverified" % (
                    n, m["trade_count"], m["zero_size_trade_fraction"]),
                "sticky": False, "action": "Review callback diagnostics and compare positive-size volume with the source"}, bar.start)
        signal_id = self.alias + ":" + str(bar.end)
        self.outcomes.bar(m, bar.end)  # Resolve old signals before registering this signal.
        self.store.write("minutes", dict(m, **b), bar.start)
        self.store.write("decisions", {
            "alias": self.alias, "signal_id": signal_id, "bar_start_utc": utc(bar.start),
            "bar_end_utc": utc(bar.end), "feature_cutoff_utc": utc(bar.end),
            "decision_available_utc": utc(available), "decision_latency_ms": delay_ms,
            "state": d.state, "direction": d.direction, "action": d.action, "reason": d.reason,
            "trigger_tick": d.trigger_tick, "invalidation_tick": d.invalidation_tick,
            "previous_state": d.previous_state, "raw_candidate": d.raw_candidate,
            "candidate_count": d.candidate_count, "state_age_bars": d.state_age,
            "baseline_n": b["baseline_n"], "data_valid": m["data_valid"], "quality_flags": m["quality_flags"],
            "data_warnings": m["data_warnings"], "zero_size_trade_count": m["zero_size_trade_count"],
            "rules_json": d.rules, "episode_json": d.episode,
            "strategy_version": VERSION, "timestamp_basis": self.timestamp_basis,
        }, bar.start)
        self.store.flush(self.cfg.fsync_at_bar_close)
        self._publish(d, available, signal_id)
        self.health_guarded = not m["data_valid"]
        self.outcomes.add(signal_id, bar.end, available, m, d)
        self.benchmarks.append(m)
        self.previous_close = m["close_tick"]

    def _advance(self, event):
        target = event.ts_ns
        while self.last_ns < target:
            stop = min(target, self.bar.end)
            self._integrate(self.last_ns, stop)
            self.last_ns = stop
            if stop == self.bar.end:
                self._finalize(event)
                # Break tracker->bar cycles promptly; otherwise large completed
                # candles survive until cyclic GC and create avoidable pauses.
                self.bar.flow_tracker = None
                self.bar.batch_tracker = None
                self.bar = self._new_bar(stop)
        self.last_ns = target

    def _mbo(self, e):
        d, bar = e.data, self.bar
        etype, oid = str(d["event_type"]), clean_id(d.get("order_id"))
        # Missing cancellation placeholders are allowed; NEW/REPLACE missing
        # fields are rejected by whole_mbo_level with field/type diagnostics.
        # Leave e.data unchanged so raw values survive CSV logging and replay.
        price, qty = d.get("price"), d.get("qty")
        boot = e.ts_ns < self.bootstrap_until
        old = self.book.orders.get(oid)
        near_old = self.book.is_near(old.side, old.price, self.cfg.near_ticks) if old else False
        pre_bid, pre_ask = self.book.best("bid"), self.book.best("ask")
        old, new, reduced, added, moved = self.book.apply(etype, oid, price, qty, e.ts_ns, boot)
        self.last_mbo_ns = e.ts_ns
        bar.counts["mbo"] += 1
        bar.counts["new" if etype.endswith("_NEW") else "replace" if etype == "REPLACE" else "cancel"] += 1
        if reduced:
            level = bar.levels[(old.side, old.price)]
            level["snapshot_reduced" if boot else "reduced"] += reduced
            if not boot:
                level["near_reduced"] += reduced if near_old else 0
                level["reprice_out"] += reduced if moved else 0
            bar.reductions.append({"ts": e.ts_ns, "seq": e.seq, "oid": oid, "side": old.side,
                "price": old.price, "qty": reduced, "near": near_old, "moved": moved, "bootstrap": boot,
                "age_ms": (e.ts_ns - old.born_ns) / 1_000_000, "age_lower_bound": old.born_in_bootstrap})
            bar.flow_tracker.reduction(bar.reductions[-1])
        if added:
            near_new = self.book.is_near(new.side, new.price, self.cfg.near_ticks)
            level = bar.levels[(new.side, new.price)]
            level["snapshot_added" if boot else "added"] += added
            if not boot:
                level["near_added"] += added if near_new else 0
                level["reprice_in"] += added if moved else 0
            bar.adds.append({"ts": e.ts_ns, "seq": e.seq, "oid": oid, "side": new.side,
                "price": new.price, "qty": added, "near": near_new, "moved": moved, "bootstrap": boot})
            bar.flow_tracker.add(bar.adds[-1])
        return {"order_id": oid, "event_type": etype, "side": new.side if new else old.side,
                "price": price, "qty": qty, "old_price": old.price if old else None,
                "old_qty": old.qty if old else None, "new_price": new.price if new else None,
                "new_qty": new.qty if new else None, "pre_bid": pre_bid, "pre_ask": pre_ask}

    def _zero_size_trade(self, e):
        """Audit a zero-size callback, without declaring it an execution/marker.

        Positive reported volume remains usable under the explicit AUDIT_SKIP
        policy. This does NOT recover missing/corrected volume upstream or
        establish that the feed is complete. Warnings and counts survive in the
        minute/decision audit. Only zero callbacks in a candle cannot make a
        valid candle. No book quantity, price, or trigger is changed here.
        """
        bar, d = self.bar, e.data
        bar.counts["zero_size_trade"] += 1
        self.zero_size_trade_count_total += 1
        self.last_zero_size_trade_ns = e.ts_ns
        first = bar.counts["zero_size_trade"] == 1
        if first:
            # Bound atomic file writes and quality warnings to one sample per
            # minute. Every callback (not just the sample) remains in events.csv.
            self.store.zero_trade({
                "engine_version": VERSION, "alias": self.alias,
                "event_utc": utc(e.ts_ns), "event_ns_text": "ns:" + str(e.ts_ns),
                "bar_start_utc": utc(bar.start), "ingest_seq": e.seq,
                "event_kind": e.kind, "timestamp_basis": self.timestamp_basis,
                "classification": "ZERO_SIZE_TRADE_SKIPPED",
                "policy": self.cfg.zero_size_trade_policy,
                "upstream_semantics_verified": False,
                "quantity_imputed": False, "batch_policy": "FENCE; do not infer start/end",
                "instrument": audit_json_value(self.metadata),
                "raw_fields": {k: {"value_repr": repr(v), "python_type": type(v).__name__}
                               for k, v in d.items()},
                "event": audit_json_value(asdict(e)),
            })
            self.store.write("quality", {"alias": self.alias, "utc": utc(e.ts_ns),
                "code": "ZERO_SIZE_TRADE_SKIPPED",
                "detail": "size_level=%r (%s); excluded from executions under AUDIT_SKIP; no volume or price inferred; batch grouping fenced" % (
                    d.get("qty"), type(d.get("qty")).__name__),
                "sticky": False, "action": "Continue positive-size trades; review zero_size_trade_event.json; see per-minute counts"}, e.ts_ns)
        return {"price": d.get("price"), "qty": d.get("qty"),
                "classification": "ZERO_SIZE_TRADE_SKIPPED", "_flush_audit": first}

    def _trade(self, e):
        d, bar = e.data, self.bar
        # Explicit OTC events are outside this on-book execution analysis.
        # Exclude them before enforcing the outright ES execution contract;
        # their untouched fields remain in events.csv, and they never update
        # volume, OHLC, order matching, batch boundaries, or trigger outcomes.
        if trade_flag(d.get("is_otc", False), "is_otc"):
            bar.counts["otc"] += 1
            return {"price": d.get("price"), "qty": d.get("qty"), "classification": "OTC_EXCLUDED"}
        raw_qty = d.get("qty")
        if (self.cfg.zero_size_trade_policy == "AUDIT_SKIP"
                and not isinstance(raw_qty, bool) and isinstance(raw_qty, (int, float))
                and raw_qty == 0):
            # A zero-size callback cannot supply a positive execution quantity.
            # Its price/side/flags are retained as diagnostics, not validated as
            # an execution or used to infer market movement. bool False is NOT 0.
            return self._zero_size_trade(e)
        # Keep e.data untouched for the callback audit. No int(qty) shortcut:
        # Fractional, negative and boolean sizes must not become trades.
        # STRICT also rejects zero as in v1.1.2.
        qty = whole_level(d.get("qty"), "TRADE", "size_level", 1, "whole ES contracts")
        price = whole_level(d.get("price"), "TRADE", "price_level", 1, "whole ES tick price")
        is_buy = trade_flag(d.get("is_buy"), "is_buy")
        start = trade_flag(d.get("start", False), "is_execution_start")
        end = trade_flag(d.get("end", False), "is_execution_end")
        side = "ask" if is_buy else "bid"
        t = {"ts": e.ts_ns, "seq": e.seq, "price": price, "qty": qty, "side": side, "is_buy": is_buy,
             "passive_id": clean_id(d.get("passive_id")), "aggressor_id": clean_id(d.get("aggressor_id")),
             "start": start, "end": end, "batch_segment": bar.counts["zero_size_trade"]}
        bar.trades.append(t)
        bar.flow_tracker.trade(t)
        bar.batch_tracker.trade(t)
        self.positive_trade_count_total += 1
        self.last_positive_trade_ns = e.ts_ns
        if bar.open_tick is None:
            bar.open_tick = bar.high_tick = bar.low_tick = price
        bar.close_tick = price
        bar.high_tick, bar.low_tick = max(bar.high_tick, price), min(bar.low_tick, price)
        level = bar.levels[(side, price)]
        level["traded"] += qty
        level["trade_count"] += 1
        self.outcomes.trade(e.ts_ns, price)
        # DO NOT mutate the book on trades: the MBO update already supplies
        # the displayed-size change, in either arrival order.
        return {"side": side, "price": price, "qty": qty, "order_id": t["passive_id"], "classification": "POSITIVE_SIZE_TRADE"}

    def _raw(self, event, observed):
        self.store.write("events", {
            "alias": self.alias, "event_utc": utc(event.ts_ns), "event_ns_text": "ns:" + str(event.ts_ns),
            "ingest_seq": event.seq, "kind": event.kind, "timestamp_basis": self.timestamp_basis,
            "processing_lag_ms": event.processing_lag_ms, "bootstrap_guard": event.ts_ns < self.bootstrap_until,
            "processing_classification": observed.get("classification", ""),
            "order_id_text": "id:" + observed.get("order_id", ""), "event_type": observed.get("event_type", ""),
            "side": observed.get("side", ""), "price_tick": observed.get("price"), "quantity": observed.get("qty"),
            "old_price_tick": observed.get("old_price"), "old_quantity": observed.get("old_qty"),
            "new_price_tick": observed.get("new_price"), "new_quantity": observed.get("new_qty"),
            "best_bid_before": observed.get("pre_bid"), "best_ask_before": observed.get("pre_ask"),
            "best_bid_after": self.book.best("bid"), "best_ask_after": self.book.best("ask"),
            # No recursive dataclass deepcopy for each market-data message.
            # The worker owns this event; its audit snapshot is immutable after
            # this call (including recorded decision-availability timestamps).
            "normalized_event_json": {"ts_ns": event.ts_ns, "seq": event.seq,
                "kind": event.kind, "data": dict(event.data),
                "processing_lag_ms": event.processing_lag_ms},
        }, event.ts_ns)

    def process(self, event):
        if self.closed:
            raise RuntimeError("Engine is closed")
        if event.data.get("_adapter_fault") and not self.fault_reason:
            self.fault(event.data["_adapter_fault"], max(event.ts_ns, self.last_ns))
        if event.seq != self.last_seq + 1:
            self.fault("Local ingest sequence gap/duplicate; reload for fresh snapshot", max(event.ts_ns, self.last_ns))
        if event.ts_ns < self.last_ns:
            self.fault("Clock/order regression; reload for fresh snapshot", self.last_ns)
            self._raw(event, {})
            self.last_seq = event.seq
            return
        self.last_seq = event.seq
        self._advance(event)  # Event at exact minute boundary belongs to the NEW candle.
        self.bar.max_lag_ms = max(self.bar.max_lag_ms, event.processing_lag_ms)
        self.bar.flow_tracker.advance(event.ts_ns)
        observed = {}
        rejected = False
        if not self.fault_reason:
            try:
                if event.kind == "MBO":
                    observed = self._mbo(event)
                elif event.kind == "TRADE":
                    observed = self._trade(event)
                elif event.kind == "FAULT":
                    self.fault(str(event.data.get("reason", "Adapter fault")), event.ts_ns)
                elif event.kind not in ("PULSE", "ACK", "STOP"):
                    raise BookError("Unsupported normalized event kind " + event.kind)
                if len(self.bar.trades) + len(self.bar.adds) + len(self.bar.reductions) > self.cfg.max_events_per_bar:
                    raise BookError("Per-candle memory guard exceeded; reload and tune capture capacity")
            except (BookError, ValueError, KeyError, TypeError) as exc:
                rejected = True
                self.store.rejected({
                    "engine_version": VERSION, "alias": self.alias,
                    "event_utc": utc(event.ts_ns), "event_ns_text": "ns:" + str(event.ts_ns),
                    "ingest_seq": event.seq, "event_kind": event.kind,
                    "timestamp_basis": self.timestamp_basis,
                    "reason": str(exc), "exception_type": type(exc).__name__,
                    "instrument": audit_json_value(self.metadata),
                    "raw_fields": {k: {"value_repr": repr(v), "python_type": type(v).__name__}
                                   for k, v in event.data.items()},
                    "event": audit_json_value(asdict(event)),
                    "policy": "Rejected; no guessed execution; disable/re-enable after correcting cause",
                })
                self.fault(str(exc), event.ts_ns)
        self._raw(event, observed)
        if rejected or observed.get("_flush_audit"):
            self.store.flush()  # Persist rejected/first-zero raw row and its diagnostic.
        if event.kind == "PULSE":
            self.heartbeat(event.ts_ns)
        if event.kind == "STOP":
            self.close(event.ts_ns)

    def close(self, ts=None):
        if self.closed:
            return
        ts = self.last_ns if ts is None else ts
        self.outcomes.censor(ts)
        self.store.write("quality", {"alias": self.alias, "utc": utc(ts), "code": "STOP",
            "detail": "Partial final candle not emitted; unmatched future horizons censored",
            "sticky": False, "action": "Restart requires fresh snapshot and warmup"}, ts)
        self.closed = True
        self._publish(Decision("STOPPED", 0, "STAND ASIDE", "Engine stopped; pending horizons censored; fresh snapshot required on restart"), ts, "")
        self.store.heartbeat({"updated_utc": utc(ts), "updated_ns_text": "ns:" + str(ts),
            "last_callback_utc": utc(self.last_ns), "last_mbo_utc": utc(self.last_mbo_ns) if self.last_mbo_ns else "",
            "fault": self.fault_reason, "stale_mbo": True, "closed": True, **self._trade_health()})
        self.store.flush(self.cfg.fsync_at_bar_close)
        self.store.close()


def print_display(display):
    print("\n".join(k + ": " + display[k] for k in ("STATE", "BIAS", "ACTION", "REASON", "TRIGGER / INVALIDATION")), flush=True)


# The GUI lives in this same file so Bookmap may copy the script to a temporary
# directory without losing a sibling live_panel.py dependency. Tk is imported
# ONLY in the child process. No market-data callback executes GUI work.
PANEL_FIELDS = ("STATE", "BIAS", "ACTION", "REASON", "TRIGGER / INVALIDATION")


def panel_safe_display(reason, state="DATA_QUALITY"):
    return dict(zip(PANEL_FIELDS, (state, "NEUTRAL", "WAIT" if state == "WARMUP" else "STAND ASIDE",
                                   reason, "No active trigger")))


def _panel_ns(value):
    return int(str(value).replace("ns:", ""))


def read_panel_display(folder, static=False, exact=False, now_ns=None):
    """Read a run atomically; never substitute another run in automatic mode."""
    folder = Path(folder).expanduser().resolve()
    direct = folder / "latest.json"
    if exact or direct.exists():
        filename = direct
    else:
        paths = list(folder.rglob("latest.json"))
        if not paths:
            return panel_safe_display("Waiting for engine output", "WARMUP"), None
        filename = max(paths, key=lambda p: p.stat().st_mtime_ns)
    if not filename.exists():
        return panel_safe_display("Waiting for engine output", "WARMUP"), None
    with filename.open(encoding="utf-8") as f:
        latest = json.load(f)
    display = latest["display"]
    if not isinstance(display, dict) or any(not isinstance(display.get(k), str) for k in PANEL_FIELDS):
        raise ValueError("Malformed five-field display")
    if static:
        return display, filename
    if filename.with_name("fatal_error.txt").exists():
        return panel_safe_display("Engine worker failed; inspect fatal_error.txt and restart"), filename
    if latest.get("timestamp_basis") != "LOCAL_ARRIVAL":
        return panel_safe_display("Offline/synthetic output is not a live signal"), filename
    with filename.with_name("health.json").open(encoding="utf-8") as f:
        health = json.load(f)
    if health.get("closed"):
        return panel_safe_display("Engine stopped; fresh snapshot required on restart", "STOPPED"), filename
    if health.get("fault"):
        return panel_safe_display(str(health["fault"])), filename
    diagnostic = health.get("diagnostics", {})
    if diagnostic.get("csv_writer_error") or diagnostic.get("dropped_events", 0):
        return panel_safe_display("Capture/audit integrity fault; inspect diagnostics.json and restart"), filename
    lag_limit = diagnostic.get("max_allowed_processing_lag_ms", 1000.0)
    if diagnostic.get("input_queue_oldest_age_ms", 0) > lag_limit:
        return panel_safe_display("Processing backlog: %.0fms, %d queued; no live action" % (
            diagnostic["input_queue_oldest_age_ms"], diagnostic.get("input_queue_depth", 0))), filename
    now = time.time_ns() if now_ns is None else now_ns
    health_age = now - _panel_ns(health["updated_ns_text"])
    decision_age = now - _panel_ns(latest["updated_ns_text"])
    if health_age < -SECOND or decision_age < -SECOND:
        return panel_safe_display("Clock moved backwards; restart before interpreting signals"), filename
    if health_age > 5 * SECOND:
        return panel_safe_display("Feed/worker heartbeat stale; no live action"), filename
    if decision_age > 75 * SECOND:
        return panel_safe_display("Completed-candle decision is stale"), filename
    # A fresh snapshot has not necessarily delivered MBO in its first few ms.
    # Only a WAIT/WARMUP may be shown in this bounded bootstrap interval.
    initial_wait = (display["STATE"] == "WARMUP" and display["ACTION"] == "WAIT"
                    and now < _panel_ns(health.get("bootstrap_until_ns_text", "ns:0")))
    if health.get("stale_mbo") and not initial_wait:
        return panel_safe_display("MBO feed stale or absent; no live action"), filename
    if diagnostic and display["STATE"] in ("WARMUP", "DATA_QUALITY"):
        display = dict(display)
        display["REASON"] += " | now: book %s, lag %.0fms, inQ %d, csvQ %d" % (
            diagnostic.get("book_status", "UNKNOWN"),
            diagnostic.get("input_queue_oldest_age_ms", 0),
            diagnostic.get("input_queue_depth", 0), diagnostic.get("csv_queue_rows", 0))
    return display, filename


def _panel_json(folder, name, value):
    """UI diagnostics have separate files; never touch the CSV worker's files."""
    path = Path(folder) / name
    temp = path.with_name(path.name + ".%d.tmp" % os.getpid())
    try:
        with temp.open("w", encoding="utf-8") as f:
            json.dump(value, f, indent=2, sort_keys=True)
        os.replace(str(temp), str(path))
    finally:
        try:
            temp.unlink()
        except OSError:
            pass


class FloatingPanelProcess:
    """One owned child per live runtime, with non-blocking start/stop requests.

    The parent keeps the child's stdin pipe open. EOF closes the GUI even after
    abrupt parent-process death. We neither send the Bookmap TCP port to the
    child nor import Bookmap in it. Closing the window does not stop capture.
    """
    def __init__(self, alias, cfg):
        self.alias, self.cfg = alias, cfg
        self.folder, self.process, self.thread = None, None, None
        self.lock = threading.Lock()
        self.stopped = threading.Event()

    def command(self):
        executable = os.environ.get("ES_ENGINE_PANEL_PYTHON") or sys.executable
        if not executable:
            raise RuntimeError("Set ES_ENGINE_PANEL_PYTHON to a Python executable with Tkinter")
        exe = Path(executable).expanduser()
        # python.exe + CREATE_NO_WINDOW retains usable redirected stdin on Windows.
        if exe.name.lower() == "pythonw.exe" and exe.with_name("python.exe").exists():
            exe = exe.with_name("python.exe")
        script = Path(__file__).resolve()
        if not script.is_file():
            raise RuntimeError("The running es_engine.py file must remain available to launch the panel")
        command = [str(exe), str(script), "--panel", str(self.folder), "--exact-run", "--owner-stdin",
                   "--alias", self.alias, "--geometry", self.cfg.panel_geometry,
                   "--refresh-ms", str(self.cfg.panel_refresh_ms)]
        if not self.cfg.panel_topmost:
            command.append("--no-topmost")
        return command

    def start(self, folder):
        override = os.environ.get("ES_ENGINE_AUTO_PANEL", "").strip().lower()
        enabled = self.cfg.auto_open_panel
        if override in ("0", "false", "no", "off"):
            enabled = False
        elif override in ("1", "true", "yes", "on"):
            enabled = True
        with self.lock:
            if not enabled or self.thread is not None or self.stopped.is_set():
                return
            self.folder = Path(folder).expanduser().resolve()
            self.thread = threading.Thread(target=self._supervise, name="ES-panel-" + self.alias, daemon=True)
            self.thread.start()

    def _record(self, state, detail=""):
        try:
            _panel_json(self.folder, "panel_process.json", {"status": state, "detail": detail,
                "updated_utc": utc(time.time_ns()), "alias": self.alias,
                "pid": self.process.pid if self.process is not None else None})
        except OSError:
            pass  # A GUI diagnostic failure must not interrupt market-data capture.

    def _error(self, detail):
        self._record("ERROR", detail)
        print("Floating panel unavailable: " + detail + "; see panel_errors.log in " + str(self.folder)
              + ". Capture and console output continue. Use Python with Tkinter; "
                "ES_ENGINE_PANEL_PYTHON can select its executable.", file=sys.stderr, flush=True)

    def _supervise(self):
        child = None
        try:
            if self.stopped.is_set():
                return
            command = self.command()
            kwargs = dict(stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, close_fds=True, shell=False)
            if os.name == "nt":
                kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW
            else:
                kwargs["start_new_session"] = True
            with (self.folder / "panel_errors.log").open("a", encoding="utf-8") as errors:
                child = subprocess.Popen(command, stderr=errors, **kwargs)
                self.process = child
                self._record("STARTING")
                ready = False
                started = time.monotonic()
                while child.poll() is None and not self.stopped.wait(0.1):
                    if not ready and (self.folder / "panel_status.json").exists():
                        with (self.folder / "panel_status.json").open(encoding="utf-8") as f:
                            ready = json.load(f).get("status") == "OPEN"
                        if ready:
                            self._record("OPEN")
                    if not ready and time.monotonic() - started > 15:
                        self._error("GUI did not report a visible window during startup")
                        break
                if child.poll() is not None:
                    if child.returncode:
                        self._error("GUI process exited with code " + str(child.returncode))
                    else:
                        self._record("CLOSED", "Window closed; reactivate the add-on to reopen")
                else:
                    # EOF is the graceful close request. The GUI never reads
                    # Bookmap's own stdin/stdout communication channel.
                    child.stdin.close()
                    try:
                        child.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        child.terminate()
                        try:
                            child.wait(timeout=1)
                        except subprocess.TimeoutExpired:
                            child.kill()
                            child.wait(timeout=1)
                    if self.stopped.is_set():
                        self._record("CLOSED", "Add-on deactivated")
        except Exception as exc:
            self._error(str(exc))
        finally:
            if child is not None:
                try:
                    if child.stdin is not None and not child.stdin.closed:
                        child.stdin.close()
                    if child.poll() is None:
                        child.terminate()
                        child.wait(timeout=1)
                except (OSError, subprocess.TimeoutExpired):
                    try:
                        child.kill()
                        child.wait(timeout=1)
                    except (OSError, subprocess.TimeoutExpired):
                        pass

    def stop(self, wait=False):
        self.stopped.set()
        if wait and self.thread is not None and self.thread is not threading.current_thread():
            self.thread.join(timeout=5)


def run_floating_panel(folder, static=False, exact=False, owner_stdin=False,
                       alias="", geometry="900x310+60+60", refresh_ms=500, topmost=True):
    """Tk runs in the GUI process's main thread, never in the feed worker."""
    import tkinter as tk
    from tkinter import ttk

    root = tk.Tk()
    root.title("ES Microstructure v" + VERSION + " | " + ("OFFLINE PREVIEW" if static else (alias or "LIVE")))
    root.geometry(geometry)
    root.minsize(650, 270)
    root.attributes("-topmost", topmost)
    frame = ttk.Frame(root, padding=16)
    frame.pack(fill="both", expand=True)
    values, labels = {}, []
    for row, name in enumerate(PANEL_FIELDS):
        ttk.Label(frame, text=name, font=("TkDefaultFont", 10, "bold")).grid(
            row=row, column=0, sticky="nw", padx=(0, 16), pady=7)
        variable = tk.StringVar(value="")
        values[name] = variable
        label = ttk.Label(frame, textvariable=variable, wraplength=590, justify="left",
                          font=("TkDefaultFont", 11, "bold" if name == "STATE" else "normal"))
        label.grid(row=row, column=1, sticky="nw", pady=7)
        labels.append(label)
    frame.columnconfigure(1, weight=1)
    owner_gone = threading.Event()
    closing = [False]
    folder = Path(folder).expanduser().resolve()

    def status(state, detail=""):
        if owner_stdin:
            try:
                _panel_json(folder, "panel_status.json", {"status": state, "detail": detail,
                    "updated_utc": utc(time.time_ns()), "pid": os.getpid(), "alias": alias,
                    "run_folder": str(folder), "geometry": root.geometry(), "topmost_requested": topmost})
            except OSError:
                pass

    def close(reason="Window closed by user"):
        if closing[0]:
            return
        closing[0] = True
        status("CLOSED", reason)
        root.destroy()

    def watch_owner():
        try:
            # Never touch Tk from this thread. The GUI observes the event.
            stream = getattr(sys.stdin, "buffer", sys.stdin)
            if stream is not None:
                while stream.read(1):
                    pass
        finally:
            owner_gone.set()

    def refresh():
        if owner_gone.is_set():
            close("Engine owner stopped or deactivated")
            return
        try:
            display, filename = read_panel_display(folder, static, exact)
        except (OSError, ValueError, KeyError, TypeError) as exc:
            display = panel_safe_display("Output unavailable: " + str(exc)[:120])
        for name in PANEL_FIELDS:
            values[name].set(display[name])
        root.after(refresh_ms, refresh)

    def resize(event):
        if event.widget is root:
            wrap = max(240, event.width - 265)
            for label in labels:
                label.configure(wraplength=wrap)

    if owner_stdin:
        threading.Thread(target=watch_owner, name="ES-panel-owner", daemon=True).start()
    root.protocol("WM_DELETE_WINDOW", close)
    root.bind("<Configure>", resize)
    refresh()
    if not closing[0]:
        root.update_idletasks()
        root.deiconify()
        root.lift()
        status("OPEN")
        root.mainloop()


class LiveRuntime:
    """Callbacks timestamp/enqueue; one engine worker and a bounded CSV writer."""
    def __init__(self, alias, cfg, output, metadata):
        self.alias, self.cfg, self.output, self.metadata = alias, cfg, output, metadata
        self.start_ns = time.time_ns()
        self.queue = queue.Queue(maxsize=cfg.event_queue_capacity)
        self.lock = threading.Lock()
        self.seq, self.dropped = 0, 0
        self.queue_highwater, self.last_lag_ms = 0, 0.0
        self.store = None
        self.rate_sample_ns, self.rate_sample_seq = time.monotonic_ns(), 0
        self.emergency, self.engine, self.stopping = "", None, False
        self.run_path = None
        self.panel = FloatingPanelProcess(alias, cfg)
        self.worker = threading.Thread(target=self._run, name="ES-" + alias, daemon=True)
        self.worker.start()

    def push(self, kind, data=None):
        with self.lock:
            if self.stopping:
                return
            if self.emergency:
                self.dropped += 1
                return
            self.seq += 1
            e = Event(time.time_ns(), self.seq, kind, data or {})
            try:
                self.queue.put_nowait(e)
                self.queue_highwater = max(self.queue_highwater, self.queue.qsize())
            except queue.Full:
                self.dropped += 1
                self.emergency = "INPUT_QUEUE_OVERFLOW; events lost; disable/re-enable for snapshot"

    def diagnostics(self):
        now = time.time_ns()
        with self.queue.mutex:
            depth = len(self.queue.queue)
            oldest = self.queue.queue[0].ts_ns if depth else now
        monotonic = time.monotonic_ns()
        seconds = (monotonic - self.rate_sample_ns) / SECOND
        rate = (self.seq - self.rate_sample_seq) / seconds if seconds > 0 else 0.0
        self.rate_sample_ns, self.rate_sample_seq = monotonic, self.seq
        row = {"input_events_per_second": rate,
               "input_queue_depth": depth, "input_queue_highwater": self.queue_highwater,
               "input_queue_oldest_age_ms": max(0.0, (now - oldest) / 1e6),
               "last_processing_lag_ms": self.last_lag_ms,
               "received_events": self.seq, "dropped_events": self.dropped}
        if isinstance(self.store, AsyncCsvStore):
            row.update(self.store.diagnostics())
        return row

    def safety(self):
        return self.emergency or (self.store._writer_error if isinstance(self.store, AsyncCsvStore) else "")

    def _run(self):
        store = None
        try:
            store_type = AsyncCsvStore if self.cfg.async_csv_logging else CsvStore
            store = store_type(self.output, self.alias, self.cfg, self.metadata)
            self.store = store
            self.run_path = store.path
            self.engine = Engine(self.alias, self.start_ns, self.cfg, store, print_display,
                now_ns=time.time_ns, safety_check=self.safety, metadata=self.metadata,
                diagnostics=self.diagnostics)
            self.engine.heartbeat(time.time_ns())
            self.panel.start(store.path)
            while not self.stopping or not self.queue.empty():
                if self.emergency and not self.engine.fault_reason:
                    self.engine.fault(self.emergency + "; dropped=" + str(self.dropped), time.time_ns())
                try:
                    e = self.queue.get(timeout=0.5)
                except queue.Empty:
                    self.engine.heartbeat(time.time_ns())
                    continue
                if self.emergency:
                    e.data["_adapter_fault"] = self.emergency
                e.processing_lag_ms = max(0.0, (time.time_ns() - e.ts_ns) / 1_000_000)
                self.last_lag_ms = e.processing_lag_ms
                self.engine.process(e)
                self.queue.task_done()
                # Health is clocked independently of delayed PULSE callbacks.
                # This does not advance candles using worker time.
                if time.time_ns() - self.engine.last_heartbeat_ns >= SECOND:
                    self.engine.heartbeat(time.time_ns())
            self.seq += 1
            self.engine.process(Event(time.time_ns(), self.seq, "STOP"))
        except Exception as exc:
            self.emergency = "Logging/worker failure: " + str(exc)
            print_display(Decision("DATA_QUALITY", 0, "STAND ASIDE", self.emergency[:160]).display())
            if store is not None:
                try:
                    with open(str(store.path / "fatal_error.txt"), "w", encoding="utf-8") as f:
                        f.write(traceback.format_exc())
                    store.close()
                except Exception:
                    pass

    def stop(self, wait=True):
        self.panel.stop(wait=False)
        with self.lock:
            self.stopping = True
        if not wait:
            return
        self.worker.join(timeout=15)
        self.panel.stop(wait=True)
        if self.worker.is_alive():
            print_display(Decision("DATA_QUALITY", 0, "STAND ASIDE", "Shutdown did not drain; trailing records may be incomplete").display())


class BookmapBridge:
    """Uses the published Bookmap Python callback signatures, not Rithmic SDK calls."""
    def __init__(self, bm, cfg, output, runtime_factory=LiveRuntime):
        self.bm, self.cfg, self.output, self.runtime_factory = bm, cfg, output, runtime_factory
        self.runtimes, self.requests, self.req_id = {}, {}, 0
        self.retired = []

    def subscribe(self, addon, alias, full_name, is_crypto, pips, size_multiplier, *tail):
        if len(tail) == 2:
            instrument_multiplier, supported = tail
        elif len(tail) == 1 and isinstance(tail[0], dict):
            instrument_multiplier, supported = None, tail[0]
        else:
            print_display(Decision("DATA_QUALITY", 0, "STAND ASIDE", "Unsupported instrument callback signature").display())
            return
        reason = ""
        if is_crypto or not re.match(self.cfg.symbol_regex, alias):
            reason = "Enable this add-on on one outright ES futures contract, not MES/spreads"
        elif not math.isclose(pips, 0.25, abs_tol=1e-12) or not math.isclose(size_multiplier, 1.0, abs_tol=1e-12):
            reason = "Unexpected ES tick/size scaling; refusing to guess units"
        elif supported.get("mbo") is False:
            reason = "MBO unavailable; enable non-aggregated Rithmic CME order data"
        elif supported.get("isDelayed") is True:
            reason = "Delayed feed detected; live decisions disabled"
        if reason:
            print_display(Decision("DATA_QUALITY", 0, "STAND ASIDE", reason).display())
            return
        if alias in self.runtimes:
            old = self.runtimes.pop(alias)
            old.stop(wait=False)
            self.retired.append(old)
        rt = self.runtime_factory(alias, self.cfg, self.output, {
            "full_name": full_name, "pips": pips, "size_multiplier": size_multiplier,
            "instrument_multiplier": instrument_multiplier, "supported_features": supported})
        self.runtimes[alias] = rt
        for kind, subscribe in (("MBO", self.bm.subscribe_to_mbo), ("TRADES", self.bm.subscribe_to_trades)):
            self.req_id += 1
            self.requests[self.req_id] = (alias, kind)
            subscribe(addon, alias, self.req_id)

    def unsubscribe(self, addon, alias):
        rt = self.runtimes.pop(alias, None)
        if rt:
            rt.stop(wait=False)
            self.retired.append(rt)

    def mbo(self, addon, alias, event_type, order_id, price_level, size_level):
        rt = self.runtimes.get(alias)
        if rt:
            rt.push("MBO", {"event_type": event_type, "order_id": order_id, "price": price_level, "qty": size_level})

    def trade(self, addon, alias, price_level, size_level, is_otc, is_bid, is_execution_start,
              is_execution_end, aggressor_order_id, passive_order_id):
        rt = self.runtimes.get(alias)
        if rt:
            # Bookmap's trade is_bid flag means BUY AGGRESSOR, not "trade hit bid".
            rt.push("TRADE", {"price": price_level, "qty": size_level, "is_otc": is_otc, "is_buy": is_bid,
                "start": is_execution_start, "end": is_execution_end,
                "aggressor_id": clean_id(aggressor_order_id), "passive_id": clean_id(passive_order_id)})

    def interval(self, addon, *aliases):
        targets = aliases if aliases else tuple(self.runtimes)
        for alias in targets:
            if alias in self.runtimes:
                self.runtimes[alias].push("PULSE")

    def response(self, addon, req_id):
        target = self.requests.get(req_id)
        if target and target[0] in self.runtimes:
            self.runtimes[target[0]].push("ACK", {"request_id": req_id, "subscription": target[1],
                "note": "Acknowledgement is NOT a snapshot-end marker"})

    def register(self, addon):
        self.bm.add_mbo_handler(addon, self.mbo)
        self.bm.add_trades_handler(addon, self.trade)
        self.bm.add_on_interval_handler(addon, self.interval)
        self.bm.add_response_data_handler(addon, self.response)

    def close(self):
        for rt in list(self.runtimes.values()) + self.retired:
            rt.stop()
        self.runtimes.clear()
        self.retired.clear()


def run_bookmap():
    try:
        import bookmap as bm
    except ImportError:
        print_display(Decision("DATA_QUALITY", 0, "STAND ASIDE", "Load es_engine.py inside Bookmap Python API; use --demo for offline testing").display())
        raise SystemExit(2)
    config_path = os.environ.get("ES_ENGINE_CONFIG")
    if not config_path:
        sibling = Path(__file__).resolve().with_name("config.json")
        config_path = str(sibling) if sibling.exists() else None
    cfg = Config.load(config_path)
    output = os.environ.get("ES_ENGINE_LOG_DIR", str(Path.home() / "es_microstructure_logs"))
    bridge = BookmapBridge(bm, cfg, output)
    addon = bm.create_addon()
    bridge.register(addon)
    try:
        bm.start_addon(addon, bridge.subscribe, bridge.unsubscribe)
        bm.wait_until_addon_is_turned_off(addon)
    finally:
        bridge.close()


def run_demo(output, config=None, minutes=48, quiet=False):
    """Deterministic synthetic data. Not a sample of real ES/Rithmic activity."""
    cfg = config or Config()
    base_ns = int(datetime(2020, 1, 6, 14, 30, tzinfo=timezone.utc).timestamp()) * SECOND
    store = CsvStore(output, "ESZ0.CME@SYNTHETIC", cfg, {"mode": "SYNTHETIC", "seed": 1729})
    engine = Engine("ESZ0.CME@SYNTHETIC", base_ns, cfg, store,
                    None if quiet else print_display, timestamp_basis="SYNTHETIC")
    rng, seq, order_counter = random.Random(1729), 0, 0
    center, ladder_size = None, None

    def send(ts, kind, data=None):
        nonlocal seq
        seq += 1
        engine.process(Event(ts, seq, kind, data or {}))

    def ladder(ts, target, displayed):
        nonlocal center, ladder_size, order_counter
        cursor = ts
        for oid in list(engine.book.orders):
            cursor += 1000
            send(cursor, "MBO", {"event_type": "CANCEL", "order_id": oid, "price": 0, "qty": 0})
        for side in SIDES:
            for i in range(20):
                order_counter += 1
                cursor += 1000
                p = target - i - 1 if side == "bid" else target + i
                send(cursor, "MBO", {"event_type": "BID_NEW" if side == "bid" else "ASK_NEW",
                                    "order_id": str(order_counter), "price": p, "qty": displayed})
        center, ladder_size = target, displayed
        return cursor

    ladder(base_ns, 20000, 100)
    for minute in range(minutes):
        for second in range(60):
            ts = base_ns + (minute * 60 + second) * SECOND + 10_000_000
            send(ts, "PULSE")
            displayed = 500 if minute in (34, 35) else 100
            if minute in (18, 19):
                target, is_buy, qty = 20002, second % 12 != 0, 45
            elif minute in (20, 21, 22):
                target = 20002 - ((minute - 20) * 6 + second // 10 + 1)
                is_buy, qty = second % 10 == 0, 35 + second % 7
            elif minute in (24, 25, 26):
                target = 19985 + (minute - 24) * 7 + second // 9
                is_buy, qty = second % 11 != 0, 38 + second % 5
            elif minute == 27:
                target, is_buy, qty = 20004 - second // 5, False, 35
            elif minute in (34, 35):
                target, is_buy, qty = 19996, bool(second % 2), 8
            else:
                target = 20000 + rng.choice((-2, -1, 0, 1, 2)) if second % 5 == 0 else center
                is_buy, qty = rng.random() > 0.5, rng.randint(2, 15)
            if target != center or displayed != ladder_size:
                ladder(ts + 1_000_000, target, displayed)
            side = "ask" if is_buy else "bid"
            price = engine.book.best(side)
            oid = next(oid for oid, o in engine.book.orders.items() if o.side == side and o.price == price)
            old_qty = engine.book.orders[oid].qty
            send(ts + 5_000_000, "TRADE", {"price": price, "qty": qty, "is_buy": is_buy, "is_otc": False,
                "passive_id": oid, "aggressor_id": "A" + str(seq), "start": True, "end": True})
            send(ts + 7_000_000, "MBO", {"event_type": "REPLACE", "order_id": oid, "price": price, "qty": old_qty - qty})
            send(ts + 9_000_000, "MBO", {"event_type": "REPLACE", "order_id": oid, "price": price, "qty": old_qty})
        send(base_ns + (minute + 1) * MINUTE, "PULSE")
    send(base_ns + minutes * MINUTE + 1000, "STOP")
    return store.path


def replay(paths, output, config=None, quiet=False):
    files = []
    for p in paths:
        path = Path(p)
        files.extend(sorted(path.rglob("events.csv")) if path.is_dir() else [path])
    files = sorted(set(files), key=str)
    if not files:
        raise ValueError("No events.csv files found")
    engine, store = None, None
    try:
        for filename in files:
            with open(str(filename), newline="", encoding="utf-8-sig") as f:
                for row in csv.DictReader(f):
                    e = Event(**decode_audit_json(json.loads(row["normalized_event_json"])))
                    if e.kind == "START":
                        if engine is not None:
                            raise ValueError("Replay one instrument/run at a time; multiple START records found")
                        cfg = config or Config(**e.data["config"]).validate()
                        store = CsvStore(output, e.data["alias"], cfg, {"mode": "REPLAY", "source_files": [str(x) for x in files]})
                        engine = Engine(e.data["alias"], e.ts_ns, cfg, store, None if quiet else print_display,
                            e.data["tick_size"], e.data["timestamp_basis"],
                            metadata=e.data.get("metadata", {}))
                    elif engine is None:
                        raise ValueError("Missing START snapshot. Include all daily events files from the beginning of ONE run")
                    else:
                        engine.process(e)
        if engine is None:
            raise ValueError("No START record found")
    finally:
        if engine is not None:
            engine.close()
    return store.path


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--panel":
        parser = argparse.ArgumentParser(description="Five-field ES floating display")
        parser.add_argument("--panel", required=True, metavar="RUN_FOLDER")
        parser.add_argument("--exact-run", action="store_true")
        parser.add_argument("--owner-stdin", action="store_true", help=argparse.SUPPRESS)
        parser.add_argument("--alias", default="")
        parser.add_argument("--geometry", default="900x310+60+60")
        parser.add_argument("--refresh-ms", type=int, default=500)
        parser.add_argument("--no-topmost", action="store_true")
        parser.add_argument("--static", action="store_true", help="OFFLINE preview; disables live freshness checks")
        args = parser.parse_args()
        if not 100 <= args.refresh_ms <= 2000:
            parser.error("--refresh-ms must be in [100, 2000]")
        if args.owner_stdin and (args.static or not args.exact_run):
            parser.error("Owned panels require --exact-run and prohibit --static")
        run_floating_panel(args.panel, args.static, args.exact_run, args.owner_stdin,
                           args.alias, args.geometry, args.refresh_ms, not args.no_topmost)
        return
    # Bookmap supplies a TCP port as argv[1]; do NOT consume it with argparse.
    if len(sys.argv) > 1 and sys.argv[1].startswith("--"):
        parser = argparse.ArgumentParser(description=__doc__)
        mode = parser.add_mutually_exclusive_group(required=True)
        mode.add_argument("--demo", action="store_true", help="Generate clearly labelled synthetic events")
        mode.add_argument("--replay", nargs="+", help="ONE run directory or its ordered daily events.csv files")
        mode.add_argument("--write-config", metavar="PATH", help="Write default JSON configuration")
        parser.add_argument("--output", default="./es_logs")
        parser.add_argument("--config", help="JSON overrides (complete or partial Config fields)")
        parser.add_argument("--minutes", type=int, default=48)
        parser.add_argument("--quiet", action="store_true")
        args = parser.parse_args()
        if args.write_config:
            with open(args.write_config, "w", encoding="utf-8") as f:
                json.dump(asdict(Config()), f, indent=2)
            return
        cfg = Config.load(args.config) if args.config else None
        if args.demo:
            if not 1 <= args.minutes <= 1440:
                parser.error("--minutes must be between 1 and 1440")
            path = run_demo(args.output, cfg, args.minutes, args.quiet)
        else:
            path = replay(args.replay, args.output, cfg, args.quiet)
        if not args.quiet:
            print("Offline output: " + str(path), file=sys.stderr)
    else:
        run_bookmap()


if __name__ == "__main__":
    main()
