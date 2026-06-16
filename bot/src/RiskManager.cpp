#include "RiskManager.h"

#include <iomanip>
#include <sstream>

namespace {

// Print a double as an integer when it is whole ("3", "100"), otherwise with
// two decimals ("3.50"). Keeps "(3x $100)" tidy without hard-coding ints.
std::string trim_num(double v) {
    std::ostringstream os;
    if (v == static_cast<long long>(v)) {
        os << static_cast<long long>(v);
    } else {
        os << std::fixed << std::setprecision(2) << v;
    }
    return os.str();
}

std::string pct(double v, int prec) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(prec) << v;
    return os.str();
}

// "$52.30" from a magnitude; caller prepends sign.
std::string usd(double v) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << v;
    return os.str();
}

} // namespace

const char* risk_reason_tag(RiskResult result) {
    switch (result) {
        case RiskResult::APPROVED:                return "approved";
        case RiskResult::BLOCKED_GHOST:           return "ghost_filter";
        case RiskResult::BLOCKED_BELOW_THRESHOLD: return "below_threshold";
        case RiskResult::BLOCKED_MIN_NET:         return "min_net_threshold";
        case RiskResult::BLOCKED_DAILY_LOSS:      return "daily_loss_limit";
        case RiskResult::BLOCKED_DEPTH:           return "insufficient_depth";
    }
    return "unknown";
}

RiskManager::RiskManager(const BotConfig& config)
    : ghost_threshold_pct(config.risk.ghost_threshold_pct),
      // min_profit_threshold is a ratio (0.0035); the gate compares against net%
      // so scale to percent once here (0.0035 -> 0.35).
      min_profit_pct(config.risk.min_profit_threshold * 100.0),
      min_net_pct(config.risk.min_net_pct),
      max_daily_loss_usdt(config.risk.max_daily_loss_usdt),
      trade_size_usdt(config.risk.trade_size_usdt),
      min_depth_multiplier(config.risk.min_depth_multiplier) {}

RiskDecision RiskManager::evaluate(
    const std::string& /*triangle_id*/,
    double gross_pct,
    double net_pct,
    double /*best_bid*/,
    double best_ask,
    double /*best_bid_qty*/,
    double best_ask_qty) {

    // Guard the running counters (blocked_today) and the daily-loss read; the
    // execution thread mutates daily_pnl_usdt/trades_today concurrently. This is
    // a FIRE-candidate-only path, not the per-message hot path, so the cost is
    // negligible and uncontended in practice.
    std::lock_guard<std::mutex> lk(stats_mtx_);

    // --- Check 1: ghost filter ------------------------------------------------
    // No real triangular arb on liquid pairs exceeds ~1% gross; anything above
    // is a stale order-book entry. Caught the INJ ghost at 2.3039% in 24h data.
    if (gross_pct > ghost_threshold_pct) {
        ++blocked_today;
        std::ostringstream r;
        r << "gross " << pct(gross_pct, 4) << "% exceeds ghost threshold "
          << pct(ghost_threshold_pct, 1) << "% — likely stale price";
        return {RiskResult::BLOCKED_GHOST, r.str()};
    }

    // --- Check 2: minimum profit threshold -----------------------------------
    // The hard floor. Real break-even is 3 taker legs of fees; this gate demands
    // a predicted net comfortably above that (min_profit_threshold, e.g. 0.35%)
    // so a marginal edge that won't clear costs never reaches the queue.
    if (net_pct < min_profit_pct) {
        ++blocked_today;
        std::ostringstream r;
        r << "net " << pct(net_pct, 4) << "% below profit threshold "
          << pct(min_profit_pct, 2) << "% — does not clear fees + cushion";
        return {RiskResult::BLOCKED_BELOW_THRESHOLD, r.str()};
    }

    // --- Check 3: minimum net threshold --------------------------------------
    // Net below the floor is too close to zero; 0.01–0.02% of real slippage
    // would flip it into a loss.
    if (net_pct < min_net_pct) {
        ++blocked_today;
        std::ostringstream r;
        r << "net " << pct(net_pct, 4) << "% below minimum threshold "
          << pct(min_net_pct, 2) << "% — slippage risk";
        return {RiskResult::BLOCKED_MIN_NET, r.str()};
    }

    // --- Check 4: daily loss limit -------------------------------------------
    // If the bot is bleeding money something is wrong — stop rather than
    // compound losses. Reset at midnight.
    if (daily_pnl_usdt < -max_daily_loss_usdt) {
        ++blocked_today;
        std::ostringstream r;
        r << "daily loss -$" << usd(-daily_pnl_usdt) << " exceeds limit -$"
          << usd(max_daily_loss_usdt) << " — bot halted for today";
        return {RiskResult::BLOCKED_DAILY_LOSS, r.str()};
    }

    // --- Check 5: order book depth -------------------------------------------
    // The first leg buys the outer asset with USDT, so liquidity sits on the
    // ask. If only a sliver is resting at the best price the order walks the
    // book and fills worse than calculated; the multiplier buffers movement.
    const double required_usdt  = trade_size_usdt * min_depth_multiplier;
    const double available_usdt = best_ask_qty * best_ask;
    if (available_usdt < required_usdt) {
        ++blocked_today;
        std::ostringstream r;
        r << "insufficient depth: $" << usd(available_usdt) << " available, need $"
          << usd(required_usdt) << " (" << trim_num(min_depth_multiplier)
          << "x $" << trim_num(trade_size_usdt) << ")";
        return {RiskResult::BLOCKED_DEPTH, r.str()};
    }

    return {RiskResult::APPROVED, "approved"};
}

void RiskManager::record_trade_result(double actual_pnl_usdt) {
    std::lock_guard<std::mutex> lk(stats_mtx_);
    daily_pnl_usdt += actual_pnl_usdt;
    ++trades_today;
}

void RiskManager::reset_daily_loss() {
    std::lock_guard<std::mutex> lk(stats_mtx_);
    daily_pnl_usdt = 0.0;
    trades_today   = 0;
    blocked_today  = 0;
}
