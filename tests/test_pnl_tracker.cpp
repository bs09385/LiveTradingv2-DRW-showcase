#include <doctest/doctest.h>
#include "common/pnl_tracker.h"

using namespace lt;

TEST_SUITE("PnlTracker") {

TEST_CASE("buy then sell realizes positive PnL") {
    PnlTracker tracker;
    AssetId a("token1");

    tracker.record_fill(a, Side::BID, 4000, qty_from_int(20));  // buy 20 @ 0.40
    CHECK(tracker.realized_pnl() == 0);  // no match yet

    tracker.record_fill(a, Side::ASK, 4500, qty_from_int(20));  // sell 20 @ 0.45
    // PnL = (4500 - 4000) * 20000000 / 10000 = 500 * 20000000 / 10000 = 1000000 ($1.00)
    CHECK(tracker.realized_pnl() == 1000000);
}

TEST_CASE("sell then buy realizes positive PnL (short first)") {
    PnlTracker tracker;
    AssetId a("token1");

    tracker.record_fill(a, Side::ASK, 5000, qty_from_int(10));  // sell 10 @ 0.50
    CHECK(tracker.realized_pnl() == 0);

    tracker.record_fill(a, Side::BID, 4000, qty_from_int(10));  // buy 10 @ 0.40
    // PnL = (5000 - 4000) * 10000000 / 10000 = 1000000 ($1.00)
    CHECK(tracker.realized_pnl() == 1000000);
}

TEST_CASE("partial match: buy 20, sell 5") {
    PnlTracker tracker;
    AssetId a("token1");

    tracker.record_fill(a, Side::BID, 4000, qty_from_int(20));  // buy 20 @ 0.40
    tracker.record_fill(a, Side::ASK, 4500, qty_from_int(5));   // sell 5 @ 0.45
    // PnL = (4500 - 4000) * 5000000 / 10000 = 250000 ($0.25)
    CHECK(tracker.realized_pnl() == 250000);
}

TEST_CASE("FIFO order: buy 10@40, buy 10@45, sell 15") {
    PnlTracker tracker;
    AssetId a("token1");

    tracker.record_fill(a, Side::BID, 4000, qty_from_int(10));  // buy 10 @ 0.40
    tracker.record_fill(a, Side::BID, 4500, qty_from_int(10));  // buy 10 @ 0.45
    tracker.record_fill(a, Side::ASK, 5000, qty_from_int(15));  // sell 15 @ 0.50

    // Match FIFO: 10 @ 0.40 → PnL = (5000-4000)*10M/10000 = 1000000
    //             5 @ 0.45 → PnL = (5000-4500)*5M/10000 = 250000
    // Total = 1250000 ($1.25)
    CHECK(tracker.realized_pnl() == 1250000);
}

TEST_CASE("negative PnL: buy high sell low") {
    PnlTracker tracker;
    AssetId a("token1");

    tracker.record_fill(a, Side::BID, 6000, qty_from_int(10));  // buy 10 @ 0.60
    tracker.record_fill(a, Side::ASK, 4000, qty_from_int(10));  // sell 10 @ 0.40
    // PnL = (4000 - 6000) * 10000000 / 10000 = -2000000 (-$2.00)
    CHECK(tracker.realized_pnl() == -2000000);
}

TEST_CASE("settlement at 10000 (UP wins): long position profits") {
    PnlTracker tracker;
    AssetId a("token1");

    tracker.record_fill(a, Side::BID, 4000, qty_from_int(20));  // buy 20 @ 0.40
    tracker.settle_asset(a, 10000);  // UP wins → $1.00

    // PnL = (10000 - 4000) * 20000000 / 10000 = 12000000 ($12.00)
    CHECK(tracker.realized_pnl() == 12000000);
}

TEST_CASE("settlement at 0 (DOWN wins): long position loses") {
    PnlTracker tracker;
    AssetId a("token1");

    tracker.record_fill(a, Side::BID, 4000, qty_from_int(20));  // buy 20 @ 0.40
    tracker.settle_asset(a, 0);  // DOWN wins → $0.00

    // PnL = (0 - 4000) * 20000000 / 10000 = -8000000 (-$8.00)
    CHECK(tracker.realized_pnl() == -8000000);
}

TEST_CASE("settlement is idempotent") {
    PnlTracker tracker;
    AssetId a("token1");

    tracker.record_fill(a, Side::BID, 5000, qty_from_int(10));
    tracker.settle_asset(a, 10000);
    int64_t pnl1 = tracker.realized_pnl();
    tracker.settle_asset(a, 10000);  // second settle — should be no-op
    CHECK(tracker.realized_pnl() == pnl1);
}

TEST_CASE("cross-market accumulation") {
    PnlTracker tracker;
    AssetId a1("market1_up");
    AssetId a2("market2_up");

    tracker.record_fill(a1, Side::BID, 4000, qty_from_int(10));
    tracker.record_fill(a1, Side::ASK, 5000, qty_from_int(10));
    // Market 1 PnL: +$1.00

    tracker.record_fill(a2, Side::BID, 6000, qty_from_int(5));
    tracker.record_fill(a2, Side::ASK, 7000, qty_from_int(5));
    // Market 2 PnL: +$0.50

    // Total = $1.50
    CHECK(tracker.realized_pnl() == 1500000);
}

TEST_CASE("reverse fill exactly undoes realized PnL") {
    PnlTracker tracker;
    AssetId a("token1");

    tracker.record_fill(a, Side::BID, 4000, qty_from_int(10));
    int64_t sell_pnl = tracker.record_fill(a, Side::ASK, 5000, qty_from_int(10));
    CHECK(tracker.realized_pnl() == 1000000);  // +$1.00
    CHECK(sell_pnl == 1000000);

    // Reverse the sell (trade FAILED) — subtracts the exact PnL delta
    tracker.reverse_fill(sell_pnl);
    CHECK(tracker.realized_pnl() == 0);  // back to $0.00
}

TEST_CASE("reverse fill with multiple trades preserves other PnL") {
    PnlTracker tracker;
    AssetId a("token1");

    // Trade 1: buy 10 @ 0.40, sell 10 @ 0.50 → +$1.00
    tracker.record_fill(a, Side::BID, 4000, qty_from_int(10));
    int64_t t1_pnl = tracker.record_fill(a, Side::ASK, 5000, qty_from_int(10));
    CHECK(t1_pnl == 1000000);

    // Trade 2: buy 5 @ 0.30, sell 5 @ 0.60 → +$1.50
    tracker.record_fill(a, Side::BID, 3000, qty_from_int(5));
    int64_t t2_pnl = tracker.record_fill(a, Side::ASK, 6000, qty_from_int(5));
    CHECK(t2_pnl == 1500000);
    CHECK(tracker.realized_pnl() == 2500000);  // $2.50

    // Trade 1 fails on-chain — reverse only its PnL
    tracker.reverse_fill(t1_pnl);
    CHECK(tracker.realized_pnl() == 1500000);  // Trade 2's $1.50 preserved
}

TEST_CASE("fractional shares: buy 20, sell 0.6") {
    PnlTracker tracker;
    AssetId a("token1");

    tracker.record_fill(a, Side::BID, 4000, qty_from_int(20));
    // sell 0.6 shares = 600000 micro-shares
    tracker.record_fill(a, Side::ASK, 4500, 600000);
    // PnL = (4500 - 4000) * 600000 / 10000 = 30000 ($0.03)
    CHECK(tracker.realized_pnl() == 30000);
}

TEST_CASE("slot close: mid >= 0.50 snaps to $1 settlement") {
    PnlTracker tracker;
    AssetId a("token1");

    // Net long after trading
    tracker.record_fill(a, Side::BID, 4000, qty_from_int(20));  // buy 20 @ 0.40
    CHECK(tracker.realized_pnl() == 0);

    // Mid >= 0.50 → snap to $1.00 (kPriceScale)
    tracker.settle_asset(a, 10000);
    // PnL = (10000 - 4000) * 20M / 10000 = 12000000 ($12.00)
    CHECK(tracker.realized_pnl() == 12000000);
}

TEST_CASE("slot close: mid < 0.50 snaps to $0 settlement") {
    PnlTracker tracker;
    AssetId a("token1");

    // Net long after trading — market crashes, mid < 0.50
    tracker.record_fill(a, Side::BID, 4700, qty_from_int(20));  // buy 20 @ 0.47
    CHECK(tracker.realized_pnl() == 0);

    // Mid < 0.50 → snap to $0
    tracker.settle_asset(a, 0);
    // PnL = (0 - 4700) * 20M / 10000 = -9400000 (-$9.40)
    CHECK(tracker.realized_pnl() == -9400000);
}

TEST_CASE("slot close: short position settled at $1 loses") {
    PnlTracker tracker;
    AssetId a("token1");

    // Net short after trading
    tracker.record_fill(a, Side::ASK, 6000, qty_from_int(10));  // sell 10 @ 0.60
    CHECK(tracker.realized_pnl() == 0);

    // UP wins → settle at $1.00
    tracker.settle_asset(a, 10000);
    // PnL = (6000 - 10000) * 10M / 10000 = -4000000 (-$4.00)
    CHECK(tracker.realized_pnl() == -4000000);
}

// ---------------------------------------------------------------------------
// Cross-asset paired matching (dual-bid model)
// ---------------------------------------------------------------------------

TEST_CASE("paired match: buy UP + buy DOWN realizes spread PnL") {
    PnlTracker tracker;
    AssetId up("up1");
    AssetId down("down1");
    tracker.register_pair(up, down);

    tracker.record_fill(up, Side::BID, 5200, qty_from_int(10));   // buy 10 UP @ 0.52
    CHECK(tracker.realized_pnl() == 0);  // no match yet

    tracker.record_fill(down, Side::BID, 4600, qty_from_int(10)); // buy 10 DOWN @ 0.46
    // PnL = (10000 - 5200 - 4600) * 10000000 / 10000 = 200 * 10000000 / 10000 = 200000 ($0.20)
    CHECK(tracker.realized_pnl() == 200000);
}

TEST_CASE("paired match: partial cross-match") {
    PnlTracker tracker;
    AssetId up("up1");
    AssetId down("down1");
    tracker.register_pair(up, down);

    tracker.record_fill(up, Side::BID, 5200, qty_from_int(10));   // buy 10 UP @ 0.52
    tracker.record_fill(down, Side::BID, 4600, qty_from_int(5));  // buy 5 DOWN @ 0.46
    // Only 5 matched: PnL = (10000 - 5200 - 4600) * 5000000 / 10000 = 100000 ($0.10)
    CHECK(tracker.realized_pnl() == 100000);
}

TEST_CASE("paired match: negative spread loses money") {
    PnlTracker tracker;
    AssetId up("up1");
    AssetId down("down1");
    tracker.register_pair(up, down);

    tracker.record_fill(up, Side::BID, 5500, qty_from_int(10));   // buy 10 UP @ 0.55
    tracker.record_fill(down, Side::BID, 4800, qty_from_int(10)); // buy 10 DOWN @ 0.48
    // PnL = (10000 - 5500 - 4800) * 10000000 / 10000 = -300 * 10000000 / 10000 = -300000 (-$0.30)
    CHECK(tracker.realized_pnl() == -300000);
}

TEST_CASE("paired match: FIFO order preserved") {
    PnlTracker tracker;
    AssetId up("up1");
    AssetId down("down1");
    tracker.register_pair(up, down);

    // Two UP buys at different prices
    tracker.record_fill(up, Side::BID, 5000, qty_from_int(10));   // buy 10 UP @ 0.50
    tracker.record_fill(up, Side::BID, 5200, qty_from_int(10));   // buy 10 UP @ 0.52

    // DOWN buy matches FIFO against first UP (5000), then second (5200)
    tracker.record_fill(down, Side::BID, 4600, qty_from_int(15)); // buy 15 DOWN @ 0.46
    // Match 10 vs UP@5000: (10000-5000-4600)*10M/10000 = 400*10M/10000 = 400000
    // Match 5 vs UP@5200: (10000-5200-4600)*5M/10000 = 200*5M/10000 = 100000
    // Total: 500000 ($0.50)
    CHECK(tracker.realized_pnl() == 500000);
}

TEST_CASE("paired match: settlement of unmatched remainder") {
    PnlTracker tracker;
    AssetId up("up1");
    AssetId down("down1");
    tracker.register_pair(up, down);

    tracker.record_fill(up, Side::BID, 5200, qty_from_int(10));
    tracker.record_fill(down, Side::BID, 4600, qty_from_int(5));
    // 5 matched: +$0.10
    CHECK(tracker.realized_pnl() == 100000);

    // Settle remaining 5 UP longs at $1
    tracker.settle_asset(up, 10000);
    // (10000 - 5200) * 5000000 / 10000 = 4800 * 5M / 10000 = 2400000
    // Total: 100000 + 2400000 = 2500000
    CHECK(tracker.realized_pnl() == 2500000);
}

TEST_CASE("paired match: works in either direction") {
    PnlTracker tracker;
    AssetId up("up1");
    AssetId down("down1");
    tracker.register_pair(up, down);

    // DOWN fills first, then UP triggers cross-match
    tracker.record_fill(down, Side::BID, 4600, qty_from_int(10));
    CHECK(tracker.realized_pnl() == 0);

    tracker.record_fill(up, Side::BID, 5200, qty_from_int(10));
    // PnL = (10000 - 5200 - 4600) * 10M / 10000 = 200000 ($0.20)
    CHECK(tracker.realized_pnl() == 200000);
}

}  // TEST_SUITE
