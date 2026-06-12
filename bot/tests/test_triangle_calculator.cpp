#include "TriangleCalculator.h"
#include <gtest/gtest.h>

// Helper to build a BookUpdate with just the prices we care about.
static BookUpdate make_book(const std::string& sym, double bid, double ask) {
    BookUpdate b;
    b.symbol = sym;
    b.best_bid = bid;
    b.best_ask = ask;
    b.best_bid_qty = 1.0;
    b.best_ask_qty = 1.0;
    b.last_update_id = 1;
    return b;
}

TEST(TriangleCalculator, AlignedMarketYieldsRatioNearOne) {
    // Perfectly aligned prices: BTC=60000, ETH=2000, ETHBTC=2000/60000=0.03333...
    auto btcusdt = make_book("BTCUSDT", 60000.0, 60000.0);
    auto ethusdt = make_book("ETHUSDT", 2000.0,  2000.0);
    auto ethbtc  = make_book("ETHBTC",  1.0/30.0, 1.0/30.0);

    auto r = TriangleCalculator::evaluate(btcusdt, ethusdt, ethbtc);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(r->forward_ratio, 1.0, 1e-9);
    EXPECT_NEAR(r->reverse_ratio, 1.0, 1e-9);
}

TEST(TriangleCalculator, MispricedTriangleYieldsRatioAboveOne) {
    // ETHBTC priced LOW vs the implied 2000/60000 — buying BTC->ETH cheap, selling ETH high
    auto btcusdt = make_book("BTCUSDT", 60000.0, 60000.0);
    auto ethusdt = make_book("ETHUSDT", 2000.0,  2000.0);
    auto ethbtc  = make_book("ETHBTC",  0.0330,  0.0330);  // implied is 0.03333

    auto r = TriangleCalculator::evaluate(btcusdt, ethusdt, ethbtc);
    ASSERT_TRUE(r.has_value());
    EXPECT_GT(r->forward_ratio, 1.0);  // forward direction is profitable
}

TEST(TriangleCalculator, RejectsZeroPrices) {
    auto btcusdt = make_book("BTCUSDT", 0.0, 60000.0);  // bad bid
    auto ethusdt = make_book("ETHUSDT", 2000.0, 2000.0);
    auto ethbtc  = make_book("ETHBTC",  0.0333, 0.0333);

    auto r = TriangleCalculator::evaluate(btcusdt, ethusdt, ethbtc);
    EXPECT_FALSE(r.has_value());
}

TEST(TriangleCalculator, SpreadCostsMoney) {
    // Wide spread should make both directions unprofitable vs zero-spread
    auto btcusdt = make_book("BTCUSDT", 59900.0, 60100.0);
    auto ethusdt = make_book("ETHUSDT", 1990.0,  2010.0);
    auto ethbtc  = make_book("ETHBTC",  0.0331,  0.0335);

    auto r = TriangleCalculator::evaluate(btcusdt, ethusdt, ethbtc);
    ASSERT_TRUE(r.has_value());
    EXPECT_LT(r->forward_ratio, 1.0);
    EXPECT_LT(r->reverse_ratio, 1.0);
}