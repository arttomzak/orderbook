#pragma once

#include <cstdint>

// Price in integer ticks. Signed so crossing-logic subtraction/comparison
// (e.g. bid - ask) never has to worry about unsigned underflow.
using Price = std::int64_t;

// Share quantity. Matches ITCH 5.0's 4-byte Shares field width exactly, so
// Layer 1 can translate the wire value straight through with no
// narrowing/widening risk.
using Quantity = std::uint32_t;

// Order Reference Number. Matches ITCH 5.0's 8-byte field exactly, so it can
// be passed through verbatim from ingestion to the id-index and on to the
// correctness harness, which diffs against ITCH ground truth keyed on the
// same numbers.
using OrderId = std::uint64_t;

// Buy/Sell, not Bid/Ask: matches ITCH's wire-level Buy/Sell Indicator field
// 1:1, so no B->Bid / S->Ask mapping is needed (and can't be gotten wrong)
// anywhere translation happens.
enum class Side : std::uint8_t { Buy, Sell };

// One execution event between exactly two orders: the order that was
// already resting in the book, and the incoming order that crossed it.
struct Fill {
  OrderId restingOrderId;
  OrderId aggressorOrderId;
  Price price;
  Quantity quantity;
  Side aggressorSide;
};
