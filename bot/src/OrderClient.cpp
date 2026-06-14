#include "OrderClient.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <nlohmann/json.hpp>
#include <openssl/hmac.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace asio  = boost::asio;
namespace ssl   = asio::ssl;
using tcp       = asio::ip::tcp;
using json      = nlohmann::json;

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// "https://testnet.binance.vision" -> host="testnet.binance.vision", port="443".
void parse_rest_url(const std::string& url, std::string& host, std::string& port) {
    std::string rest = url;
    bool tls = true;
    if (rest.rfind("https://", 0) == 0)      { tls = true;  rest = rest.substr(8); }
    else if (rest.rfind("http://", 0) == 0)  { tls = false; rest = rest.substr(7); }
    auto slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    auto colon = rest.find(':');
    if (colon != std::string::npos) { host = rest.substr(0, colon); port = rest.substr(colon + 1); }
    else                            { host = rest; port = tls ? "443" : "80"; }
}

// Drop trailing zeros (and a dangling '.') so "12.10000000" -> "12.1".
std::string trim_decimal(std::string s) {
    if (s.find('.') == std::string::npos) return s;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

} // namespace

std::string hmac_sha256_hex(const std::string& key, const std::string& data) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int  out_len = 0;
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         out, &out_len);

    static const char* hexd = "0123456789abcdef";
    std::string hex;
    hex.reserve(out_len * 2);
    for (unsigned int i = 0; i < out_len; ++i) {
        hex.push_back(hexd[(out[i] >> 4) & 0xF]);
        hex.push_back(hexd[out[i] & 0xF]);
    }
    return hex;
}

// Persistent TLS connection, reused across requests (HTTP/1.1 keep-alive).
struct OrderClient::Conn {
    asio::io_context ioc;
    ssl::context     ctx{ssl::context::tls_client};
    std::unique_ptr<ssl::stream<beast::tcp_stream>> stream;
    beast::flat_buffer buffer;   // retains any bytes left between responses
};

OrderClient::OrderClient(const BotConfig& config)
    : api_key_(config.execution.api_key),
      api_secret_(config.execution.api_secret) {
    parse_rest_url(config.execution.rest_base_url, host_, port_);
}

OrderClient::~OrderClient() { close_connection(); }

void OrderClient::open_connection(int timeout_ms) {
    auto c = std::make_unique<Conn>();
    c->ctx.set_default_verify_paths();
    // Testnet sandbox (fake money): relaxed cert verification, as before.
    c->ctx.set_verify_mode(ssl::verify_none);
    c->stream = std::make_unique<ssl::stream<beast::tcp_stream>>(c->ioc, c->ctx);

    if (!SSL_set_tlsext_host_name(c->stream->native_handle(), host_.c_str()))
        throw std::runtime_error("SNI set failed");

    tcp::resolver resolver(c->ioc);
    auto const results = resolver.resolve(host_, port_);
    beast::get_lowest_layer(*c->stream).expires_after(std::chrono::milliseconds(timeout_ms));
    beast::get_lowest_layer(*c->stream).connect(results);
    c->stream->handshake(ssl::stream_base::client);
    // OS-level keepalive probes on the idle socket (CURLOPT_TCP_KEEPALIVE).
    beast::get_lowest_layer(*c->stream).socket().set_option(asio::socket_base::keep_alive(true));

    conn_ = std::move(c);
}

void OrderClient::close_connection() {
    if (conn_ && conn_->stream) {
        beast::error_code ec;
        conn_->stream->shutdown(ec);  // best-effort
    }
    conn_.reset();
}

std::string OrderClient::sign(const std::string& query_string) {
    return hmac_sha256_hex(api_secret_, query_string);
}

std::string OrderClient::generate_client_order_id(const std::string& tag) {
    static std::atomic<uint64_t> ctr{0};
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return tag + std::to_string(us) + std::to_string(ctr.fetch_add(1) % 1000);
}

std::string OrderClient::signed_query(const std::string& base_params) {
    std::string q = base_params;
    if (!q.empty()) q += "&";
    q += "recvWindow=5000&timestamp=" + std::to_string(now_ms());
    return q + "&signature=" + sign(q);
}

double OrderClient::floor_to_step(double qty, double step) {
    if (step <= 0.0) return qty;
    // Epsilon nudges values that are a hair below an exact multiple (fp error)
    // back up before flooring, without ever crossing a real sub-step boundary.
    const double n = std::floor(qty / step + 1e-7);
    return n * step;
}

double OrderClient::round_to_lot(const std::string& symbol, double qty) {
    auto it = lot_step_size.find(symbol);
    if (it == lot_step_size.end()) return qty;
    return floor_to_step(qty, it->second);
}

std::string OrderClient::http_request(const std::string& method, const std::string& target,
                                      bool send_api_key, int timeout_ms,
                                      bool& out_ok, int& out_status) {
    out_ok = false;
    out_status = 0;

    http::verb verb = http::verb::get;
    if (method == "POST")        verb = http::verb::post;
    else if (method == "DELETE") verb = http::verb::delete_;

    // Two attempts: a kept-alive socket that the server closed while idle fails
    // the first write/read; we then reopen and retry once.
    std::string last_err;
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            if (!conn_ || !conn_->stream) open_connection(timeout_ms);
            auto& stream = *conn_->stream;

            http::request<http::string_body> req{verb, target, 11};
            req.set(http::field::host, host_);
            req.set(http::field::user_agent, "arb_bot/1.0");
            req.keep_alive(true);  // ask the server to keep the connection open
            if (send_api_key) req.set("X-MBX-APIKEY", api_key_);
            req.prepare_payload();

            beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
            http::write(stream, req);

            http::response<http::string_body> res;
            beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
            http::read(stream, conn_->buffer, res);

            out_status = res.result_int();
            out_ok = (out_status >= 200 && out_status < 300);

            // If the server won't keep the socket, drop it so the next call
            // opens a fresh one rather than failing.
            if (!res.keep_alive()) close_connection();
            return res.body();
        } catch (const std::exception& e) {
            last_err = e.what();
            close_connection();  // force a clean reconnect on retry
        }
    }
    out_ok = false;
    return std::string(R"({"__transport_error":")") + last_err + R"("})";
}

OrderResult OrderClient::parse_order_response(const std::string& body, int http_status) {
    OrderResult r;
    try {
        auto j = json::parse(body);

        if (j.contains("__transport_error")) {
            r.success = false;
            r.error_msg = "transport: " + j["__transport_error"].get<std::string>();
            return r;
        }
        if (j.contains("code") && j.contains("msg")) {  // Binance API error envelope
            r.success = false;
            r.error_msg = "binance_err " + std::to_string(j["code"].get<int>()) +
                          ": " + j["msg"].get<std::string>();
            return r;
        }

        if (j.contains("orderId"))       r.order_id = std::to_string(j["orderId"].get<long long>());
        if (j.contains("clientOrderId")) r.client_order_id = j["clientOrderId"].get<std::string>();
        if (j.contains("status"))        r.status = j["status"].get<std::string>();
        if (j.contains("executedQty"))   r.executed_qty = std::stod(j["executedQty"].get<std::string>());
        if (j.contains("cummulativeQuoteQty"))
            r.cummulative_quote_qty = std::stod(j["cummulativeQuoteQty"].get<std::string>());
        if (j.contains("transactTime"))  r.transact_time_ms = j["transactTime"].get<int64_t>();

        double comm = 0.0;
        if (j.contains("fills") && j["fills"].is_array()) {
            for (const auto& f : j["fills"]) {
                if (f.contains("commission")) comm += std::stod(f["commission"].get<std::string>());
            }
        }
        r.commission = comm;
        if (r.executed_qty > 0.0) r.avg_fill_price = r.cummulative_quote_qty / r.executed_qty;

        r.success = (http_status >= 200 && http_status < 300) &&
                    (r.status == "FILLED" || r.status == "PARTIALLY_FILLED");
        if (r.error_msg.empty() && !r.success && !r.status.empty())
            r.error_msg = "unfilled status: " + r.status;
    } catch (const std::exception& e) {
        r.success = false;
        r.error_msg = std::string("parse_error: ") + e.what() + " body=" + body.substr(0, 180);
    }
    return r;
}

OrderResult OrderClient::place_market_order(const OrderRequest& req, int timeout_ms) {
    const std::string coid =
        req.client_order_id.empty() ? generate_client_order_id("ab") : req.client_order_id;

    std::ostringstream qs;
    qs << std::fixed << std::setprecision(8) << req.quantity;

    const std::string base = "symbol=" + req.symbol +
                             "&side=" + req.side +
                             "&type=" + req.type +
                             "&quantity=" + trim_decimal(qs.str()) +
                             "&newOrderRespType=FULL" +
                             "&newClientOrderId=" + coid;

    const std::string target = "/api/v3/order?" + signed_query(base);

    bool ok; int status;
    const std::string body = http_request("POST", target, true, timeout_ms, ok, status);

    OrderResult r = parse_order_response(body, status);
    if (r.client_order_id.empty()) r.client_order_id = coid;
    return r;
}

OrderResult OrderClient::query_order(const std::string& symbol,
                                     const std::string& client_order_id) {
    const std::string base = "symbol=" + symbol + "&origClientOrderId=" + client_order_id;
    const std::string target = "/api/v3/order?" + signed_query(base);

    bool ok; int status;
    const std::string body = http_request("GET", target, true, 5000, ok, status);
    OrderResult r = parse_order_response(body, status);
    if (r.client_order_id.empty()) r.client_order_id = client_order_id;
    return r;
}

OrderResult OrderClient::cancel_all(const std::string& symbol) {
    const std::string target = "/api/v3/openOrders?" + signed_query("symbol=" + symbol);

    bool ok; int status;
    const std::string body = http_request("DELETE", target, true, 5000, ok, status);

    OrderResult r;
    // A successful cancel returns an array (possibly empty); errors use the
    // {code,msg} envelope handled below.
    try {
        auto j = json::parse(body);
        if (j.contains("__transport_error")) {
            r.error_msg = "transport: " + j["__transport_error"].get<std::string>();
        } else if (j.contains("code") && j.contains("msg")) {
            r.error_msg = "binance_err " + std::to_string(j["code"].get<int>()) +
                          ": " + j["msg"].get<std::string>();
        } else {
            r.success = ok;
            r.status = "CANCELED";
        }
    } catch (const std::exception& e) {
        r.error_msg = std::string("parse_error: ") + e.what();
    }
    return r;
}

double OrderClient::get_balance(const std::string& asset) {
    const std::string target = "/api/v3/account?" + signed_query("");

    bool ok; int status;
    const std::string body = http_request("GET", target, true, 5000, ok, status);
    try {
        auto j = json::parse(body);
        if (j.contains("balances") && j["balances"].is_array()) {
            for (const auto& b : j["balances"]) {
                if (b.value("asset", std::string()) == asset)
                    return std::stod(b.value("free", std::string("0")));
            }
        }
    } catch (const std::exception&) { /* fall through to 0 */ }
    return 0.0;
}

double OrderClient::get_price(const std::string& symbol) {
    bool ok; int status;
    const std::string body =
        http_request("GET", "/api/v3/ticker/price?symbol=" + symbol, false, 5000, ok, status);
    try {
        auto j = json::parse(body);
        if (j.contains("price")) return std::stod(j["price"].get<std::string>());
    } catch (const std::exception&) { /* fall through */ }
    return 0.0;
}

void OrderClient::load_exchange_info() {
    bool ok; int status;
    const std::string body = http_request("GET", "/api/v3/exchangeInfo", false, 10000, ok, status);
    if (!ok) return;
    try {
        auto j = json::parse(body);
        if (!j.contains("symbols")) return;
        for (const auto& s : j["symbols"]) {
            const std::string sym = s.value("symbol", std::string());
            if (sym.empty() || !s.contains("filters")) continue;
            for (const auto& f : s["filters"]) {
                if (f.value("filterType", std::string()) == "LOT_SIZE") {
                    lot_step_size[sym] = std::stod(f.value("stepSize", std::string("0")));
                    break;
                }
            }
        }
    } catch (const std::exception&) { /* leave cache as-is on parse failure */ }
}
