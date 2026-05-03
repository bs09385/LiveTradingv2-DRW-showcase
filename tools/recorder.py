#!/usr/bin/env python3
"""Standalone recorder for Polymarket RTDS, Market, and Binance WebSocket data.

Records BTC price data (Binance + Chainlink), BTC UPDOWN 5m market data,
and Binance BTC/USDT bookTicker (best bid/ask) to daily-rotating JSONL files.
Flushes every line. No buffering in memory. Safe for overnight unattended operation.

Output:
    {output_dir}/rtds/YYYY-MM-DD.jsonl
    {output_dir}/market/YYYY-MM-DD.jsonl
    {output_dir}/binance/YYYY-MM-DD.jsonl

Line format: {recv_timestamp_utc_ms}<TAB>{raw_json_message}

Dependencies: pip install websockets

Usage:
    python recorder.py
    python recorder.py --output-dir /path/to/data
    python recorder.py -o data
"""

import argparse
import asyncio
import json
import logging
import signal
import ssl
import sys
import time
import urllib.request
from contextlib import suppress
from datetime import datetime, timezone
from pathlib import Path

try:
    import websockets
except ImportError:
    print("ERROR: websockets not installed. Run: pip install websockets")
    sys.exit(1)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s.%(msecs)03d UTC [%(name)-7s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)

# ── Endpoints ────────────────────────────────────────────────
RTDS_WS_URL = "wss://ws-live-data.polymarket.com"
MARKET_WS_URL = "wss://ws-subscriptions-clob.polymarket.com/ws/market"
BINANCE_WS_URL = "wss://stream.binance.com:9443/ws/btcusdt@bookTicker"
GAMMA_API_URL = "https://gamma-api.polymarket.com"

# ── Timing ───────────────────────────────────────────────────
RTDS_PING_S = 5
MARKET_PING_S = 10
RECONNECT_BASE_S = 1
RECONNECT_MAX_S = 30
WINDOW_PERIOD_S = 300  # 5 min
DISCOVERY_RETRY_S = 15
STATS_INTERVAL_S = 60

# ── SSL (no cert verify, matches C++ engine) ─────────────────
SSL_CTX = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
SSL_CTX.check_hostname = False
SSL_CTX.verify_mode = ssl.CERT_NONE


# ─────────────────────────────────────────────────────────────
# Daily-rotating JSONL writer
# ─────────────────────────────────────────────────────────────

class RotatingWriter:
    """Append-only, daily-rotating (midnight UTC), flush-per-write JSONL."""

    def __init__(self, base_dir: str, subdir: str):
        self.dir = Path(base_dir) / subdir
        self.dir.mkdir(parents=True, exist_ok=True)
        self._f = None
        self._date: str | None = None
        self.count = 0

    def write(self, recv_ms: int, raw: str):
        today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        if today != self._date:
            self._rotate(today)
        self._f.write(f"{recv_ms}\t{raw.rstrip(chr(10) + chr(13))}\n")
        self._f.flush()
        self.count += 1

    def _rotate(self, today: str):
        self.close()
        path = self.dir / f"{today}.jsonl"
        self._f = open(path, "a", encoding="utf-8")
        self._date = today
        logging.getLogger("writer").info(f"Opened {path}")

    def close(self):
        if self._f:
            self._f.flush()
            self._f.close()
            self._f = None


# ─────────────────────────────────────────────────────────────
# Market discovery (gamma API)
# ─────────────────────────────────────────────────────────────

def _discover_sync(window_ts: int) -> dict | None:
    """Blocking HTTPS call to gamma API. Returns market dict or None."""
    url = f"{GAMMA_API_URL}/events?slug=btc-updown-5m-{window_ts}"
    req = urllib.request.Request(url, headers={
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        "Accept": "application/json",
    })
    try:
        with urllib.request.urlopen(req, timeout=10, context=SSL_CTX) as resp:
            events = json.loads(resp.read())
    except Exception:
        return None
    if not events:
        return None
    for event in events:
        for mkt in event.get("markets", []):
            cond = mkt.get("conditionId")
            if not cond:
                continue
            outcomes = mkt.get("outcomes", [])
            up_i, dn_i = 0, 1
            for i, o in enumerate(outcomes):
                if o == "Up":
                    up_i = i
                elif o == "Down":
                    dn_i = i
            toks = mkt.get("clobTokenIds", [])
            if isinstance(toks, str):
                toks = json.loads(toks)
            if len(toks) > max(up_i, dn_i):
                return {
                    "condition_id": cond,
                    "token_up": toks[up_i],
                    "token_down": toks[dn_i],
                    "closed": mkt.get("closed", False),
                }
    return None


async def discover_market(window_ts: int) -> dict | None:
    """Async wrapper — runs blocking discovery in thread executor."""
    return await asyncio.get_running_loop().run_in_executor(
        None, _discover_sync, window_ts)


def current_window() -> int:
    """Current 5-min window start as unix seconds."""
    return (int(time.time()) // WINDOW_PERIOD_S) * WINDOW_PERIOD_S


# ─────────────────────────────────────────────────────────────
# RTDS recorder
# ─────────────────────────────────────────────────────────────

def _is_btc_rtds(msg: str) -> bool:
    """Return True if this RTDS message is BTC-related and should be recorded."""
    if not msg or msg in ("PONG", "pong"):
        return False
    # Binance BTC updates contain "btcusdt", Chainlink contain "btc/usd"
    if '"btcusdt"' in msg or '"btc/usd"' in msg:
        return True
    # Chainlink topic messages (server-filtered to btc/usd already)
    if '"crypto_prices_chainlink"' in msg:
        return True
    # Messages without a "symbol" field are metadata/initial data — keep them
    if '"symbol"' not in msg:
        return True
    # Has a symbol field but not BTC — skip (ethusdt, solusdt, etc.)
    return False


async def rtds_recorder(writer: RotatingWriter, shutdown: asyncio.Event):
    log = logging.getLogger("rtds")
    # Subscribe to both topics without filters (server-side filters break
    # live streaming). Filter to BTC client-side via _is_btc_rtds().
    sub_msg = json.dumps({
        "action": "subscribe",
        "subscriptions": [
            {"topic": "crypto_prices", "type": "update"},
            {"topic": "crypto_prices_chainlink", "type": "*"},
        ],
    })
    delay = RECONNECT_BASE_S

    while not shutdown.is_set():
        try:
            async with websockets.connect(
                RTDS_WS_URL, ssl=SSL_CTX,
                ping_interval=None, ping_timeout=None,
                max_size=1 << 20, close_timeout=5,
            ) as ws:
                log.info("Connected")
                delay = RECONNECT_BASE_S
                await ws.send(sub_msg)
                log.info("Subscribed: crypto_prices + "
                         "crypto_prices_chainlink (BTC filtered client-side)")

                # Application-level PING every 5s (not WS ping frame)
                async def ping_loop():
                    while True:
                        await asyncio.sleep(RTDS_PING_S)
                        await ws.send("PING")

                ping_task = asyncio.create_task(ping_loop())
                try:
                    async for msg in ws:
                        if isinstance(msg, bytes):
                            msg = msg.decode("utf-8", errors="replace")
                        if not _is_btc_rtds(msg):
                            continue
                        recv_ms = int(time.time() * 1000)
                        try:
                            writer.write(recv_ms, msg)
                        except OSError as e:
                            log.error(f"Write failed: {e}")
                finally:
                    ping_task.cancel()
                    with suppress(asyncio.CancelledError):
                        await ping_task

        except asyncio.CancelledError:
            return
        except Exception as e:
            if shutdown.is_set():
                return
            log.warning(f"Error: {e} — reconnect in {delay}s")
            try:
                await asyncio.wait_for(shutdown.wait(), delay)
                return
            except asyncio.TimeoutError:
                pass
            delay = min(delay * 2, RECONNECT_MAX_S)


# ─────────────────────────────────────────────────────────────
# Market recorder
# ─────────────────────────────────────────────────────────────

async def market_recorder(writer: RotatingWriter, shutdown: asyncio.Event):
    log = logging.getLogger("market")
    delay = RECONNECT_BASE_S

    while not shutdown.is_set():
        # Discover current market
        win = current_window()
        mkt = await discover_market(win)
        if not mkt or mkt.get("closed"):
            mkt = await discover_market(win + WINDOW_PERIOD_S)
        if not mkt:
            log.warning(f"No market found (window={win}). "
                        f"Retrying in {DISCOVERY_RETRY_S}s...")
            try:
                await asyncio.wait_for(shutdown.wait(), DISCOVERY_RETRY_S)
                return
            except asyncio.TimeoutError:
                continue

        tokens = [mkt["token_up"], mkt["token_down"]]
        log.info(f"Discovered: cond={mkt['condition_id'][:24]}... "
                 f"tokens=[{tokens[0][:16]}..., {tokens[1][:16]}...]")

        # Track which window this connection serves
        conn_window = current_window()

        try:
            async with websockets.connect(
                MARKET_WS_URL, ssl=SSL_CTX,
                ping_interval=None, ping_timeout=None,
                max_size=16 << 20, close_timeout=5,
            ) as ws:
                log.info("Connected")
                delay = RECONNECT_BASE_S

                await ws.send(json.dumps({
                    "assets_ids": tokens,
                    "type": "market",
                    "custom_feature_enabled": True,
                }))
                log.info(f"Subscribed to {len(tokens)} tokens")

                # Rotation flag — maintenance sets this to break the read loop
                rotate = asyncio.Event()

                # PING every 10s + detect window change
                async def maintenance():
                    tick = 0
                    while True:
                        await asyncio.sleep(1)
                        tick += 1

                        if tick % MARKET_PING_S == 0:
                            await ws.send("PING")

                        # When window changes, signal a full reconnect
                        if current_window() != conn_window:
                            log.info("Window changed, closing connection "
                                     "for reconnect...")
                            rotate.set()
                            await ws.close()
                            return

                maint_task = asyncio.create_task(maintenance())
                try:
                    async for msg in ws:
                        if isinstance(msg, bytes):
                            msg = msg.decode("utf-8", errors="replace")
                        # Skip PONG heartbeat responses
                        if msg in ("PONG", "pong"):
                            continue
                        recv_ms = int(time.time() * 1000)
                        try:
                            writer.write(recv_ms, msg)
                        except OSError as e:
                            log.error(f"Write failed: {e}")
                finally:
                    maint_task.cancel()
                    with suppress(asyncio.CancelledError):
                        await maint_task

                # If rotation triggered the close, loop back to discover
                # the new market and open a fresh connection
                if rotate.is_set():
                    log.info("Reconnecting with new market tokens...")
                    continue

        except asyncio.CancelledError:
            return
        except Exception as e:
            if shutdown.is_set():
                return
            log.warning(f"Error: {e} — reconnect in {delay}s")
            try:
                await asyncio.wait_for(shutdown.wait(), delay)
                return
            except asyncio.TimeoutError:
                pass
            delay = min(delay * 2, RECONNECT_MAX_S)


# ─────────────────────────────────────────────────────────────
# Binance bookTicker recorder
# ─────────────────────────────────────────────────────────────

async def binance_recorder(writer: RotatingWriter, shutdown: asyncio.Event):
    log = logging.getLogger("binance")
    delay = RECONNECT_BASE_S

    while not shutdown.is_set():
        try:
            async with websockets.connect(
                BINANCE_WS_URL, ssl=SSL_CTX,
                ping_interval=20, ping_timeout=10,
                max_size=1 << 20, close_timeout=5,
            ) as ws:
                log.info("Connected")
                delay = RECONNECT_BASE_S

                async for msg in ws:
                    if isinstance(msg, bytes):
                        msg = msg.decode("utf-8", errors="replace")
                    recv_ms = int(time.time() * 1000)
                    try:
                        writer.write(recv_ms, msg)
                    except OSError as e:
                        log.error(f"Write failed: {e}")

        except asyncio.CancelledError:
            return
        except Exception as e:
            if shutdown.is_set():
                return
            log.warning(f"Error: {e} — reconnect in {delay}s")
            try:
                await asyncio.wait_for(shutdown.wait(), delay)
                return
            except asyncio.TimeoutError:
                pass
            delay = min(delay * 2, RECONNECT_MAX_S)


# ─────────────────────────────────────────────────────────────
# Stats logger (periodic heartbeat so you know it's alive)
# ─────────────────────────────────────────────────────────────

async def stats_logger(writers: dict, shutdown: asyncio.Event):
    log = logging.getLogger("stats")
    while not shutdown.is_set():
        try:
            await asyncio.wait_for(shutdown.wait(), STATS_INTERVAL_S)
            return
        except asyncio.TimeoutError:
            pass
        parts = ", ".join(f"{k}={w.count}" for k, w in writers.items())
        log.info(f"Messages recorded: {parts}")


# ─────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────

async def main():
    ap = argparse.ArgumentParser(
        description="Polymarket BTC WebSocket Recorder")
    ap.add_argument("-o", "--output-dir", default="data",
                    help="Output directory (default: data)")
    args = ap.parse_args()

    log = logging.getLogger("main")
    log.info(f"Recorder starting — output: {Path(args.output_dir).resolve()}")

    shutdown = asyncio.Event()

    # Graceful shutdown via signal (Unix only; Windows uses KeyboardInterrupt)
    if sys.platform != "win32":
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(sig, shutdown.set)

    rtds_w = RotatingWriter(args.output_dir, "rtds")
    market_w = RotatingWriter(args.output_dir, "market")
    binance_w = RotatingWriter(args.output_dir, "binance")

    tasks = [
        asyncio.create_task(rtds_recorder(rtds_w, shutdown)),
        asyncio.create_task(market_recorder(market_w, shutdown)),
        asyncio.create_task(binance_recorder(binance_w, shutdown)),
        asyncio.create_task(stats_logger(
            {"rtds": rtds_w, "market": market_w, "binance": binance_w},
            shutdown)),
    ]

    try:
        await asyncio.gather(*tasks)
    except asyncio.CancelledError:
        pass
    finally:
        rtds_w.close()
        market_w.close()
        binance_w.close()

    log.info(f"Done — rtds={rtds_w.count}, market={market_w.count}, "
             f"binance={binance_w.count} messages")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nRecorder stopped.")
