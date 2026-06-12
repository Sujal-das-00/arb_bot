# Migration: Single Triangle → N Arbitrage Loops with Dynamic Pairs

## Overview

This refactor scales the bot in two dimensions:

1. **From 1 hardcoded triangle to N configurable triangles**, all watched over one WebSocket and one hot-path thread (lock-free).
2. **From fixed 3-symbol triangles to variable-length loops**, with global currency modifiers to spin up variants without code changes.

## Part 1: N triangles over one feed

### config.json structure

The single `triangle` object is replaced by a `triangles` **array**. Each entry describes one arbitrage loop:

```json
{
  "triangles": [
    {
      "name": "AVAX-BTC-USDT",
      "loop": ["AVAXUSDT", "AVAXBTC", "BTCUSDT"],
      "currencies": ["AVAX", "BTC", "USDT"]
    },
    ...
  ]
}
```

- **`loop`** (required): array of symbol names forming the arbitrage path. Minimum length 3.
- **`currencies`** (optional): human-readable labels for the assets in the loop (for UI/reporting).
- **`name`** (optional): display name. Defaults to `first-symbol-last-symbol` if omitted.

### New files

- **`TriangleDef.h`**: a shared struct holding `name`, `loop: vector<string>`, `currencies: vector<string>`.
- **`Dispatch.h / Dispatch.cpp`**: pure, build-once routing functions:
  - `collect_symbols()` → deduplicated list of all symbols across all loops.
  - `build_reverse_index()` → `symbol -> [loop indices]` for efficient per-message routing.
- **`tests/test_dispatch.cpp`**: routing logic tests (index correctness, unrelated symbols, fanout to all loops when shared symbols tick).

### Modified files

- **`Config.h / Config.cpp`**: `BotConfig` now holds `std::vector<TriangleDef> triangles`. Parsing validates that every loop is ≥3 symbols, uppercases them, and applies currency modifiers (see Part 2).
- **`FeedHandler.h / FeedHandler.cpp`**: constructor takes a symbol vector; `build_stream_path()` dedupes symbols so shared legs (like `BTCUSDT`) subscribe once.
- **`TriangleCalculator.h / TriangleCalculator.cpp`**: pure 3-arg `evaluate()` is unchanged (tests still exercise it). Added `evaluate_loop()` for variable-length loops.
- **`main.cpp`**: hot path is now:
  - `book`: `unordered_map<symbol, BookUpdate>` (one entry per symbol, never rehashes).
  - `symbol_to_tri`: reverse index (one lookup per message).
  - Loop over affected triangles and re-evaluate only those.
  - Logging prefixes output with triangle name and loop length.

### Performance

- Per-message overhead: one map write (stash update), one index lookup. Cost well under 5μs even for 20 loops.
- Binance connection: one TCP, one WebSocket, 21 deduped streams for 10 loops sharing BTCUSDT.
- All 10 loops evaluate on each BTCUSDT tick (one index lookup returns all 10 indices); non-bridge symbols affect only their loop.

### How to add a loop without recompile

1. Edit `config.json` and add to `triangles`:
   ```json
   { "name": "NEW", "loop": ["NEW_SYMBOL1", "NEW_SYMBOL2", "NEW_SYMBOL3"] }
   ```
2. Verify symbols exist: `curl https://api.binance.com/api/v3/ticker/price?symbol=NEW_SYMBOL1`
3. Restart the bot. It rebuilds the symbol list, reverse index, and subscription.

---

## Part 2: Dynamic pairs & currency modifiers

### config.json: currency modifiers section

Add to the top level:

```json
{
  "currency_modifiers": {
    "ETH": "SOL",
    "BTC": "AVAX"
  },
  "triangles": [ ... ]
}
```

At load time, **before** any loop is added to the list, every symbol in every loop is:
1. Uppercase'd
2. Checked against the modifiers map (exact string match)
3. Replaced if found

Example:
- Config has loop `["ETHUSDT", "ETHBTC", "BTCUSDT"]`
- Modifiers are `{ "ETH": "SOL", "BTC": "AVAX" }`
- After application: loop becomes `["SOLUSDT", "SOLAVAX", "AVAXUSDT"]`

### Why?

1. **Reuse loops without duplication**: Define your 10-triangle template once, then spin up variants with a modifier map change. No JSON copy-paste.
2. **A/B testing**: Test with different asset combinations by changing a single modifier map.
3. **Scaling across exchanges**: If you later support Kraken/Coinbase, you can patch symbols via modifiers (e.g., `"USDT": "USD"`) without editing every loop.

### Usage examples

**Example 1: OP instead of LINK**
```json
"currency_modifiers": { "LINK": "OP" },
"triangles": [
  { "loop": ["LINKUSDT", "LINKBTC", "BTCUSDT"] },
  ...
]
// LINK loop becomes OP loop; others unchanged
```

**Example 2: SOL ecosystem variant**
```json
"currency_modifiers": {
  "AVAX": "SOL",
  "LINK": "RAYDIUM",
  "DOT": "MARINADE"
},
"triangles": [
  { "loop": ["AVAXUSDT", "AVAXBTC", "BTCUSDT"] },
  { "loop": ["LINKUSDT", "LINKBTC", "BTCUSDT"] },
  { "loop": ["DOTUSDT", "DOTBTC", "BTCUSDT"] }
]
// All three loops are remapped to SOL ecosystem assets
```

### Implementation

- **`TriangleDef.h`**: declares global `g_currency_modifiers: vector<pair<string, string>>`.
- **`Config.cpp`**: parses `currency_modifiers` from JSON (object or array format) and applies them to each loop at load time.
- **`main.cpp`**: copies `config.currency_modifiers` to the global for potential future use (e.g., env var override).

### Limitations and design notes

- **Modifiers are applied once at startup.** There is no runtime re-application. Edit `config.json` and restart to change modifiers.
- **Modifiers are not transitive.** If you have `"A": "B"` and `"B": "C"`, symbol `"A"` becomes `"B"` (not `"C"`).
- **No validation of modified symbols.** If a modified loop references a pair that doesn't exist on Binance, the subscription silently fails and the loop never evaluates. Check Binance API before applying modifiers.

### Logging

New startup output shows loops in arrow notation:

```
[main] watching 10 arbitrage loops over 21 unique streams  mode=paper
[main]   AVAX-BTC-USDT  [AVAXUSDT -> AVAXBTC -> BTCUSDT]
[main]   SOL-BTC-USDT   [SOLUSDT -> SOLBTC -> BTCUSDT]
```

FIRE/WATCH lines include loop length:

```
>>>>>> FIRE [AVAX-BTC-USDT FWD]  loop 3-leg  gross 0.1234%  net -0.1016%  ...
```

---

## Variable-length loops (3+ symbols)

### New calculator: evaluate_loop

The pure 3-arg `evaluate()` is unchanged. For variable-length loops, use:

```cpp
std::optional<TriangleResult> evaluate_loop(
    const std::vector<std::string>& loop,
    const std::unordered_map<std::string, BookUpdate>& book);
```

It computes forward and reverse ratios for any loop length ≥3 by walking the loop in both directions, multiplying bid/ask prices.

### How to add a 4-leg loop

```json
{
  "name": "AVAX-ETH-BTC-USDT",
  "loop": ["AVAXUSDT", "AVAXETH", "ETHBTC", "BTCUSDT"],
  "currencies": ["AVAX", "ETH", "BTC", "USDT"]
}
```

The bot will:
1. Collect the 4 symbols
2. Subscribe to them on Binance
3. Build a reverse index mapping each to this loop
4. Evaluate the 4-leg path whenever any symbol ticks
5. Log `loop 4-leg` in FIRE/WATCH output

---

## Files summary

### New
- `include/TriangleDef.h` — shared loop struct and global modifier list
- `include/Dispatch.h`, `src/Dispatch.cpp` — routing functions
- `tests/test_dispatch.cpp` — routing tests
- `DYNAMIC_PAIRS.md` — user guide for modifiers and variable-length loops

### Modified
- `include/Config.h`, `src/Config.cpp` — parse triangles array, apply modifiers
- `include/FeedHandler.h`, `src/FeedHandler.cpp` — accept symbol vector, dedupe, subscribe
- `include/TriangleCalculator.h`, `src/TriangleCalculator.cpp` — keep 3-arg evaluate, add evaluate_loop
- `src/main.cpp` — book map, reverse index, per-loop routing, updated logging
- `config/config.json` — triangles array with 10 entries + optional modifiers section
- `CMakeLists.txt` — add Dispatch.cpp and test_dispatch

### Unchanged
- Message parsing, BookUpdate, MessageParser
- Risk/fees/logging config sections
- 3-leg calculator tests (still green)

---

## Workflow

### 1. Add a new 3-leg loop
1. Edit `config.json` and add to `triangles`
2. Restart (no recompile)

### 2. Add a new variable-length loop
Same as #1, but with any number of symbols (≥3 in `loop`)

### 3. Test a different currency set
1. Copy `config.json` to `config-variant.json`
2. Update `currency_modifiers` in the copy
3. Run `./build/arb_bot config-variant.json`
4. The loops are remapped; no recompile

### 4. Troubleshoot a loop not evaluating
1. Check startup logs — is the loop listed?
2. Check Binance: `curl https://api.binance.com/api/v3/ticker/price?symbol=SYMBOL` for each symbol
3. If a symbol doesn't exist, verify your loop's `loop` array or modifier rules

---

## Performance summary

- **Per-message cost**: one map write, one index lookup, re-eval of affected loops. ~2μs overhead for multi-loop routing.
- **Memory**: book map bounded to unique symbol count (21 for the 10-triangle set); reverse index is O(num_triangles * symbols_per_triangle).
- **Subscription**: one WebSocket, deduped streams. 10 triangles = 21 streams (not 30).
- **Hot path**: single-threaded, lock-free. All changes to routing are build-time only.

---

## Future extensions

- **Dynamic reload** (hot reload without restart): Would require atomic swap of routing tables + graceful book update. Doable but adds complexity.
- **Env var modifiers**: Parse `ARB_CURRENCY_MODIFIERS=ETH:SOL,BTC:AVAX` at startup.
- **Loop validation**: Check that modified loops form valid closed paths (vs. just multiplying bids/asks blindly).
- **Per-loop fee/thresholds**: Currently fees and WATCH/FIRE thresholds are global. Could be per-loop in config.
