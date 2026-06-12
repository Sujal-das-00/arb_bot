#include "TriangleCalculator.h"

std::optional<TriangleResult> TriangleCalculator::evaluate(
    const BookUpdate& btcusdt,
    const BookUpdate& ethusdt,
    const BookUpdate& ethbtc)
{
    // Defensive: no zero or negative prices
    if (btcusdt.best_ask <= 0 || btcusdt.best_bid <= 0 ||
        ethusdt.best_ask <= 0 || ethusdt.best_bid <= 0 ||
        ethbtc.best_ask  <= 0 || ethbtc.best_bid  <= 0)
    {
        return std::nullopt;
    }

    // Forward: USDT -> BTC -> ETH -> USDT
    // Buy BTC at ask, buy ETH (paying with BTC) at ethbtc ask, sell ETH at ethusdt bid
    const double btc_got  = 1.0 / btcusdt.best_ask;
    const double eth_got  = btc_got / ethbtc.best_ask;
    const double usdt_fwd = eth_got * ethusdt.best_bid;

    // Reverse: USDT -> ETH -> BTC -> USDT
    // Buy ETH at ask, sell ETH for BTC at ethbtc bid, sell BTC at btcusdt bid
    const double eth_got2 = 1.0 / ethusdt.best_ask;
    const double btc_got2 = eth_got2 * ethbtc.best_bid;
    const double usdt_rev = btc_got2 * btcusdt.best_bid;

    return TriangleResult{ usdt_fwd, usdt_rev };
}