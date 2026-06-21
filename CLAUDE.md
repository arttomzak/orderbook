# Low-Latency Orderbook & Matching Engine

## What this is

A single-symbol central limit order book (CLOB) and matching engine in C++,
benchmarked against a full day of real Nasdaq order flow from TotalView-ITCH 5.0.

Resume target (fill in X when measured):

> Single-symbol CLOB in C++ — price-time priority matching engine with
> object-pooled order store, O(1) cancel via intrusive linked lists, and flat
> price-tick array; benchmarked against a full day of raw Nasdaq
> TotalView-ITCH 5.0 order flow achieving X million orders/sec at
> sub-microsecond p99.

Every design decision should be in service of that sentence: correctness
first, then a real, honestly-measured latency/throughput number.

## Architecture — three layers, strict separation

**Layer 1 — Data Ingestion** (`src/itch_adapter.cpp`)
Wraps the `itchcpp` parser. Reads the raw ITCH binary, deserializes messages,
filters to the target symbol, translates ITCH structs (A, F, D, U, X, E) into
our `Order` type, and calls into the engine. Pure I/O and translation — no
matching logic, no timing. Genuinely a separate translation unit; not
inlining-sensitive the way Layer 2 is.

**Layer 2 — Matching Engine + Order Book** (`include/`, the core, header-only)
Zero I/O, zero file/network access. Only sees `Order` structs in and `Fill`
structs out.
- `OrderBook` is the **sole owner of mutation** — the only thing that
  touches the id-index, the object pool, and the intrusive list pointers.
  Exposes a narrow, concrete, non-virtual API: `restOrder(Order)`,
  `bestBid()` / `bestAsk()` (return raw `PriceLevel*`, not copies),
  `reduceQty(OrderId, Quantity)`, `removeOrder(OrderId)`. Flat price-tick
  array of `PriceLevel`s, intrusive doubly-linked lists per level for time
  priority, fixed-capacity object pool, flat id-index.
- `MatchingEngine` orchestrates: takes an incoming order, crosses it against
  the opposite side via `OrderBook`'s API using price-time priority, emits
  `Fill`s, rests any remainder. It never reaches into `PriceLevel`/pool/list
  internals directly — always goes through `OrderBook`'s methods, so
  id-index/pool/list invariants can only be broken in one place.

**Layer 3 — Harness** (`tests/`, `bench/`, `src/main.cpp`)
Two modes over the same engine:
- *Correctness*: replay ITCH stream, after each execution event (E/C) diff
  our book's best bid/ask and level volumes against ITCH ground truth.
- *Benchmark*: replay as fast as possible, time every `submit()`/`cancel()`
  with `clock_gettime(CLOCK_MONOTONIC)` into a pre-allocated vector, compute
  p50/p99/p99.9 and throughput at the end.

Data flow: `.gz ITCH file → itchcpp → symbol filter → Order translation →
MatchingEngine::submit()/cancel() → OrderBook → Fill → harness records
latency → report`.

## Directory layout (flattened at repo root)

```
.
├── include/
│   ├── types.hpp           # Price, Quantity, OrderId, Side, Fill
│   ├── object_pool.hpp     # Fixed-capacity pool, O(1) alloc/free
│   ├── order_book.hpp      # PriceLevel, OrderBook (storage + mutation, sole mutator)
│   └── matching_engine.hpp # MatchingEngine (crossing logic, orchestrates OrderBook)
├── src/
│   ├── itch_adapter.cpp    # itchcpp callback -> Order translation, symbol filter
│   └── main.cpp            # CLI entry point: correctness or benchmark mode
├── bench/
│   └── harness.cpp         # Timing loop, percentile computation, results output
├── tests/
│   └── correctness.cpp     # Replays ITCH, diffs book state against ground truth
├── third_party/
│   └── itchcpp/            # Git submodule (added in Layer 1 phase, not blocking scaffold)
└── CMakeLists.txt
```

## Tech stack & conventions

- **C++20**, CMake build, GoogleTest via `FetchContent` for unit tests.
- **Single-threaded** matching engine, no locks on the hot path. Required for
  the sub-microsecond p99 claim to be honest — don't introduce concurrency
  into Layer 2.
- **Price = `int64_t` integer ticks**, never floating point. Index price
  levels directly off the tick value; no rounding error, ever.
- **No allocation on the hot path.** Orders come from a fixed-capacity object
  pool. If a code path can call `new`/`malloc` during `submit()` or
  `cancel()`, that's a bug.
- **Header-only Layer 2** (`types.hpp`, `object_pool.hpp`, `order_book.hpp`,
  `matching_engine.hpp`). Not because of templates/genericity — there isn't
  any here — but to force the compiler to see `MatchingEngine` and
  `OrderBook` in full at every call site so it inlines across what would
  otherwise be a `.cpp` boundary, without depending on LTO doing the right
  thing. Mechanically: define methods **inside the class body** (implicitly
  `inline`, no ODR risk) rather than out-of-class-in-header — we'll have at
  least three TUs (`main.cpp`, `harness.cpp`, `correctness.cpp`) including
  these headers, so this isn't optional. Keep the four-file split for
  readability; header-only is orthogonal to file decomposition. Header
  dependency graph is a DAG: `types.hpp` → `object_pool.hpp` →
  `order_book.hpp` → `matching_engine.hpp`. Every header gets `#pragma once`.
- **OrderBook/MatchingEngine seam stays thin but not loose**: OrderBook is
  the only mutator (id-index, pool, intrusive lists all change in one place
  only); MatchingEngine never touches those structures directly. No virtual
  dispatch anywhere on the hot path — concrete classes, non-virtual methods,
  header-only means the compiler inlines straight through the seam anyway.
- Layer boundaries are load-bearing: Layer 2 must compile and be fully unit
  tested with zero dependency on `itchcpp` or any file I/O.

## Build order (current plan)

1. **Repo scaffold** — directory structure, CMakeLists skeleton, GoogleTest
   wired via FetchContent. No real logic yet. *(We are here.)*
2. **`types.hpp` + `object_pool.hpp`** — core types and the allocator, unit
   tested standalone.
3. **`order_book.hpp`** — `PriceLevel` + `OrderBook` storage/mutation API,
   intrusive lists, id-index. Unit tested with hand-written synthetic orders
   (insert, cancel, best bid/ask correctness) — no crossing logic yet.
4. **`matching_engine.hpp`** — crossing logic, price-time priority, `Fill`
   emission, built entirely on `OrderBook`'s public API. Unit tested against
   hand-crafted order sequences with known expected fills.
5. **Layer 1 wiring** — add `itchcpp` as a submodule, write
   `itch_adapter.cpp`, get a real ITCH sample file, translate messages into
   `Order`s.
6. **Correctness harness** (`tests/correctness.cpp`) — replay full day,
   diff against ITCH ground truth.
7. **Benchmark harness** (`bench/harness.cpp`) — timing loop, percentile
   report. Also: sanity-check the header-only inlining assumption with
   `objdump`/`perf` on a Release build before trusting the numbers. This
   step produces the resume number.

## Non-goals (explicitly out of scope)

- Multi-symbol support — single symbol only (e.g. AAPL).
- Multi-threaded matching, lock-free queues, or networked order entry.
- Persistence/durability, recovery, or any kind of WAL.
- Anything beyond price-time priority (no pro-rata, no hidden/iceberg orders,
  no auctions) unless ITCH ground-truth comparison forces it.

## Commands

(To be filled in once CMakeLists.txt exists.)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Unit tests: `ctest --test-dir build`
- Benchmark: `./build/bench/harness <path-to-itch-file>`
