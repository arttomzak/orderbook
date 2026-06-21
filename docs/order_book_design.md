# `order_book.hpp` design notes

Step 3 of the build order in `CLAUDE.md`. Storage and mutation only — no
crossing/matching logic (that's `MatchingEngine`, step 4). Builds directly
on `types.hpp` and `object_pool.hpp` (already complete, tested).

## What this file is responsible for

`OrderBook` is the live filing system for one symbol's resting orders: every
order that's arrived and hasn't yet traded or been canceled. It answers
"what's resting at this price, in what arrival order, and what's the best
price on each side right now" — it does not decide *when* a trade happens;
that's `MatchingEngine`'s job, which will call into `OrderBook`'s methods
(mainly `reduceQty`) as part of its own logic.

## `Order`

```cpp
struct Order {
  OrderId id;
  Price price;
  Quantity quantity;   // remaining, mutable via reduceQty
  Side side;
  std::uint32_t prev;  // intrusive list link (a pool Handle)
  std::uint32_t next;
  std::int32_t levelIndex;  // which PriceLevel this order lives in
};
```

- Lives inside `OrderBook`'s `ObjectPool<Order, Capacity>` — allocated via
  `pool_.allocate(...)` in `restOrder`, freed via `pool_.deallocate(h)` in
  `removeOrder`/`reduceQty`'s auto-remove path.
- **`prev`/`next`** thread this order into its `PriceLevel`'s time-priority
  queue. This is an **intrusive** linked list: the link fields live directly
  inside `Order` itself rather than in a separate wrapper "node" object (the
  way `std::list<T>` would do internally). That avoids a second allocation
  per list node — the list rides along with the same pool slot the order
  already occupies. Links are `Handle`s (pool indices), not raw `Order*`,
  for the same reasons `ObjectPool::Handle` is an index: smaller (4 bytes
  vs. 8), and consistent with how everything else here addresses orders.
- **`levelIndex`** is what keeps `removeOrder`/`reduceQty` O(1) — given a
  `Handle` (found via the id-index), this field says exactly which
  `PriceLevel` to mutate, without re-deriving it from `price`.
- `prev`/`next`/`levelIndex` are filled in by `OrderBook` immediately after
  allocation, before the order is reachable from any list or index — only
  `OrderBook` ever constructs a fully-wired `Order`.

### Why `Order` isn't in `types.hpp`

It needs intrusive-list and pool-handle bookkeeping that only `OrderBook`
should construct or touch — keeping it here, not in the shared value-types
header, makes that boundary a file-organization fact, not just a
convention.

## `PriceLevel`

```cpp
struct PriceLevel {
  std::uint32_t head = kInvalidHandle;
  std::uint32_t tail = kInvalidHandle;
  Quantity totalVolume = 0;
  std::uint32_t orderCount = 0;
};
```

- One exact price's worth of resting orders, oldest-first. **A price level
  is one single exact price, never a range** — `OrderBook`'s flat array
  maps each index to exactly one price via `index = price - basePrice`.
- `head`/`tail` are `Handle`s into the same intrusive chain as `Order`'s own
  `prev`/`next` — `PriceLevel` doesn't store the list itself, just where it
  starts and ends.
- `totalVolume` is a cached running sum, updated incrementally on every
  insert/reduce/remove — **never** recomputed by walking the list. Required
  because the correctness harness (step 6) diffs level volumes against ITCH
  ground truth after every execution event; that has to be an O(1) read.
- **No separate "is this level occupied" flag.** `head == kInvalidHandle`
  (equivalently `orderCount == 0`) is the single source of truth, and it's
  free: a default-constructed `PriceLevel` is already in the empty state,
  so a freshly-built array of thousands of levels starts "all empty" with
  no initialization pass needed.
- Deliberately does **not** store its own price or side — both are implied
  by the level's position in the flat array and which array (bid/ask) it
  belongs to. `OrderBook` always reaches a `PriceLevel*` already knowing
  both, so storing them redundantly on the level itself buys nothing.

## The flat price-tick array

Two separate `std::vector<PriceLevel>` members, one per side (`bidLevels_`,
`askLevels_`), each covering the same shared window `[basePrice,
basePrice + numTicks)`.

- **Two arrays, not one split array** — bid and ask never share a level
  pre-crossing, and keeping them separate makes side-independence (an
  explicit test requirement) structurally true rather than just behaviorally
  true.
- **Sized at runtime (constructor arguments), not as template parameters.**
  Unlike the pool's `Capacity` (compile-time, baked into the binary),
  `basePrice`/`numTicks` are symbol-specific market knowledge only known
  once a symbol is chosen — forcing a recompile per symbol/day would be the
  wrong trade. The `std::vector` is sized once at construction and never
  grows afterward, so its runtime behavior is identical to a fixed array —
  this is a one-time setup cost, not a hot-path allocation.
- **Indexing**: `index = price - basePrice`. Valid iff
  `0 <= index < numTicks`.
- **Price precision**: ticks match ITCH 5.0's own wire format exactly —
  prices are scaled by 10,000 (four decimal places), not just cents. A
  price of $150.02 is the integer tick value `1500200`. This means an ITCH
  price field can be copied into `Order::price` with zero conversion and
  zero rounding, which matters because the correctness harness compares
  directly against ITCH ground truth, and CLAUDE.md requires no rounding
  error, ever. (In practice almost every real resting order will land on an
  exact penny boundary, since Reg NMS restricts sub-penny quoting for
  normally-priced stocks — but the representation itself doesn't assume
  that.)
- **Out-of-range price is rejected, not asserted/crashed.** `restOrder`
  returns `kInvalidHandle` immediately, before touching the pool or
  id-index, if the price falls outside the configured window. A multi-hour
  ITCH replay hitting one unexpected price shouldn't take down the whole
  correctness run.

## The id-index — `OrderId -> Handle`

`OrderId` (Nasdaq's externally-assigned Order Reference Number) and
`ObjectPool::Handle` (a pool slot index) are **not unifiable** — order ids
arrive as arbitrary 64-bit values, not sequential pool-friendly numbers. So
`OrderBook` needs its own lookup from one to the other.

**Decision: a custom fixed-capacity open-addressing hash table
(`IdIndex<N>`), not `std::unordered_map`.**

Why not `std::unordered_map`: every single `submit()`/`cancel()` touches
this exactly once, so it's on the hot path by definition (not an occasional
cold-path cost) — and `std::unordered_map` allocates a heap node per insert
under every common implementation, which would mean a heap allocation on
every single order event. That directly violates the "no allocation on the
hot path" rule, on every order, not as an edge case.

What's built instead, mirroring the philosophy already used for
`ObjectPool`'s own free-list (one big array, reserved once, never grows):

```cpp
enum class SlotState : std::uint8_t { Empty, Occupied, Tombstone };

struct Slot {
  OrderId key;
  Handle value;
  SlotState state = SlotState::Empty;
};

std::array<Slot, IndexCapacity> slots_;
std::size_t count_;
```

- **Collision strategy: linear probing.** Hash the `OrderId` to a starting
  slot; if it's occupied by a different key, check the next slot, and the
  next, until an empty slot or a matching key is found. Contiguous probing
  (not chasing pointers to separate heap nodes) keeps cache behavior good.
- **Three-state slots, not just used/unused.** `Tombstone` matters
  specifically for `erase`: if a slot is simply marked "empty" on erase,
  a *later* lookup for some other key that originally probed past this now-
  "empty" slot would incorrectly stop early and report "not found," even
  though it's actually further along. Tombstones let probing continue
  through erased slots while still allowing future inserts to reclaim them.
- **Hash function**: ITCH order reference numbers are assigned by Nasdaq in
  a non-adversarial, roughly-increasing fashion — a simple fast integer
  mix is sufficient, no cryptographic hash needed.
- **Capacity**: sized with headroom relative to the pool's `Capacity` (e.g.
  ~2x, kept a power of two for cheap masking instead of modulo), so the
  id-index can never fill up before the pool itself reports full — pool
  exhaustion is always the binding constraint, no separate "index full"
  check needed on the hot path.
- **Encapsulation**: only ever touched via `insert`/`find`/`erase`, only
  from inside `OrderBook`'s own methods — never exposed publicly. That
  narrow seam means swapping the backing structure later (if benchmarking
  reveals it's not actually the bottleneck, or reveals it is) is a local
  change inside this header, not a change to `OrderBook`'s public API or
  to any test.

## `OrderBook`

```cpp
template <std::size_t Capacity>
class OrderBook {
 public:
  using Handle = typename ObjectPool<Order, Capacity>::Handle;
  static constexpr Handle kInvalidHandle = ObjectPool<Order, Capacity>::kInvalidHandle;

  OrderBook(Price basePrice, std::size_t numTicks);

  Handle restOrder(Order order);
  PriceLevel* bestBid();
  PriceLevel* bestAsk();
  bool reduceQty(OrderId id, Quantity reduceBy);
  bool removeOrder(OrderId id);

 private:
  std::int64_t priceToIndex(Price p) const;
  void removeByHandle(Handle h);  // shared by removeOrder and reduceQty's zero-or-below path

  ObjectPool<Order, Capacity> pool_;
  IdIndex<...> idIndex_;
  Price basePrice_;
  std::size_t numTicks_;
  std::vector<PriceLevel> bidLevels_;
  std::vector<PriceLevel> askLevels_;
  std::int64_t bestBidIndex_;  // -1 means "side is empty"
  std::int64_t bestAskIndex_;
};
```

### `restOrder(Order order)` sequence

1. Validate price via `priceToIndex` — out of range returns `kInvalidHandle`
   immediately, nothing else touched.
2. `pool_.allocate(...)` — pool full returns `kInvalidHandle`, nothing else
   touched.
3. Fill in `levelIndex` on the newly-allocated order.
4. Splice onto the **tail** of the level's list (new orders are
   lowest time-priority at that price — FIFO).
5. Update the level's `totalVolume`/`orderCount`.
6. Insert into the id-index.
7. Update the cached best-bid/ask if this order beats the current best —
   O(1), since a single new order can only ever improve or tie the best,
   never require a search.
8. Return the handle.

### `bestBid()`/`bestAsk()`

O(1) reads of the cached index (`nullptr` if that side is empty — the
idiomatic, zero-cost "no value" for a pointer-returning API, the same
spirit as `kInvalidHandle` for `ObjectPool`). All the work happens at
*removal* time, not on every read:

When a removal empties the level that happened to be the cached best, the
code scans from that now-empty index **toward the worse-price direction**
(down for bids, up for asks) until it finds the next occupied level, or
confirms the whole side is now empty (cache reset to `-1`). This is
O(numTicks) worst case but cheap in practice — real order flow essentially
never empties many consecutive ticks simultaneously, and the scan only
triggers when the *best* level specifically is the one being emptied (most
removals are of non-best orders and never touch the cache).

### `reduceQty(OrderId id, Quantity reduceBy)`

Looks up the id-index; unknown id returns `false`. If the reduction would
take quantity to zero or below, the order is **automatically and fully
removed** (delegates to the same internal logic `removeOrder` uses) rather
than left resting with zero/negative quantity. This mirrors ITCH's own
semantics (a fully-executed order is gone, not "resting with 0 shares") and
keeps that invariant enforced in exactly one place — neither the future
ITCH adapter nor `MatchingEngine` has to separately remember to check
"did this hit zero, do I also need to call removeOrder."

### `removeOrder(OrderId id)`

Looks up the id-index; unknown id returns `false`. Otherwise: unlink from
the intrusive list in O(1) using the order's own stored `prev`/`next`
(reaching directly into its two neighbors, no list walk); update the
level's volume/count; if this emptied the cached best level, run the
re-scan described above; erase from the id-index; deallocate from the pool
— in that order, since reading any of the order's fields after
`pool_.deallocate()` would be a dangling reference.

## Test plan summary (`tests/test_order_book.cpp`)

~25 cases across: single-order best-bid/ask correctness; best tracking
across multiple price levels as they fill and empty (including skipping
over several empty levels at once); FIFO ordering within one price level;
aggregate volume correctness; `reduceQty`'s zero-and-below auto-remove
behavior (including over-reduction past zero); full `removeOrder` cleanup
(id-index, pool slot, level volume, best-cache); price-out-of-range
rejection at both window boundaries; pool-exhaustion behavior; and bid/ask
side independence. No crossing/matching tests — that's step 4.

## File-by-file

- `include/order_book.hpp` (new)
- `tests/test_order_book.cpp` (new)
- `CMakeLists.txt` (add the new test file to `unit_tests`)

No changes to `types.hpp` or `object_pool.hpp`.
