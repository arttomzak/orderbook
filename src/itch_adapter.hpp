#pragma once

// Layer 1 - ITCH ingestion. Translates a raw TotalView-ITCH 5.0 stream into
// our Order events and drives the matching engine. Pure I/O + translation: no
// matching logic lives here. This header deliberately exposes NONE of itchcpp's
// types - the only place "itch/parser.hpp" is included is itch_adapter.cpp, so
// the dependency stays firewalled to that one translation unit.

#include "matching_engine.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <istream>
#include <string>
#include <unordered_map>
#include <vector>

// Capacity of the order pool / id-index: the maximum number of orders that can
// be RESTING (live) at once for the single symbol we replay - not the total
// number of orders in the day. TODO: tune against the real ITCH file's peak
// live-order count for our symbol; this is a placeholder.
inline constexpr std::size_t kPoolCapacity = 1u << 20;

// The one concrete engine instantiation the whole program uses. Single symbol,
// fixed pool -> exactly one Capacity, so the adapter's .cpp can instantiate
// MatchingEngine<kPoolCapacity> in its own TU and the engine stays header-only.
using Engine = MatchingEngine<kPoolCapacity>;

class ItchAdapter {
 public:
  // engine is constructed by the caller (it owns basePrice/numTicks sizing,
  // which depends on the symbol's price range for the day). The adapter just
  // borrows it. symbol is the target ticker, e.g. "AAPL".
  ItchAdapter(Engine& engine, std::string symbol);

  // Reads the entire ITCH stream and replays every message for our symbol into
  // the engine, in order. Blocking, single pass.
  void run(std::istream& in);

  // Per-message-type counts for our symbol, accumulated across run(). Pure
  // observability - lets the harness sanity-check that filtering and dispatch
  // actually fired (e.g. "AAPL: 1.2M adds, 900k deletes").
  struct Stats {
    std::uint64_t adds = 0;
    std::uint64_t deletes = 0;
    std::uint64_t cancels = 0;
    std::uint64_t executes = 0;
    std::uint64_t replaces = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  // Per-message handlers. These take only OUR primitive types - never itchcpp
  // types - so this header carries no dependency on the parser. The .cpp's
  // dispatch does the itch-struct -> primitive translation before calling in.

  // A / F (Add Order). Already symbol-filtered by the caller in the .cpp.
  void onAdd(OrderId id, Side side, Price price, Quantity qty);
  // D (Order Delete) - full removal of a resting order.
  void onDelete(OrderId id);
  // X (Order Cancel) - partial: reduce resting qty by cancelledShares.
  void onCancel(OrderId id, Quantity cancelledShares);
  // E / C (Order Executed [With Price]) - reduce resting qty by execShares.
  void onExecute(OrderId id, Quantity execShares);
  // U (Order Replace) - cancel oldId, rest a new order (newId) on the SAME side
  // as the original, at newPrice/newQty.
  void onReplace(OrderId oldId, OrderId newId, Price newPrice, Quantity newQty);

  Engine& engine_;
  std::string symbol_;

  // Membership + side recovery for messages that carry no stock symbol (D, X,
  // E, C, U all reference only an order id). An id is "ours" iff it's in here.
  // Stores Side because a U replace must resubmit on the original's side.
  //
  // We never erase on full execution (reduceQty can't report it, and a dead id
  // never gets a later message anyway - ITCH order ids are unique per day, so
  // no reuse can cause a false membership hit). Entries for fully-consumed
  // orders linger; the map grows toward the day's total order count. This is
  // off the timed hot path, so it's a memory question only, not correctness.
  std::unordered_map<OrderId, Side> sideByOrder_;

  Stats stats_;

  // Reused scratch for engine_.submit's fills output. Cleared (not reallocated)
  // per add. During clean replay this stays empty - a non-empty result means
  // our book has drifted from ITCH ground truth (see header comment #1).
  std::vector<Fill> fills_;
};
