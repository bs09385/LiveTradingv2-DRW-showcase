# LiveTradingv2

> Solo project. I'm really into prediction markets and Polymarket's
> short-dated BTC markets are the corner I find most interesting, so I
> built my own stack to trade them, from the WebSocket framing up to
> the WPF dashboard. I'm primarily a researcher, not a C++ developer by
> background, and the whole point of building this myself was so the
> infra wouldn't get in the way later: whatever strategy I want to test
> or run, the plumbing is already there. It does have the things you
> actually need to be competitive on this venue (TCP_NODELAY, kept-warm
> sockets with periodic heartbeats, HTTP/2 stream multiplexing, dedicated
> cancel connections, EIP-712 signing offloaded onto its own pre-warmed
> path) but I also went overboard in a few places (thread pinning,
> cycle-batched metrics) that the venue doesn't really need. I just enjoy
> that side of things and figured doing it properly here was a good way
> to learn.

A C++20 low-latency market-making engine for Polymarket prediction markets,
built from scratch as a single-developer project. Quotes Polymarket's
short-dated BTC up/down binary markets against a live Binance Spot
reference price, with a WPF control UI and a 24/7 binary data recorder.

This repository is a **showcase snapshot** of the codebase right before
the Polymarket CLOB v2 migration started. It is not maintained here.

---

## UI

<img width="665" height="840" alt="image" src="https://github.com/user-attachments/assets/02fab05b-a4ad-4ca2-9feb-ccc26953bb08" />
Ladders
<img width="1179" height="1007" alt="image" src="https://github.com/user-attachments/assets/41f542bd-ab77-49c2-890c-d36a4f1939ab" />
Dashboard (note the data on this dashboard is fabricated for the purpose of displaying the format and current features)


## The market

The engine targets Polymarket's short-dated **BTC up/down** binary markets:
"will BTC be higher or lower over the next short interval?" Each market
is a pair of complementary outcome tokens (UP and DOWN) priced in `[0, 1]`,
and the firm reference for fair value is the live BTC/USDT mid from
Binance Spot. Settlement is via Chainlink's BTC/USD data stream.

That problem shape is what every design choice in this repo serves: a
short-life binary market with a faster external reference price, and a
pair of complementary outcomes whose inventory you can split, merge, or
redeem on-chain. Whether you want to be a maker or taker, this engine supports that.

## Strategy (not published)

The full quoting logic has been replaced with a boilerplate stub in
`src/scheduler/quoter_v2_strategy.{h,cpp}`. The strategy I run on top of
this is based on:

- A fair-value estimate built from the Binance reference and the
  Polymarket orderbook.
- Laddered quotes around fair value.
- Dynamic skew.
- Reaction to Binance price moves: when the reference shifts, requote
  before the Polymarket book catches up. Polymarket imposes a ~250 ms
  delay on taker orders, while a cancel round-trip is ~15 ms RTT, so
  there's a real window to pull stale quotes before takers can hit them.
  (P99 is a different story, Polymarket's own infra latency takes care
  of widening that distribution all on its own.)
- Inventory rules: hard caps that cancel-and-flatten via FAK, soft caps
  that bias new quotes, and on-chain split / merge / redeem of CTF
  tokens to recycle inventory.

## Design philosophy

Several subsystems aren't required by the strategy I currently run on top
of it. They're there so research isn't blocked by infrastructure later.
For example:

- **RTDS** (Polymarket's Chainlink price feed) is wired up but not
  currently consumed by the strategy. If a future variant needed precise
  strike-price tracking, say quoting near a known resolution oracle
  rather than against Binance, RTDS is already a first-class event source
  feeding T2.
- **The rotation coordinator** existed before the slot manager subsumed
  most of its responsibility. Both are still in the tree because each
  expresses a different model (single-market rolling vs. portfolio).
- **The binary recorder** is built in but not currently active. I run a
  separate personal recording setup outside this engine, so the in-tree
  recorder is dormant. It's still wired up end-to-end (drains its own
  SPSC queues, has the framing format and a Parquet conversion tool) so
  it can be turned on later without any plumbing work.

The intent was a base I could do anything with: strategy research,
microstructure analysis, latency optimisation, new venue integration,
without rewiring the plumbing each time.

It's also, candidly, over the top for the actual problem. Polymarket is
not a low-microsecond venue; round-trip is dominated by internet RTT and
the exchange's own response time. Thread pinning, cycle-batched atomics,
and the no-heap-on-hot-path discipline aren't strictly necessary here. I
built it that way because I enjoy optimisation and fine engineering
details, and because doing it properly on a venue with a generous latency
budget is a cheap way to learn the craft. Treat it as practice on a real
workload, not as the minimum viable approach.

The intended deployment target is a single AWS **`c7i.4xlarge`**
(16 vCPU Intel Sapphire Rapids, 32 GB). It's sized to pin the cores that
actually matter (T0, T1, T2, T3) with
enough room to spare, and it's a fixed-performance instance rather than
a burstable / flex type, so there's no risk of CPU credit throttling
mid-session.

## Core components

At the highest level, the system is held together by four pieces:

- **`src/main.cpp`** does the thread wiring, queue construction, and
  shutdown cascade. The whole system fits on one page here.
- **`src/scheduler/strategy_scheduler.cpp`** is the priority event loop.
  It orders USER_WS, EXEC_INTERNAL, MARKET_WS and CONTROL events with
  per-pass work limits, and uses `for_each_market_sorted` for
  deterministic market iteration.
- **`src/rest/http2_client.cpp`** and **`src/exec/execution_gateway.cpp`**
  handle the outbound side: persistent HTTP/2 connection, stream
  multiplexing, redundant cancel pipeline, rate limiting.
- **`src/crypto/eip712.cpp`** does the EIP-712 typed-data signing for
  Polymarket orders, with `tests/test_eip712.cpp` verifying the
  signed-order vectors against Polymarket's official Python and Rust
  reference clients.

---

## What it does

- Subscribes to Polymarket's CLOB market and user WebSocket feeds and
  maintains a local L2 order book for every traded token.
- Subscribes to Binance Spot `bookTicker` for BTC/USDT as a low-latency
  reference price.
- Runs a quoting strategy (`QuoterV2`) that places resting maker orders on
  short-dated BTC up/down markets, requoting on BBO change, fill, cancel,
  or reject.
- Signs orders with EIP-712 / secp256k1 and submits them via a persistent
  HTTP/2 REST connection with stream multiplexing, rate limiting, and
  heartbeats.
- Manages a portfolio of concurrent markets via a slot manager or a
  rotation coordinator with auto-discovery from Polymarket's gamma API.
- Performs on-chain split / merge / redeem of CTF outcome tokens through a
  Polymarket relayer (PROXY wallet flow, EIP-712 signed).
- Records every Polymarket and Binance message to disk in a binary format
  for offline backtesting (Parquet conversion tool included).
- Exposes a 20 Hz state snapshot to a C# / WPF dashboard that also acts
  as the engine launcher. The UI surfaces the standard things you need to
  manage a running strategy and monitor its performance (positions,
  working orders, fills, PnL, mode switches, kill switches), plus live
  ladders for the active markets to help build intuition about current
  microstructure.

---

## Architecture

Single-writer-per-domain. Lock-free SPSC queues between threads. No heap
allocation, no exceptions, no blocking I/O on the hot path.

| Thread | Role |
|--------|------|
| T0 | Polymarket market-data WS ingestion, parsing, order book maintenance |
| T1 | Polymarket user WS, order / trade lifecycle, position tracking |
| T2 | Strategy scheduler, 4-queue priority event loop, quoting, risk |
| T3 | REST execution gateway, order placement, cancels, heartbeats |
| T5 | Async logger, drains per-producer log queues |
| T6 | UI IPC bridge, WS server, 20 Hz snapshot assembly, command routing |
| T7 | Slot manager OR rotation coordinator (mode-dependent) |
| T_binance | Binance Spot WS feed for BTC/USDT reference price |
| T_rtds | Polymarket RTDS crypto-price feed |
| T_rec | Binary data recorder (drains 4 SPSC queues to disk) |

The hot path is a chain of:

```
WS frame -> simdjson parse -> book update -> SPSC push
         -> strategy evaluate -> risk check -> intent emit
         -> REST send -> ack -> SPSC push -> requote
```

T2 drains its inbound queues in strict priority order (USER_WS >
EXEC_INTERNAL > MARKET_WS > CONTROL) with per-pass work limits to prevent
starvation, and iterates markets in a deterministic lexicographic order.

See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the full thread model, queue
topology, state-ownership rules, and hard invariants.

---

## Notable engineering details

- **Dense order book.** Polymarket prices are a fixed grid of 10 001 ticks
  (0.0000 to 1.0000 in 0.0001 steps). Books are stored as a flat
  `std::array<Qty_t, 10001>`, giving O(1) updates and BBO recompute.
- **Fixed-point prices.** `Price_t` is `int32_t` at 10 000x scale;
  arithmetic is integer throughout, no floating point on the hot path.
- **HTTP/2 multiplexing.** A single persistent TLS connection carries
  concurrent order placements and cancels as independent HTTP/2 streams,
  with a configurable in-flight cap. Cancels run on dedicated redundant
  connections kept warm by heartbeats to absorb burst cancel storms.
  Originally HTTP/1.1 with request pipelining; upgraded to HTTP/2 for
  true concurrency without head-of-line blocking.
- **EIP-712 signing.** Custom Keccak-256, secp256k1, and EIP-712 typed-data
  hasher. Signed-order vectors are checksum-verified against the official
  Polymarket Python and Rust reference clients.
- **Polygon Alchemy RPC** for on-chain functionality.
- **Inventory safety.** SELL intents are gated against on-chain token
  balances; the planner converts un-sellable SELLs to BUY-complement at
  `1 - p` on the opposite outcome rather than failing at the gateway.
- **Determinism.** Cycle counters batch all per-cycle metric writes into a
  single relaxed-atomic flush. Market iteration is sorted. Drop policies
  are explicit per queue.
- **Recorder.** A separate thread drains three (later four) recording
  queues to a binary format with framing, designed for 24/7 unattended
  operation. Offline `tools/convert_to_parquet.py` produces Parquet for
  analysis.

---

## Tech stack

C++20, CMake 3.20+, MinGW GCC 13.2 (Windows) and GCC 13+ (Linux).
Dependencies fetched via `FetchContent`:

| Library | Use |
|---------|-----|
| Boost.Asio / Beast 1.87 | TCP, TLS, WebSocket, HTTP/1.1 + HTTP/2 |
| simdjson 3.12 | Hot-path JSON parsing |
| rigtorp/SPSCQueue 1.1 | Lock-free single-producer / single-consumer queues |
| doctest 2.4 | 853 test cases |
| secp256k1 0.6 | ECDSA signing |
| SHA3IUF | Keccak-256 |
| OpenSSL 3 | TLS, HMAC-SHA256 |

UI is a separate .NET 8 / WPF MVVM app under `ui/PolymarketUI/`,
communicating with the engine over a local WebSocket.

---

## Build

### Linux

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### MinGW / MSYS2 (Windows)

```bash
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j$(nproc)
```

## Test

```bash
ctest --test-dir build --output-on-failure --timeout 60
```

Benchmarks are excluded from the default run; opt in with
`ctest --test-dir build -R benchmark_tests --timeout 180`.

## Run

```bash
./build/engine.exe config/default.json accounts/main.json
```

`config/default.json` holds engine parameters (~120 fields).
`accounts/*.json` are gitignored and hold per-account credentials and
on-chain addresses. See `accounts/example.json` for the schema.

---

## Repository layout

```
src/
  ws/         Polymarket market + user WebSocket clients
  parser/     simdjson-based message parsers
  book/       Order book (dense ladder)
  state/      Market and execution state stores
  scheduler/  Priority event loop, QuoterV2 strategy, risk gate, tracker
  exec/       Execution gateway, order builder, inventory safety
  rest/       Persistent HTTP/2 client, L2 HMAC auth
  crypto/     Keccak, HMAC-SHA256, EIP-712, order signer
  events/     Scheduler / market / user POD event types
  queue/      SPSC queue wrapper
  logger/     Async logger, ~150 metric counters
  ui_bridge/  Local WS server, 20 Hz snapshots, command routing, watcher
  slot/       Market slot manager
  rotation/   Rotation coordinator
  inventory/  Relayer client, ABI encoder, PROXY/Safe tx builders
  binance/    Binance Spot WS feed
  rtds/       Polymarket RTDS crypto-price WS feed
  recorder/   Binary data recorder
  common/     Shared types, config, account, discovery
ui/PolymarketUI/   .NET 8 / WPF dashboard
tools/             Parquet converter, ad-hoc utilities
tests/             doctest suite (853 cases)
config/default.json
```

---

## Why it isn't running live right now

Not running live at the moment due to ghost-fill / nonce-abuse exploits
that let a counterparty cause a matched trade to fail if the price
moves against them.

---

## A note on how this was built

I'm a single developer and C++ is not my primary background. My day-to-day
is research, mostly Python, and I picked up C++ specifically for this
project. It was built with AI coding assistance throughout, and I'm
flagging that up front rather than letting it be a question.
