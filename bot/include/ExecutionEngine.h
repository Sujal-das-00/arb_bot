#pragma once

#include "BookUpdate.h"
#include "Config.h"
#include "OrderClient.h"
#include "RiskManager.h"
#include "TriangleDef.h"

#include <boost/lockfree/spsc_queue.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Direction of the loop being executed.
//   REV: USDT -> outer -> BTC -> USDT   (buy outer, sell for BTC, sell BTC)
//   FWD: USDT -> BTC   -> outer -> USDT (buy BTC, buy outer, sell outer)
enum class TriDir { FWD, REV };

enum class TradeState {
    IDLE, LEG1_PENDING, LEG1_FILLED, LEG2_PENDING, LEG2_FILLED,
    LEG3_PENDING, COMPLETE, ABORTED, ERROR_RECOVERY
};

// What Thread 1 hands to Thread 2 on a FIRE. Trivially copyable on purpose so
// it can ride a lock-free SPSC ring buffer. Carries the book snapshot at the
// instant of the signal so the paper record can be priced with zero latency.
struct TradeSignal {
    std::size_t tri_index = 0;
    TriDir      dir = TriDir::REV;
    int64_t     signal_ts_ms = 0;       // captured on Thread 1, the moment FIRE fires
    double      predicted_net_pct = 0;  // net% from the detector (already fee-adjusted)

    double outer_usdt_bid = 0, outer_usdt_ask = 0;
    double outer_btc_bid  = 0, outer_btc_ask  = 0;
    double btc_usdt_bid   = 0, btc_usdt_ask   = 0;
};

struct LegRecord {
    std::string symbol, side;
    double  intended_qty = 0, actual_qty = 0, actual_price = 0, commission = 0;
    int64_t order_sent_ts = 0, fill_confirmed_ts = 0, leg_duration_ms = 0;
};

struct TradeRecord {
    std::string trade_id;        // shared by the paper and testnet rows
    std::string type;            // "paper" | "testnet"
    std::string triangle_id;     // "ATOM-BTC-USDT REV"
    int64_t signal_ts = 0;
    int64_t leg1_sent_ts = 0, leg3_confirmed_ts = 0, total_duration_ms = 0;
    LegRecord legs[3];
    double starting_usdt = 0, ending_usdt = 0;
    double wallet_usdt = 0;   // simulated running balance AFTER this trade (compounding)
    double predicted_net_pct = 0, actual_net_pct = 0, slippage_pct = 0;
    TradeState final_state = TradeState::IDLE;
    std::string abort_reason;
};

// ---------------------------------------------------------------------------
// ExecutionEngine — three-thread dual-accounting harness.
//
//   Thread 1 (caller / feed): submit() pushes a TradeSignal, never blocks.
//   Thread 2 (execution):     drains signals, runs the 3-leg sequence on the
//                             testnet, one trade at a time; also synthesizes
//                             the instant "paper" record from the snapshot.
//   Thread 3 (logger):        drains finished TradeRecords, writes trades.csv,
//                             prints the console breakdown, rolls up latency /
//                             slippage / abort heartbeats and the comparison.
//
// All of this only runs when config.execution.enabled is true; otherwise the
// engine is inert and the bot stays a pure detector.
// ---------------------------------------------------------------------------
class ExecutionEngine {
public:
    ExecutionEngine(const BotConfig& config,
                    std::vector<TriangleDef> triangles,
                    RiskManager& risk,
                    OrderClient& client);
    ~ExecutionEngine();

    void start();   // launches Thread 2 + Thread 3
    void stop();    // signals shutdown and joins

    // Thread 1: non-blocking push. Returns false if the ring is full (dropped).
    bool submit(const TradeSignal& sig);

    // Thread 1: keep the engine's view of top-of-book fresh for rechecks.
    void on_book_update(const std::string& symbol, const BookUpdate& bu);

private:
    void execution_loop();   // Thread 2 body
    void logger_loop();      // Thread 3 body

    // One full testnet trade sized to `notional` USDT. Fills `rec`; returns with
    // final_state set.
    void run_testnet_trade(const TradeSignal& sig, TradeRecord& rec, double notional);
    // The instant book-price baseline, sized to `notional`. Never aborts, zero latency.
    void build_paper_record(const TradeSignal& sig, TradeRecord& rec, double notional);

    // Recompute the live loop ratio (>1.0 = still profitable) for a recheck.
    bool current_ratio(const TradeSignal& sig, double& ratio_out);

    // Sell whatever non-USDT asset a half-finished trade is holding back to USDT.
    void emergency_unwind(const TradeSignal& sig, int last_filled_leg, TradeRecord& rec);
    void error_recovery(const TradeSignal& sig, TradeRecord& rec);

    void push_result(const TradeRecord& rec);
    void write_csv_row(const TradeRecord& rec);  // per-day logs/trades_YYYY-MM-DD.csv
    void log_trade_console(const TradeRecord& rec);
    void maybe_log_heartbeat();
    void maybe_log_comparison();

    BotConfig                config_;
    std::vector<TriangleDef> triangles_;
    RiskManager&             risk_;
    OrderClient&             client_;

    double fee_frac_;        // taker fee as a fraction, e.g. 0.001
    int    order_timeout_ms_;
    double recheck_threshold_;
    double leg3_threshold_;
    bool   paper_only_;   // true: simulate only (paper records, no testnet orders)

    // Freely-compounding simulated wallet, owned by the execution thread (Thread 2).
    // Each trade multiplies wallet_usdt_ by its realized return ratio (ending/start).
    // In paper mode the order deploys the whole balance; in testnet mode the order
    // is a FIXED wallet_seed_-sized probe (to respect sandbox limits) and only the
    // % return feeds the wallet. wallet_seed_ is the starting capital (= trade_size_usdt),
    // used as the testnet probe size and the return-% baseline. The logger thread
    // never touches wallet_usdt_; it reads the balance off each TradeRecord.
    double wallet_usdt_;
    double wallet_seed_;

    // Thread 1 -> Thread 2 (hot path, lock-free).
    boost::lockfree::spsc_queue<TradeSignal, boost::lockfree::capacity<1024>> signal_q_;

    // Thread 1 -> Thread 2 live book for rechecks (guarded; only touched when
    // execution is enabled, so the detector's hot path pays nothing otherwise).
    std::mutex book_mtx_;
    std::unordered_map<std::string, BookUpdate> book_;

    // Thread 2 -> Thread 3 result queue.
    std::mutex result_mtx_;
    std::deque<TradeRecord> result_q_;

    std::thread exec_thread_;
    std::thread logger_thread_;
    std::atomic<bool> running_{false};

    // Logger-thread-only stats.
    std::vector<int64_t> durations_ms_;
    std::vector<double>  slippages_;
    long long completed_ = 0;
    long long aborted_   = 0;
    long long errored_   = 0;
    long long primary_rows_ = 0;   // reported rows: testnet rows, or paper rows in paper-only mode
    double    last_wallet_usdt_ = 0;   // Thread 3: latest balance seen, for the heartbeat
    // Per-triangle paper/testnet net% accumulators for the comparison table.
    struct TriStat { double paper_sum = 0; long paper_n = 0;
                     double testnet_sum = 0; long testnet_n = 0; long aborts = 0; };
    std::unordered_map<std::string, TriStat> tri_stats_;
};
