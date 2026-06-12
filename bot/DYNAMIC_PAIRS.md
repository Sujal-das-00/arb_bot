# Dynamic Pairs & Currency Modifiers

The bot now supports **variable-length arbitrage loops** (not just 3-symbol triplets) and **global currency modifiers** for easy pair management without recompile.

## Quick examples

### 1. Add a 4-leg arbitrage loop

Add to `triangles` array in `config.json`:

```json
{
  "name": "AVAX-ETH-BTC-USDT",
  "loop": ["AVAXUSDT", "AVAXETH", "ETHBTC", "BTCUSDT"],
  "currencies": ["AVAX", "ETH", "BTC", "USDT"]
}
```

Then restart the bot. It will:
- Collect the 4 symbols and subscribe to them
- Build the reverse index mapping each symbol to this loop
- Evaluate the 4-leg path every time any symbol ticks
- Log any WATCH/FIRE with `loop 4-leg` in the output

The loop must **form a closed trading path** (e.g., USDT → AVAX → ETH → BTC → USDT), though the calculator doesn't validate this — it just multiplies bid/ask ratios in order.

### 2. Use currency modifiers to spin up variants

Add to the top of `config.json`:

```json
"currency_modifiers": {
  "ETH": "SOL",
  "BTC": "AVAX"
},
```

This means: before loading any loop, replace every `ETH` with `SOL` and every `BTC` with `AVAX`. So a loop like:

```json
{ "loop": ["ETHUSDT", "ETHBTC", "BTCUSDT"] }
```

Becomes:

```json
{ "loop": ["SOLUSDT", "SOLAVAX", "AVAXUSDT"] }
```

**Why?** You can define a "template" set of loops and spin up variants by just changing the modifier map. No JSON duplication.

### 3. Complex modifier example

Watch the same 10 triangles, but for **OP instead of LINK**:

```json
"currency_modifiers": {
  "LINK": "OP"
}
```

The LINK triangle becomes an OP triangle; the other 9 are unchanged. Restart and the bot will:
- Collect symbols (now 22 instead of 21, since OPUSDT and OPBTC are new)
- Subscribe once to the new symbols
- Evaluate the OP loop alongside the other 9

### 4. Custom 5-leg loop

```json
{
  "name": "BNB-USDC-BUSD-FDUSD-TUSD",
  "loop": ["BNBUSDC", "BBBUSDC", "USDC", "BUSD", "FDUSD", "TUSD"],
  "currencies": ["BNB", "USDC", "BUSD", "FDUSD", "TUSD"]
}
```

(Note: `USDC` would need to be a valid Binance pair for this to work; adjust as needed.)

## How modifiers are applied

1. **Load config.json** → parse `currency_modifiers` object (optional)
2. **For each triangle:**
   - Read the `loop` array
   - For each symbol in the loop:
     - Uppercase it
     - Apply modifiers in order (first match wins)
     - Store the result
3. **Build symbol collection & reverse index** from the modified loops
4. **Subscribe to Binance** for all unique symbols
5. **Start evaluating** — on each message, route to affected loops and evaluate

Modifiers are applied **once at startup**; there is no runtime re-application. To change modifiers, edit `config.json` and restart.

## Logging output

With the new format, logs show loop structure and length:

```
[23:50:28] [main] watching 10 arbitrage loops over 21 unique streams  mode=paper
[23:50:28] [main]   AVAX-BTC-USDT  [AVAXUSDT -> AVAXBTC -> BTCUSDT]
[23:50:28] [main]   SOL-BTC-USDT   [SOLUSDT -> SOLBTC -> BTCUSDT]
...
```

And FIRE/WATCH lines now include loop length:

```
>>>>>> FIRE [AVAX-BTC-USDT FWD]  loop 3-leg  gross 0.1234%  net -0.1016%  ...
 · WATCH [SOL-BTC-USDT REV]  loop 3-leg  gross 0.0678%  net -0.1722%  ...
```

If you add a 4-leg loop:
```
>>>>>> FIRE [AVAX-ETH-BTC-USDT FWD]  loop 4-leg  gross 0.0456%  net -0.2294%  ...
```

## Workflow: Add a new arbitrage loop without recompile

1. **Edit `config.json`** and add an entry to `triangles`:
   ```json
   {
     "name": "NEW-PAIR-BTC-USDT",
     "loop": ["NEWPAIRUSDT", "NEWPAIRBTC", "BTCUSDT"],
     "currencies": ["NEWPAIR", "BTC", "USDT"]
   }
   ```

2. **Verify pairs exist on Binance:**
   ```bash
   curl -s https://api.binance.com/api/v3/ticker/price?symbol=NEWPAIRUSDT
   curl -s https://api.binance.com/api/v3/ticker/price?symbol=NEWPAIRBTC
   ```

3. **Restart the bot.** On startup it will:
   - Load the new config
   - Collect the two new symbols + BTCUSDT (already subscribed)
   - Add one or two new streams to the WebSocket subscription
   - Build the reverse index
   - Start evaluating the new loop

   No recompile, no rebuilding the binary. Just restart.

## Workflow: Test 10 triangles variants with different base currencies

Say you have a `config-base.json` with your template 10 triangles all using AVAX, ETH, etc. To test with SOL, ADA, etc. instead:

1. Copy the config:
   ```bash
   cp config/config.json config/config-sol-ada.json
   ```

2. Modify `currency_modifiers` in the copy:
   ```json
   "currency_modifiers": {
     "AVAX": "SOL",
     "SOL": "ADA",
     ...
   },
   ```

3. Run the bot with the alternate config:
   ```bash
   ./build/arb_bot config/config-sol-ada.json
   ```

The loops are now remapped; you get the same structure with different currencies.

## Technical notes

- **Per-symbol cost:** Modifiers are applied once at load time, so there is no per-message overhead.
- **Modifier order:** Modifiers are stored in an `std::vector<pair>`, so the order depends on JSON object iteration (in most modern implementations, insertion order). If you need guaranteed order, use an array instead: `"currency_modifiers": [["OLD1", "NEW1"], ["OLD2", "NEW2"]]`.
- **Multiple modifiers:** Modifiers are applied sequentially. E.g. if you have `"A": "B"` and `"B": "C"`, a symbol `"A"` becomes `"B"` (not `"C"`). There is no transitive closure.
- **Invalid pairs:** If a modified loop references a pair that doesn't exist on Binance, the subscription will silently fail for that stream, and the loop will never produce evaluations (all its symbols will appear to never tick). Check `curl api.binance.com` before adding.

## FAQ

**Q: Can I reload config without restarting?**
A: Not yet. The routing tables (`symbol_to_tri` reverse index and `book` map) are built once and never updated. Dynamic reload would require a more complex architecture (atomic swap of routing tables, etc.). Currently, restart the bot to apply config changes.

**Q: Can I remove a loop at runtime?**
A: No — see above. Stop the bot, edit `config.json` to remove the loop's entry, and restart.

**Q: What if I have a typo in a symbol (e.g., `AVAXUDT` instead of `AVAXUSDT`)?**
A: The bot will load successfully but the misspelled loop will never tick (Binance doesn't have an `AVAXUDT` stream). Check the startup logs:
```
[main]   AVAX-BTC-USDT  [AVAXUDT -> AVAXBTC -> BTCUSDT]   # <-- typo!
```
The loop is listed, but if it never fires/watches, suspect a typo. Verify each symbol with `curl api.binance.com/api/v3/ticker/price?symbol=SYMBOL`.

**Q: Can I use environment variables to set currency modifiers?**
A: Not yet. They are currently JSON-only. If you need env var support, edit `Config.cpp` to parse `ARB_CURRENCY_MODIFIERS` (e.g., as comma-separated pairs like `ETH:SOL,BTC:AVAX`).
