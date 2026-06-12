#include "Config.h"
#include "FeedHandler.h"
#include "MessageParser.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/signal_set.hpp>

#include <iostream>
#include <iomanip>
#include <string>

// Live feed: connect to Binance, subscribe to the configured triangle, parse
// each message into a BookUpdate, and print real top-of-book. Run it, watch the
// numbers move, confirm they match Binance in a browser.

int main(int argc, char* argv[]) {
    const std::string config_path = (argc > 1) ? argv[1] : "config/config.json";

    try {
        BotConfig config = BotConfig::load(config_path);

        std::cout << "[main] triangle: " << config.triangle.name
                  << "  (anchor " << config.triangle.base_asset << ")\n";
        std::cout << "[main] pairs: ";
        for (const auto& p : config.triangle.pairs)
            std::cout << p.symbol << "(" << p.base << "/" << p.quote << ") ";
        std::cout << "\n[main] endpoint: " << config.feed.host
                  << ":" << config.feed.port
                  << "  depth" << config.feed.depth_level
                  << "  mode=" << config.mode << "\n\n";

        asio::io_context ioc;
        ssl::context ssl_ctx(ssl::context::tls_client);
        ssl_ctx.set_verify_mode(ssl::verify_peer);
        ssl_ctx.set_default_verify_paths();

        FeedHandler feed(ioc, ssl_ctx, config);

        int parsed = 0, skipped = 0;
        const int max_parsed = 15;

        feed.on_message([&](const std::string& raw) {
            auto update = MessageParser::parse(raw);
            if (!update) { ++skipped; return; }

            ++parsed;
            std::cout << std::fixed << std::setprecision(8)
                      << "[" << std::setw(2) << parsed << "] "
                      << std::setw(8) << update->symbol
                      << "  bid " << update->best_bid << " x " << update->best_bid_qty
                      << "   ask " << update->best_ask << " x " << update->best_ask_qty
                      << "  (id " << update->last_update_id << ")\n";

            if (parsed >= max_parsed) {
                std::cout << "\n[main] parsed " << parsed << " updates, skipped "
                          << skipped << " non-data frames. stopping.\n";
                ioc.stop();
            }
        });

        feed.start();

        asio::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&](const beast::error_code&, int){
            std::cout << "\n[main] signal received, stopping.\n";
            ioc.stop();
        });

        ioc.run();
    }
    catch (const std::exception& e) {
        std::cerr << "[main] fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
