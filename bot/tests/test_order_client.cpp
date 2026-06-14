#include "OrderClient.h"
#include "Config.h"

#include <gtest/gtest.h>

// --- HMAC-SHA256: RFC-style canonical vectors -----------------------------
// key="key", data="The quick brown fox jumps over the lazy dog"
//   -> f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8
TEST(OrderClient, HmacKnownVector) {
    EXPECT_EQ(hmac_sha256_hex("key", "The quick brown fox jumps over the lazy dog"),
              "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
}

// Empty key + empty data has a well-known digest.
TEST(OrderClient, HmacEmptyInputs) {
    EXPECT_EQ(hmac_sha256_hex("", ""),
              "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
}

// Output is always 64 lowercase hex chars.
TEST(OrderClient, HmacOutputShape) {
    const std::string h = hmac_sha256_hex("secret", "symbol=ATOMUSDT&side=BUY&timestamp=1700000000000");
    EXPECT_EQ(h.size(), 64u);
    for (char c : h) EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
}

// --- Lot flooring: floor(qty/step)*step, never round up -------------------
TEST(OrderClient, FloorToStepBasic) {
    EXPECT_NEAR(OrderClient::floor_to_step(12.137, 0.001), 12.137, 1e-9);
    EXPECT_NEAR(OrderClient::floor_to_step(12.1379, 0.01), 12.13,  1e-9);
    EXPECT_NEAR(OrderClient::floor_to_step(12.19,   0.1),  12.1,   1e-9);
    EXPECT_NEAR(OrderClient::floor_to_step(0.9999,  1.0),  0.0,    1e-9);
    EXPECT_NEAR(OrderClient::floor_to_step(5.0,     1.0),  5.0,    1e-9);
}

// Values sitting exactly on a multiple must not drop a step due to fp error.
TEST(OrderClient, FloorToStepExactMultiple) {
    EXPECT_NEAR(OrderClient::floor_to_step(0.003, 0.001), 0.003, 1e-9);
    EXPECT_NEAR(OrderClient::floor_to_step(1.230, 0.010), 1.230, 1e-9);
}

// A zero/absent step is a no-op (qty passes through unchanged).
TEST(OrderClient, FloorToStepZeroStep) {
    EXPECT_DOUBLE_EQ(OrderClient::floor_to_step(7.77, 0.0), 7.77);
}

// round_to_lot uses the cached step; unknown symbols pass through.
TEST(OrderClient, RoundToLotUsesCachedStep) {
    BotConfig cfg;  // no network touched
    OrderClient client(cfg);
    client.set_lot_step("ATOMUSDT", 0.01);
    EXPECT_NEAR(client.round_to_lot("ATOMUSDT", 12.1379), 12.13, 1e-9);
    EXPECT_DOUBLE_EQ(client.round_to_lot("UNKNOWNUSDT", 3.14159), 3.14159);
}
