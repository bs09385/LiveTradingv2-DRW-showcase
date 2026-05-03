#!/usr/bin/env python3
"""Analyze RTDS recording: Chainlink vs Binance around 5-min boundaries."""
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

def main():
    rtds_dir = Path("data/rtds")
    files = sorted(rtds_dir.glob("*.jsonl"))
    if not files:
        print("No RTDS jsonl files found")
        return

    binance = []
    chainlink_burst = []
    chainlink_live = []

    for fp in files:
        with open(fp, encoding="utf-8") as f:
            for line in f:
                parts = line.strip().split("\t", 1)
                if len(parts) < 2:
                    continue
                recv_ms = int(parts[0])
                try:
                    d = json.loads(parts[1])
                except Exception:
                    continue

                topic = d.get("topic", "")
                msg_type = d.get("type", "")
                payload = d.get("payload", {})

                if msg_type == "subscribe" and "data" in payload:
                    for pt in payload.get("data", []):
                        chainlink_burst.append((pt["timestamp"], pt["value"]))
                elif "crypto_prices_chainlink" in topic:
                    chainlink_live.append({
                        "recv_ms": recv_ms,
                        "exchange_ms": payload.get("timestamp", 0),
                        "outer_ms": d.get("timestamp", 0),
                        "value": payload.get("value", 0),
                        "symbol": payload.get("symbol", ""),
                    })
                elif payload.get("symbol") == "btcusdt":
                    binance.append((
                        payload.get("timestamp", 0),
                        recv_ms,
                        payload.get("value", 0),
                    ))

    def fmt(ms):
        return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime(
            "%H:%M:%S.%f")[:-3]

    def fmt_full(ms):
        return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime(
            "%Y-%m-%d %H:%M:%S")

    print(f"Binance BTC updates:    {len(binance)}")
    print(f"Chainlink burst pts:    {len(chainlink_burst)}")
    print(f"Chainlink LIVE updates: {len(chainlink_live)}")
    print()

    if binance:
        span_min = (binance[-1][0] - binance[0][0]) / 60000
        print(f"Recording: {fmt_full(binance[0][0])} to "
              f"{fmt_full(binance[-1][0])} ({span_min:.1f} min)")
        print()

    # 5-minute boundaries
    if binance:
        first_s = binance[0][0] // 1000
        last_s = binance[-1][0] // 1000
        boundaries = []
        b = (first_s // 300) * 300
        while b <= last_s + 300:
            boundaries.append(b)
            b += 300

        print(f"5-min boundaries crossed: {len(boundaries)}")
        for bnd in boundaries:
            print(f"  {fmt_full(bnd * 1000)} UTC  (unix={bnd})")
        print()

    # Binance around each boundary
    print("=== BINANCE PRICES AROUND 5-MIN BOUNDARIES ===\n")
    for bnd in boundaries:
        b_ms = bnd * 1000
        bt = datetime.fromtimestamp(bnd, tz=timezone.utc).strftime("%H:%M:%S")
        before = [(ts, v) for ts, _, v in binance
                  if 0 <= b_ms - ts <= 3000]
        after = [(ts, v) for ts, _, v in binance
                 if 0 < ts - b_ms <= 3000]
        if before:
            last_b = before[-1]
            print(f"  {bt}:  before=${last_b[1]:.2f} "
                  f"({b_ms - last_b[0]}ms before)")
        if after:
            first_a = after[0]
            print(f"          after =${first_a[1]:.2f} "
                  f"({first_a[0] - b_ms}ms after)")
        if not before and not after:
            print(f"  {bt}:  no Binance data within 3s")
        print()

    # Chainlink live updates
    if chainlink_live:
        print(f"=== CHAINLINK LIVE UPDATES ({len(chainlink_live)}) ===\n")
        for c in chainlink_live:
            ct = fmt(c["exchange_ms"])
            rt = fmt(c["recv_ms"])
            nearest_b = min(boundaries,
                            key=lambda b: abs(b * 1000 - c["exchange_ms"]))
            dist_ms = c["exchange_ms"] - nearest_b * 1000
            print(f"  exchange={ct}  recv={rt}  "
                  f"value=${c['value']:.3f}  "
                  f"dist_to_5min_boundary={dist_ms:+d}ms "
                  f"({dist_ms / 1000:+.1f}s)")
    else:
        print(f"=== NO CHAINLINK LIVE UPDATES ===\n")
        print(f"Zero crypto_prices_chainlink messages in "
              f"{span_min:.0f} min of recording.")
        print()
        print("Possible reasons:")
        print("  1. Price was stable (within Chainlink deviation threshold)")
        print("  2. Live topic doesn't push at 5-min boundaries")
        print("  3. Chainlink live subscription not receiving updates")
        print()
        print("The initial burst (subscribe) gave us 1-second interpolated")
        print(f"data ({len(chainlink_burst)} points). But no streaming updates after.")
        print()
        print("To test boundary behavior, we need a longer recording")
        print("spanning multiple 5-min windows, ideally with price movement.")


if __name__ == "__main__":
    main()
