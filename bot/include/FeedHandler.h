#pragma once

#include "Config.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/beast/core/flat_buffer.hpp>

#include <functional>
#include <string>
#include <vector>

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace asio      = boost::asio;
namespace ssl       = asio::ssl;
using tcp           = asio::ip::tcp;

// FeedHandler owns one SSL WebSocket connection to Binance and streams raw
// order book messages out through a callback.
//
// Stage scope here: connect, subscribe, deliver raw JSON strings. Parsing and
// storage happen downstream; reconnection is a later stage.
class FeedHandler {
public:
    using MessageCallback = std::function<void(const std::string&)>;
    using ConnectedCallback = std::function<void()>;

    // `stream_symbols` is the full set of symbols to subscribe to (e.g. the 21
    // unique legs across all triangles). Duplicates are tolerated — the handler
    // dedupes when building the combined-stream URL.
    FeedHandler(asio::io_context& ioc, ssl::context& ssl_ctx,
                const BotConfig& config, std::vector<std::string> stream_symbols);

    void on_message(MessageCallback cb);
    void on_connected(ConnectedCallback cb);  // fired once streaming begins

    void start();   // begins async connect -> handshake -> subscribe -> read

private:
    void do_resolve();
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results);
    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep);
    void on_ssl_handshake(beast::error_code ec);
    void on_ws_handshake(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);

    std::string build_stream_path() const;  // /stream?streams=...@depth5/...
    void fail(beast::error_code ec, const char* what);

    asio::io_context& ioc_;
    ssl::context&     ssl_ctx_;
    BotConfig         config_;
    std::vector<std::string> stream_symbols_;  // symbols to subscribe to

    tcp::resolver resolver_;
    websocket::stream<ssl::stream<beast::tcp_stream>> ws_;
    beast::flat_buffer buffer_;

    MessageCallback   message_cb_;
    ConnectedCallback connected_cb_;
};
