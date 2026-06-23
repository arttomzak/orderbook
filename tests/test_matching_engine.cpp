#include "matching_engine.hpp"

#include <gtest/gtest.h>

namespace {

// prev/next/levelIndex are OrderBook-owned bookkeeping that restOrder()
// overwrites unconditionally - placeholder values here are never read.
Order makeOrder(OrderId id, Price price, Quantity qty, Side side) {
  return Order{id, price, qty, side, kInvalidHandle, kInvalidHandle, -1};
}

class MatchingEngineTest : public ::testing::Test {};

}  // namespace

// ---- no cross ----

TEST_F(MatchingEngineTest, RestsWhenOppositeSideEmpty) {
  MatchingEngine<32> engine(100, 100);
  std::vector<Fill> fills;

  engine.submit(makeOrder(1, 105, 10, Side::Buy), fills);

  EXPECT_TRUE(fills.empty());
  ASSERT_NE(engine.bestBid(), nullptr);
  EXPECT_EQ(engine.bestBid()->totalVolume, 10u);
  EXPECT_EQ(engine.bestAsk(), nullptr);
}

TEST_F(MatchingEngineTest, RestsWhenPricedAwayFromBestOpposite) {
  MatchingEngine<32> engine(100, 100);
  std::vector<Fill> fills;

  engine.submit(makeOrder(1, 110, 5, Side::Sell), fills);   // resting ask
  engine.submit(makeOrder(2, 105, 10, Side::Buy), fills);   // doesn't cross 110

  EXPECT_TRUE(fills.empty());
  ASSERT_NE(engine.bestAsk(), nullptr);
  EXPECT_EQ(engine.bestAsk()->totalVolume, 5u);   // resting ask untouched
  ASSERT_NE(engine.bestBid(), nullptr);
  EXPECT_EQ(engine.bestBid()->totalVolume, 10u);  // new bid rests separately
}

// ---- exact-quantity fills, also exercising crosses()'s equality boundary ----

TEST_F(MatchingEngineTest, ExactQuantityFillBuyCrossesAtEqualPrice) {
  MatchingEngine<32> engine(100, 100);
  std::vector<Fill> fills;

  engine.submit(makeOrder(1, 110, 10, Side::Sell), fills);  // resting ask
  engine.submit(makeOrder(2, 110, 10, Side::Buy), fills);   // incomingPrice == restingPrice

  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].quantity, 10u);
  EXPECT_EQ(engine.bestAsk(), nullptr);  // resting order fully consumed
  EXPECT_EQ(engine.bestBid(), nullptr);  // incoming fully filled, nothing rests
}

TEST_F(MatchingEngineTest, ExactQuantityFillSellCrossesAtEqualPrice) {
  MatchingEngine<32> engine(100, 100);
  std::vector<Fill> fills;

  engine.submit(makeOrder(1, 110, 10, Side::Buy), fills);   // resting bid
  engine.submit(makeOrder(2, 110, 10, Side::Sell), fills);  // incomingPrice == restingPrice

  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].quantity, 10u);
  EXPECT_EQ(engine.bestBid(), nullptr);
  EXPECT_EQ(engine.bestAsk(), nullptr);
}

// ---- partial fills ----

TEST_F(MatchingEngineTest, IncomingSmallerThanRestingPartiallyConsumesRestingOrder) {
  MatchingEngine<32> engine(100, 100);
  std::vector<Fill> fills;

  engine.submit(makeOrder(1, 110, 20, Side::Sell), fills);  // resting ask, 20
  engine.submit(makeOrder(2, 110, 8, Side::Buy), fills);    // smaller incoming

  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].quantity, 8u);

  ASSERT_NE(engine.bestAsk(), nullptr);
  EXPECT_EQ(engine.bestAsk()->totalVolume, 12u);  // 20 - 8, still resting
  EXPECT_EQ(engine.bestBid(), nullptr);           // incoming fully filled
}

TEST_F(MatchingEngineTest, IncomingLargerThanRestingRestsRemainder) {
  MatchingEngine<32> engine(100, 100);
  std::vector<Fill> fills;

  engine.submit(makeOrder(1, 110, 10, Side::Sell), fills);  // resting ask, 10
  engine.submit(makeOrder(2, 110, 25, Side::Buy), fills);   // larger incoming

  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].quantity, 10u);

  EXPECT_EQ(engine.bestAsk(), nullptr);  // resting order fully consumed

  ASSERT_NE(engine.bestBid(), nullptr);  // 15 remaining rests as a new bid
  EXPECT_EQ(engine.bestBid()->totalVolume, 15u);
  EXPECT_EQ(engine.bestBid()->orderCount, 1u);
}

// ---- fill field correctness ----

TEST_F(MatchingEngineTest, FillFieldsRecordRestingPriceAndCorrectIdsAndSide) {
  MatchingEngine<32> engine(100, 100);
  std::vector<Fill> fills;

  engine.submit(makeOrder(1, 108, 10, Side::Sell), fills);  // resting ask @ 108
  engine.submit(makeOrder(2, 112, 10, Side::Buy), fills);   // willing to pay up to 112

  ASSERT_EQ(fills.size(), 1u);
  const Fill& fill = fills[0];
  EXPECT_EQ(fill.restingOrderId, 1u);
  EXPECT_EQ(fill.aggressorOrderId, 2u);
  EXPECT_EQ(fill.price, 108);  // executes at the resting price, not 112
  EXPECT_EQ(fill.quantity, 10u);
  EXPECT_EQ(fill.aggressorSide, Side::Buy);
}

// ---- multi-order / multi-level sweeps ----

TEST_F(MatchingEngineTest, FifoFillsConsumeRestingOrdersInTimeOrder) {
  MatchingEngine<32> engine(100, 100);
  std::vector<Fill> fills;

  engine.submit(makeOrder(1, 110, 5, Side::Sell), fills);  // oldest
  engine.submit(makeOrder(2, 110, 5, Side::Sell), fills);
  engine.submit(makeOrder(3, 110, 5, Side::Sell), fills);  // newest

  engine.submit(makeOrder(4, 110, 12, Side::Buy), fills);  // sweeps 1, 2, partial 3

  ASSERT_EQ(fills.size(), 3u);
  EXPECT_EQ(fills[0].restingOrderId, 1u);
  EXPECT_EQ(fills[0].quantity, 5u);
  EXPECT_EQ(fills[1].restingOrderId, 2u);
  EXPECT_EQ(fills[1].quantity, 5u);
  EXPECT_EQ(fills[2].restingOrderId, 3u);
  EXPECT_EQ(fills[2].quantity, 2u);

  ASSERT_NE(engine.bestAsk(), nullptr);
  EXPECT_EQ(engine.bestAsk()->totalVolume, 3u);   // order 3's leftover
  EXPECT_EQ(engine.bestAsk()->orderCount, 1u);
  EXPECT_EQ(engine.bestBid(), nullptr);           // incoming fully filled
}

TEST_F(MatchingEngineTest, SweepConsumesMultipleLevelsInPriceOrder) {
  MatchingEngine<32> engine(100, 100);
  std::vector<Fill> fills;

  engine.submit(makeOrder(1, 110, 5, Side::Sell), fills);   // best ask
  engine.submit(makeOrder(2, 112, 10, Side::Sell), fills);  // next-best ask

  engine.submit(makeOrder(3, 115, 12, Side::Buy), fills);   // crosses both levels

  ASSERT_EQ(fills.size(), 2u);
  EXPECT_EQ(fills[0].restingOrderId, 1u);
  EXPECT_EQ(fills[0].price, 110);
  EXPECT_EQ(fills[0].quantity, 5u);
  EXPECT_EQ(fills[1].restingOrderId, 2u);
  EXPECT_EQ(fills[1].price, 112);
  EXPECT_EQ(fills[1].quantity, 7u);  // 12 - 5

  ASSERT_NE(engine.bestAsk(), nullptr);
  EXPECT_EQ(engine.bestAsk()->totalVolume, 3u);  // order 2's leftover (10 - 7)
  EXPECT_EQ(engine.bestBid(), nullptr);          // incoming fully filled
}

// Sweeping must stop the moment a level no longer satisfies crosses(), not
// just when the incoming order runs dry - and the remainder must rest at
// the incoming order's own original limit price, not at the price of the
// level that stopped it (order.price is never mutated during matching).
TEST_F(MatchingEngineTest, PartialSweepStopsAtNonCrossingLevelAndRestsRemainderAtOwnPrice) {
  MatchingEngine<32> engine(100, 100);
  std::vector<Fill> fills;

  engine.submit(makeOrder(1, 150, 10, Side::Sell), fills);   // fully swept
  engine.submit(makeOrder(2, 152, 100, Side::Sell), fills);  // never reached: 151 < 152

  engine.submit(makeOrder(3, 151, 30, Side::Buy), fills);

  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].restingOrderId, 1u);
  EXPECT_EQ(fills[0].price, 150);
  EXPECT_EQ(fills[0].quantity, 10u);

  ASSERT_NE(engine.bestAsk(), nullptr);
  EXPECT_EQ(engine.bestAsk()->totalVolume, 100u);  // order 2 completely untouched

  ASSERT_NE(engine.bestBid(), nullptr);
  EXPECT_EQ(engine.bestBid()->totalVolume, 20u);   // 30 - 10, resting at order 3's own price
  EXPECT_EQ(engine.bestBid()->orderCount, 1u);
}

// ---- pass-throughs ----

TEST_F(MatchingEngineTest, CancelRemovesRestingOrder) {
  MatchingEngine<32> engine(100, 100);
  std::vector<Fill> fills;
  engine.submit(makeOrder(1, 105, 10, Side::Buy), fills);

  ASSERT_TRUE(engine.cancel(1));

  EXPECT_EQ(engine.bestBid(), nullptr);
}

TEST_F(MatchingEngineTest, ReduceQtyReducesRestingOrder) {
  MatchingEngine<32> engine(100, 100);
  std::vector<Fill> fills;
  engine.submit(makeOrder(1, 105, 10, Side::Buy), fills);

  ASSERT_TRUE(engine.reduceQty(1, 4));

  ASSERT_NE(engine.bestBid(), nullptr);
  EXPECT_EQ(engine.bestBid()->totalVolume, 6u);
}
