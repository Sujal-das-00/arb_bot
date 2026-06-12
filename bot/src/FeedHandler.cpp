#include "FeedHandler.h"
#include <boost/asio/strand.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <iostream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>

FeedHandler::FeedHandler(asio::io_context& ioc, ssl::context& ssl_ctx,
                         const BotConfig& config, std::vector<std::string> stream_symbols)
    : ioc_(ioc)
    , ssl_ctx_(ssl_ctx)
    , config_(config)
    , stream_symbols_(std::move(stream_symbols))
    , resolver_(asio::make_strand(ioc))
    , ws_(asio::make_strand(ioc), ssl_ctx)
{}

void FeedHandler::on_message(MessageCallback cb)   { message_cb_   = std::move(cb); }
void FeedHandler::on_connected(ConnectedCallback cb){ connected_cb_ = std::move(cb); }

void FeedHandler::start() { do_resolve(); }

// Binance wants lowercase symbols: BTCUSDT -> btcusdt.
// Combined stream form: /stream?streams=<s1>@depth5/<s2>@depth5/...
// We dedupe here (via a sorted set) so the shared bridge leg is subscribed once
// even if every triangle lists it. Subscribing the same stream twice isn't fatal
// on Binance, but it wastes the connection's stream budget and muddies logs.
std::string FeedHandler::build_stream_path() const {
    const int depth = config_.feed.depth_level;

    std::set<std::string> unique;  // sorted + deduped
    for (const auto& sym : stream_symbols_) {
        std::string lower = sym;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        unique.insert(lower);
    }

    std::string path = "/stream?streams=";
    bool first = true;
    for (const auto& sym : unique) {
        if (!first) path += "/";
        path += sym + "@depth" + std::to_string(depth);
        first = false;
    }
    return path;
}

void FeedHandler::fail(beast::error_code ec, const char* what) {
    std::cerr << "[FeedHandler] " << what << ": " << ec.message() << "\n";
}

void FeedHandler::do_resolve() {
    std::cout << "[FeedHandler] resolving " << config_.feed.host
              << ":" << config_.feed.port << "\n";
    resolver_.async_resolve(
        config_.feed.host, config_.feed.port,
        beast::bind_front_handler(&FeedHandler::on_resolve, this));
}

void FeedHandler::on_resolve(beast::error_code ec,
                             tcp::resolver::results_type results) {
    if (ec) return fail(ec, "resolve");
    beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(ws_).async_connect(
        results, beast::bind_front_handler(&FeedHandler::on_connect, this));
}

void FeedHandler::on_connect(beast::error_code ec,
                             tcp::resolver::results_type::endpoint_type ep) {
    if (ec) return fail(ec, "connect");
    std::cout << "[FeedHandler] TCP connected to "
              << ep.address().to_string() << ":" << ep.port() << "\n";

    // SNI is required or Binance's TLS handshake fails.
    if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(),
                                  config_.feed.host.c_str())) {
        beast::error_code sni_ec{ static_cast<int>(::ERR_get_error()),
                                  asio::error::get_ssl_category() };
        return fail(sni_ec, "SNI");
    }

    ws_.next_layer().set_verify_mode(ssl::verify_peer);
    ws_.next_layer().set_verify_callback(
        ssl::host_name_verification(config_.feed.host));

    beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
    ws_.next_layer().async_handshake(
        ssl::stream_base::client,
        beast::bind_front_handler(&FeedHandler::on_ssl_handshake, this));
}

void FeedHandler::on_ssl_handshake(beast::error_code ec) {
    if (ec) return fail(ec, "ssl_handshake");
    std::cout << "[FeedHandler] SSL handshake complete\n";

    beast::get_lowest_layer(ws_).expires_never();
    ws_.set_option(websocket::stream_base::decorator(
        [](websocket::request_type& req){
            req.set(beast::http::field::user_agent, "arb-bot-feed/0.1");
        }));

    const std::string path = build_stream_path();
    std::cout << "[FeedHandler] websocket handshake, path=" << path << "\n";
    const std::string host_header = config_.feed.host + ":" + config_.feed.port;

    ws_.async_handshake(host_header, path,
        beast::bind_front_handler(&FeedHandler::on_ws_handshake, this));
}

void FeedHandler::on_ws_handshake(beast::error_code ec) {
    if (ec) return fail(ec, "ws_handshake");
    std::cout << "[FeedHandler] websocket connected — streaming\n";
    if (connected_cb_) connected_cb_();
    do_read();
}

void FeedHandler::do_read() {
    ws_.async_read(buffer_,
        beast::bind_front_handler(&FeedHandler::on_read, this));
}

void FeedHandler::on_read(beast::error_code ec, std::size_t /*bytes*/) {
    if (ec) return fail(ec, "read");
    if (message_cb_) {
        message_cb_(beast::buffers_to_string(buffer_.data()));
    }
    buffer_.consume(buffer_.size());
    do_read();
}
