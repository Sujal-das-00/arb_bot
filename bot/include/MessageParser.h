#pragma once

#include "BookUpdate.h"

#include <string>
#include <optional>

// MessageParser converts a raw Binance combined-stream message into a
// BookUpdate. It is deliberately pure and stateless so it can be unit-tested
// in isolation with hand-written JSON — no network required.
//
// Expected envelope (partial book depth, combined stream):
//   {
//     "stream": "btcusdt@depth5",
//     "data": {
//       "lastUpdateId": 160,
//       "bids": [ ["price","qty"], ... ],
//       "asks": [ ["price","qty"], ... ]
//     }
//   }
//
// The parser returns std::nullopt (rather than throwing) for any message it
// cannot turn into a usable BookUpdate: control frames, malformed JSON,
// unexpected shape, or an empty book side. The caller decides what to do with
// a miss — typically log and continue. Never let one bad message kill the feed.
class MessageParser {
public:
    // Parse one raw payload. Returns a BookUpdate on success, nullopt on any
    // shape we don't recognize or can't use. Does not throw on bad input.
    static std::optional<BookUpdate> parse(const std::string& raw);

private:
    // Local monotonic timestamp in nanoseconds, taken at parse time.
    static int64_t now_ns();
};
