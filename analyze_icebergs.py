#!/usr/bin/env python3
"""Rank likely iceberg-order locations from the ACSIL DOM event CSV.

Usage:
    python analyze_icebergs.py path/to/dom_events.csv

The script uses only the Python standard library. It reconstructs the visible
lifetime of each price level, attaches executions at that price, and ranks
locations where executed volume repeatedly exceeded displayed liquidity.
Results are candidates, not proof that a native exchange iceberg order existed.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from dataclasses import dataclass
from datetime import datetime
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


OUT_OF_ORDER_TOLERANCE_SECONDS = 5.0


REQUIRED_COLUMNS = {
    "timestamp",
    "record_type",
    "action",
    "symbol",
    "side",
    "price",
    "quantity",
    "num_orders",
    "tick_size",
}


@dataclass
class Episode:
    session_id: int
    symbol: str
    side: str
    price: Decimal
    tick_size: Decimal
    start_time: datetime
    end_time: datetime
    initial_qty: int
    peak_qty: int
    minimum_qty: int
    last_qty: int
    displayed_qty_sum: int
    display_observations: int
    initial_num_orders: int
    minimum_num_orders: int
    maximum_num_orders: int
    depth_updates: int = 1
    replenishment_events: int = 0
    displayed_added_qty: int = 0
    displayed_removed_qty: int = 0
    refilled_after_trades_qty: int = 0
    executed_volume: int = 0
    trade_count: int = 0
    largest_trade: int = 0
    trades_pending_replenishment: int = 0
    volume_pending_replenishment: int = 0
    first_trade_time: Optional[datetime] = None
    last_trade_time: Optional[datetime] = None
    ended_by: str = ""

    def update_depth(self, timestamp: datetime, quantity: int, num_orders: int) -> None:
        previous_quantity = self.last_qty
        quantity_delta = quantity - previous_quantity

        if quantity_delta > 0:
            self.displayed_added_qty += quantity_delta
            if self.trades_pending_replenishment > 0:
                self.replenishment_events += 1
                self.refilled_after_trades_qty += quantity_delta
            self.trades_pending_replenishment = 0
            self.volume_pending_replenishment = 0
        elif quantity_delta < 0:
            self.displayed_removed_qty += -quantity_delta

        self.last_qty = quantity
        self.peak_qty = max(self.peak_qty, quantity)
        self.minimum_qty = min(self.minimum_qty, quantity)
        self.displayed_qty_sum += quantity
        self.display_observations += 1
        self.depth_updates += 1
        self.end_time = max(self.end_time, timestamp)

        if num_orders > 0:
            if self.minimum_num_orders == 0:
                self.minimum_num_orders = num_orders
            else:
                self.minimum_num_orders = min(self.minimum_num_orders, num_orders)
            self.maximum_num_orders = max(self.maximum_num_orders, num_orders)

    def add_trade(self, timestamp: datetime, quantity: int) -> None:
        self.executed_volume += quantity
        self.trade_count += 1
        self.largest_trade = max(self.largest_trade, quantity)
        self.trades_pending_replenishment += 1
        self.volume_pending_replenishment += quantity
        self.end_time = max(self.end_time, timestamp)

        if self.first_trade_time is None:
            self.first_trade_time = timestamp
        self.last_trade_time = timestamp

    def close(self, timestamp: datetime, ended_by: str) -> None:
        self.end_time = max(self.end_time, timestamp)
        self.ended_by = ended_by

    @property
    def duration_ms(self) -> int:
        return max(0, int(round((self.end_time - self.start_time).total_seconds() * 1000.0)))

    @property
    def average_displayed_qty(self) -> float:
        if self.display_observations == 0:
            return 0.0
        return self.displayed_qty_sum / self.display_observations

    @property
    def executed_to_peak_ratio(self) -> float:
        return self.executed_volume / max(1, self.peak_qty)

    @property
    def volume_beyond_peak_display(self) -> int:
        return max(0, self.executed_volume - self.peak_qty)


@dataclass
class AnalysisStats:
    rows: int = 0
    session_rows: int = 0
    snapshot_rows: int = 0
    depth_rows: int = 0
    trade_rows: int = 0
    matched_trade_rows: int = 0
    unmatched_trade_rows: int = 0
    skipped_rows: int = 0


EpisodeKey = Tuple[int, str, str, str, int]


def parse_timestamp(value: str) -> datetime:
    return datetime.fromisoformat(value.strip())


def parse_int(value: str, default: int = 0) -> int:
    text = (value or "").strip()
    if not text:
        return default
    try:
        return int(Decimal(text))
    except (InvalidOperation, ValueError):
        return default


def parse_decimal(value: str, default: Decimal = Decimal("0")) -> Decimal:
    text = (value or "").strip()
    if not text:
        return default
    try:
        parsed = Decimal(text)
    except InvalidOperation:
        return default
    return parsed if parsed.is_finite() else default


def price_key(
    session_id: int,
    symbol: str,
    side: str,
    price: Decimal,
    tick_size: Decimal,
) -> EpisodeKey:
    if tick_size > 0:
        ticks = int((price / tick_size).to_integral_value(rounding=ROUND_HALF_UP))
        tick_identity = format(tick_size.normalize(), "f")
    else:
        ticks = int((price * Decimal("1000000")).to_integral_value(rounding=ROUND_HALF_UP))
        tick_identity = "0"
    return session_id, symbol, side, tick_identity, ticks


def close_active_episodes(
    active: Dict[EpisodeKey, Episode],
    completed: List[Episode],
    timestamp: datetime,
    ended_by: str,
) -> None:
    for episode in active.values():
        episode.close(timestamp, ended_by)
        completed.append(episode)
    active.clear()


def read_episodes(csv_path: Path) -> Tuple[List[Episode], AnalysisStats]:
    stats = AnalysisStats()
    active: Dict[EpisodeKey, Episode] = {}
    completed: List[Episode] = []
    session_id = 0
    last_timestamp: Optional[datetime] = None

    with csv_path.open("r", newline="", encoding="utf-8-sig") as source:
        reader = csv.DictReader(source)
        if reader.fieldnames is None:
            raise ValueError("The CSV file has no header row.")

        missing = REQUIRED_COLUMNS.difference(reader.fieldnames)
        if missing:
            raise ValueError(
                "The CSV is missing required columns: " + ", ".join(sorted(missing))
            )

        for row in reader:
            stats.rows += 1

            if row.get("record_type", "").strip().lower() == "record_type":
                # Tolerate a repeated header in manually concatenated files.
                stats.skipped_rows += 1
                continue

            try:
                timestamp = parse_timestamp(row.get("timestamp", ""))
            except (TypeError, ValueError):
                stats.skipped_rows += 1
                continue

            if last_timestamp is not None and timestamp < last_timestamp:
                backward_seconds = (last_timestamp - timestamp).total_seconds()
                if backward_seconds > OUT_OF_ORDER_TOLERANCE_SECONDS:
                    # A replay restart or concatenated capture can move time far backwards.
                    close_active_episodes(active, completed, last_timestamp, "TIME_RESET")
                    session_id += 1
                    last_timestamp = timestamp
                # Small timestamp reversals can occur when buffered trades arrive slightly
                # out of order. Keep the latest progress time and process the row normally.
            else:
                last_timestamp = timestamp
            record_type = row.get("record_type", "").strip().upper()
            action = row.get("action", "").strip().upper()
            symbol = row.get("symbol", "").strip()
            side = row.get("side", "").strip().upper()

            if record_type == "SESSION":
                stats.session_rows += 1
                if active:
                    close_active_episodes(active, completed, timestamp, action or "SESSION")
                session_id += 1
                continue

            if record_type == "SNAPSHOT":
                stats.snapshot_rows += 1
                continue

            if record_type not in {"DEPTH", "TRADE"}:
                stats.skipped_rows += 1
                continue

            if side not in {"BID", "ASK"} or not symbol:
                stats.skipped_rows += 1
                continue

            price = parse_decimal(row.get("price", ""))
            tick_size = parse_decimal(row.get("tick_size", ""))
            key = price_key(session_id, symbol, side, price, tick_size)

            if record_type == "DEPTH":
                stats.depth_rows += 1
                quantity = max(0, parse_int(row.get("quantity", "")))
                num_orders = max(0, parse_int(row.get("num_orders", "")))

                if action == "DELETE":
                    episode = active.pop(key, None)
                    if episode is not None:
                        episode.close(timestamp, "DELETE")
                        completed.append(episode)
                    continue

                if action != "UPSERT":
                    stats.skipped_rows += 1
                    continue

                episode = active.get(key)
                if episode is None:
                    episode = Episode(
                        session_id=session_id,
                        symbol=symbol,
                        side=side,
                        price=price,
                        tick_size=tick_size,
                        start_time=timestamp,
                        end_time=timestamp,
                        initial_qty=quantity,
                        peak_qty=quantity,
                        minimum_qty=quantity,
                        last_qty=quantity,
                        displayed_qty_sum=quantity,
                        display_observations=1,
                        initial_num_orders=num_orders,
                        minimum_num_orders=num_orders,
                        maximum_num_orders=num_orders,
                    )
                    active[key] = episode
                else:
                    episode.update_depth(timestamp, quantity, num_orders)

            elif record_type == "TRADE":
                stats.trade_rows += 1
                quantity = max(0, parse_int(row.get("quantity", "")))
                episode = active.get(key)

                if episode is None or quantity <= 0:
                    stats.unmatched_trade_rows += 1
                    continue

                episode.add_trade(timestamp, quantity)
                stats.matched_trade_rows += 1

    if active:
        close_time = last_timestamp or datetime.min
        close_active_episodes(active, completed, close_time, "EOF")

    return completed, stats


def iceberg_score(episode: Episode) -> float:
    ratio = episode.executed_to_peak_ratio
    ratio_component = min(1.0, math.log1p(ratio) / math.log1p(10.0))
    trade_component = min(1.0, math.log1p(episode.trade_count) / math.log1p(20.0))
    refill_component = min(1.0, episode.replenishment_events / 4.0)
    duration_component = min(1.0, math.log1p(episode.duration_ms) / math.log1p(10000.0))
    persistence_component = 1.0 if episode.ended_by in {"EOF", "STOP", "SESSION"} else 0.5

    score = 100.0 * (
        0.48 * ratio_component
        + 0.20 * trade_component
        + 0.18 * refill_component
        + 0.09 * duration_component
        + 0.05 * persistence_component
    )
    return max(0.0, min(100.0, score))


def confidence_label(episode: Episode, score: float) -> str:
    if (
        score >= 70.0
        and episode.executed_to_peak_ratio >= 5.0
        and (episode.replenishment_events >= 2 or episode.trade_count >= 10)
    ):
        return "HIGH"
    if score >= 50.0:
        return "MEDIUM"
    return "WATCH"


def candidate_note(episode: Episode) -> str:
    pieces = [
        f"executed {episode.executed_to_peak_ratio:.2f}x peak displayed",
        f"{episode.trade_count} trades",
    ]
    if episode.replenishment_events:
        pieces.append(f"{episode.replenishment_events} refill(s) after trades")
    if episode.ended_by in {"EOF", "STOP", "SESSION"}:
        pieces.append("level remained displayed at capture end")
    elif episode.ended_by == "DELETE":
        pieces.append("level later left captured DOM")
    return "; ".join(pieces)


def select_candidates(
    episodes: Iterable[Episode],
    min_ratio: float,
    min_trades: int,
    min_duration_ms: int,
    min_score: float,
) -> List[Tuple[float, Episode]]:
    candidates: List[Tuple[float, Episode]] = []

    for episode in episodes:
        if episode.executed_volume <= 0 or episode.peak_qty <= 0:
            continue
        if episode.executed_to_peak_ratio < min_ratio:
            continue
        if episode.trade_count < min_trades:
            continue
        if episode.duration_ms < min_duration_ms:
            continue

        supporting_evidence = (
            episode.replenishment_events > 0
            or episode.executed_to_peak_ratio >= max(5.0, min_ratio * 1.5)
            or episode.ended_by in {"EOF", "STOP", "SESSION"}
        )
        if not supporting_evidence:
            continue

        score = iceberg_score(episode)
        if score >= min_score:
            candidates.append((score, episode))

    candidates.sort(
        key=lambda item: (
            item[0],
            item[1].executed_to_peak_ratio,
            item[1].executed_volume,
        ),
        reverse=True,
    )
    return candidates


def write_candidates(
    output_path: Path,
    candidates: List[Tuple[float, Episode]],
    limit: int,
) -> int:
    output_path.parent.mkdir(parents=True, exist_ok=True)

    fields = [
        "rank",
        "score",
        "confidence",
        "session_id",
        "symbol",
        "side",
        "price",
        "tick_size",
        "start_time",
        "end_time",
        "first_trade_time",
        "last_trade_time",
        "duration_ms",
        "initial_displayed_qty",
        "peak_displayed_qty",
        "minimum_displayed_qty",
        "average_displayed_qty",
        "last_displayed_qty",
        "initial_num_orders",
        "minimum_num_orders",
        "maximum_num_orders",
        "depth_updates",
        "replenishment_events",
        "displayed_added_qty",
        "refilled_after_trades_qty",
        "displayed_removed_qty",
        "executed_volume",
        "trade_count",
        "largest_trade",
        "executed_to_peak_ratio",
        "volume_beyond_peak_display",
        "ended_by",
        "interpretation",
    ]

    selected = candidates if limit <= 0 else candidates[:limit]

    with output_path.open("w", newline="", encoding="utf-8") as target:
        writer = csv.DictWriter(target, fieldnames=fields)
        writer.writeheader()

        for rank, (score, episode) in enumerate(selected, start=1):
            writer.writerow(
                {
                    "rank": rank,
                    "score": f"{score:.2f}",
                    "confidence": confidence_label(episode, score),
                    "session_id": episode.session_id,
                    "symbol": episode.symbol,
                    "side": episode.side,
                    "price": format(episode.price, "f"),
                    "tick_size": format(episode.tick_size, "f"),
                    "start_time": episode.start_time.isoformat(timespec="milliseconds"),
                    "end_time": episode.end_time.isoformat(timespec="milliseconds"),
                    "first_trade_time": (
                        episode.first_trade_time.isoformat(timespec="milliseconds")
                        if episode.first_trade_time is not None
                        else ""
                    ),
                    "last_trade_time": (
                        episode.last_trade_time.isoformat(timespec="milliseconds")
                        if episode.last_trade_time is not None
                        else ""
                    ),
                    "duration_ms": episode.duration_ms,
                    "initial_displayed_qty": episode.initial_qty,
                    "peak_displayed_qty": episode.peak_qty,
                    "minimum_displayed_qty": episode.minimum_qty,
                    "average_displayed_qty": f"{episode.average_displayed_qty:.2f}",
                    "last_displayed_qty": episode.last_qty,
                    "initial_num_orders": episode.initial_num_orders,
                    "minimum_num_orders": episode.minimum_num_orders,
                    "maximum_num_orders": episode.maximum_num_orders,
                    "depth_updates": episode.depth_updates,
                    "replenishment_events": episode.replenishment_events,
                    "displayed_added_qty": episode.displayed_added_qty,
                    "refilled_after_trades_qty": episode.refilled_after_trades_qty,
                    "displayed_removed_qty": episode.displayed_removed_qty,
                    "executed_volume": episode.executed_volume,
                    "trade_count": episode.trade_count,
                    "largest_trade": episode.largest_trade,
                    "executed_to_peak_ratio": f"{episode.executed_to_peak_ratio:.4f}",
                    "volume_beyond_peak_display": episode.volume_beyond_peak_display,
                    "ended_by": episode.ended_by,
                    "interpretation": candidate_note(episode),
                }
            )

    return len(selected)


def print_summary(
    csv_path: Path,
    output_path: Path,
    episodes: List[Episode],
    candidates: List[Tuple[float, Episode]],
    written: int,
    stats: AnalysisStats,
) -> None:
    print(f"Input:  {csv_path}")
    print(f"Output: {output_path}")
    print(
        "Rows: "
        f"{stats.rows:,} total, {stats.depth_rows:,} depth, "
        f"{stats.trade_rows:,} trades, {stats.snapshot_rows:,} snapshots"
    )
    print(
        "Trade matching: "
        f"{stats.matched_trade_rows:,} matched to captured levels, "
        f"{stats.unmatched_trade_rows:,} unmatched"
    )
    print(f"Episodes reconstructed: {len(episodes):,}")
    print(f"Candidates found: {len(candidates):,}; rows written: {written:,}")

    if not candidates:
        print("No rows passed the current thresholds.")
        return

    print("\nTop candidates:")
    print("rank  score  conf    symbol        side  price          exec/peak  trades  refills")
    for rank, (score, episode) in enumerate(candidates[:10], start=1):
        print(
            f"{rank:>4}  {score:>5.1f}  {confidence_label(episode, score):<7} "
            f"{episode.symbol:<13.13} {episode.side:<4}  "
            f"{format(episode.price, 'f'):<13.13} "
            f"{episode.executed_to_peak_ratio:>9.2f}  "
            f"{episode.trade_count:>6}  {episode.replenishment_events:>7}"
        )


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Find likely iceberg-order locations in a DOM/trade CSV produced by "
            "ACSIL_DOM_Iceberg_Exporter.cpp."
        )
    )
    parser.add_argument("csv_file", type=Path, help="Path to the exported DOM CSV file")
    parser.add_argument(
        "--output",
        type=Path,
        help="Output CSV path; defaults to <input>_iceberg_candidates.csv",
    )
    parser.add_argument(
        "--min-ratio",
        type=float,
        default=3.0,
        help="Minimum executed-volume / peak-displayed ratio (default: 3.0)",
    )
    parser.add_argument(
        "--min-trades",
        type=int,
        default=4,
        help="Minimum executions at the level (default: 4)",
    )
    parser.add_argument(
        "--min-duration-ms",
        type=int,
        default=200,
        help="Minimum visible episode duration in milliseconds (default: 200)",
    )
    parser.add_argument(
        "--min-score",
        type=float,
        default=42.0,
        help="Minimum composite candidate score from 0 to 100 (default: 42)",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=250,
        help="Maximum rows written; use 0 for all candidates (default: 250)",
    )
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_argument_parser()
    args = parser.parse_args(argv)

    csv_path: Path = args.csv_file.expanduser().resolve()
    if not csv_path.is_file():
        parser.error(f"CSV file does not exist: {csv_path}")

    output_path = args.output
    if output_path is None:
        output_path = csv_path.with_name(f"{csv_path.stem}_iceberg_candidates.csv")
    else:
        output_path = output_path.expanduser().resolve()

    if args.min_ratio <= 0:
        parser.error("--min-ratio must be greater than zero")
    if args.min_trades < 1:
        parser.error("--min-trades must be at least 1")
    if args.min_duration_ms < 0:
        parser.error("--min-duration-ms cannot be negative")
    if not 0 <= args.min_score <= 100:
        parser.error("--min-score must be between 0 and 100")
    if args.top < 0:
        parser.error("--top cannot be negative")

    try:
        episodes, stats = read_episodes(csv_path)
    except (OSError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 2

    candidates = select_candidates(
        episodes,
        min_ratio=args.min_ratio,
        min_trades=args.min_trades,
        min_duration_ms=args.min_duration_ms,
        min_score=args.min_score,
    )

    try:
        written = write_candidates(output_path, candidates, args.top)
    except OSError as exc:
        print(f"Error writing {output_path}: {exc}", file=sys.stderr)
        return 2

    print_summary(csv_path, output_path, episodes, candidates, written, stats)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
