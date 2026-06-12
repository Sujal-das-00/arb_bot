#include "Config.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

using json = nlohmann::json;

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

// Given a symbol like "ETHBTC" and the set of assets in the triangle
// ({"USDT","BTC","ETH"}), find which asset is the base (prefix) and which is
// the quote (suffix). Binance symbols are BASE+QUOTE concatenated with no
// separator, so we test each asset as a possible suffix.
void derive_base_quote(const std::string& symbol,
                       const std::set<std::string>& assets,
                       std::string& base, std::string& quote) {
    for (const auto& a : assets) {
        if (symbol.size() > a.size() &&
            symbol.compare(symbol.size() - a.size(), a.size(), a) == 0) {
            std::string candidate_base = symbol.substr(0, symbol.size() - a.size());
            if (assets.count(candidate_base)) {
                base = candidate_base;
                quote = a;
                return;
            }
        }
    }
    throw std::runtime_error(
        "Config: cannot split symbol '" + symbol +
        "' into two of the triangle's assets — check the pairs form a real loop");
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

    // ---- triangle ----
    const auto& tri = j.at("triangle");
    cfg.triangle.name       = tri.value("name", std::string{});
    cfg.triangle.base_asset = tri.at("base_asset").get<std::string>();

    std::vector<std::string> symbols =
        tri.at("pairs").get<std::vector<std::string>>();
    if (symbols.size() != 3) {
        throw std::runtime_error(
            "Config: triangle must have exactly 3 pairs, found " +
            std::to_string(symbols.size()));
    }

    // Collect the distinct assets by trying every known major quote against
    // the symbols. We seed with base_asset and discover the rest by splitting.
    // First pass: gather all candidate assets from a known set + base_asset.
    // To keep this robust without hardcoding, we infer assets iteratively.
    //
    // Simple, reliable approach: the union of all characters can't tell us
    // boundaries, so we use the fact that across 3 pairs each asset appears
    // exactly twice. We brute-force consistent splits using base_asset as anchor.
    //
    // Practical method: assume each symbol ends in one of the assets we know.
    // We know base_asset for sure. The other two assets are the leftover parts.
    // We solve by trying common quote currencies present in the symbols.
    std::set<std::string> assets;
    assets.insert(cfg.triangle.base_asset);

    // Discover the two non-anchor assets. For each symbol, peel off base_asset
    // if it appears as a prefix or suffix; the remainder is another asset.
    for (const auto& sym : symbols) {
        const std::string& a = cfg.triangle.base_asset;
        if (sym.size() > a.size() &&
            sym.compare(0, a.size(), a) == 0) {
            assets.insert(sym.substr(a.size()));            // anchor is prefix
        } else if (sym.size() > a.size() &&
                   sym.compare(sym.size() - a.size(), a.size(), a) == 0) {
            assets.insert(sym.substr(0, sym.size() - a.size())); // anchor is suffix
        }
    }

    // The bridge pair (neither side is the anchor) introduces the relationship
    // between the two non-anchor assets; both should already be in `assets`
    // from the two anchored pairs. If we don't have exactly 3 assets, the
    // triangle is misconfigured.
    if (assets.size() != 3) {
        throw std::runtime_error(
            "Config: could not identify exactly 3 assets from pairs + base_asset '" +
            cfg.triangle.base_asset + "'. Found " + std::to_string(assets.size()) +
            ". Check that two pairs include the anchor and the symbols are correct.");
    }

    // Now split every symbol into base/quote against the discovered assets.
    std::map<std::string,int> appearances;
    for (const auto& sym : symbols) {
        PairConfig pc;
        pc.symbol = sym;
        derive_base_quote(sym, assets, pc.base, pc.quote);
        appearances[pc.base]++;
        appearances[pc.quote]++;
        cfg.triangle.pairs.push_back(pc);
    }

    // Validate closed loop: 3 assets, each appearing exactly twice.
    if (appearances.size() != 3) {
        throw std::runtime_error(
            "Config: triangle does not involve exactly 3 assets after splitting.");
    }
    for (const auto& [asset, count] : appearances) {
        if (count != 2) {
            throw std::runtime_error(
                "Config: asset '" + asset + "' appears " + std::to_string(count) +
                " time(s), expected 2 — triangle does not close cleanly.");
        }
    }
    if (!appearances.count(cfg.triangle.base_asset)) {
        throw std::runtime_error(
            "Config: base_asset '" + cfg.triangle.base_asset +
            "' is not part of the triangle.");
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
