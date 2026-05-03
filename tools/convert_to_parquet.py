#!/usr/bin/env python3
"""
Convert LiveTradingv2 binary recording files (.bin) to Parquet format.

Usage:
    python convert_to_parquet.py data/rtds/20260305_14.bin
    python convert_to_parquet.py data/rtds/           # convert all .bin in dir
    python convert_to_parquet.py data/                 # convert all sources

Output: creates .parquet file next to each .bin file.

File formats:
  RTDS (source=0):       [16B header] [64B RtdsRecord]...
  Market raw (source=1): [16B header] [24B RawRecordHeader][JSON]...
  User raw (source=2):   [16B header] [24B RawRecordHeader][JSON]...
  Journal (source=3):    [16B header] [256B JournalRecord]...

Dependencies: pip install pyarrow
"""

import json
import struct
import sys
from pathlib import Path

try:
    import pyarrow as pa
    import pyarrow.parquet as pq
except ImportError:
    print("ERROR: pyarrow not installed. Run: pip install pyarrow")
    sys.exit(1)

# File header: magic(4) + version(2) + source(1) + reserved(1) + record_size(4) + reserved(4)
HEADER_FMT = "<4sHBBII"
HEADER_SIZE = 16

# RtdsRecord: 64 bytes
RTDS_FMT = "<qqqd24sB7s"
RTDS_SIZE = 64

# RawRecordHeader: 24 bytes
RAW_HDR_FMT = "<qqII"
RAW_HDR_SIZE = 24

SOURCE_NAMES = {0: "rtds", 1: "market_raw", 2: "user_raw", 3: "journal"}

# JournalRecord: 256 bytes
# Header (32B): wall_clock_ms(q) recv_ts(q) proc_ts(q) seq(H) type(B) flags(B) pad(4s)
JOURNAL_HDR_FMT = "<qqqHBB4s"
JOURNAL_HDR_SIZE = 32
# Common (72B): JournalId = data(64s) hash(Q)
JOURNAL_ID_FMT = "<64sQ"
JOURNAL_ID_SIZE = 72
# Payload (152B): raw bytes, interpreted per type
JOURNAL_PAYLOAD_SIZE = 152
JOURNAL_SIZE = 256

JOURNAL_TYPE_NAMES = {
    0: "STRATEGY_EVAL",
    1: "RISK_DECISION",
    2: "ORDER_SENT",
    3: "EXEC_FEEDBACK",
    4: "ORDER_STATUS",
    5: "FILL",
    6: "MODE_CHANGE",
    7: "CANCEL_ALL",
}

RISK_DENY_REASONS = {0: "NONE", 1: "POSITION_LIMIT", 2: "EXPOSURE_LIMIT", 3: "NOTIONAL_LIMIT"}
EXEC_MODES = {0: "DRY_RUN", 1: "LIVE"}
INTENT_ACTIONS = {0: "PLACE_BID", 1: "PLACE_ASK", 2: "CANCEL_BID", 3: "CANCEL_ASK", 4: "CANCEL_ALL"}
SIDES = {0: "BID", 1: "ASK"}


def read_header(f):
    data = f.read(HEADER_SIZE)
    if len(data) < HEADER_SIZE:
        return None
    magic, version, source, _, record_size, _ = struct.unpack(HEADER_FMT, data)
    if magic != b"LTRB":
        return None
    return {"version": version, "source": source, "record_size": record_size}


def convert_rtds(path: Path):
    """Convert RTDS binary to Parquet."""
    rows = []
    with open(path, "rb") as f:
        hdr = read_header(f)
        if not hdr or hdr["source"] != 0:
            print(f"  Skip {path} (not RTDS)")
            return
        while True:
            data = f.read(RTDS_SIZE)
            if len(data) < RTDS_SIZE:
                break
            recv_ts, wall_ms, exch_ms, value, symbol_raw, sym_len, _ = struct.unpack(RTDS_FMT, data)
            symbol = symbol_raw[:sym_len].decode("utf-8", errors="replace")
            rows.append({
                "recv_ts_ns": recv_ts,
                "wall_clock_ms": wall_ms,
                "exchange_ts_ms": exch_ms,
                "value": value,
                "symbol": symbol,
            })

    if not rows:
        print(f"  Skip {path} (empty)")
        return

    table = pa.table({
        "recv_ts_ns": pa.array([r["recv_ts_ns"] for r in rows], type=pa.int64()),
        "wall_clock_ms": pa.array([r["wall_clock_ms"] for r in rows], type=pa.int64()),
        "exchange_ts_ms": pa.array([r["exchange_ts_ms"] for r in rows], type=pa.int64()),
        "value": pa.array([r["value"] for r in rows], type=pa.float64()),
        "symbol": pa.array([r["symbol"] for r in rows], type=pa.string()),
    })
    out = path.with_suffix(".parquet")
    pq.write_table(table, out, compression="snappy")
    print(f"  {path} -> {out} ({len(rows)} records)")


def convert_raw(path: Path, expected_source: int, source_name: str):
    """Convert variable-length raw JSON binary to Parquet.

    Each record on disk: [RawRecordHeader 24B][JSON payload_len bytes]
    The JSON payload is stored as a string column for maximum flexibility.
    """
    rows = []
    with open(path, "rb") as f:
        hdr = read_header(f)
        if not hdr or hdr["source"] != expected_source:
            print(f"  Skip {path} (not {source_name})")
            return
        while True:
            hdr_data = f.read(RAW_HDR_SIZE)
            if len(hdr_data) < RAW_HDR_SIZE:
                break
            recv_ts, wall_ms, payload_len, flags = struct.unpack(RAW_HDR_FMT, hdr_data)
            if payload_len > 0:
                payload = f.read(payload_len)
                if len(payload) < payload_len:
                    break
            else:
                payload = b""

            json_str = payload.decode("utf-8", errors="replace")
            truncated = bool(flags & 1)

            rows.append({
                "recv_ts_ns": recv_ts,
                "wall_clock_ms": wall_ms,
                "payload": json_str,
                "payload_len": payload_len,
                "truncated": truncated,
            })

    if not rows:
        print(f"  Skip {path} (empty)")
        return

    table = pa.table({
        "recv_ts_ns": pa.array([r["recv_ts_ns"] for r in rows], type=pa.int64()),
        "wall_clock_ms": pa.array([r["wall_clock_ms"] for r in rows], type=pa.int64()),
        "payload": pa.array([r["payload"] for r in rows], type=pa.large_string()),
        "payload_len": pa.array([r["payload_len"] for r in rows], type=pa.uint32()),
        "truncated": pa.array([r["truncated"] for r in rows], type=pa.bool_()),
    })
    out = path.with_suffix(".parquet")
    pq.write_table(table, out, compression="snappy")
    print(f"  {path} -> {out} ({len(rows)} records)")


def convert_market(path: Path):
    convert_raw(path, 1, "market_raw")


def convert_user(path: Path):
    convert_raw(path, 2, "user_raw")


def convert_journal(path: Path):
    """Convert Journal binary to Parquet.

    Each record is 256 bytes fixed-size. Splits into per-type tables.
    """
    records = []
    with open(path, "rb") as f:
        hdr = read_header(f)
        if not hdr or hdr["source"] != 3:
            print(f"  Skip {path} (not journal)")
            return
        while True:
            data = f.read(JOURNAL_SIZE)
            if len(data) < JOURNAL_SIZE:
                break

            # Parse header (32B)
            wall_ms, recv_ts, proc_ts, seq, rec_type, flags = struct.unpack_from(
                "<qqqHBB", data, 0)
            is_dry_run = bool(flags & 0x01)
            type_name = JOURNAL_TYPE_NAMES.get(rec_type, f"UNKNOWN_{rec_type}")

            # Parse common asset_id (72B at offset 32)
            asset_data, asset_hash = struct.unpack_from(JOURNAL_ID_FMT, data, JOURNAL_HDR_SIZE)
            asset_id = asset_data.split(b"\x00", 1)[0].decode("utf-8", errors="replace")

            # Base fields
            rec = {
                "wall_clock_ms": wall_ms,
                "recv_ts_ns": recv_ts,
                "proc_ts_ns": proc_ts,
                "seq": seq,
                "type": type_name,
                "is_dry_run": is_dry_run,
                "asset_id": asset_id,
                "asset_id_hash": asset_hash,
            }

            # Parse type-specific payload (152B at offset 104)
            payload_off = JOURNAL_HDR_SIZE + JOURNAL_ID_SIZE
            if rec_type == 0:  # STRATEGY_EVAL
                bbo_bid, bbo_ask, desired_bid, desired_ask, qty, intent_count, tsrc, tkind = \
                    struct.unpack_from("<iiiiqi BBB5s", data, payload_off)
                rec.update(bbo_bid=bbo_bid, bbo_ask=bbo_ask,
                           desired_bid=desired_bid, desired_ask=desired_ask,
                           qty=qty, intent_count=intent_count)
            elif rec_type == 1:  # RISK_DECISION
                action, decision, deny_reason, side, price, qty, position, notional = \
                    struct.unpack_from("<BBBBiqqq", data, payload_off)
                rec.update(action=INTENT_ACTIONS.get(action, str(action)),
                           decision="ALLOW" if decision == 0 else "DENY",
                           deny_reason=RISK_DENY_REASONS.get(deny_reason, str(deny_reason)),
                           side=SIDES.get(side, str(side)),
                           price=price, qty=qty, position=position, notional=notional)
            elif rec_type == 2:  # ORDER_SENT
                action, side, level, _, price, qty, bbo_bid, bbo_ask = \
                    struct.unpack_from("<BBBBiqii", data, payload_off)
                rec.update(action=INTENT_ACTIONS.get(action, str(action)),
                           side=SIDES.get(side, str(side)),
                           level=level, price=price, qty=qty,
                           bbo_bid=bbo_bid, bbo_ask=bbo_ask)
            elif rec_type == 3:  # EXEC_FEEDBACK
                fb_kind = struct.unpack_from("<B", data, payload_off)[0]
                http_status = struct.unpack_from("<i", data, payload_off + 4)[0]
                latency_ns = struct.unpack_from("<q", data, payload_off + 8)[0]
                rec.update(feedback_kind=fb_kind, http_status=http_status,
                           latency_ns=latency_ns)
            elif rec_type == 4:  # ORDER_STATUS
                # order_id JournalId (72B) then status, side, pad, price, orig, filled
                oid_data = struct.unpack_from(JOURNAL_ID_FMT, data, payload_off)
                order_id = oid_data[0].split(b"\x00", 1)[0].decode("utf-8", errors="replace")
                status, side, _, _, price, orig, filled = struct.unpack_from(
                    "<BB2siqq", data, payload_off + JOURNAL_ID_SIZE)
                rec.update(order_id=order_id, status=status, side=SIDES.get(side, str(side)),
                           price=price, original_size=orig, filled_size=filled)
            elif rec_type == 5:  # FILL
                tid_data = struct.unpack_from(JOURNAL_ID_FMT, data, payload_off)
                trade_id = tid_data[0].split(b"\x00", 1)[0].decode("utf-8", errors="replace")
                fill_price, fill_size, net_pos, side = struct.unpack_from(
                    "<iqqB", data, payload_off + JOURNAL_ID_SIZE)
                rec.update(trade_id=trade_id, fill_price=fill_price, fill_size=fill_size,
                           net_position_after=net_pos, side=SIDES.get(side, str(side)))
            elif rec_type == 6:  # MODE_CHANGE
                old_mode, new_mode = struct.unpack_from("<BB", data, payload_off)
                rec.update(old_mode=EXEC_MODES.get(old_mode, str(old_mode)),
                           new_mode=EXEC_MODES.get(new_mode, str(new_mode)))
            elif rec_type == 7:  # CANCEL_ALL
                trigger_src = struct.unpack_from("<B", data, payload_off)[0]
                working = struct.unpack_from("<i", data, payload_off + 4)[0]
                rec.update(trigger_source=trigger_src, working_count=working)

            records.append(rec)

    if not records:
        print(f"  Skip {path} (empty)")
        return

    # Build unified table with all possible columns (missing values = None)
    all_keys = set()
    for r in records:
        all_keys.update(r.keys())

    columns = {}
    for key in sorted(all_keys):
        values = [r.get(key) for r in records]
        # Infer type from first non-None value
        sample = next((v for v in values if v is not None), None)
        if isinstance(sample, bool):
            columns[key] = pa.array(values, type=pa.bool_())
        elif isinstance(sample, int):
            columns[key] = pa.array(values, type=pa.int64())
        elif isinstance(sample, float):
            columns[key] = pa.array(values, type=pa.float64())
        else:
            columns[key] = pa.array([str(v) if v is not None else None for v in values],
                                     type=pa.string())

    table = pa.table(columns)
    out = path.with_suffix(".parquet")
    pq.write_table(table, out, compression="snappy")
    print(f"  {path} -> {out} ({len(records)} journal records)")


CONVERTERS = {0: convert_rtds, 1: convert_market, 2: convert_user, 3: convert_journal}


def convert_file(path: Path):
    """Auto-detect source type from header and convert."""
    with open(path, "rb") as f:
        hdr = read_header(f)
    if not hdr:
        print(f"  Skip {path} (invalid header)")
        return
    converter = CONVERTERS.get(hdr["source"])
    if not converter:
        print(f"  Skip {path} (unknown source {hdr['source']})")
        return
    converter(path)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    target = Path(sys.argv[1])

    if target.is_file() and target.suffix == ".bin":
        convert_file(target)
    elif target.is_dir():
        bin_files = sorted(target.rglob("*.bin"))
        if not bin_files:
            print(f"No .bin files found in {target}")
            sys.exit(1)
        print(f"Converting {len(bin_files)} files...")
        for p in bin_files:
            convert_file(p)
    else:
        print(f"ERROR: {target} is not a .bin file or directory")
        sys.exit(1)

    print("Done.")


if __name__ == "__main__":
    main()
