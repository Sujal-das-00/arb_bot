#include "Config.h"
#include "FeedHandler.h"
#include "MessageParser.h"
#include "TriangleCalculator.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/signal_set.hpp>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>

// Live feed: connect to Binance, subscribe to the configured triangle, parse
// each message into a BookUpdate, calculate the triangle, and log opportunities
// and network health.

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

        // --- State across evaluations ---
        std::optional<BookUpdate> btcusdt, xrpusdt, xrpbtc;

        // Compute-time stats (how fast our hot path is)
        long long total_ns = 0;
        long long max_ns   = 0;
        int       evaluations = 0;

        // Feed-lag stats (how stale Binance's data is by the time we see it)
        long long total_lag_ms = 0;
        long long max_lag_ms   = 0;
        long long min_lag_ms   = 999999;
        int       lag_samples  = 0;

        feed.on_message([&](const std::string& raw) {
            const auto t0 = std::chrono::steady_clock::now();

            auto update = MessageParser::parse(raw);
            if (!update) return;

            // --- Network health: feed latency ---
            // How long ago did Binance generate this message?
            long long feed_lag_ms = -1;  // -1 = unmeasured (no E field)
            if (update->event_time_ms > 0) {
                const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                feed_lag_ms = now_ms - update->event_time_ms;

                total_lag_ms += feed_lag_ms;
                if (feed_lag_ms > max_lag_ms) max_lag_ms = feed_lag_ms;
                if (feed_lag_ms < min_lag_ms) min_lag_ms = feed_lag_ms;
                ++lag_samples;
            }

            // Stash latest book by symbol
            if      (update->symbol == "XRPUSDT") xrpusdt = update;
            else if (update->symbol == "XRPBTC")  xrpbtc  = update;
            else if (update->symbol == "BTCUSDT") btcusdt  = update;

            // Need all three before we can calculate
            if (!btcusdt || !xrpbtc || !xrpusdt) return;

            // Argument order is positional: (bridge BTC/USDT, X/USDT, X/BTC).
            // SOL takes ETH's slot here — the math is identical.
            auto result = TriangleCalculator::evaluate(*btcusdt, *xrpusdt, *xrpbtc);
            if (!result) return;

            // --- Compute time for this evaluation ---
            const auto t1 = std::chrono::steady_clock::now();
            const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            total_ns += ns;
            if (ns > max_ns) max_ns = ns;
            ++evaluations;

            // --- Cost model ---
            constexpr double FEES_PCT       = 0.225;   // 3 × 0.075% taker (BNB discount)
            constexpr double SLIPPAGE_PCT   = 0.05;    // estimated execution drift
            constexpr double TOTAL_COST_PCT = FEES_PCT + SLIPPAGE_PCT;

            // --- Thresholds ---
            constexpr double WATCH_THRESHOLD_PCT = 0.05;
            constexpr double FIRE_THRESHOLD_PCT  = 0.00;

            const double fwd_pct    = (result->forward_ratio - 1.0) * 100.0;
            const double rev_pct    = (result->reverse_ratio - 1.0) * 100.0;
            const double best_gross = std::max(fwd_pct, rev_pct);
            const char*  dir        = (fwd_pct > rev_pct) ? "FWD" : "REV";
            const double net_pct    = best_gross - TOTAL_COST_PCT;

            if (net_pct > FIRE_THRESHOLD_PCT) {
                // *** Would actually fire ***
                std::cout << "\033[1;32m"
                          << ">>>>>> FIRE  [" << dir << "]"
                          << "  gross "    << std::fixed << std::setprecision(4) << best_gross << "%"
                          << "  net "      << net_pct   << "%"
                          << "   eval#"    << evaluations
                          << "   compute " << ns        << "ns"
                          << "   feed_lag "<< feed_lag_ms << "ms"
                          << "\033[0m\n";
            }
            else if (best_gross > WATCH_THRESHOLD_PCT) {
                // Interesting but not fire-able
                std::cout << "  · WATCH [" << dir << "]"
                          << "  gross "    << std::fixed << std::setprecision(4) << best_gross << "%"
                          << "  net "      << net_pct   << "%"
                          << "   eval#"    << evaluations
                          << "   compute " << ns        << "ns"
                          << "   feed_lag "<< feed_lag_ms << "ms\n";
            }

            // --- Heartbeat every 1000 evals ---
            if (evaluations % 500 == 0) {
                const long long avg_ns = total_ns / evaluations;

                std::cout << "  [heartbeat " << evaluations << "]"
                          << "  fwd " << std::fixed << std::setprecision(4) << fwd_pct << "%"
                          << "  rev " << rev_pct << "%"
                          << "  | compute avg " << avg_ns << "ns  max " << max_ns << "ns"
                          << "  | feed_lag ";

                if (lag_samples > 0) {
                    const long long avg_lag = total_lag_ms / lag_samples;
                    std::cout << "min " << min_lag_ms << "ms  avg " << avg_lag << "ms  max " << max_lag_ms << "ms";
                } else {
                    std::cout << "n/a (stream type has no timestamp)";
                }

                std::cout << "\n";
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