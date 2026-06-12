#include "Dispatch.h"
#include "TriangleDef.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

// The production set: 10 triangles all sharing the BTCUSDT bridge leg.
static std::vector<TriangleDef> make_ten() {
    auto t = [](const char* name, const char* outer_usdt, const char* outer_btc) {
        return TriangleDef{name, outer_usdt, outer_btc, "BTCUSDT"};
    };
    return {
        t("AVAX-BTC-USDT", "AVAXUSDT", "AVAXBTC"),
        t("SOL-BTC-USDT",  "SOLUSDT",  "SOLBTC"),
        t("LINK-BTC-USDT", "LINKUSDT", "LINKBTC"),
        t("DOT-BTC-USDT",  "DOTUSDT",  "DOTBTC"),
        t("ATOM-BTC-USDT", "ATOMUSDT", "ATOMBTC"),
        t("INJ-BTC-USDT",  "INJUSDT",  "INJBTC"),
        t("NEAR-BTC-USDT", "NEARUSDT", "NEARBTC"),
        t("ADA-BTC-USDT",  "ADAUSDT",  "ADABTC"),
        t("XRP-BTC-USDT",  "XRPUSDT",  "XRPBTC"),
        t("DOGE-BTC-USDT", "DOGEUSDT", "DOGEBTC"),
    };
}

// ---- collect_symbols: dedupe the shared bridge to one entry ----

TEST(Dispatch, CollectSymbolsDedupesSharedBridge) {
    auto syms = dispatch::collect_symbols(make_ten());
    // 10 outer-USDT + 10 outer-BTC + 1 shared BTCUSDT = 21 unique.
    EXPECT_EQ(syms.size(), 21u);

    const long btc = std::count(syms.begin(), syms.end(), std::string("BTCUSDT"));
    EXPECT_EQ(btc, 1) << "BTCUSDT must appear exactly once after dedupe";
}

// ---- reverse index correctness ----

TEST(Dispatch, BridgeMapsToAllTriangles) {
    auto tris  = make_ten();
    auto index = dispatch::build_reverse_index(tris);

    ASSERT_TRUE(index.count("BTCUSDT"));
    EXPECT_EQ(index.at("BTCUSDT").size(), tris.size());  // all 10
}

TEST(Dispatch, OuterLegMapsToExactlyOneTriangle) {
    auto index = dispatch::build_reverse_index(make_ten());

    ASSERT_TRUE(index.count("AVAXUSDT"));
    ASSERT_EQ(index.at("AVAXUSDT").size(), 1u);
    EXPECT_EQ(index.at("AVAXUSDT").front(), 0u);  // AVAX is triangle #0

    ASSERT_TRUE(index.count("AVAXBTC"));
    ASSERT_EQ(index.at("AVAXBTC").size(), 1u);
    EXPECT_EQ(index.at("AVAXBTC").front(), 0u);

    // DOGE is the last triangle (#9); both its legs route only there.
    ASSERT_TRUE(index.count("DOGEUSDT"));
    ASSERT_EQ(index.at("DOGEUSDT").size(), 1u);
    EXPECT_EQ(index.at("DOGEUSDT").front(), 9u);
}

TEST(Dispatch, UnrelatedSymbolTriggersNoEvaluation) {
    auto index = dispatch::build_reverse_index(make_ten());

    // A stream we never subscribed to (or a stray frame) must not be in the
    // index at all, so the hot path's find() misses and does zero work.
    EXPECT_EQ(index.find("ETHUSDT"), index.end());
    EXPECT_EQ(index.find("PEPEUSDT"), index.end());
}

// ---- per-message re-evaluation set: who recomputes when symbol X moves ----

TEST(Dispatch, BtcUpdateReevaluatesEveryTriangle) {
    auto tris  = make_ten();
    auto index = dispatch::build_reverse_index(tris);

    // Simulate a BTCUSDT tick: the routed set is every triangle index, once each.
    const auto& routed = index.at("BTCUSDT");
    std::vector<std::size_t> seen(routed.begin(), routed.end());
    std::sort(seen.begin(), seen.end());

    std::vector<std::size_t> expected(tris.size());
    for (std::size_t i = 0; i < tris.size(); ++i) expected[i] = i;
    EXPECT_EQ(seen, expected);
}

TEST(Dispatch, OuterUpdateReevaluatesOnlyItsTriangle) {
    auto index = dispatch::build_reverse_index(make_ten());
    // A SOLBTC tick routes to exactly triangle #1 and nothing else.
    EXPECT_EQ(index.at("SOLBTC"), (std::vector<std::size_t>{1}));
}
