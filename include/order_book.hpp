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
  static_assert((IndexCapacity & (IndexCapacity - 1)) == 0,
                "IndexCapacity must be a power of two");

 public:
  bool insert(OrderId id, std::uint32_t handle) {
    std::size_t idx = slotFor(id);
    for (std::size_t probes = 0; probes < IndexCapacity; ++probes) {
      Slot& slot = slots_[idx];
      if (slot.state != SlotState::Occupied) {
        slot.key = id;
        slot.value = handle;
        slot.state = SlotState::Occupied;
        ++count_;
        return true;
      }
      if (slot.key == id) {
        return false;  // already present
      }
      idx = (idx + 1) & (IndexCapacity - 1); // bitwise trick to get next valid index (modulo is expensive)
    }
    return false;  // full - shouldn't happen given IndexCapacity's sizing
  }

  std::uint32_t find(OrderId id) const {
    std::size_t idx = slotFor(id);
    for (std::size_t probes = 0; probes < IndexCapacity; ++probes) {
      const Slot& slot = slots_[idx];
      if (slot.state == SlotState::Empty) {
        return kInvalidHandle;
      }
      if (slot.state == SlotState::Occupied && slot.key == id) {
        return slot.value;
      }
      idx = (idx + 1) & (IndexCapacity - 1);
    }
    return kInvalidHandle;
  }

  bool erase(OrderId id) {
    std::size_t idx = slotFor(id);
    for (std::size_t probes = 0; probes < IndexCapacity; ++probes) {
      Slot& slot = slots_[idx];
      if (slot.state == SlotState::Empty) {
        return false;
      }
      if (slot.state == SlotState::Occupied && slot.key == id) {
        slot.state = SlotState::Tombstone;
        --count_;
        return true;
      }
      idx = (idx + 1) & (IndexCapacity - 1);
    }
    return false;
  }

 private:
  // Empty: never used, probing stops here. Occupied: live entry.
  // Tombstone: erased - probing must continue past these.
  enum class SlotState : std::uint8_t { Empty, Occupied, Tombstone };

  struct Slot {
    OrderId key;
    std::uint32_t value;
    SlotState state = SlotState::Empty;
  };

  // splitmix64-style finalizer - ITCH order ids aren't adversarial, just
  // need a fast, well-distributed mix.
  static std::size_t slotFor(OrderId id) {
    std::uint64_t x = id;

    // our hashing function that is a lil bit faster than something security focused
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return static_cast<std::size_t>(x) & (IndexCapacity - 1);
  }

  std::array<Slot, IndexCapacity> slots_;
  std::size_t count_ = 0;
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
