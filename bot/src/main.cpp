#include "Config.h"
#include "Dispatch.h"
#include "FeedHandler.h"
#include "MessageParser.h"
#include "TriangleCalculator.h"
#include "TriangleDef.h"
#include "Logger.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/signal_set.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// Live feed: connect to Binance once, subscribe to every leg of every configured
// triangle over that single connection, parse each message into a BookUpdate, and
// re-evaluate only the triangles that reference the symbol that just changed.
//
// Hot path is single-threaded (the io_context strand) and lock-free. The only
// per-message work the multi-triangle change adds is: one book map write, one
// reverse-index lookup, and a re-evaluation of just the affected triangles —
// at most three triangles per non-bridge symbol, and all N when BTCUSDT moves.

int main(int argc, char* argv[]) {
    const std::string config_path = (argc > 1) ? argv[1] : "config/config.json";

    try {
        BotConfig config = BotConfig::load(config_path);
        Logger& logger = Logger::instance();
        logger.init(config.logging.log_dir);

        // Copy modifiers to global for potential use elsewhere (env var override, etc).
        g_currency_modifiers = config.currency_modifiers;

        // --- Triangle set + routing tables (built once, never touched on hot path) ---
        std::vector<TriangleDef> triangles = config.triangles;
        std::vector<std::string> stream_symbols = dispatch::collect_symbols(triangles);
        std::unordered_map<std::string, std::vector<std::size_t>> symbol_to_tri =
            dispatch::build_reverse_index(triangles);

        // Latest top-of-book per symbol. Reserve up front so the hot path never
        // rehashes; the symbol set is fixed for the life of the process.
        std::unordered_map<std::string, BookUpdate> book;
        book.reserve(stream_symbols.size() * 2);

        logger.log("[main] watching " + std::to_string(triangles.size()) +
                   " arbitrage loops over " + std::to_string(stream_symbols.size()) +
                   " unique streams  mode=" + config.mode);
        for (const auto& t : triangles) {
            std::ostringstream loop_str;
            loop_str << "[main]   " << t.name << "  ["
                     << t.outer_usdt << " -> " << t.outer_btc << " -> " << t.btc_usdt << "]";
            logger.log(loop_str.str());
        }

        std::ostringstream endpoint_msg;
        endpoint_msg << "[main] endpoint: " << config.feed.host << ":" << config.feed.port
                     << "  depth" << config.feed.depth_level;
        logger.log(endpoint_msg.str());

        asio::io_context ioc;
        ssl::context ssl_ctx(ssl::context::tls_client);
        ssl_ctx.set_verify_mode(ssl::verify_peer);
        ssl_ctx.set_default_verify_paths();

        FeedHandler feed(ioc, ssl_ctx, config, stream_symbols);

        // --- Global stats across all triangles ---
        long long total_ns = 0;       // summed per-evaluation compute
        long long max_ns   = 0;
        long long total_evaluations = 0;
        long long watch_count = 0;
        long long fire_count  = 0;
        std::vector<long long> tri_eval_counts(triangles.size(), 0);  // per-triangle, for "active" tally

        // --- Feed-lag stats (how stale Binance's data is by the time we see it) ---
        long long total_lag_ms = 0;
        long long max_lag_ms   = 0;
        long long min_lag_ms   = 999999;
        long long lag_samples  = 0;

        // --- Cost model + thresholds (shared by every triangle) ---
        constexpr double FEES_PCT       = 0.225;   // 3 × 0.075% taker (BNB discount)
        constexpr double SLIPPAGE_PCT   = 0.05;    // estimated execution drift
        constexpr double TOTAL_COST_PCT = FEES_PCT + SLIPPAGE_PCT;
        constexpr double WATCH_THRESHOLD_PCT = 0.05;
        constexpr double FIRE_THRESHOLD_PCT  = 0.00;
        constexpr long long HEARTBEAT_EVERY  = 5000;

        feed.on_message([&](const std::string& raw) {
            auto update = MessageParser::parse(raw);
            if (!update) return;

            // --- Network health: feed latency (per message, cheap) ---
            long long feed_lag_ms = -1;  // -1 = unmeasured (no E field on depth5)
            if (update->event_time_ms > 0) {
                const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                feed_lag_ms = now_ms - update->event_time_ms;
                total_lag_ms += feed_lag_ms;
                if (feed_lag_ms > max_lag_ms) max_lag_ms = feed_lag_ms;
                if (feed_lag_ms < min_lag_ms) min_lag_ms = feed_lag_ms;
                ++lag_samples;
            }

            // Stash latest book, then route: which triangles reference this symbol?
            const std::string symbol = update->symbol;
            book[symbol] = std::move(*update);

            const auto it = symbol_to_tri.find(symbol);
            if (it == symbol_to_tri.end()) return;  // no triangle uses it -> zero work

            // Re-evaluate only the affected triangles. Non-bridge symbols touch
            // at most one triangle; shared symbols touch multiple.
            for (const std::size_t idx : it->second) {
                const TriangleDef& td = triangles[idx];

                const auto it1 = book.find(td.btc_usdt);
                const auto it2 = book.find(td.outer_usdt);
                const auto it3 = book.find(td.outer_btc);
                if (it1 == book.end() || it2 == book.end() || it3 == book.end())
                    continue;  // a leg not ready yet — not counted

                const auto t0 = std::chrono::steady_clock::now();
                auto result = TriangleCalculator::evaluate(it1->second, it2->second, it3->second);
                if (!result) continue;  // a leg not ready yet — not counted

                const double fwd_pct    = (result->forward_ratio - 1.0) * 100.0;
                const double rev_pct    = (result->reverse_ratio - 1.0) * 100.0;
                const double best_gross = std::max(fwd_pct, rev_pct);
                const char*  dir        = (fwd_pct > rev_pct) ? "FWD" : "REV";
                const double net_pct    = best_gross - TOTAL_COST_PCT;
                const auto t1 = std::chrono::steady_clock::now();

                const long long ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                total_ns += ns;
                if (ns > max_ns) max_ns = ns;
                ++total_evaluations;
                ++tri_eval_counts[idx];

                if (net_pct > FIRE_THRESHOLD_PCT) {
                    ++fire_count;
                    std::ostringstream m;
                    m << ">>>>>> FIRE [" << td.name << " " << dir << "]  loop 3-leg"
                      << "  gross "    << std::fixed << std::setprecision(4) << best_gross << "%"
                      << "  net "      << net_pct   << "%"
                      << "  eval#"     << total_evaluations
                      << "  compute "  << ns        << "ns"
                      << "  feed_lag " << feed_lag_ms << "ms";
                    logger.log(m.str());
                }
                else if (best_gross > WATCH_THRESHOLD_PCT) {
                    ++watch_count;
                    std::ostringstream m;
                    m << " · WATCH [" << td.name << " " << dir << "]  loop 3-leg"
                      << "  gross "    << std::fixed << std::setprecision(4) << best_gross << "%"
                      << "  net "      << net_pct   << "%"
                      << "  eval#"     << total_evaluations
                      << "  compute "  << ns        << "ns"
                      << "  feed_lag " << feed_lag_ms << "ms";
                    logger.log(m.str());
                }

                // --- Global heartbeat every 5000 evaluations ---
                if (total_evaluations % HEARTBEAT_EVERY == 0) {
                    const long long avg_ns = total_ns / total_evaluations;
                    std::size_t active = 0;
                    for (const auto c : tri_eval_counts) if (c > 0) ++active;

                    std::ostringstream m;
                    m << "  [heartbeat] evals " << total_evaluations
                      << "  active " << active << "/" << triangles.size()
                      << "  | compute avg " << avg_ns << "ns  max " << max_ns << "ns"
                      << "  | WATCH " << watch_count << "  FIRE " << fire_count
                      << "  | feed_lag ";
                    if (lag_samples > 0) {
                        const long long avg_lag = total_lag_ms / lag_samples;
                        m << "min " << min_lag_ms << "ms  avg " << avg_lag
                          << "ms  max " << max_lag_ms << "ms";
                    } else {
                        m << "n/a (stream type has no timestamp)";
                    }
                    logger.log(m.str());
                }
            }
        });

        feed.start();

        asio::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&](const beast::error_code&, int){
            logger.log("[main] signal received, stopping.");
            ioc.stop();
        });

        ioc.run();
    }
    catch (const std::exception& e) {
        Logger::instance().error(std::string("[main] fatal: ") + e.what());
        return 1;
    }
    return 0;
}
