#include "MessageParser.h"

#include <gtest/gtest.h>

// A well-formed combined-stream depth5 message for BTCUSDT.
static const char* kValidBtc = R"({
  "stream": "btcusdt@depth5",
  "data": {
    "lastUpdateId": 12345,
    "bids": [
      ["100.10", "2.500"],
      ["100.05", "1.200"]
    ],
    "asks": [
      ["100.15", "0.800"],
      ["100.20", "3.100"]
    ]
  }
})";

TEST(MessageParser, ParsesValidMessage) {
    auto u = MessageParser::parse(kValidBtc);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->symbol, "BTCUSDT");           // normalized to uppercase
    EXPECT_DOUBLE_EQ(u->best_bid, 100.10);
    EXPECT_DOUBLE_EQ(u->best_bid_qty, 2.500);
    EXPECT_DOUBLE_EQ(u->best_ask, 100.15);
    EXPECT_DOUBLE_EQ(u->best_ask_qty, 0.800);
    EXPECT_EQ(u->last_update_id, 12345u);
    EXPECT_GT(u->recv_time_ns, 0);
    EXPECT_TRUE(u->valid());
}

TEST(MessageParser, TakesTopOfBookFromIndexZero) {
    // Best bid must be the FIRST bid (highest), best ask the FIRST ask (lowest).
    auto u = MessageParser::parse(kValidBtc);
    ASSERT_TRUE(u.has_value());
    EXPECT_DOUBLE_EQ(u->best_bid, 100.10);  // not 100.05
    EXPECT_DOUBLE_EQ(u->best_ask, 100.15);  // not 100.20
}

TEST(MessageParser, RejectsMalformedJson) {
    EXPECT_FALSE(MessageParser::parse("{ this is not json ").has_value());
    EXPECT_FALSE(MessageParser::parse("").has_value());
    EXPECT_FALSE(MessageParser::parse("12345").has_value());
}

TEST(MessageParser, RejectsMissingData) {
    EXPECT_FALSE(MessageParser::parse(R"({"stream":"btcusdt@depth5"})").has_value());
}

TEST(MessageParser, RejectsMissingStream) {
    EXPECT_FALSE(MessageParser::parse(
        R"({"data":{"bids":[["1","1"]],"asks":[["2","1"]]}})").has_value());
}

TEST(MessageParser, RejectsEmptyBookSide) {
    const char* emptyBids = R"({
      "stream": "ethusdt@depth5",
      "data": { "bids": [], "asks": [["2","1"]] }
    })";
    EXPECT_FALSE(MessageParser::parse(emptyBids).has_value());
}

TEST(MessageParser, RejectsCrossedBook) {
    // best_bid >= best_ask is corrupt and must be rejected.
    const char* crossed = R"({
      "stream": "ethusdt@depth5",
      "data": { "bids": [["101.0","1"]], "asks": [["100.0","1"]] }
    })";
    EXPECT_FALSE(MessageParser::parse(crossed).has_value());
}

TEST(MessageParser, RejectsZeroQuantity) {
    const char* zeroQty = R"({
      "stream": "ethusdt@depth5",
      "data": { "bids": [["100.0","0"]], "asks": [["100.5","1"]] }
    })";
    EXPECT_FALSE(MessageParser::parse(zeroQty).has_value());
}

TEST(MessageParser, RejectsNonNumericPrice) {
    const char* garbage = R"({
      "stream": "ethusdt@depth5",
      "data": { "bids": [["abc","1"]], "asks": [["100.5","1"]] }
    })";
    EXPECT_FALSE(MessageParser::parse(garbage).has_value());
}

TEST(MessageParser, NormalizesSymbolFromStreamName) {
    const char* eth = R"({
      "stream": "ethbtc@depth5",
      "data": { "bids": [["0.060","5"]], "asks": [["0.061","5"]] }
    })";
    auto u = MessageParser::parse(eth);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->symbol, "ETHBTC");
}

TEST(MessageParser, HandlesMissingLastUpdateId) {
    const char* noId = R"({
      "stream": "ethbtc@depth5",
      "data": { "bids": [["0.060","5"]], "asks": [["0.061","5"]] }
    })";
    auto u = MessageParser::parse(noId);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->last_update_id, 0u);  // defaults cleanly, no crash
}
