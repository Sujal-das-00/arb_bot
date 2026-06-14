#include "ExecutionEngine.h"

#include "Logger.h"
#include "TriangleCalculator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

const char* dir_str(TriDir d) { return d == TriDir::FWD ? "FWD" : "REV"; }

const char* state_str(TradeState s) {
    switch (s) {
        case TradeState::COMPLETE:       return "COMPLETE";
        case TradeState::ABORTED:        return "ABORTED";
        case TradeState::ERROR_RECOVERY: return "ERROR_RECOVERY";
        default:                         return "INCOMPLETE";
    }
}

std::string make_trade_id() {
    static std::atomic<uint64_t> ctr{0};
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "T" + std::to_string(us) + "-" + std::to_string(ctr.fetch_add(1) % 1000);
}

// percentile from an unsorted copy (p in [0,100])
int64_t pct(std::vector<int64_t> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const double idx = (p / 100.0) * (v.size() - 1);
    return v[static_cast<size_t>(std::lround(idx))];
}

} // namespace

ExecutionEngine::ExecutionEngine(const BotConfig& config,
                                 std::vector<TriangleDef> triangles,
                                 RiskManager& risk,
                                 OrderClient& client)
    : config_(config),
      triangles_(std::move(triangles)),
      risk_(risk),
      client_(client),
      trade_size_usdt_(config.risk.trade_size_usdt),
      fee_frac_(config.fees.taker_fee_pct / 100.0),
      order_timeout_ms_(static_cast<int>(config.execution.order_timeout_ms)),
      recheck_threshold_(config.execution.recheck_threshold),
      leg3_threshold_(config.execution.leg3_threshold) {}

ExecutionEngine::~ExecutionEngine() { stop(); }

void ExecutionEngine::start() {
    if (running_.exchange(true)) return;
    exec_thread_   = std::thread([this] { execution_loop(); });
    logger_thread_ = std::thread([this] { logger_loop(); });
}

void ExecutionEngine::stop() {
    if (!running_.exchange(false)) return;
    if (exec_thread_.joinable())   exec_thread_.join();
    if (logger_thread_.joinable()) logger_thread_.join();
}

bool ExecutionEngine::submit(const TradeSignal& sig) {
    return signal_q_.push(sig);
}

void ExecutionEngine::on_book_update(const std::string& symbol, const BookUpdate& bu) {
    std::lock_guard<std::mutex> lk(book_mtx_);
    book_[symbol] = bu;
}

// --------------------------------------------------------------------------
// Thread 2 — execution
// --------------------------------------------------------------------------
void ExecutionEngine::execution_loop() {
    while (running_.load()) {
        // Drain everything currently queued, then dedupe by (triangle,dir):
        // duplicate signals that piled up while we were busy collapse to one.
        std::vector<TradeSignal> batch;
        TradeSignal s;
        while (signal_q_.pop(s)) batch.push_back(s);

        if (batch.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        std::vector<TradeSignal> unique;
        for (const auto& sig : batch) {
            auto same = [&](const TradeSignal& u) {
                return u.tri_index == sig.tri_index && u.dir == sig.dir;
            };
            auto it = std::find_if(unique.begin(), unique.end(), same);
            if (it == unique.end()) unique.push_back(sig);
            else                    *it = sig;  // keep the freshest snapshot
        }

        for (const auto& sig : unique) {
            if (!running_.load()) break;
            const std::string tid = make_trade_id();

            TradeRecord paper;
            build_paper_record(sig, paper);
            paper.trade_id = tid;
            push_result(paper);

            TradeRecord tn;
            run_testnet_trade(sig, tn);
            tn.trade_id = tid;
            push_result(tn);

            if (tn.final_state == TradeState::COMPLETE) {
                risk_.record_trade_result(tn.ending_usdt - tn.starting_usdt);
            }
        }
    }
}

void ExecutionEngine::build_paper_record(const TradeSignal& sig, TradeRecord& rec) {
    const TriangleDef& td = triangles_[sig.tri_index];
    rec.type = "paper";
    rec.triangle_id = td.name + " " + dir_str(sig.dir);
    rec.signal_ts = sig.signal_ts_ms;
    rec.predicted_net_pct = sig.predicted_net_pct;
    rec.starting_usdt = trade_size_usdt_;
    rec.total_duration_ms = 0;  // instant, by definition
    rec.final_state = TradeState::COMPLETE;

    const double f = 1.0 - fee_frac_;  // multiplicative commission per leg
    const double start = trade_size_usdt_;

    if (sig.dir == TriDir::REV) {
        // USDT -> outer (buy@ask) -> BTC (sell@bid) -> USDT (sell@bid)
        const double outer = (start / sig.outer_usdt_ask) * f;
        const double btc   = (outer * sig.outer_btc_bid) * f;
        const double usdt  = (btc   * sig.btc_usdt_bid)  * f;
        rec.legs[0] = {td.outer_usdt, "BUY",  start / sig.outer_usdt_ask, start / sig.outer_usdt_ask, sig.outer_usdt_ask, 0, 0, 0, 0};
        rec.legs[1] = {td.outer_btc,  "SELL", outer, outer, sig.outer_btc_bid, 0, 0, 0, 0};
        rec.legs[2] = {td.btc_usdt,   "SELL", btc,   btc,   sig.btc_usdt_bid,  0, 0, 0, 0};
        rec.ending_usdt = usdt;
    } else {
        // USDT -> BTC (buy@ask) -> outer (buy@ask) -> USDT (sell@bid)
        const double btc   = (start / sig.btc_usdt_ask) * f;
        const double outer = (btc / sig.outer_btc_ask)  * f;
        const double usdt  = (outer * sig.outer_usdt_bid) * f;
        rec.legs[0] = {td.btc_usdt,   "BUY",  start / sig.btc_usdt_ask, start / sig.btc_usdt_ask, sig.btc_usdt_ask, 0, 0, 0, 0};
        rec.legs[1] = {td.outer_btc,  "BUY",  btc / sig.outer_btc_ask, btc / sig.outer_btc_ask, sig.outer_btc_ask, 0, 0, 0, 0};
        rec.legs[2] = {td.outer_usdt, "SELL", outer, outer, sig.outer_usdt_bid, 0, 0, 0, 0};
        rec.ending_usdt = usdt;
    }

    rec.actual_net_pct = (rec.ending_usdt - start) / start * 100.0;
    rec.slippage_pct   = rec.predicted_net_pct - rec.actual_net_pct;
}

bool ExecutionEngine::current_ratio(const TradeSignal& sig, double& ratio_out) {
    const TriangleDef& td = triangles_[sig.tri_index];
    BookUpdate b1, b2, b3;
    {
        std::lock_guard<std::mutex> lk(book_mtx_);
        auto it1 = book_.find(td.btc_usdt);
        auto it2 = book_.find(td.outer_usdt);
        auto it3 = book_.find(td.outer_btc);
        if (it1 == book_.end() || it2 == book_.end() || it3 == book_.end()) return false;
        b1 = it1->second; b2 = it2->second; b3 = it3->second;
    }
    auto r = TriangleCalculator::evaluate(b1, b2, b3);
    if (!r) return false;
    ratio_out = (sig.dir == TriDir::FWD) ? r->forward_ratio : r->reverse_ratio;
    return true;
}

void ExecutionEngine::run_testnet_trade(const TradeSignal& sig, TradeRecord& rec) {
    const TriangleDef& td = triangles_[sig.tri_index];
    rec.type = "testnet";
    rec.triangle_id = td.name + " " + dir_str(sig.dir);
    rec.signal_ts = sig.signal_ts_ms;
    rec.predicted_net_pct = sig.predicted_net_pct;
    rec.starting_usdt = trade_size_usdt_;

    // Define the three legs (symbol, side, and how to size each) per direction.
    std::string sym[3], side[3];
    if (sig.dir == TriDir::REV) {
        sym[0] = td.outer_usdt; side[0] = "BUY";
        sym[1] = td.outer_btc;  side[1] = "SELL";
        sym[2] = td.btc_usdt;   side[2] = "SELL";
    } else {
        sym[0] = td.btc_usdt;   side[0] = "BUY";
        sym[1] = td.outer_btc;  side[1] = "BUY";
        sym[2] = td.outer_usdt; side[2] = "SELL";
    }

    auto send_leg = [&](int i, double base_qty) -> bool {
        OrderRequest req;
        req.symbol = sym[i];
        req.side   = side[i];
        req.quantity = client_.round_to_lot(sym[i], base_qty);
        rec.legs[i].symbol = sym[i];
        rec.legs[i].side   = side[i];
        rec.legs[i].intended_qty = req.quantity;
        rec.legs[i].order_sent_ts = now_ms();
        if (i == 0) rec.leg1_sent_ts = rec.legs[0].order_sent_ts;

        OrderResult res = client_.place_market_order(req, order_timeout_ms_);
        if (!res.success && !res.client_order_id.empty()) {
            // Transport hiccup: ask the exchange what actually happened.
            OrderResult q = client_.query_order(sym[i], res.client_order_id);
            if (q.success) res = q;
        }
        rec.legs[i].fill_confirmed_ts = now_ms();
        rec.legs[i].leg_duration_ms = rec.legs[i].fill_confirmed_ts - rec.legs[i].order_sent_ts;

        if (!res.success) {
            rec.abort_reason = "leg" + std::to_string(i + 1) + "_failed: " + res.error_msg;
            return false;
        }
        rec.legs[i].actual_qty   = res.executed_qty;
        rec.legs[i].actual_price = res.avg_fill_price;
        rec.legs[i].commission   = res.commission;
        return true;
    };

    // What we actually hold after a leg, net of commission. Binance takes the
    // fee out of the asset you RECEIVE: the bought base after a BUY, the quote
    // proceeds after a SELL. Forwarding the gross qty would oversell leg 2.
    auto held_after_buy  = [](const LegRecord& l) { return l.actual_qty - l.commission; };
    auto held_after_sell = [](const LegRecord& l) { return l.actual_qty * l.actual_price - l.commission; };

    // ---- Leg 1 ----
    const double base1 = (sig.dir == TriDir::REV)
        ? trade_size_usdt_ / sig.outer_usdt_ask    // buy outer with USDT
        : trade_size_usdt_ / sig.btc_usdt_ask;     // buy BTC with USDT
    if (!send_leg(0, base1)) { error_recovery(sig, rec); return; }

    // ---- Recheck before leg 2 ----
    double ratio = 0.0;
    if (current_ratio(sig, ratio) && ratio < recheck_threshold_) {
        emergency_unwind(sig, 1, rec);
        rec.final_state = TradeState::ABORTED;
        rec.abort_reason = "gap_closed_before_leg2";
        Logger::instance().log("  [exec] ABORT " + rec.triangle_id +
                               " gap closed before leg2 (ratio " + std::to_string(ratio) + ")");
        return;
    }

    // ---- Leg 2 ----
    const double base2 = (sig.dir == TriDir::REV)
        ? held_after_buy(rec.legs[0])                       // sell the outer we hold
        : held_after_buy(rec.legs[0]) / sig.outer_btc_ask;  // buy outer with the BTC we hold
    if (!send_leg(1, base2)) { error_recovery(sig, rec); return; }

    // ---- Recheck before leg 3 ----
    if (current_ratio(sig, ratio) && ratio < leg3_threshold_) {
        emergency_unwind(sig, 2, rec);
        rec.final_state = TradeState::ABORTED;
        rec.abort_reason = "gap_closed_before_leg3";
        Logger::instance().log("  [exec] ABORT " + rec.triangle_id +
                               " gap closed before leg3 (ratio " + std::to_string(ratio) + ")");
        return;
    }

    // ---- Leg 3 ----
    const double base3 = (sig.dir == TriDir::REV)
        ? held_after_sell(rec.legs[1])   // BTC received from leg2 sell, net of fee
        : held_after_buy(rec.legs[1]);   // outer received from leg2 buy, net of fee
    if (!send_leg(2, base3)) { error_recovery(sig, rec); return; }

    // ---- Complete ----
    rec.leg3_confirmed_ts = rec.legs[2].fill_confirmed_ts;
    rec.total_duration_ms = rec.leg3_confirmed_ts - rec.signal_ts;
    rec.ending_usdt = held_after_sell(rec.legs[2]);  // USDT out of final sell, net of fee
    rec.actual_net_pct = (rec.ending_usdt - rec.starting_usdt) / rec.starting_usdt * 100.0;
    rec.slippage_pct = rec.predicted_net_pct - rec.actual_net_pct;
    rec.final_state = TradeState::COMPLETE;
}

void ExecutionEngine::emergency_unwind(const TradeSignal& sig, int last_filled_leg, TradeRecord& rec) {
    const TriangleDef& td = triangles_[sig.tri_index];

    // Figure out what non-USDT asset we are stuck holding, and the pair that
    // sells it straight back to USDT.
    std::string unwind_sym;
    double held_qty = 0.0;
    if (last_filled_leg == 1) {
        if (sig.dir == TriDir::REV) { unwind_sym = td.outer_usdt; held_qty = rec.legs[0].actual_qty; }
        else                        { unwind_sym = td.btc_usdt;   held_qty = rec.legs[0].actual_qty; }
    } else {  // last_filled_leg == 2
        if (sig.dir == TriDir::REV) { unwind_sym = td.btc_usdt;   held_qty = rec.legs[1].actual_qty * rec.legs[1].actual_price; }
        else                        { unwind_sym = td.outer_usdt; held_qty = rec.legs[1].actual_qty; }
    }

    OrderRequest req;
    req.symbol = unwind_sym;
    req.side = "SELL";
    req.quantity = client_.round_to_lot(unwind_sym, held_qty);
    OrderResult res = client_.place_market_order(req, order_timeout_ms_);

    rec.ending_usdt = res.success ? res.executed_qty * res.avg_fill_price : 0.0;
    rec.actual_net_pct = (rec.ending_usdt - rec.starting_usdt) / rec.starting_usdt * 100.0;
    rec.slippage_pct = rec.predicted_net_pct - rec.actual_net_pct;
}

void ExecutionEngine::error_recovery(const TradeSignal& sig, TradeRecord& rec) {
    const TriangleDef& td = triangles_[sig.tri_index];
    rec.final_state = TradeState::ERROR_RECOVERY;
    if (rec.abort_reason.empty()) rec.abort_reason = "leg_error";

    // Cancel anything still open on the involved pairs.
    client_.cancel_all(td.outer_usdt);
    client_.cancel_all(td.outer_btc);
    client_.cancel_all(td.btc_usdt);

    // Best-effort flatten: sell any non-USDT we ended up holding back to USDT.
    const std::string outer_asset =
        td.outer_usdt.size() > 4 ? td.outer_usdt.substr(0, td.outer_usdt.size() - 4) : td.outer_usdt;

    double recovered = 0.0;
    for (const auto& [asset, sym] : {std::make_pair(outer_asset, td.outer_usdt),
                                     std::make_pair(std::string("BTC"), td.btc_usdt)}) {
        const double bal = client_.get_balance(asset);
        if (bal <= 0.0) continue;
        OrderRequest req;
        req.symbol = sym; req.side = "SELL";
        req.quantity = client_.round_to_lot(sym, bal);
        if (req.quantity <= 0.0) continue;
        OrderResult res = client_.place_market_order(req, order_timeout_ms_);
        if (res.success) recovered += res.executed_qty * res.avg_fill_price;
    }
    rec.ending_usdt = recovered;
    rec.actual_net_pct = (rec.ending_usdt - rec.starting_usdt) / rec.starting_usdt * 100.0;

    Logger::instance().error("  [exec] ERROR_RECOVERY " + rec.triangle_id +
                             " reason=" + rec.abort_reason +
                             " recovered=$" + std::to_string(recovered));
}

// --------------------------------------------------------------------------
// Thread 3 — logger / CSV / stats
// --------------------------------------------------------------------------
void ExecutionEngine::push_result(const TradeRecord& rec) {
    std::lock_guard<std::mutex> lk(result_mtx_);
    result_q_.push_back(rec);
}

void ExecutionEngine::logger_loop() {
    write_csv_header_if_needed();
    while (running_.load() || !result_q_.empty()) {
        std::deque<TradeRecord> batch;
        {
            std::lock_guard<std::mutex> lk(result_mtx_);
            batch.swap(result_q_);
        }
        if (batch.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        for (const auto& rec : batch) {
            write_csv_row(rec);

            auto& ts = tri_stats_[rec.triangle_id];
            if (rec.type == "paper") {
                ts.paper_sum += rec.actual_net_pct; ++ts.paper_n;
                continue;  // console + rolling latency stats are testnet-only
            }

            // testnet row
            ++testnet_rows_;
            ts.testnet_sum += rec.actual_net_pct; ++ts.testnet_n;
            if (rec.final_state == TradeState::COMPLETE) {
                ++completed_;
                durations_ms_.push_back(rec.total_duration_ms);
                slippages_.push_back(rec.slippage_pct);
            } else if (rec.final_state == TradeState::ABORTED) {
                ++aborted_; ++ts.aborts;
            } else if (rec.final_state == TradeState::ERROR_RECOVERY) {
                ++errored_;
            }
            log_trade_console(rec);
            maybe_log_heartbeat();
            maybe_log_comparison();
        }
    }
}

void ExecutionEngine::write_csv_header_if_needed() {
    if (csv_header_written_) return;
    const fs::path p = config_.execution.trades_csv;
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    const bool exists = fs::exists(p) && fs::file_size(p) > 0;
    std::ofstream f(p, std::ios::app);
    if (f && !exists) {
        f << "trade_id,type,triangle_id,signal_ts,total_duration_ms,"
             "signal_to_leg1_ms,leg1_duration_ms,leg2_duration_ms,leg3_duration_ms,"
             "predicted_net_pct,actual_net_pct,slippage_pct,"
             "starting_usdt,ending_usdt,"
             "leg1_price,leg2_price,leg3_price,"
             "final_state,abort_reason\n";
    }
    csv_header_written_ = true;
}

void ExecutionEngine::write_csv_row(const TradeRecord& rec) {
    std::ofstream f(config_.execution.trades_csv, std::ios::app);
    if (!f) return;
    const int64_t signal_to_leg1 = (rec.leg1_sent_ts > 0 && rec.signal_ts > 0)
        ? rec.leg1_sent_ts - rec.signal_ts : 0;
    f << std::fixed << std::setprecision(6)
      << rec.trade_id << ',' << rec.type << ',' << rec.triangle_id << ','
      << rec.signal_ts << ',' << rec.total_duration_ms << ','
      << signal_to_leg1 << ',' << rec.legs[0].leg_duration_ms << ','
      << rec.legs[1].leg_duration_ms << ',' << rec.legs[2].leg_duration_ms << ','
      << rec.predicted_net_pct << ',' << rec.actual_net_pct << ',' << rec.slippage_pct << ','
      << rec.starting_usdt << ',' << rec.ending_usdt << ','
      << rec.legs[0].actual_price << ',' << rec.legs[1].actual_price << ',' << rec.legs[2].actual_price << ','
      << state_str(rec.final_state) << ',' << rec.abort_reason << '\n';
}

void ExecutionEngine::log_trade_console(const TradeRecord& rec) {
    std::ostringstream m;
    m << "\n  ┌─ TRADE " << rec.trade_id << "  [" << rec.triangle_id << "]  "
      << state_str(rec.final_state) << "\n";
    m << "  │ latency:  signal→leg1 " << (rec.leg1_sent_ts - rec.signal_ts) << "ms"
      << "  leg1 " << rec.legs[0].leg_duration_ms << "ms"
      << "  leg2 " << rec.legs[1].leg_duration_ms << "ms"
      << "  leg3 " << rec.legs[2].leg_duration_ms << "ms"
      << "  TOTAL " << rec.total_duration_ms << "ms\n";
    m << "  │ P&L:      predicted " << std::fixed << std::setprecision(4) << rec.predicted_net_pct
      << "%  actual " << rec.actual_net_pct << "%  slippage " << rec.slippage_pct << "%\n";
    for (int i = 0; i < 3; ++i) {
        m << "  │ leg" << (i + 1) << ":     " << rec.legs[i].symbol << " " << rec.legs[i].side
          << "  qty " << std::setprecision(6) << rec.legs[i].actual_qty
          << "  @ " << rec.legs[i].actual_price << "\n";
    }
    if (!rec.abort_reason.empty()) m << "  │ note:     " << rec.abort_reason << "\n";
    m << "  └─ start $" << std::setprecision(2) << rec.starting_usdt
      << " → end $" << rec.ending_usdt;
    Logger::instance().log(m.str());
}

void ExecutionEngine::maybe_log_heartbeat() {
    if (testnet_rows_ == 0 || testnet_rows_ % 10 != 0) return;
    double slip_sum = 0, slip_max = 0;
    for (double s : slippages_) { slip_sum += s; slip_max = std::max(slip_max, std::fabs(s)); }
    const double slip_avg = slippages_.empty() ? 0.0 : slip_sum / slippages_.size();
    std::ostringstream m;
    m << "  [exec_heartbeat] exec_latency: p50=" << pct(durations_ms_, 50)
      << "ms p95=" << pct(durations_ms_, 95) << "ms p99=" << pct(durations_ms_, 99) << "ms"
      << "  | slippage avg " << std::fixed << std::setprecision(4) << slip_avg
      << "% max " << slip_max << "%"
      << "  | abort_rate " << aborted_ << "/" << testnet_rows_;
    Logger::instance().log(m.str());
}

void ExecutionEngine::maybe_log_comparison() {
    if (testnet_rows_ == 0 || testnet_rows_ % 50 != 0) return;
    std::ostringstream m;
    m << "\n  ╔═ COMPARISON (paper vs testnet) ═══════════════════════════════\n";
    m << "  ║ triangle                paper_net  testnet_net  reality  aborts  hint\n";
    for (const auto& [name, s] : tri_stats_) {
        const double pavg = s.paper_n   ? s.paper_sum   / s.paper_n   : 0.0;
        const double tavg = s.testnet_n ? s.testnet_sum / s.testnet_n : 0.0;
        const double reality = (pavg != 0.0) ? tavg / pavg : 0.0;
        const double abort_rate = s.testnet_n ? double(s.aborts) / s.testnet_n : 0.0;
        const char* hint = "—";
        if (reality > 0.70 && abort_rate < 0.05)      hint = "live candidate";
        else if (reality < 0.30 || abort_rate > 0.30) hint = "needs work";
        m << "  ║ " << std::left << std::setw(22) << name << std::right
          << std::fixed << std::setprecision(4)
          << std::setw(10) << pavg << std::setw(13) << tavg
          << std::setprecision(2) << std::setw(9) << reality
          << std::setw(8) << s.aborts << "  " << hint << "\n";
    }
    m << "  ╚════════════════════════════════════════════════════════════════";
    Logger::instance().log(m.str());
}
