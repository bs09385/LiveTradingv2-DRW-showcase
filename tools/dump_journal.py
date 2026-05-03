#!/usr/bin/env python3
"""
Dump LiveTradingv2 journal .bin files as human-readable text or JSON Lines.

Zero-dependency — uses only Python stdlib.

Usage:
    python tools/dump_journal.py data/journal/20260321_12.bin
    python tools/dump_journal.py data/journal/*.bin --summary
    python tools/dump_journal.py data/journal/*.bin --type FILL --limit 50
    python tools/dump_journal.py data/journal/*.bin --json | jq .
"""

import argparse
import json
import struct
import sys
from datetime import datetime, timezone
from pathlib import Path

# --- File header ---
HEADER_FMT = "<4sHBBII"
HEADER_SIZE = 16
JOURNAL_SIZE = 256
JOURNAL_SOURCE = 3

# --- Struct offsets ---
HDR_OFF = 0        # 32B record header
ID_OFF = 32        # 72B JournalId (asset_id)
PAYLOAD_OFF = 104  # 152B payload union

# --- Enum maps ---
TYPE_NAMES = {
    0: "STRATEGY_EVAL", 1: "RISK_DECISION", 2: "ORDER_SENT", 3: "EXEC_FEEDBACK",
    4: "ORDER_STATUS", 5: "FILL", 6: "MODE_CHANGE", 7: "CANCEL_ALL",
}

INTENT_ACTIONS = {0: "PLACE_BID", 1: "PLACE_ASK", 2: "CANCEL_BID", 3: "CANCEL_ASK", 4: "CANCEL_ALL"}
SIDES = {0: "BID", 1: "ASK"}
RISK_DECISIONS = {0: "ALLOW", 1: "DENY"}
RISK_DENY_REASONS = {0: "NONE", 1: "POSITION_LIMIT", 2: "EXPOSURE_LIMIT", 3: "NOTIONAL_LIMIT"}
EXEC_MODES = {0: "DRY_RUN", 1: "LIVE"}
FEEDBACK_KINDS = {
    0: "REQUEST_SENT", 1: "ORDER_ACCEPTED", 2: "ORDER_REJECTED", 3: "CANCEL_CONFIRMED",
    4: "RATE_LIMITED", 5: "EXCHANGE_UNAVAILABLE", 6: "TIMEOUT", 7: "HEARTBEAT_OK",
    8: "HEARTBEAT_FAILED", 9: "GATEWAY_DEGRADED", 10: "GATEWAY_RECOVERED",
}
ORDER_STATUS = {0: "UNKNOWN", 1: "LIVE", 2: "PARTIAL", 3: "FILLED", 4: "CANCELED", 5: "FAILED"}
EVENT_SOURCES = {0: "MARKET_WS", 1: "USER_WS", 2: "EXEC_INTERNAL", 3: "CONTROL"}


def fmt_price(p):
    """10000x fixed-point to decimal string."""
    return f"{p / 10000:.4f}"


def fmt_qty(q):
    """Quantity with scale factor (1e6) to shares."""
    return f"{q / 1000000:.6f}"


def fmt_time(wall_ms):
    """Wall clock ms since epoch to HH:MM:SS.mmm."""
    if wall_ms <= 0:
        return "00:00:00.000"
    dt = datetime.fromtimestamp(wall_ms / 1000.0, tz=timezone.utc)
    return dt.strftime("%H:%M:%S.") + f"{wall_ms % 1000:03d}"


def read_id(data, offset):
    """Read JournalId (64s + Q) and return decoded string."""
    raw = struct.unpack_from("<64sQ", data, offset)
    return raw[0].split(b"\x00", 1)[0].decode("utf-8", errors="replace"), raw[1]


def truncate_id(s, maxlen=20):
    """Truncate an ID for table display."""
    return s if len(s) <= maxlen else s[:maxlen - 3] + "..."


def parse_record(data):
    """Parse a 256-byte journal record into a dict."""
    wall_ms, recv_ts, proc_ts, seq, rec_type, flags = struct.unpack_from("<qqqHBB", data, HDR_OFF)
    is_dry = bool(flags & 0x01)
    type_name = TYPE_NAMES.get(rec_type, f"UNKNOWN_{rec_type}")
    asset_str, asset_hash = read_id(data, ID_OFF)

    rec = {
        "wall_clock_ms": wall_ms,
        "recv_ts_ns": recv_ts,
        "proc_ts_ns": proc_ts,
        "seq": seq,
        "type": type_name,
        "is_dry_run": is_dry,
        "asset_id": asset_str,
        "asset_id_hash": asset_hash,
    }

    p = PAYLOAD_OFF
    if rec_type == 0:  # STRATEGY_EVAL
        bbo_bid, bbo_ask, desired_bid, desired_ask, qty, intent_count, tsrc, tkind = \
            struct.unpack_from("<iiiiqBBB", data, p)
        rec.update(bbo_bid=bbo_bid, bbo_ask=bbo_ask, desired_bid=desired_bid,
                   desired_ask=desired_ask, qty=qty, intent_count=intent_count,
                   trigger_source=EVENT_SOURCES.get(tsrc, str(tsrc)),
                   trigger_kind=tkind)
    elif rec_type == 1:  # RISK_DECISION
        action, decision, deny_reason, side, price, qty, position, notional = \
            struct.unpack_from("<BBBBiqqq", data, p)
        rec.update(action=INTENT_ACTIONS.get(action, str(action)),
                   decision=RISK_DECISIONS.get(decision, str(decision)),
                   deny_reason=RISK_DENY_REASONS.get(deny_reason, str(deny_reason)),
                   side=SIDES.get(side, str(side)),
                   price=price, qty=qty, position=position, notional=notional)
    elif rec_type == 2:  # ORDER_SENT
        action, side, level, _, price, qty, bbo_bid, bbo_ask = \
            struct.unpack_from("<BBBBiqii", data, p)
        oid_str, _ = read_id(data, p + 24)
        rec.update(action=INTENT_ACTIONS.get(action, str(action)),
                   side=SIDES.get(side, str(side)), level=level,
                   price=price, qty=qty, bbo_bid=bbo_bid, bbo_ask=bbo_ask,
                   client_order_id=oid_str)
    elif rec_type == 3:  # EXEC_FEEDBACK
        fb_kind = struct.unpack_from("<B", data, p)[0]
        http_status = struct.unpack_from("<i", data, p + 4)[0]
        latency_ns = struct.unpack_from("<q", data, p + 8)[0]
        oid_str, _ = read_id(data, p + 16)
        rec.update(feedback_kind=FEEDBACK_KINDS.get(fb_kind, str(fb_kind)),
                   http_status=http_status, latency_ns=latency_ns, order_id=oid_str)
    elif rec_type == 4:  # ORDER_STATUS
        oid_str, _ = read_id(data, p)
        status, side = struct.unpack_from("<BB", data, p + 72)
        price, orig, filled = struct.unpack_from("<iqq", data, p + 76)
        rec.update(order_id=oid_str, status=ORDER_STATUS.get(status, str(status)),
                   side=SIDES.get(side, str(side)),
                   price=price, original_size=orig, filled_size=filled)
    elif rec_type == 5:  # FILL
        tid_str, _ = read_id(data, p)
        # Note: 4 bytes of alignment padding between Price_t (i32) and Qty_t (i64)
        fill_price, fill_size, net_pos, side = struct.unpack_from("<i4xqqB", data, p + 72)
        rec.update(trade_id=tid_str, fill_price=fill_price, fill_size=fill_size,
                   net_position_after=net_pos, side=SIDES.get(side, str(side)))
    elif rec_type == 6:  # MODE_CHANGE
        old_mode, new_mode = struct.unpack_from("<BB", data, p)
        rec.update(old_mode=EXEC_MODES.get(old_mode, str(old_mode)),
                   new_mode=EXEC_MODES.get(new_mode, str(new_mode)))
    elif rec_type == 7:  # CANCEL_ALL
        trigger_src = struct.unpack_from("<B", data, p)[0]
        working = struct.unpack_from("<i", data, p + 4)[0]
        rec.update(trigger_source=EVENT_SOURCES.get(trigger_src, str(trigger_src)),
                   working_count=working)

    return rec


def format_table_line(rec):
    """Format a record as a single human-readable line."""
    ts = fmt_time(rec["wall_clock_ms"])
    seq = f"#{rec['seq']:04d}"
    typ = rec["type"].ljust(15)
    dry = "[DRY] " if rec["is_dry_run"] else "      "
    asset = truncate_id(rec["asset_id"])

    detail = ""
    t = rec["type"]
    if t == "STRATEGY_EVAL":
        detail = (f"bbo={fmt_price(rec['bbo_bid'])}/{fmt_price(rec['bbo_ask'])} "
                  f"desired={fmt_price(rec['desired_bid'])}/{fmt_price(rec['desired_ask'])} "
                  f"qty={fmt_qty(rec['qty'])} intents={rec['intent_count']}")
    elif t == "RISK_DECISION":
        detail = (f"{rec['action']} {rec['decision']} price={fmt_price(rec['price'])} "
                  f"qty={fmt_qty(rec['qty'])} pos={fmt_qty(rec['position'])} "
                  f"notional={rec['notional']}")
        if rec["decision"] == "DENY":
            detail += f" reason={rec['deny_reason']}"
    elif t == "ORDER_SENT":
        detail = (f"{rec['action']} price={fmt_price(rec['price'])} qty={fmt_qty(rec['qty'])} "
                  f"bbo={fmt_price(rec['bbo_bid'])}/{fmt_price(rec['bbo_ask'])}")
    elif t == "EXEC_FEEDBACK":
        detail = (f"{rec['feedback_kind']} http={rec['http_status']} "
                  f"latency={rec['latency_ns']}ns")
        if rec.get("order_id"):
            detail += f" oid={truncate_id(rec['order_id'])}"
    elif t == "ORDER_STATUS":
        detail = (f"{rec['status']} {rec['side']} price={fmt_price(rec['price'])} "
                  f"orig={fmt_qty(rec['original_size'])} filled={fmt_qty(rec['filled_size'])}")
        if rec.get("order_id"):
            detail += f" oid={truncate_id(rec['order_id'])}"
    elif t == "FILL":
        detail = (f"{rec['side']} price={fmt_price(rec['fill_price'])} "
                  f"size={fmt_qty(rec['fill_size'])} net_pos={fmt_qty(rec['net_position_after'])}")
        if rec.get("trade_id"):
            detail += f" tid={truncate_id(rec['trade_id'])}"
    elif t == "MODE_CHANGE":
        detail = f"{rec['old_mode']} -> {rec['new_mode']}"
    elif t == "CANCEL_ALL":
        detail = f"source={rec.get('trigger_source', '?')} working={rec.get('working_count', 0)}"

    return f"{ts}  {seq}  {typ} {dry}{asset.ljust(20)}  {detail}"


def dump_file(path, args):
    """Read and dump a single journal .bin file. Returns list of parsed records."""
    with open(path, "rb") as f:
        hdr_data = f.read(HEADER_SIZE)
        if len(hdr_data) < HEADER_SIZE:
            print(f"ERROR: {path}: file too small for header", file=sys.stderr)
            return []
        magic, version, source, _, record_size, _ = struct.unpack(HEADER_FMT, hdr_data)
        if magic != b"LTRB":
            print(f"ERROR: {path}: bad magic {magic!r} (expected LTRB)", file=sys.stderr)
            return []
        if source != JOURNAL_SOURCE:
            print(f"ERROR: {path}: source={source} (expected 3=journal)", file=sys.stderr)
            return []
        if record_size != JOURNAL_SIZE:
            print(f"ERROR: {path}: record_size={record_size} (expected 256)", file=sys.stderr)
            return []

        records = []
        count = 0
        while True:
            data = f.read(JOURNAL_SIZE)
            if len(data) < JOURNAL_SIZE:
                break
            rec = parse_record(data)

            # Apply --type filter
            if args.type_filter and rec["type"] != args.type_filter:
                continue

            records.append(rec)
            count += 1

            if not args.summary:
                if args.json_output:
                    print(json.dumps(rec))
                else:
                    print(format_table_line(rec))

            if args.limit and count >= args.limit:
                break

        return records


def print_summary(records, paths):
    """Print summary statistics."""
    if not records:
        print("No records found.")
        return

    type_counts = {}
    dry_count = 0
    min_ts = float("inf")
    max_ts = 0

    for r in records:
        t = r["type"]
        type_counts[t] = type_counts.get(t, 0) + 1
        if r["is_dry_run"]:
            dry_count += 1
        if r["wall_clock_ms"] > 0:
            min_ts = min(min_ts, r["wall_clock_ms"])
            max_ts = max(max_ts, r["wall_clock_ms"])

    total = len(records)
    files_str = ", ".join(str(p) for p in paths)
    print(f"Journal: {files_str}")
    print(f"Records: {total:,} total")

    # Sort by canonical enum order
    for type_val in range(8):
        name = TYPE_NAMES.get(type_val)
        if name and name in type_counts:
            c = type_counts[name]
            pct = 100.0 * c / total
            print(f"  {name + ':':18s} {c:>6,}  ({pct:.1f}%)")

    pct_dry = 100.0 * dry_count / total if total else 0
    print(f"Dry-run: {dry_count:,} ({pct_dry:.1f}%)")

    if min_ts < float("inf") and max_ts > 0:
        span_s = (max_ts - min_ts) / 1000.0
        print(f"Time span: {fmt_time(int(min_ts))} -> {fmt_time(int(max_ts))} ({span_s:.1f}s)")


def main():
    parser = argparse.ArgumentParser(
        description="Dump LiveTradingv2 journal .bin files as human-readable text or JSON.")
    parser.add_argument("files", nargs="+", help="Journal .bin file(s) to read")
    parser.add_argument("--json", dest="json_output", action="store_true",
                        help="Output as JSON Lines (one JSON object per record)")
    parser.add_argument("--type", dest="type_filter", default=None,
                        choices=list(TYPE_NAMES.values()),
                        help="Show only records of this type")
    parser.add_argument("--limit", type=int, default=0,
                        help="Show only first N records (0 = unlimited)")
    parser.add_argument("--summary", action="store_true",
                        help="Print record counts and summary only")

    args = parser.parse_args()

    all_records = []
    paths = []
    for file_arg in args.files:
        p = Path(file_arg)
        if not p.exists():
            print(f"ERROR: {p} does not exist", file=sys.stderr)
            continue
        if not p.is_file():
            print(f"ERROR: {p} is not a file", file=sys.stderr)
            continue
        paths.append(p)
        records = dump_file(p, args)
        all_records.extend(records)

    if args.summary:
        print_summary(all_records, paths)

    if not all_records and not args.summary:
        print("No records found.", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
