#include "RiskManager.h"
#include "Config.h"

#include <gtest/gtest.h>

// A BotConfig carrying the documented default risk gates:
//   ghost 1.0%   min_net 0.02%   max_daily_loss $50   trade_size $100   depth 3x
//   cooldown 2000ms
// required depth = trade_size * multiplier = 100 * 3 = $300.
static BotConfig make_config() {
    BotConfig cfg;
    cfg.risk.ghost_threshold_pct  = 1.0;
    cfg.risk.min_net_pct          = 0.02;
    cfg.risk.max_daily_loss_usdt  = 50.0;
    cfg.risk.trade_size_usdt      = 100.0;
    cfg.risk.min_depth_multiplier = 3.0;
    cfg.risk.cooldown_ms          = 2000;
    // Hold the profit-threshold gate effectively open (0.0001 -> 0.01%) so the
    // other gates can be tested in isolation; BelowThreshold has its own tests.
    cfg.risk.min_profit_threshold = 0.0001;
    cfg.mode = "paper";
    return cfg;
}

// Arbitrary baseline timestamp (ms) for tests that don't care about cooldown.
constexpr int64_t kBaseTs = 1'000'000;

// Deep order book (≫ $300) so the depth check never trips unless a test wants
// it to: best_ask 1.0 * qty 1000 = $1000 available.
static RiskDecision eval_default(RiskManager& rm, double gross, double net,
                                  int64_t now_ms = kBaseTs) {
    return rm.evaluate("ATOM-BTC-USDT", gross, net,
                       /*bid*/ 1.0, /*ask*/ 1.0,
                       /*bid_qty*/ 1000.0, /*ask_qty*/ 1000.0,
                       now_ms);
}

// 1. Ghost filter: gross 2.3039% → BLOCKED_GHOST
TEST(RiskManager, GhostFilterBlocksHighGross) {
    RiskManager rm(make_config());
    auto d = eval_default(rm, 2.3039, 2.0289);
    EXPECT_EQ(d.result, RiskResult::BLOCKED_GHOST);
}

// 2. Ghost filter: gross 0.9999% → passes ghost (here: approved overall)
TEST(RiskManager, GhostFilterPassesBelowThreshold) {
    RiskManager rm(make_config());
    auto d = eval_default(rm, 0.9999, 0.5);
    EXPECT_NE(d.result, RiskResult::BLOCKED_GHOST);
    EXPECT_EQ(d.result, RiskResult::APPROVED);
}

// 3. Min net: net 0.0191% → BLOCKED_MIN_NET
TEST(RiskManager, MinNetBlocksTinyNet) {
    RiskManager rm(make_config());
    auto d = eval_default(rm, 0.2941, 0.0191);
    EXPECT_EQ(d.result, RiskResult::BLOCKED_MIN_NET);
}

// 4. Min net: net 0.0208% → passes min net
TEST(RiskManager, MinNetPassesAtThreshold) {
    RiskManager rm(make_config());
    auto d = eval_default(rm, 0.30, 0.0208);
    EXPECT_NE(d.result, RiskResult::BLOCKED_MIN_NET);
    EXPECT_EQ(d.result, RiskResult::APPROVED);
}

// 5. Daily loss: record -$51 → next trade BLOCKED_DAILY_LOSS
TEST(RiskManager, DailyLossBlocksAfterLimit) {
    RiskManager rm(make_config());
    rm.record_trade_result(-51.0);
    auto d = eval_default(rm, 0.30, 0.10);
    EXPECT_EQ(d.result, RiskResult::BLOCKED_DAILY_LOSS);
}

// 6. Daily loss: record -$49 → next trade approved
TEST(RiskManager, DailyLossAllowsUnderLimit) {
    RiskManager rm(make_config());
    rm.record_trade_result(-49.0);
    auto d = eval_default(rm, 0.30, 0.10);
    EXPECT_EQ(d.result, RiskResult::APPROVED);
}

// 7. Daily reset: record -$51 → reset → next trade approved
TEST(RiskManager, DailyResetClearsLoss) {
    RiskManager rm(make_config());
    rm.record_trade_result(-51.0);
    rm.reset_daily_loss();
    auto d = eval_default(rm, 0.30, 0.10);
    EXPECT_EQ(d.result, RiskResult::APPROVED);
    EXPECT_DOUBLE_EQ(rm.get_daily_pnl(), 0.0);
    EXPECT_EQ(rm.get_trades_today(), 0);
}

// 8. Depth: available $299, need $300 → BLOCKED_DEPTH
TEST(RiskManager, DepthBlocksThinBook) {
    RiskManager rm(make_config());
    // ask 1.0 * qty 299 = $299 available < $300 required.
    auto d = rm.evaluate("ATOM-BTC-USDT", 0.30, 0.10,
                         1.0, 1.0, 1000.0, 299.0, kBaseTs);
    EXPECT_EQ(d.result, RiskResult::BLOCKED_DEPTH);
}

// 9. Depth: available $301, need $300 → passes depth
TEST(RiskManager, DepthPassesDeepBook) {
    RiskManager rm(make_config());
    auto d = rm.evaluate("ATOM-BTC-USDT", 0.30, 0.10,
                         1.0, 1.0, 1000.0, 301.0, kBaseTs);
    EXPECT_NE(d.result, RiskResult::BLOCKED_DEPTH);
    EXPECT_EQ(d.result, RiskResult::APPROVED);
}

// 10. All checks pass → APPROVED
TEST(RiskManager, AllChecksPassApproved) {
    RiskManager rm(make_config());
    auto d = eval_default(rm, 0.4249, 0.1499);
    EXPECT_EQ(d.result, RiskResult::APPROVED);
}

// 11. Ghost fires before min net (order matters)
TEST(RiskManager, GhostTakesPrecedenceOverMinNet) {
    RiskManager rm(make_config());
    // Both ghost (gross > 1.0) and min-net (net < 0.02) would trip; ghost wins.
    auto d = eval_default(rm, 2.3039, 0.0191);
    EXPECT_EQ(d.result, RiskResult::BLOCKED_GHOST);
}

// 12. Daily loss fires before depth (order matters)
TEST(RiskManager, DailyLossTakesPrecedenceOverDepth) {
    RiskManager rm(make_config());
    rm.record_trade_result(-51.0);
    // Thin book ($50) would trip depth, but daily-loss is checked first.
    auto d = rm.evaluate("ATOM-BTC-USDT", 0.30, 0.10,
                         1.0, 1.0, 1000.0, 50.0, kBaseTs);
    EXPECT_EQ(d.result, RiskResult::BLOCKED_DAILY_LOSS);
}

// A config whose profit-threshold gate is the documented 0.35% (0.0035 ratio).
static BotConfig make_config_threshold() {
    BotConfig cfg = make_config();
    cfg.risk.min_profit_threshold = 0.0035;   // 0.35%
    return cfg;
}

// 13. Profit threshold: net 0.15% (< 0.35%) → BLOCKED_BELOW_THRESHOLD
TEST(RiskManager, BelowThresholdBlocksMarginalNet) {
    RiskManager rm(make_config_threshold());
    auto d = eval_default(rm, 0.45, 0.15);
    EXPECT_EQ(d.result, RiskResult::BLOCKED_BELOW_THRESHOLD);
}

// 14. Profit threshold: net 0.40% (above the 0.35% floor) → passes the gate
TEST(RiskManager, BelowThresholdPassesAboveFloor) {
    RiskManager rm(make_config_threshold());
    auto d = eval_default(rm, 0.70, 0.40);
    EXPECT_NE(d.result, RiskResult::BLOCKED_BELOW_THRESHOLD);
    EXPECT_EQ(d.result, RiskResult::APPROVED);
}

// 15. Ghost fires before profit threshold (order matters)
TEST(RiskManager, GhostTakesPrecedenceOverBelowThreshold) {
    RiskManager rm(make_config_threshold());
    // gross 2.3% is a ghost AND net 0.15% is below floor; ghost wins.
    auto d = eval_default(rm, 2.3039, 0.15);
    EXPECT_EQ(d.result, RiskResult::BLOCKED_GHOST);
}

// 16. Profit threshold fires before min-net (order matters)
TEST(RiskManager, BelowThresholdTakesPrecedenceOverMinNet) {
    RiskManager rm(make_config_threshold());
    // net 0.01% trips both the 0.35% floor and the 0.02% min-net; floor wins.
    auto d = eval_default(rm, 0.30, 0.01);
    EXPECT_EQ(d.result, RiskResult::BLOCKED_BELOW_THRESHOLD);
}

// 17. Cooldown: re-firing the same triangle within cooldown_ms → BLOCKED_COOLDOWN
TEST(RiskManager, CooldownBlocksSameTriangleWithinWindow) {
    RiskManager rm(make_config());  // cooldown_ms = 2000
    rm.record_fire("ATOM-BTC-USDT", kBaseTs);
    auto d = eval_default(rm, 0.4249, 0.1499, kBaseTs + 500);  // 500ms later
    EXPECT_EQ(d.result, RiskResult::BLOCKED_COOLDOWN);
}

// 18. Cooldown: a different triangle within the same window is unaffected
TEST(RiskManager, CooldownDoesNotAffectOtherTriangles) {
    RiskManager rm(make_config());
    rm.record_fire("ATOM-BTC-USDT", kBaseTs);
    auto d = rm.evaluate("NEAR-BTC-USDT REV", 0.4249, 0.1499,
                         1.0, 1.0, 1000.0, 1000.0, kBaseTs + 500);
    EXPECT_EQ(d.result, RiskResult::APPROVED);
}

// 19. Cooldown: same triangle after cooldown_ms has elapsed → approved again
TEST(RiskManager, CooldownClearsAfterWindowElapses) {
    RiskManager rm(make_config());  // cooldown_ms = 2000
    rm.record_fire("ATOM-BTC-USDT", kBaseTs);
    auto d = eval_default(rm, 0.4249, 0.1499, kBaseTs + 2000);  // exactly at the edge
    EXPECT_NE(d.result, RiskResult::BLOCKED_COOLDOWN);
    EXPECT_EQ(d.result, RiskResult::APPROVED);
}

// 20. Cooldown fires before the ghost filter (order matters — cheapest check first)
TEST(RiskManager, CooldownTakesPrecedenceOverGhost) {
    RiskManager rm(make_config());
    rm.record_fire("ATOM-BTC-USDT", kBaseTs);
    // gross 2.3% would trip the ghost filter, but cooldown is checked first.
    auto d = eval_default(rm, 2.3039, 0.1499, kBaseTs + 500);
    EXPECT_EQ(d.result, RiskResult::BLOCKED_COOLDOWN);
}
