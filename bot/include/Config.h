#pragma once

#include "TriangleDef.h"

#include <string>
#include <vector>
#include <cstdint>

// ---------------------------------------------------------------------------
// Config mirrors the structure of YOUR config.json exactly, section by section.
// The bot now watches a list of triangles (config key "triangles"); each entry
// names its three legs by role (outer_usdt, outer_btc, btc_usdt) so no base/quote
// derivation is needed. Anything here can be overridden by an environment
// variable (see .cpp).
// ---------------------------------------------------------------------------

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
    // --- Risk Manager gates (see RiskManager) ---
    double   ghost_threshold_pct = 1.0;     // gross above this is a stale-price ghost
    double   min_net_pct = 0.02;            // net below this is too close to zero to fire
    double   min_depth_multiplier = 3.0;    // need this many trade-sizes of liquidity at best price
    double   trade_size_usdt = 100.0;       // notional per leg; the only position-size knob
    double   max_daily_loss_usdt = 50.0;    // halt for the day once cumulative loss passes this
    double   min_profit_threshold = 0.0035; // reserved: minimum profit ratio gate

    // --- legacy / reserved ---
    double   min_profit_ratio = 1.003;
    uint32_t trade_cooldown_ms = 2000;
};

struct LoggingConfig {
    std::string log_dir = "logs";
    std::string signals_db = "data/signals.db";
    std::string trades_db = "data/trades.db";
};

// Execution harness (Binance testnet REST). When `enabled` is false the bot
// behaves exactly as the detection-only paper bot — no orders, no threads.
struct ExecutionConfig {
    std::string rest_base_url   = "https://testnet.binance.vision";
    std::string api_key;
    std::string api_secret;
    uint32_t    order_timeout_ms = 500;
    double      recheck_threshold = 1.001;  // abort before leg2 if ratio drops below
    double      leg3_threshold    = 1.000;  // abort before leg3 if ratio drops below
    bool        enabled = false;            // off by default; turn on for testnet runs
    std::string trades_csv = "data/trades.csv";
};

struct BotConfig {
    std::vector<TriangleDef> triangles; // one or more arbitrage loops
    std::vector<std::pair<std::string, std::string>> currency_modifiers;  // global replacements
    FeedConfig     feed;
    FeesConfig     fees;
    RiskConfig     risk;
    LoggingConfig  logging;
    ExecutionConfig execution;
    std::string    mode = "paper";    // "paper" or "live"

    // Loads config.json from `path`, applies currency modifiers to every loop,
    // then applies env-var overrides. Throws std::runtime_error on any problem.
    static BotConfig load(const std::string& path);
};
