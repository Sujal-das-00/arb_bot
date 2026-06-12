#include "MessageParser.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>

using json = nlohmann::json;

int64_t MessageParser::now_ns() {
    // steady_clock is monotonic — it never jumps backward, unlike system_clock.
    // We only ever compare these to each other (freshness, latency), so a
    // monotonic source is exactly what we want.
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::optional<BookUpdate> MessageParser::parse(const std::string& raw) {
    // Parse JSON without exceptions: nlohmann can return a discarded value
    // instead of throwing when we pass allow_exceptions=false. A single
    // malformed frame must never bring down the read loop.
    json j = json::parse(raw, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        return std::nullopt;
    }

    // Combined-stream envelope: the payload we care about lives under "data",
    // and the stream name (e.g. "btcusdt@depth5") tells us the symbol.
    if (!j.contains("data") || !j.contains("stream")) {
        return std::nullopt;
    }

    const auto& data = j["data"];
    if (!data.is_object()) {
        return std::nullopt;
    }

    // ---- symbol, recovered from the stream name "btcusdt@depth5" ----
    std::string stream = j["stream"].get<std::string>();
    auto at = stream.find('@');
    if (at == std::string::npos || at == 0) {
        return std::nullopt;
    }
    std::string symbol = stream.substr(0, at);
    // Normalize to uppercase so it matches our config symbols ("BTCUSDT").
    std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    // ---- bids / asks arrays ----
    if (!data.contains("bids") || !data.contains("asks")) {
        return std::nullopt;
    }
    const auto& bids = data["bids"];
    const auto& asks = data["asks"];
    if (!bids.is_array() || !asks.is_array() || bids.empty() || asks.empty()) {
        // An empty side means no liquidity on that side this instant.
        // Not an error — just unusable. Skip it.
        return std::nullopt;
    }

    // Each level is ["price", "qty"] as strings. Best bid/ask is index 0:
    // Binance sends bids descending (best = highest first) and asks ascending
    // (best = lowest first), so [0] is the top of book on both sides.
    const auto& top_bid = bids[0];
    const auto& top_ask = asks[0];
    if (!top_bid.is_array() || top_bid.size() < 2 ||
        !top_ask.is_array() || top_ask.size() < 2) {
        return std::nullopt;
    }

    BookUpdate update;
    update.symbol = std::move(symbol);

    // Prices and quantities arrive as JSON strings; convert defensively.
    // std::stod can throw on garbage, so guard the whole block.
    try {
        update.best_bid     = std::stod(top_bid[0].get<std::string>());
        update.best_bid_qty = std::stod(top_bid[1].get<std::string>());
        update.best_ask     = std::stod(top_ask[0].get<std::string>());
        update.best_ask_qty = std::stod(top_ask[1].get<std::string>());
    } catch (const std::exception&) {
        return std::nullopt;
    }

    // lastUpdateId is optional in our handling but useful for ordering and
    // detecting gaps. Default 0 if absent or wrong type.
    if (data.contains("lastUpdateId") && data["lastUpdateId"].is_number()) {
        update.last_update_id = data["lastUpdateId"].get<uint64_t>();
    }

    update.event_time_ms = data.value("E", 0LL);

    update.recv_time_ns = now_ns();

    // Final sanity gate: reject crossed or zero books. A best_bid >= best_ask
    // would mean the book is crossed (shouldn't happen on a clean snapshot)
    // and signals corrupt data we must not trade on.
    if (!update.valid() || update.best_bid >= update.best_ask) {
        return std::nullopt;
    }

    return update;
}
