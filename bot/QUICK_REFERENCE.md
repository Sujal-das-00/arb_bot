# Quick Reference: Dynamic Pairs

## Common tasks

### Add a new arbitrage loop
Edit `config/config.json`, add to `triangles` array:
```json
{
  "name": "YOUR-LOOP",
  "loop": ["PAIR1", "PAIR2", "PAIR3"],
  "currencies": ["ASSET1", "ASSET2", "ASSET3"]
}
```
Then **restart the bot** (no recompile).

### Change a currency across all loops
Edit `config/config.json`:
```json
"currency_modifiers": {
  "AVAX": "SOL"
}
```
Then **restart** (all AVAX loops become SOL loops).

### Test a 4+ leg loop
Add to `triangles`:
```json
{
  "name": "4-LEG",
  "loop": ["S1", "S2", "S3", "S4"]
}
```
Output will show `loop 4-leg`.

### Check if a symbol exists on Binance
```bash
curl -s https://api.binance.com/api/v3/ticker/price?symbol=SYMBOL
```

### See which loops are running
Check startup logs:
```
[main] watching N arbitrage loops over M unique streams
[main]   NAME1  [SYMBOL1 -> SYMBOL2 -> SYMBOL3]
[main]   NAME2  [SYMBOL4 -> ...]
```

### Debug: a loop never fires/watches
1. Check startup logs — is it listed?
2. Check Binance — does each symbol exist? (curl test above)
3. Check `currency_modifiers` — did they break the symbol names?
4. Check logs file (`logs/bot_*.log`) — any parse errors?

---

## config.json structure

```json
{
  "currency_modifiers": {
    "OLD_ASSET": "NEW_ASSET"
  },
  "triangles": [
    {
      "name": "Display-Name",
      "loop": ["SYMBOL1", "SYMBOL2", "SYMBOL3"],
      "currencies": ["ASSET1", "ASSET2", "ASSET3"]
    }
  ],
  "feed": { ... },
  "fees": { ... },
  "risk": { ... },
  "logging": { ... },
  "mode": "paper"
}
```

All sections except `triangles` are optional and have defaults.

---

## Output format

### Startup
```
[23:50:28] [main] watching 10 arbitrage loops over 21 unique streams  mode=paper
[23:50:28] [main]   AVAX-BTC-USDT  [AVAXUSDT -> AVAXBTC -> BTCUSDT]
```

### Detection
```
>>>>>> FIRE [AVAX-BTC-USDT FWD]  loop 3-leg  gross 0.1234%  net -0.1016%  ...
 · WATCH [SOL-BTC-USDT REV]  loop 3-leg  gross 0.0678%  net -0.1722%  ...
```

Everything also goes to `logs/bot_YYYYMMDD_HHMMSS.log`.

---

## Performance

| Metric | Value |
|--------|-------|
| Per-message overhead | ~2μs |
| Max compute (10 loops) | ~6.6μs |
| Memory | Fixed at startup |
| WebSocket connections | 1 (deduped) |
| Hot-path threads | 1 (lock-free) |

---

## Workflow: test multiple currency variants

1. **Base config** (`config/config.json`):
   ```json
   {
     "triangles": [
       { "loop": ["AVAXUSDT", "AVAXBTC", "BTCUSDT"] },
       { "loop": ["LINKUSDT", "LINKBTC", "BTCUSDT"] }
     ]
   }
   ```

2. **Variant 1** (`config/config-sol.json`):
   ```json
   {
     "currency_modifiers": { "AVAX": "SOL", "LINK": "RAY" },
     "triangles": [ ... same ... ]
   }
   ```

3. **Variant 2** (`config/config-ada.json`):
   ```json
   {
     "currency_modifiers": { "AVAX": "ADA", "LINK": "DJED" },
     "triangles": [ ... same ... ]
   }
   ```

4. **Run each**:
   ```bash
   ./build/arb_bot config/config.json          # AVAX/LINK variant
   ./build/arb_bot config/config-sol.json      # SOL/RAY variant
   ./build/arb_bot config/config-ada.json      # ADA/DJED variant
   ```

---

## Modifier syntax

### Object format (recommended for small sets)
```json
"currency_modifiers": {
  "KEY1": "VALUE1",
  "KEY2": "VALUE2"
}
```

### Array format (recommended for many modifiers)
```json
"currency_modifiers": [
  ["KEY1", "VALUE1"],
  ["KEY2", "VALUE2"]
]
```

Both are equivalent; use whichever reads better.

---

## Limits

- **Loop length**: minimum 3 symbols, no maximum (tested up to 4-leg)
- **Symbol deduping**: BTCUSDT in 10 loops = 1 subscription
- **Total unique symbols**: varies (21 for 10 triplets)
- **Recompile required?** Never (config-only)
- **Restart required?** Yes, to apply changes

---

## Examples

### Multi-stablecoin arbitrage
```json
"currency_modifiers": {
  "USDT": "USDC",
  "BTC": "ETH"
},
"triangles": [
  { "loop": ["AVAXUSDT", "AVAXBTC", "BTCUSDT"] }
]
// Becomes: AVAXUSDC, AVAXETH, ETHUSDC
```

### Multi-exchange prep
```json
"currency_modifiers": {
  "USDT": "USD",
  "BUSD": "USDC"
},
"triangles": [
  { "loop": ["AVAXUSDT", "AVAXBUSD", "BUSDUSDT"] }
]
// Ready for Kraken/Coinbase (where USDT = USD)
```

### Ecosystem analysis
```json
"currency_modifiers": {
  "BTC": "AVAX",
  "ETH": "MATIC"
},
"triangles": [
  { "loop": ["SOLBTC", "SOLETH", "ETHBTC"] }
]
// Becomes: SOLAVAX, SOLMATIC, MATICAVAX
```
