#pragma once

#include <string>
#include <cstdint>

// One parsed top-of-book snapshot for a single symbol.
//
// The partial book depth stream (@depth5) gives us the best bid/ask plus
// volume available at that best level. For triangular arb we care most about
// the best price and whether there is enough volume there to fill our leg.
//
// IMPORTANT: the @depth5 stream does not carry an exchange-side event time,
// so `recv_time_ns` is a LOCAL timestamp taken the instant we parsed the
// message. It measures "how fresh is this in our process", not "when did
// Binance emit it". If you later need exchange time, switch the subscription
// to the diff stream (@depth) which includes an "E" event-time field.
struct BookUpdate {
    std::string symbol;        // e.g. "BTCUSDT" (uppercase, normalized)

    double best_bid = 0.0;     // highest price a buyer will pay
    double best_bid_qty = 0.0; // volume available at best_bid
    double best_ask = 0.0;     // lowest price a seller will accept
    double best_ask_qty = 0.0; // volume available at best_ask

    uint64_t last_update_id = 0;  // Binance book version; strictly increasing
    int64_t  recv_time_ns = 0;    // local steady-clock time at parse, nanoseconds
    long long   event_time_ms{0};
    // A snapshot is only usable if both sides have a real price and volume.
    // An empty book side (no bids or no asks) makes the symbol untradeable
    // for that instant and the calculator must skip it.
    bool valid() const {
        return best_bid > 0.0 && best_ask > 0.0
            && best_bid_qty > 0.0 && best_ask_qty > 0.0;
    }
};
