#pragma once

#include "Config.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

// ---------------------------------------------------------------------------
// OrderClient — a thin synchronous wrapper over the Binance Spot REST API
// (testnet by default). It signs requests with HMAC-SHA256, places market
// orders, queries order status, cancels, reads balances, and caches LOT_SIZE
// step sizes so quantities can be floored to a valid lot.
//
// NOTE ON TRANSPORT: the spec calls for libcurl. libcurl is not installable in
// this build environment, so the HTTP layer is implemented on Boost.Beast —
// already a project dependency (the feed uses it). Behaviour is equivalent: a
// signed HTTPS request with a hard per-call timeout. Swapping back to libcurl
// later only touches OrderClient::http_request().
//
// CONNECTION REUSE: the client holds ONE persistent TLS connection (Conn) and
// reuses it across requests via HTTP/1.1 keep-alive — the Beast equivalent of
// keeping a single libcurl handle alive (CURLOPT_TCP_KEEPALIVE). Only the first
// call pays the TCP+TLS handshake (~300ms); subsequent calls are a single
// round-trip (~20-50ms). A dropped/idle connection is transparently reopened
// with one retry.
//
// Threading: an OrderClient instance is NOT thread-safe. The ExecutionEngine
// owns one and drives it from its single execution thread.
// ---------------------------------------------------------------------------

struct OrderRequest {
    std::string symbol;            // "ATOMUSDT"
    std::string side;              // "BUY" / "SELL"
    std::string type = "MARKET";
    double      quantity = 0.0;
    std::string client_order_id;   // optional; generated if empty
};

struct OrderResult {
    bool        success = false;
    std::string order_id;
    std::string client_order_id;
    std::string status;            // "FILLED", "PARTIALLY_FILLED", ...
    double      executed_qty = 0;
    double      cummulative_quote_qty = 0;  // total quote (USDT/BTC) moved
    double      avg_fill_price = 0;
    double      commission = 0;
    int64_t     transact_time_ms = 0;
    std::string error_msg;
};

// Free helper, exposed for unit testing: lowercase hex HMAC-SHA256(key, data).
std::string hmac_sha256_hex(const std::string& key, const std::string& data);

class OrderClient {
public:
    explicit OrderClient(const BotConfig& config);
    ~OrderClient();   // out-of-line: Conn is incomplete here (pImpl)

    // POST /api/v3/order (signed). On transport timeout/error: success=false,
    // error_msg set — the caller should query_order() to learn the true state.
    OrderResult place_market_order(const OrderRequest& req, int timeout_ms);

    // GET /api/v3/order (signed), keyed by origClientOrderId.
    OrderResult query_order(const std::string& symbol,
                            const std::string& client_order_id);

    // DELETE /api/v3/openOrders (signed) — cancel all open orders for a symbol.
    OrderResult cancel_all(const std::string& symbol);

    // GET /api/v3/account (signed) -> free balance for one asset.
    double get_balance(const std::string& asset);

    // GET /api/v3/ticker/price (unsigned) -> last price for one symbol, or 0.
    double get_price(const std::string& symbol);

    // GET /api/v3/exchangeInfo -> cache LOT_SIZE stepSize per symbol.
    void   load_exchange_info();

    // floor(qty / step) * step, rendered at the step's precision. If the symbol
    // has no cached step, qty is returned unchanged.
    double round_to_lot(const std::string& symbol, double qty);

    // Pure lot-flooring, exposed for testing and reuse.
    static double floor_to_step(double qty, double step);

    // Test/diagnostic seam: inject a step size without hitting the network.
    void set_lot_step(const std::string& symbol, double step) { lot_step_size[symbol] = step; }

private:
    std::string sign(const std::string& query_string);   // HMAC-SHA256 hex
    std::string generate_client_order_id(const std::string& tag);

    // One signed/unsigned HTTPS round-trip over the persistent connection.
    // Returns body; sets out_status and out_ok. `method` is "GET"/"POST"/
    // "DELETE". `target` includes the path and (for signed calls) the full
    // query string with signature appended. Reopens + retries once on a
    // dropped connection.
    std::string http_request(const std::string& method, const std::string& target,
                             bool send_api_key, int timeout_ms,
                             bool& out_ok, int& out_status);

    struct Conn;                     // persistent TLS connection (defined in .cpp)
    void open_connection(int timeout_ms);
    void close_connection();
    std::unique_ptr<Conn> conn_;

    // Build "k1=v1&k2=v2&...&timestamp=now", append "&signature=...".
    std::string signed_query(const std::string& base_params);

    // Parse a Binance order JSON body into an OrderResult.
    OrderResult parse_order_response(const std::string& body, int http_status);

    std::map<std::string, double> lot_step_size;  // symbol -> stepSize
    std::string api_key_;
    std::string api_secret_;
    std::string host_;   // "testnet.binance.vision"
    std::string port_;   // "443"
};
