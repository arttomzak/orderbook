#include "itch_adapter.hpp"

// The ONE place itchcpp is included - keeps the parser dependency walled off to
// this translation unit (see itch_adapter.hpp header comment).
#include "itch/parser.hpp"

#include <cassert>
#include <type_traits>
#include <utility>
#include <variant>

ItchAdapter::ItchAdapter(Engine& engine, std::string symbol)
    : engine_(engine), symbol_(std::move(symbol)) {
  // Pre-size the fills scratch so onAdd never allocates on the submit path.
  fills_.reserve(64);
}

void ItchAdapter::run(std::istream& in) {
  itch::Parser parser;

  parser.parse(in, [this](const itch::Message& msg) {
    std::visit(
        [this](const auto& m) {
          using T = std::decay_t<decltype(m)>;

          if constexpr (std::is_same_v<T, itch::AddOrderMessage> ||
                        std::is_same_v<T, itch::AddOrderMPIDAttributionMessage>) {
            // Only A/F carry the stock symbol, so the symbol filter lives here.
            if (itch::to_string(m.stock, itch::STOCK_LEN) != symbol_) {
              return;
            }
            const Side side =
                (m.buy_sell_indicator == 'B') ? Side::Buy : Side::Sell;
            onAdd(m.order_reference_number, side,
                  static_cast<Price>(m.price), m.shares);

          } else if constexpr (std::is_same_v<T, itch::OrderDeleteMessage>) {
            onDelete(m.order_reference_number);

          } else if constexpr (std::is_same_v<T, itch::OrderCancelMessage>) {
            onCancel(m.order_reference_number, m.cancelled_shares);

          } else if constexpr (std::is_same_v<T, itch::OrderExecutedMessage>) {
            onExecute(m.order_reference_number, m.executed_shares);

          } else if constexpr (std::is_same_v<
                                   T, itch::OrderExecutedWithPriceMessage>) {
            // C is E with an explicit print price; for book reconstruction the
            // qty reduction is identical - we only care about executed_shares.
            onExecute(m.order_reference_number, m.executed_shares);

          } else if constexpr (std::is_same_v<T, itch::OrderReplaceMessage>) {
            onReplace(m.original_order_reference_number,
                      m.new_order_reference_number,
                      static_cast<Price>(m.price), m.shares);
          }
          // All other message types (system event, stock directory, trades,
          // NOII, etc.) carry no book mutation we replay - ignored.
        },
        msg);
  });
}

void ItchAdapter::onAdd(OrderId id, Side side, Price price, Quantity qty) {
  fills_.clear();
  engine_.submit(Order{.id = id, .price = price, .quantity = qty, .side = side},
                 fills_);
  // Tier-1 invariant: a clean A never crosses the existing book, so submit must
  // emit no fills (see header comment #1). Debug-only - synthetic matching tests
  // legitimately cross, so this lives here in the replay path, not in submit().
  assert(fills_.empty());
  sideByOrder_.emplace(id, side);
  ++stats_.adds;
}

void ItchAdapter::onDelete(OrderId id) {
  auto it = sideByOrder_.find(id);
  if (it == sideByOrder_.end()) {
    return;  // not our symbol
  }
  engine_.cancel(id);
  sideByOrder_.erase(it);
  ++stats_.deletes;
}

void ItchAdapter::onCancel(OrderId id, Quantity cancelledShares) {
  auto it = sideByOrder_.find(id);
  if (it == sideByOrder_.end()) {
    return;  // not our symbol
  }
  engine_.reduceQty(id, cancelledShares);
  // Entry intentionally left in sideByOrder_ even on full consumption - see the
  // map's declaration comment for why that's safe.
  ++stats_.cancels;
}

void ItchAdapter::onExecute(OrderId id, Quantity execShares) {
  auto it = sideByOrder_.find(id);
  if (it == sideByOrder_.end()) {
    return;  // not our symbol
  }
  engine_.reduceQty(id, execShares);
  // Entry intentionally left in sideByOrder_ even on full consumption - see the
  // map's declaration comment for why that's safe.
  ++stats_.executes;
}

void ItchAdapter::onReplace(OrderId oldId, OrderId newId, Price newPrice,
                            Quantity newQty) {
  auto it = sideByOrder_.find(oldId);
  if (it == sideByOrder_.end()) {
    return;  // original wasn't ours -> replacement isn't either
  }
  const Side side = it->second;  // U carries no side; recover the original's
  engine_.cancel(oldId);
  sideByOrder_.erase(it);
  ++stats_.replaces;
  onAdd(newId, side, newPrice, newQty);  // also bumps stats_.adds (it rests a
                                         // genuinely new order)
}
