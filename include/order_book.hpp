#pragma once

#include "object_pool.hpp"
#include "types.hpp"

#include <cassert>
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

// Smallest power of two >= n. Used to size IdIndex, which needs a
// power-of-two capacity for its bitmask probing - independent of
// OrderBook's own Capacity, which has no such constraint.
constexpr std::size_t nextPow2(std::size_t n) {
  std::size_t p = 1;
  while (p < n) {
    p <<= 1;
  }
  return p;
}

// Sole owner of mutation for the book: the only thing that touches the
// id-index, the object pool, and the intrusive list pointers. Storage and
// mutation only - no crossing/matching logic (that's MatchingEngine).
template <std::size_t Capacity>
class OrderBook {
 public:
  using Handle = typename ObjectPool<Order, Capacity>::Handle;
  static constexpr Handle kInvalidHandle =
      ObjectPool<Order, Capacity>::kInvalidHandle;

  // bidLevels_/askLevels_ are sized to numTicks_ up front and never resized
  // again - every PriceLevel in range default-constructs to empty
  // (head/tail = kInvalidHandle), so there's no separate init pass needed.
  OrderBook(Price basePrice, std::size_t numTicks)
      : basePrice_(basePrice),
        numTicks_(numTicks),
        bidLevels_(numTicks),
        askLevels_(numTicks) {}

  // Bounds-checks order.price, allocates a pool slot, links it into the
  // tail of its PriceLevel's intrusive list (time priority - later orders
  // append after earlier ones), indexes it by id, and updates the
  // best-price cache if needed. Returns kInvalidHandle if the price falls
  // outside [basePrice_, basePrice_ + numTicks_) or the pool is full - both
  // expected, caller-checked outcomes, same pattern as ObjectPool::allocate.
  Handle restOrder(Order order) {
    const std::int64_t index = priceToIndex(order.price);
    if (index < 0 || static_cast<std::size_t>(index) >= numTicks_) {
      return kInvalidHandle;
    }

    // prev/next/levelIndex are OrderBook-owned bookkeeping, not caller
    // concerns - whatever the caller put there is overwritten here.
    order.prev = kInvalidHandle;
    order.next = kInvalidHandle;
    order.levelIndex = static_cast<std::int32_t>(index);

    const Handle h = pool_.allocate(order);
    if (h == kInvalidHandle) {
      return kInvalidHandle;
    }

    PriceLevel& level = (order.side == Side::Buy ? bidLevels_ : askLevels_)
                            [static_cast<std::size_t>(index)];

    if (level.tail == kInvalidHandle) {
      level.head = h;
    } else {
      pool_.get(level.tail).next = h;
      pool_.get(h).prev = level.tail;
    }
    level.tail = h;
    level.totalVolume += order.quantity;
    ++level.orderCount;

    [[maybe_unused]] const bool inserted = idIndex_.insert(order.id, h);
    assert(inserted);  // duplicate order id would mean bad upstream data

    if (order.side == Side::Buy) {
      if (bestBidIndex_ == -1 || index > bestBidIndex_) {
        bestBidIndex_ = index;
      }
    } else {
      if (bestAskIndex_ == -1 || index < bestAskIndex_) {
        bestAskIndex_ = index;
      }
    }

    return h;
  }

  // Raw pointer into bidLevels_/askLevels_, not a copy - MatchingEngine
  // reads price/totalVolume/head straight off this without OrderBook having
  // to hand back a snapshot. nullptr means that side is empty, mirroring
  // the -1 sentinel on bestBidIndex_/bestAskIndex_.
  PriceLevel* bestBid() {
    return bestBidIndex_ == -1
               ? nullptr
               : &bidLevels_[static_cast<std::size_t>(bestBidIndex_)];
  }

  PriceLevel* bestAsk() {
    return bestAskIndex_ == -1
               ? nullptr
               : &askLevels_[static_cast<std::size_t>(bestAskIndex_)];
  }
  // delta is shares to subtract from the resting quantity (matches ITCH
  // Order Executed/Cancel messages, which give a share count, not a new
  // absolute quantity). delta == current quantity fully consumes the order
  // and removes it. Returns false if id isn't resting - not found is an
  // expected, caller-checked outcome (e.g. a duplicate/late ITCH message),
  // not a programmer error.
  bool reduceQty(OrderId id, Quantity delta) {
    const Handle h = idIndex_.find(id);
    if (h == kInvalidHandle) {
      return false;
    }

    Order& o = pool_.get(h);
    // delta > quantity would mean a fill/cancel for more than is actually
    // resting - that's a bug upstream (matching engine miscomputed a fill
    // size, or our book has drifted from ITCH ground truth), not a valid
    // input to silently clamp.
    assert(delta <= o.quantity);
    if (delta == o.quantity) {
      removeByHandle(h);
      return true;
    }

    o.quantity -= delta;
    PriceLevel& level = (o.side == Side::Buy ? bidLevels_ : askLevels_)
                            [static_cast<std::size_t>(o.levelIndex)];
    level.totalVolume -= delta;
    return true;
  }

  // Same not-found contract as reduceQty: false if id isn't resting.
  bool removeOrder(OrderId id) {
    const Handle h = idIndex_.find(id);
    if (h == kInvalidHandle) {
      return false;
    }
    removeByHandle(h);
    return true;
  }

 private:
  // Converts an absolute tick price into a zero-based index into
  // bidLevels_/askLevels_. No bounds checking here - callers (restOrder
  // etc.) are responsible for validating the result against numTicks_.
  std::int64_t priceToIndex(Price price) const {
    return price - basePrice_;
  }

  // Unlinks h from its PriceLevel's intrusive list, updates level
  // bookkeeping, advances the best-price cache if the level that just
  // emptied was the best on its side, then erases the id and frees the
  // pool slot. Shared by removeOrder and reduceQty's fully-consumed path.
  void removeByHandle(Handle h) {
    Order& o = pool_.get(h);
    const OrderId id = o.id;
    const Side side = o.side;
    const auto levelIndex = static_cast<std::size_t>(o.levelIndex);

    std::vector<PriceLevel>& levels = (side == Side::Buy) ? bidLevels_ : askLevels_;
    PriceLevel& level = levels[levelIndex];

    if (o.prev != kInvalidHandle) {
      pool_.get(o.prev).next = o.next;
    } else {
      level.head = o.next;
    }
    if (o.next != kInvalidHandle) {
      pool_.get(o.next).prev = o.prev;
    } else {
      level.tail = o.prev;
    }

    level.totalVolume -= o.quantity;
    --level.orderCount;

    // If the level that just emptied was the cached best price on its
    // side, scan toward worse prices for the next occupied level - linear
    // for now, only triggers when the best level itself empties.
    if (level.head == kInvalidHandle) {
      if (side == Side::Buy && bestBidIndex_ == static_cast<std::int64_t>(levelIndex)) {
        std::int64_t idx = bestBidIndex_ - 1;
        while (idx >= 0 &&
               bidLevels_[static_cast<std::size_t>(idx)].head == kInvalidHandle) {
          --idx;
        }
        bestBidIndex_ = idx;  // -1 if none found, matching the empty-side sentinel
      } else if (side == Side::Sell &&
                 bestAskIndex_ == static_cast<std::int64_t>(levelIndex)) {
        const auto numTicksIdx = static_cast<std::int64_t>(numTicks_);
        std::int64_t idx = bestAskIndex_ + 1;
        while (idx < numTicksIdx &&
               askLevels_[static_cast<std::size_t>(idx)].head == kInvalidHandle) {
          ++idx;
        }
        bestAskIndex_ = (idx < numTicksIdx) ? idx : -1;
      }
    }

    idIndex_.erase(id);
    pool_.deallocate(h);
  }

  ObjectPool<Order, Capacity> pool_;

  // 2x Capacity keeps IdIndex's load factor at <= 50%, so linear-probing
  // chains stay short even when the pool is nearly full.
  IdIndex<nextPow2(Capacity * 2)> idIndex_;

  Price basePrice_;
  std::size_t numTicks_;

  std::vector<PriceLevel> bidLevels_;
  std::vector<PriceLevel> askLevels_;

  // Index into bidLevels_/askLevels_ of the best (highest bid / lowest ask)
  // currently-occupied price level. -1 means "no resting orders on this
  // side" - checked explicitly by bestBid()/bestAsk() before dereferencing.
  std::int64_t bestBidIndex_ = -1;
  std::int64_t bestAskIndex_ = -1;
};
