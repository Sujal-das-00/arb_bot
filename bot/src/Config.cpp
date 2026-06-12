#include "Config.h"
#include "TriangleDef.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <algorithm>
#include <cctype>

using json = nlohmann::json;

// Global currency modifier list (defined here, declared in TriangleDef.h).
std::vector<std::pair<std::string, std::string>> g_currency_modifiers;

namespace {

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}
void override_str(const char* name, std::string& target) {
    std::string v = env_or_empty(name);
    if (!v.empty()) target = v;
}
void override_int(const char* name, int& target) {
    std::string v = env_or_empty(name);
    if (!v.empty()) target = std::stoi(v);
}
void override_dbl(const char* name, double& target) {
    std::string v = env_or_empty(name);
    if (!v.empty()) target = std::stod(v);
}

// Split "wss://stream.binance.com:9443" into host, port, ssl flag.
// Accepts wss:// (ssl) or ws:// (plain). Port defaults to 9443 (wss) / 80 (ws).
void parse_ws_url(const std::string& url,
                  std::string& host, std::string& port, bool& use_ssl) {
    std::string rest = url;

    if (rest.rfind("wss://", 0) == 0) {
        use_ssl = true;
        rest = rest.substr(6);
    } else if (rest.rfind("ws://", 0) == 0) {
        use_ssl = false;
        rest = rest.substr(5);
    } else {
        throw std::runtime_error(
            "Config: ws_base_url must start with wss:// or ws:// — got: " + url);
    }

    // Strip any trailing path; we only want host[:port].
    auto slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);

    auto colon = rest.find(':');
    if (colon != std::string::npos) {
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
    } else {
        host = rest;
        port = use_ssl ? "9443" : "80";
    }

    if (host.empty()) {
        throw std::runtime_error("Config: could not parse host from ws_base_url: " + url);
    }
}

// Normalize a Binance symbol to the uppercase form the rest of the bot keys on
// ("avaxbtc" -> "AVAXBTC"). MessageParser uppercases symbols too, so the book
// map and the reverse index must agree on case.
std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return s;
}

} // namespace

BotConfig BotConfig::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Config: cannot open file: " + path);
    }

    json j;
    try {
        file >> j;
    } catch (const json::parse_error& e) {
        throw std::runtime_error("Config: invalid JSON in " + path + ": " + e.what());
    }

    BotConfig cfg;

    // ---- currency_modifiers (optional) ----
    // Global replacements: if present, { "ETH": "SOL" } means "swap every ETH for SOL"
    // in every loop. Useful for spinning up N variants without duplicating JSON.
    // Format: object of { "OLD": "NEW", ... } or a list of [["OLD", "NEW"], ...].
    if (j.contains("currency_modifiers")) {
        const auto& mods = j.at("currency_modifiers");
        if (mods.is_object()) {
            for (auto it = mods.begin(); it != mods.end(); ++it) {
                cfg.currency_modifiers.push_back({to_upper(it.key()), to_upper(it.value())});
            }
        } else if (mods.is_array()) {
            for (const auto& pair : mods) {
                if (pair.is_array() && pair.size() == 2) {
                    cfg.currency_modifiers.push_back({
                        to_upper(pair[0].get<std::string>()),
                        to_upper(pair[1].get<std::string>())
                    });
                }
            }
        }
    }

    // Helper to apply all modifiers to a symbol: each (old, new) pair is a
    // substring replacement, e.g. {"AVAX","SOL"} turns "AVAXUSDT" into
    // "SOLUSDT". Applied in order; not transitive.
    auto apply_modifiers = [&](std::string sym) {
        for (const auto& [old, newv] : cfg.currency_modifiers) {
            if (old.empty()) continue;
            std::size_t pos = 0;
            while ((pos = sym.find(old, pos)) != std::string::npos) {
                sym.replace(pos, old.size(), newv);
                pos += newv.size();
            }
        }
        return sym;
    };

    // Parse one leg: uppercase, apply modifiers, and reject if empty.
    auto parse_leg = [&](const json& t, const char* field, const std::string& tri_name) {
        if (!t.contains(field) || !t.at(field).is_string() || t.at(field).get<std::string>().empty()) {
            throw std::runtime_error(
                "Config: triangle '" + (tri_name.empty() ? std::string("<unnamed>") : tri_name) +
                "' must have a non-empty \"" + field + "\" field");
        }
        std::string sym = apply_modifiers(to_upper(t.at(field).get<std::string>()));
        if (sym.empty()) {
            throw std::runtime_error(
                "Config: triangle '" + tri_name + "' has an empty \"" + field +
                "\" after modifiers");
        }
        return sym;
    };

    // ---- triangles ----
    // Each entry names its three legs by role: outer_usdt, outer_btc, btc_usdt.
    // All are uppercase-normalized and then currency modifiers are applied.
    if (!j.contains("triangles") || !j.at("triangles").is_array() ||
        j.at("triangles").empty()) {
        throw std::runtime_error(
            "Config: \"triangles\" must be a non-empty array of "
            "{name, outer_usdt, outer_btc, btc_usdt} objects");
    }

    for (const auto& t : j.at("triangles")) {
        TriangleDef d;
        d.name = t.value("name", std::string{});

        d.outer_usdt = parse_leg(t, "outer_usdt", d.name);
        d.outer_btc  = parse_leg(t, "outer_btc",  d.name);
        d.btc_usdt   = parse_leg(t, "btc_usdt",   d.name);

        if (d.name.empty()) {
            // Best-effort label: outer leg and bridge.
            d.name = d.outer_usdt + "-" + d.btc_usdt;
        }

        cfg.triangles.push_back(std::move(d));
    }

    // ---- feed ----
    const auto& feed = j.at("feed");
    cfg.feed.ws_base_url           = feed.at("ws_base_url").get<std::string>();
    cfg.feed.depth_level           = feed.value("depth_level", 5);
    cfg.feed.reconnect_delay_ms    = feed.value("reconnect_delay_ms", 1000u);
    cfg.feed.max_reconnect_delay_ms= feed.value("max_reconnect_delay_ms", 30000u);
    cfg.feed.heartbeat_timeout_ms  = feed.value("heartbeat_timeout_ms", 60000u);
    parse_ws_url(cfg.feed.ws_base_url, cfg.feed.host, cfg.feed.port, cfg.feed.use_ssl);

    // ---- fees ----
    if (j.contains("fees")) {
        const auto& fees = j.at("fees");
        cfg.fees.taker_fee_pct = fees.value("taker_fee_pct", 0.075);
        cfg.fees.bnb_discount  = fees.value("bnb_discount", true);
    }

    // ---- risk ----
    if (j.contains("risk")) {
        const auto& risk = j.at("risk");
        cfg.risk.min_profit_ratio     = risk.value("min_profit_ratio", 1.003);
        cfg.risk.min_depth_multiplier = risk.value("min_depth_multiplier", 3.0);
        cfg.risk.trade_size_usdt      = risk.value("trade_size_usdt", 50.0);
        cfg.risk.max_daily_loss_usdt  = risk.value("max_daily_loss_usdt", 25.0);
        cfg.risk.trade_cooldown_ms    = risk.value("trade_cooldown_ms", 2000u);
    }

    // ---- logging ----
    if (j.contains("logging")) {
        const auto& lg = j.at("logging");
        cfg.logging.log_dir    = lg.value("log_dir", std::string("logs"));
        cfg.logging.signals_db = lg.value("signals_db", std::string("data/signals.db"));
        cfg.logging.trades_db  = lg.value("trades_db", std::string("data/trades.db"));
    }

    // ---- mode ----
    cfg.mode = j.value("mode", std::string("paper"));

    // ---- env overrides (handy for tests / per-machine tuning) ----
    override_str("ARB_WS_URL", cfg.feed.ws_base_url);
    if (!env_or_empty("ARB_WS_URL").empty()) {
        parse_ws_url(cfg.feed.ws_base_url, cfg.feed.host, cfg.feed.port, cfg.feed.use_ssl);
    }
    override_int("ARB_FEED_DEPTH", cfg.feed.depth_level);
    override_str("ARB_MODE", cfg.mode);
    override_dbl("ARB_TRADE_SIZE_USDT", cfg.risk.trade_size_usdt);

    return cfg;
}
