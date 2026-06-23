#pragma once

#include "order_book.hpp"
#include "types.hpp"

#include <algorithm>
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
  }

  bool cancel(OrderId id) {
    return book_.removeOrder(id);
  }

  bool reduceQty(OrderId id, Quantity delta) {
    return book_.reduceQty(id, delta);
  }

  PriceLevel* bestBid() { return book_.bestBid(); }
  PriceLevel* bestAsk() { return book_.bestAsk(); }

 private:
  // Buy crosses if willing to pay at least the resting ask; sell crosses if
  // willing to accept at most the resting bid.
  static bool crosses(Side side, Price incomingPrice, Price restingPrice) {
    return side == Side::Buy ? incomingPrice >= restingPrice
                              : incomingPrice <= restingPrice;
  }

  OrderBook<Capacity> book_;
};
