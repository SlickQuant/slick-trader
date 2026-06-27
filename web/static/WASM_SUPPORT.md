# WASM Support for SlickLadder

## Current Status

The web dashboard currently uses the **JavaScript version** of the SlickLadder price ladder component from the `@slickquant/slick-ladder` npm package (v0.1.2).

## Why Not WASM?

The package provides two separate implementations:
1. **`PriceLadder`** - Pure JavaScript implementation (exported in UMD bundle)
2. **`WasmPriceLadder`** - WASM-powered version using .NET 8 runtime

However, we're loading the UMD bundle (`slick-ladder.js`) which **only exports the JavaScript version**. The `WasmPriceLadder` class is available separately as an ES module export (`slick-ladder/wasm-adapter`).

## How to Enable WASM (Future Enhancement)

To use the WASM version, you would need to:

### Option 1: Switch to ES Modules (Recommended)

1. Convert frontend to ES modules
2. Use import statements:
   ```javascript
   import { WasmPriceLadder } from '@slickquant/slick-ladder/wasm-adapter';
   ```
3. Create WASM ladder instance:
   ```javascript
   const ladder = new WasmPriceLadder({
       container: ladderContainer,
       // ... config
   });
   await ladder.waitForReady(); // Wait for WASM initialization
   ```
4. Copy WASM files to `/wasm/` directory:
   ```bash
   cp -r node_modules/@slickquant/slick-ladder/dist/wasm/* web/static/wasm/
   ```

### Option 2: Build Custom Bundle with Webpack

Follow the pattern from `slick-ladder/examples/web`:
- Set up webpack configuration
- Copy WASM files to public/wasm directory
- Bundle with ts-loader and CopyWebpackPlugin

## Performance Comparison

From package documentation:
- **JavaScript**: ~5-10ms processing time for 1000 updates/sec
- **WASM**: <1ms processing time for 1000 updates/sec (10x faster)

For typical market data rates (<100 updates/sec), the JavaScript version performs well and WASM optimization is not critical.

## Current Architecture

```
Coinbase WebSocket
    ↓
md_queue_ (SPSC lock-free queue)
    ├→ StrategyManager thread (cursor: md_cursor_)
    └→ WebServer thread (cursor: ws_md_cursor_)
         ↓
     WebSocket (uWS)
         ↓
     Browser
         ↓
     SlickLadder.PriceLadder (JavaScript)
```

## References

- Package: https://www.npmjs.com/package/@slickquant/slick-ladder
- Example: `node_modules/@slickquant/slick-ladder/examples/web/src/demo.ts`
- WASM Worker: `node_modules/@slickquant/slick-ladder/src/web/src/wasm-worker-module.ts`
