#pragma once

#include "Config.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

// ---------------------------------------------------------------------------
// RiskManager — the gate between the calculator and a FIRE decision.
//
// Every potential FIRE must pass evaluate() first. The checks run in a
// fixed order and the FIRST failure short-circuits with a BLOCKED_ reason:
//
//   0. Cooldown          fired < cooldown_ms ago       → BLOCKED_COOLDOWN
//   1. Ghost filter      gross > ghost_threshold_pct  → BLOCKED_GHOST
//   2. Profit threshold  net   < min_profit_pct       → BLOCKED_BELOW_THRESHOLD
//   3. Minimum net       net   < min_net_pct          → BLOCKED_MIN_NET
//   4. Daily loss limit  daily_pnl < -max_daily_loss  → BLOCKED_DAILY_LOSS
//   5. Order book depth  available < trade*multiplier → BLOCKED_DEPTH
//
// The manager also tracks running paper/live P&L so the daily-loss circuit
// breaker and the heartbeat have something to read. evaluate() runs on the feed
// thread while record_trade_result()/reset() run on the execution thread, so the
// running P&L counters are guarded by a small mutex; the gate config is immutable
// after construction and needs no lock.
// ---------------------------------------------------------------------------

enum class RiskResult {
    APPROVED,
    BLOCKED_COOLDOWN,         // fired on this triangle within cooldown_ms
    BLOCKED_GHOST,            // gross > ghost_threshold
    BLOCKED_BELOW_THRESHOLD,  // predicted net < min_profit_threshold (the hard floor)
    BLOCKED_MIN_NET,          // net < min_net_threshold
    BLOCKED_DAILY_LOSS,       // daily loss limit breached
    BLOCKED_DEPTH,            // insufficient order book depth
};

struct RiskDecision {
    RiskResult  result;
    std::string reason;   // human readable, for logging
};

// Short machine token used in the RISK_BLOCK log line (e.g. "ghost_filter").
const char* risk_reason_tag(RiskResult result);

class RiskManager {
public:
    explicit RiskManager(const BotConfig& config);

    // Called before every potential FIRE. Returns APPROVED or a BLOCKED_ reason.
    // best_bid/ask + qty are the top of book on the first leg (outer/USDT).
    RiskDecision evaluate(
        const std::string& triangle_id,  // e.g. "ATOM-BTC-USDT REV"
        double gross_pct,                 // e.g. 0.3131
        double net_pct,                   // e.g. 0.0381
        double best_bid,                  // top-of-book bid on first leg
        double best_ask,                  // top-of-book ask on first leg
        double best_bid_qty,              // quantity available at best bid
        double best_ask_qty,              // quantity available at best ask
        int64_t now_ms);                  // current time, ms since epoch

    // Called the instant a signal is APPROVED and handed to the execution
    // queue — records the fire time so the next evaluate() on this triangle
    // is gated by the cooldown. Must be called synchronously on the same
    // thread as evaluate(), before the next signal for this triangle can be
    // decided, so a tight burst is still blocked at decision time.
    void record_fire(const std::string& triangle_id, int64_t now_ms);

    // Called by the execution engine after a trade completes or fails.
    // actual_pnl_usdt: positive = profit, negative = loss.
    void record_trade_result(double actual_pnl_usdt);

    // Called at midnight (UTC) to reset the daily loss counter and tallies.
    void reset_daily_loss();

    // For heartbeat logging (read from the feed thread).
    double get_daily_pnl()    const { std::lock_guard<std::mutex> lk(stats_mtx_); return daily_pnl_usdt; }
    int    get_trades_today() const { std::lock_guard<std::mutex> lk(stats_mtx_); return trades_today; }
    int    get_blocked_today() const { std::lock_guard<std::mutex> lk(stats_mtx_); return blocked_today; }

private:
    // Config values (loaded once at startup).
    double ghost_threshold_pct;    // default 1.0
    double min_profit_pct;         // hard floor on predicted net%, from min_profit_threshold (e.g. 0.35)
    double min_net_pct;            // default 0.02
    double max_daily_loss_usdt;    // default 50.0
    double trade_size_usdt;        // from config
    double min_depth_multiplier;   // default 3.0 (need 3x trade size)
    int64_t cooldown_ms;           // default 2000 (see config.risk.cooldown_ms)

    // Runtime state, shared between the feed and execution threads.
    mutable std::mutex stats_mtx_;
    double daily_pnl_usdt = 0.0;
    int    trades_today   = 0;
    int    blocked_today  = 0;
    std::unordered_map<std::string, int64_t> last_fired_ms_;  // triangle+dir -> last fire time
};
