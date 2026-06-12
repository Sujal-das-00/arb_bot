#pragma once
#include "BookUpdate.h"
#include <optional>

// Result of one triangle evaluation.
// ratio = final_usdt / starting_usdt, before fees.
// A ratio of 1.0040 means a raw 0.40% gap.
// After 0.3% in taker fees you keep 0.10% — fire if ratio > 1.003.
struct TriangleResult {
    double forward_ratio;   // USDT -> BTC -> ETH -> USDT
    double reverse_ratio;   // USDT -> ETH -> BTC -> USDT
};

class TriangleCalculator {
public:
    // Pure function. No state. No throws. Returns nullopt only if any price
    // is zero or negative (shouldn't happen with valid book data, but defensive).
    static std::optional<TriangleResult> evaluate(
        const BookUpdate& btcusdt,
        const BookUpdate& ethusdt,
        const BookUpdate& ethbtc);
};