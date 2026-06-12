#pragma once

#include <string>
#include <vector>

// One arbitrage triangle: USDT -> BTC -> OUTER -> USDT (and reverse). The
// three legs are named by role so TriangleCalculator::evaluate() can track
// exactly which currency is held at each step (no generic loop-walking).
struct TriangleDef {
    std::string name;        // "AVAX-BTC-USDT" (display/log only)
    std::string outer_usdt;  // e.g. "AVAXUSDT"
    std::string outer_btc;   // e.g. "AVAXBTC"
    std::string btc_usdt;    // "BTCUSDT"
};

// Global currency modifier: a list of substring replacements applied to
// outer_usdt/outer_btc/btc_usdt at load time. e.g. {"AVAX","SOL"} turns
// "AVAXUSDT" into "SOLUSDT" and "AVAXBTC" into "SOLBTC".
// Loaded from config.json["currency_modifiers"]. Allows spinning up
// variants of the triangle set without duplicating JSON.
extern std::vector<std::pair<std::string, std::string>> g_currency_modifiers;
