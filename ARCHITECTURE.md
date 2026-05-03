# LiveTradingv2 — Architecture

C++20 low-latency trading engine for Polymarket prediction markets.
Single-writer-per-domain, lock-free queues, zero-allocation hot path.

---

## 1. System Overview

### Thread Model

| Thread | Role | Entry Point | Key Components |
|--------|------|-------------|----------------|
| **T0** | Market data ingestion | `ws_client.run()` in main.cpp | MarketWsClient, MarketMessageParser, MarketStateStore, OrderBooks |
| **T1** | User order/trade lifecycle | `user_ws_client->run()` | UserWsClient, UserMessageParser, ExecStateStore |
| **T2** | Strategy scheduler (priority event loop) | `scheduler.run()` | StrategyScheduler, QuoterV2Strategy, RiskGate, WorkingOrderTracker |
| **T3** | REST execution gateway | `gateway->run()` | ExecutionGateway, RestClient, OrderBuilder, RateLimiter |
| **T5** | Async log writer | `logger.start()` | AsyncLogger (drains per-producer SPSC log queues) |
| **T6** | UI IPC bridge | `ipc_bridge->run()` | IpcBridge (WS server, 20Hz snapshots, command routing, WatcherService) |
| **T7** | Main thread (conditional) | `slot_manager->run()` or `rotation_coordinator->run()` | MarketSlotManager (slot mode) OR RotationCoordinator (rotation mode) |
| **T_rtds** | RTDS crypto price data | `rtds_client->run()` | RtdsWsClient, RtdsMessageParser (crypto_prices -> T2 + T_rec) |
| **T_rec** | Historical data recording | `data_recorder->run()` | DataRecorder (binary writer, drains 3 recording queues) |

### Hot Path

The latency-critical event processing chain:

```
T0: WS frame -> parse (simdjson) -> book update -> SPSC push
T2: SPSC pop -> strategy evaluate -> risk check -> intent emit -> SPSC push to T3
T1: WS frame -> parse -> lifecycle update -> SPSC push to T2
```

Hot path rules: No exceptions. No heap allocation. No blocking I/O. No mutex.
Use ErrorCode returns, pre-allocated buffers, lock-free queues, relaxed atomics only.

### Determinism Guarantees

- T2 drains queues in strict priority order: USER_WS > EXEC_INTERNAL > MARKET_WS > CONTROL
- Per-pass work limits prevent starvation (configurable per queue)
- `for_each_market_sorted()` provides deterministic market iteration (lexicographic AssetId sort)
- CycleCounters batch all atomic metric writes to one flush per cycle

---

## 2. Queue Topology

```
T0 --strategy_queue-----------> T2 --strategy_to_exec_queue--> T3
T0 --ui_book_queue--------------------------------------> T6
T0 --market_rec_queue-----------------------------------> T_rec
T1 --user_queue--------------> T2
T1 --user_rec_queue-------------------------------------> T_rec
T3 --exec_queue--------------> T2
T6 --control_queue-----------> T2
T2 --ui_state_queue-------------------------------------> T6
T7 --slot_queue--------------> T2 (slot mode only)
T0,T1,T2,T3,T6 --log_queues--> T5
T_rtds --rtds_queue-----------> T2
T_rtds --rtds_rec_queue-------> T_rec
```

| Queue | Payload | Cap | Drop Policy |
|-------|---------|-----|-------------|
| strategy_queue | MarketNotification | 65536 | Drop + metric |
| user_queue | SchedulerEvent | 4096 | Spin-retry then FATAL |
| exec_queue | SchedulerEvent | 4096 | Spin-retry; fatal after 1000 drops |
| control_queue | SchedulerEvent | 1024 | Yield-retry until shutdown, then drop + metric |
| strategy_to_exec_queue | ExecIntent | 4096 | Drop + metric |
| ui_book_queue | UiBookUpdate | 256 | Drop + metric |
| ui_state_queue | UiStateSnapshot | 256 | Drop + metric |
| rtds_queue | CryptoPriceUpdate | 4096 | Drop + metric |
| rtds_rec_queue | RtdsRecord | configurable | Drop + metric |
| market_rec_queue | RawWsMessage | 4096 | Drop + metric |
| user_rec_queue | RawWsMessage | 4096 | Drop + metric |
| slot_queue | SchedulerEvent | 64 | Drop + metric |

All queues are `SpscQueue<T>` (rigtorp lock-free, single-producer single-consumer).

**Shutdown cascade**: T0 -> T_rtds -> T6 -> T1 -> T2 -> T3 -> InventoryService -> T_rec -> background -> T5.
Producers stop before their consumers. Logger stops last.

---

## 3. State Ownership Rules

**Single-writer-per-domain. No thread may write state it does not own.**

| Thread | Exclusively Owns | Writes To (queues) | Shared Reads |
|--------|-----------------|-------------------|--------------|
| T0 | MarketStateStore, OrderBooks | strategy_queue, ui_book_queue, market_rec_queue | -- |
| T1 | ExecStateStore, TrackedOrders | user_queue, user_rec_queue, TokenInventory (atomics) | -- |
| T2 | Scheduler, Strategy, RiskGate, Tracker | strategy_to_exec_queue, ui_state_queue | TokenInventory, MarketPairRegistry, Metrics |
| T3 | ExecutionGateway, RestClient, Signer | exec_queue | strategy_to_exec_queue, TokenInventory, Metrics |
| T6 | IpcBridge, WatcherService, snapshot state | control_queue | ui_book/state_queue, Metrics, MarketPairRegistry |
| T7 | MarketSlotManager or RotationCoordinator | slot_queue (slot mode) | -- |

**Frozen after setup**: EngineConfig, MarketPairRegistry (map structure).
**Shared atomics**: TokenInventory (map frozen, values atomic), Metrics (relaxed), g_shutdown/g_fatal.

---

## 4. How to Safely Modify

### Strategy Logic
- **Where**: `src/scheduler/quoter_v2_strategy.h/cpp`, `src/scheduler/quote_planner.h/cpp`
- **Interface**: Implement `Strategy` base class (`src/scheduler/strategy.h`)
- **Pipeline**: Strategy -> QuotePlanner -> RiskGate -> ModeFilteredSink -> ExecQueueSink
- **Rules**: T2-owned. No blocking. No heap alloc in evaluate(). Output is IntentBatch (max 8).
- **Reevaluation**: BBO change, fill, cancel, reject trigger requoting

### Execution Logic
- **Where**: `src/exec/execution_gateway.h/cpp`, `src/exec/order_builder.h/cpp`
- **Transport**: Consumes ExecIntent from strategy_to_exec_queue, publishes ExecFeedback to exec_queue
- **REST**: `src/rest/rest_client.h/cpp` (persistent HTTPS, HTTP/1.1 pipelining), `src/rest/rest_auth.h/cpp` (L2 HMAC)
- **Pipelining**: Decoupled write/read async loops on T3's io_context. `max_pipeline_depth` (default 4) gates
  concurrent in-flight requests. Collapses N requests from ~N*RTT to ~1 RTT. Set to 1 for serial fallback.
- **Crypto**: `src/crypto/` (Keccak-256, EIP-712, secp256k1 signing)
- **Batch orders**: Optional `POST /orders` batching (disabled by default). Accumulates PLACE intents per poll
  cycle up to `batch_max_size` (default 15), sends as JSON array. Per-order inventory check + response parsing.
- **Rules**: T3-owned. Rate-limited. Heartbeat required. SELL must pass inventory check.

### Order Book Logic
- **Where**: `src/book/order_book.h/cpp`, `src/state/market_state_store.h/cpp`
- **Structure**: Dense array `std::array<Qty_t, 10001>` indexed by 10000x price
- **Rules**: T0-owned. O(1) operations. BBO recomputed after every update.
- **DOWN token filter**: MarketStateStore filters book/price_change/tick_size/BBO events for DOWN
  tokens (registered via `register_down_token()`). Only UP orderbooks are maintained. DOWN
  `LastTradePriceEvent` still passes through. WS subscriptions remain for both tokens.
- **Parsing**: `src/parser/market_message_parser.h/cpp` (5 event types)

### UI Logic
- **Where**: `src/ui_bridge/` (C++ server), `ui/PolymarketUI/` (C# WPF client)
- **Data flow**: T0->T6 (books), T2->T6 (state), T6->T2 (commands via control_queue)
- **Rules**: Non-latency-critical (20Hz). Latest-only semantics. Drop-ok queues.
- **Commands**: 6 types in `ui_command_parser.h/cpp`, routed as SchedulerEvent to T2

---

## 5. Hard Invariants

1. **No blocking on hot path** -- T0, T1, T2 never block. No mutex, no condition_variable,
   no blocking I/O. SPSC queues, relaxed atomics, ErrorCode returns only.

2. **No synchronous logging on hot path** -- All logging via per-producer SPSC queues
   to T5. try_push (non-blocking); drop if full.

3. **Fixed-size queues** -- All SPSC queues have fixed capacity at construction.
   No dynamic resizing. Overflow policy varies per queue (see topology table).

4. **SELL requires inventory** -- T3 checks TokenInventory before sending SELL orders
   (`src/exec/inventory_safety.h`). QuotePlanner converts unavailable SELL to BUY complement.

5. **Planner performs complement conversion, not execution** -- QuotePlanner converts
   SELL-unavailable intents to BUY-complement at `10000 - price`. Execution sends
   exactly what it receives. Price transformation happens in T2, not T3.

6. **Single-writer per domain** -- Each mutable state object owned by exactly one thread.
   Cross-thread communication exclusively via SPSC queues or frozen/atomic shared state.

7. **No heap allocation in steady state** -- Containers pre-allocated at startup/warmup.
   IntentBatch fixed 8 slots. AssetId/OrderId/TradeId are stack arrays. LogEntry fixed 256B.
   Exception: T1 ExecStateStore uses unordered_map (not latency-critical).

8. **User events never dropped** -- T1->T2 user_queue uses push_spin() with fatal flag
   on exhaustion. Position/fill data must not be lost.

9. **Shutdown order** -- T0 -> T_rtds -> T6 -> T1 -> T2 -> T3 -> InventoryService -> T_rec -> background -> T5.
   Producers stop before consumers. Logger last.

---

## 6. Feature -> Directory Map

| Feature | Directory | Key Files |
|---------|-----------|-----------|
| Market data ingestion | `src/ws/`, `src/parser/`, `src/book/`, `src/state/` | market_ws_client, market_message_parser, order_book, market_state_store |
| User order lifecycle | `src/ws/`, `src/parser/`, `src/state/` | user_ws_client, user_message_parser, exec_state_store |
| Strategy / quoting | `src/scheduler/` | quoter_v2_strategy, quote_planner, strategy.h |
| Risk management | `src/scheduler/` | risk_gate, mode_filtered_sink, execution_mode.h |
| Working order tracking | `src/scheduler/` | working_order_tracker |
| Order execution (REST) | `src/exec/`, `src/rest/` | execution_gateway, rest_client, order_builder |
| Crypto / signing | `src/crypto/` | eip712, order_signer, keccak, hmac_sha256 |
| Event model | `src/events/` | scheduler_events.h, market_events.h, user_events.h |
| Queue infrastructure | `src/queue/` | spsc_queue.h |
| Logging / metrics | `src/logger/` | async_logger, metrics.h (147 counters) |
| UI bridge (C++) | `src/ui_bridge/` | ipc_bridge, ui_serializer, ui_command_parser |
| BTC ladder watcher | `src/ui_bridge/` | watcher_service, watch_manager, watcher_book_store, series_rolling_fsm, btc_series_registry |
| Market slot manager | `src/slot/` | market_slot_manager, market_slot_types, slot_token_map |
| Market rotation | `src/rotation/` | rotation_coordinator |
| Inventory relayer | `src/inventory/` | abi_encoder, relayer_auth, safe_tx_builder, proxy_tx_builder, relayer_client, inventory_service |
| RTDS crypto prices | `src/rtds/` | rtds_ws_client, rtds_message_parser, rtds_types |
| Data recording | `src/recorder/` | data_recorder, record_types |
| Market discovery | `src/common/` | discovery.h/cpp |
| Account management | `src/common/` | account.h/cpp |
| UI client (C#) | `ui/PolymarketUI/` | EngineConnectionService, ViewModels/, Views/ |
| Shared types | `src/common/` | types.h, error.h, config.h, token_inventory.h, market_pair.h |
| Thread wiring / main | `src/` | main.cpp |
| Configuration | `config/` | default.json (124 fields) |
| Tests | `tests/` | 853 cases (doctest) |
| Build | `cmake/`, root CMakeLists.txt | FetchDependencies.cmake, CompilerWarnings.cmake |
| Parquet converter | `tools/` | convert_to_parquet.py |

---

## 7. Build & Test

**Bash scripts:**
- `build_and_test.sh` — configure, build, run all tests
- `start.sh` — build (if needed) and start the engine
- `deploy.sh` — pull, build, test, restart (for deployment)

**Manual (bash / MSYS2):**
```bash
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j$(nproc)
cd build/tests && \
  PATH="/c/ProgramData/mingw64/mingw64/opt/bin:/c/ProgramData/mingw64/mingw64/bin:$PATH" \
  ./tests.exe
```

**Test stall guardrails (important):**
- `ctest` now defines two targets:
  - `all_tests` (default): excludes `Benchmarks` test suite.
  - `benchmark_tests` (disabled by default): opt-in only.
- Recommended default run:
  - `ctest --test-dir build --output-on-failure --timeout 60`
- Run benchmarks only when explicitly needed:
  - `ctest --test-dir build -R benchmark_tests --output-on-failure --timeout 180`
- If a prior interrupted run left orphan test processes:
  - `Get-Process tests -ErrorAction SilentlyContinue | Stop-Process -Force`

**Run engine:**
```bash
PATH="/c/ProgramData/mingw64/mingw64/opt/bin:/c/ProgramData/mingw64/mingw64/bin:$PATH" \
  ./build/engine.exe config/default.json
```

Toolchain: MinGW GCC 13.2.0, CMake 3.20+, C++20.
Dependencies (FetchContent): Boost 1.87.0, simdjson 3.12.2, rigtorp/SPSCQueue 1.1,
doctest 2.4.11, SHA3IUF, secp256k1 0.6.0. OpenSSL 3.1.1 at system path.

---

## 8. Key Type Conventions

| Type | Size | Description |
|------|------|-------------|
| Price_t | int32_t | 10000x fixed-point. "0.52" = 5200. Ladder index 0..10000. |
| AssetId | 128B stack | FNV-1a hash. No std::string on hot path. |
| OrderId | 80B stack | FNV-1a hash. Polymarket order hashes: 66 chars. |
| TradeId | 48B stack | FNV-1a hash. UUIDs: 36 chars. |
| ErrorCode | enum | 27 codes. Expected<T> = value + ErrorCode. |
| SchedulerEvent | fixed POD | Unified envelope. EventSource = priority enum. |
| IntentBatch | fixed 8 slots | No heap. ExecutionIntent -> ExecIntent -> ExecFeedback (POD chain). |

---

## 9. Platform Notes

- `#undef ERROR` required after Boost.Asio include when using LogLevel::ERROR
- OpenSSL path: `C:/ProgramData/mingw64/mingw64/opt/`
- BUILD_SHARED_LIBS forced OFF (secp256k1 can flip it ON)
- Windows link deps: ws2_32, wsock32, mswsock, bcrypt
