#pragma once

#include "object_pool.hpp"
#include "types.hpp"

#include <cstdint>
#include <limits>
#include <vector>

// Same value as ObjectPool<T, Capacity>::kInvalidHandle for any T/Capacity
// (it's always just uint32_t's max, independent of the template
// parameters) - defined once here, free-standing, so PriceLevel (which
// isn't itself templated on Capacity) can use it as a default member
// initializer.
inline constexpr std::uint32_t kInvalidHandle =
    std::numeric_limits<std::uint32_t>::max();

// One resting order. Lives inside an ObjectPool<Order, Capacity> owned by
// OrderBook - this struct is both the data AND the intrusive list node
// (see prev/next).
struct Order {
  OrderId id;
  Price price;
  Quantity quantity;  // remaining, mutable via OrderBook::reduceQty
  Side side;

  // Intrusive doubly-linked list pointers, threading this order into its
  // PriceLevel's time-priority queue. Handles, not Order*, for the same
  // reasons ObjectPool's own Handle is an index: smaller, and consistent
  // with how everything else here addresses orders.
  std::uint32_t prev;
  std::uint32_t next;

  // Index into OrderBook's flat bidLevels_/askLevels_ array (whichever
  // matches `side`) for this order's price. Lets removeOrder/reduceQty
  // find the right PriceLevel in O(1), without re-deriving it from price.
  std::int32_t levelIndex;
};

// One price's worth of resting orders, oldest-first. A freshly-constructed
// level defaults to empty automatically: head/tail = kInvalidHandle means
// "no orders here," with no separate initialization pass needed.
struct PriceLevel {
  std::uint32_t head = kInvalidHandle;
  std::uint32_t tail = kInvalidHandle;
  Quantity totalVolume = 0;  // cached sum of resting quantity at this price
  std::uint32_t orderCount = 0;
};

// Fixed-capacity OrderId -> pool Handle lookup. Open-addressing (linear
// probing) hash table, no heap allocation - same philosophy as ObjectPool's
// own free-list, applied to id lookup instead of slot allocation.
//
// Why not std::unordered_map: every submit()/cancel() touches this exactly
// once, so it's on the hot path by definition - and std::unordered_map
// allocates a node per insert under every common implementation, which
// would mean a heap allocation on every single order event.
template <std::size_t IndexCapacity>
class IdIndex {
 public:
  // TODO: insert(OrderId, Handle) -> bool
  // TODO: find(OrderId) const -> Handle (kInvalidHandle on miss)
  // TODO: erase(OrderId) -> bool

 private:
  // TODO: slots_ (array of {OrderId key, Handle value, state}), count_
};

// Sole owner of mutation for the book: the only thing that touches the
// id-index, the object pool, and the intrusive list pointers. Storage and
// mutation only - no crossing/matching logic (that's MatchingEngine).
template <std::size_t Capacity>
class OrderBook {
 public:
  using Handle = typename ObjectPool<Order, Capacity>::Handle;
  static constexpr Handle kInvalidHandle =
      ObjectPool<Order, Capacity>::kInvalidHandle;

  OrderBook(Price basePrice, std::size_t numTicks);

  // TODO: restOrder(Order) -> Handle
  // TODO: bestBid() -> PriceLevel*
  // TODO: bestAsk() -> PriceLevel*
  // TODO: reduceQty(OrderId, Quantity) -> bool
  // TODO: removeOrder(OrderId) -> bool

 private:
  // TODO: priceToIndex(Price) const -> std::int64_t
  // TODO: removeByHandle(Handle) - shared by removeOrder and reduceQty's
  // zero-or-below path.

  ObjectPool<Order, Capacity> pool_;
  // TODO: idIndex_, basePrice_, numTicks_, bidLevels_, askLevels_,
  // bestBidIndex_, bestAskIndex_.
};
