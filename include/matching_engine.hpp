#pragma once

#include "order_book.hpp"
#include "types.hpp"

#include <algorithm>
#include <cassert>
#include <vector>

// Orchestrates crossing: takes an incoming order, matches it against the
// opposite side of the book via OrderBook's public API, emits Fills, and
// rests any remainder. Never reaches into PriceLevel/pool/list internals
// directly - OrderBook stays the sole mutator of those structures.
template <std::size_t Capacity>
class MatchingEngine {
 public:
  using Handle = typename OrderBook<Capacity>::Handle;

  MatchingEngine(Price basePrice, std::size_t numTicks)
      : book_(basePrice, numTicks) {}

  void submit(Order order, std::vector<Fill>& fills) {
    PriceLevel* best = (order.side == Side::Buy) ? book_.bestAsk() : book_.bestBid();
    while (order.quantity > 0 && best != nullptr ) {
     Price best_price = book_.peek(best->head).price;

     if (!crosses(order.side, order.price, best_price)) {
         break;
     }
     Handle h = best->head;
     while (order.quantity > 0 && h != kInvalidHandle) {
       const Order& resting = book_.peek(h);
        Quantity matchQty = std::min(order.quantity, resting.quantity);
        fills.push_back(Fill{resting.id, order.id, resting.price, matchQty, order.side});

        order.quantity -= matchQty;

        const OrderId restingId = resting.id;
        const Handle next = resting.next;

        book_.reduceQty(restingId, matchQty); // note that this mutates resting, meaning that we got everything we needed above

        h = next;
     }

      best = (order.side == Side::Buy) ? book_.bestAsk() : book_.bestBid();
    }

    if (order.quantity > 0) {
      book_.restOrder(order);
    }

    // Tier-1 invariant: a completed submit must never leave the book crossed.
    // Folded into the assert expression so the whole check is elided under
    // NDEBUG (zero Release cost). Compares inside prices only when both sides
    // are populated.
    assert(!(book_.bestBid() && book_.bestAsk() &&
             book_.bestBidPrice() >= book_.bestAskPrice()));
  }

  bool cancel(OrderId id) {
    return book_.removeOrder(id);
  }

  bool reduceQty(OrderId id, Quantity delta) {
    return book_.reduceQty(id, delta);
  }

  PriceLevel* bestBid() { return book_.bestBid(); }
  PriceLevel* bestAsk() { return book_.bestAsk(); }

  // Inside price on each side. Undefined if empty - gate on bestBid()/bestAsk().
  Price bestBidPrice() { return book_.bestBidPrice(); }
  Price bestAskPrice() { return book_.bestAskPrice(); }

  // Tier-1 structural audit of the whole book - see OrderBook::auditInvariants.
  bool audit() const { return book_.auditInvariants(); }

  // Capacity diagnostics / drop gate - see OrderBook for the contract.
  std::size_t peakLiveOrders() const { return book_.peakLiveOrders(); }
  std::uint64_t droppedOrders() const {
    return book_.droppedPoolFull() + book_.droppedPriceRange();
  }
  std::uint64_t droppedPoolFull() const { return book_.droppedPoolFull(); }
  std::uint64_t droppedPriceRange() const { return book_.droppedPriceRange(); }

  // Is this price inside the book's flat-array window? Adapter filters on this.
  bool inPriceRange(Price price) const { return book_.inPriceRange(price); }

 private:
  // Buy crosses if willing to pay at least the resting ask; sell crosses if
  // willing to accept at most the resting bid.
  static bool crosses(Side side, Price incomingPrice, Price restingPrice) {
    return side == Side::Buy ? incomingPrice >= restingPrice
                              : incomingPrice <= restingPrice;
  }

  OrderBook<Capacity> book_;
};
