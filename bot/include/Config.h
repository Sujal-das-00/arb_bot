#pragma once

#include <string>
#include <vector>
#include <cstdint>

// ---------------------------------------------------------------------------
// Config mirrors the structure of YOUR config.json exactly, section by section.
// The triangle pairs are simple strings ("BTCUSDT"); we derive base/quote from
// each symbol at load time so the rest of the bot still knows how to walk the
// loop. Anything here can be overridden by an environment variable (see .cpp).
// ---------------------------------------------------------------------------

// One pair, with base/quote derived from the symbol against the known assets
// in the triangle. e.g. symbol "ETHBTC" with assets {USDT,BTC,ETH} -> base ETH,
// quote BTC.
struct PairConfig {
    std::string symbol;   // "BTCUSDT"
    std::string base;     // "BTC"  (derived)
    std::string quote;    // "USDT" (derived)
};

struct TriangleConfig {
    std::string name;                 // "BTC-ETH-USDT"
    std::string base_asset;           // "USDT"  (the anchor you start/end on)
    std::vector<PairConfig> pairs;    // exactly 3, in loop order
};

struct FeedConfig {
    std::string ws_base_url;          // "wss://stream.binance.com:9443"
    int         depth_level = 5;
    uint32_t    reconnect_delay_ms = 1000;
    uint32_t    max_reconnect_delay_ms = 30000;
    uint32_t    heartbeat_timeout_ms = 60000;

    // Derived from ws_base_url at load time, since Boost needs them split out.
    std::string host;                 // "stream.binance.com"
    std::string port;                 // "9443"
    bool        use_ssl = true;       // wss -> true, ws -> false
};

struct FeesConfig {
    double taker_fee_pct = 0.075;     // percent per leg, e.g. 0.075 = 0.075%
    bool   bnb_discount = true;
};

struct RiskConfig {
    double   min_profit_ratio = 1.003;
    double   min_depth_multiplier = 3.0;
    double   trade_size_usdt = 50.0;
    double   max_daily_loss_usdt = 25.0;
    uint32_t trade_cooldown_ms = 2000;
};

struct LoggingConfig {
    std::string log_dir = "logs";
    std::string signals_db = "data/signals.db";
    std::string trades_db = "data/trades.db";
};

struct BotConfig {
    TriangleConfig triangle;
    FeedConfig     feed;
    FeesConfig     fees;
    RiskConfig     risk;
    LoggingConfig  logging;
    std::string    mode = "paper";    // "paper" or "live"

    // Loads config.json from `path`, derives host/port and base/quote,
    // validates the triangle closes, then applies env-var overrides.
    // Throws std::runtime_error on any problem, with a clear message.
    static BotConfig load(const std::string& path);
};
