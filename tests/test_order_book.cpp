#include "order_book.hpp"

#include <gtest/gtest.h>

namespace {

// prev/next/levelIndex are OrderBook-owned bookkeeping that restOrder()
// overwrites unconditionally - placeholder values here are never read.
Order makeOrder(OrderId id, Price price, Quantity qty, Side side) {
  return Order{id, price, qty, side, kInvalidHandle, kInvalidHandle, -1};
}

class OrderBookTest : public ::testing::Test {};

}  // namespace

// ---- restOrder ----

TEST_F(OrderBookTest, RestOrderReturnsValidHandleAndUpdatesLevel) {
  OrderBook<16> book(100, 20);
  auto h = book.restOrder(makeOrder(1, 105, 10, Side::Buy));
  ASSERT_NE(h, (decltype(book)::kInvalidHandle));

  PriceLevel* level = book.bestBid();
  ASSERT_NE(level, nullptr);
  EXPECT_EQ(level->head, h);
  EXPECT_EQ(level->tail, h);
  EXPECT_EQ(level->totalVolume, 10u);
  EXPECT_EQ(level->orderCount, 1u);
}

TEST_F(OrderBookTest, RestOrderOutOfRangePriceBelowReturnsInvalidHandle) {
  OrderBook<16> book(100, 20);
  auto h = book.restOrder(makeOrder(1, 99, 10, Side::Buy));
  EXPECT_EQ(h, (decltype(book)::kInvalidHandle));
}

TEST_F(OrderBookTest, RestOrderOutOfRangePriceAboveReturnsInvalidHandle) {
  OrderBook<16> book(100, 20);
  auto h = book.restOrder(makeOrder(1, 120, 10, Side::Buy));  // [100, 120) -> 120 is out
  EXPECT_EQ(h, (decltype(book)::kInvalidHandle));
}

TEST_F(OrderBookTest, RestOrderUpdatesBestBidOnBetterPrice) {
  OrderBook<16> book(100, 20);
  book.restOrder(makeOrder(1, 103, 10, Side::Buy));
  EXPECT_EQ(book.bestBid()->head, 0u);  // first handle allocated

  auto hBetter = book.restOrder(makeOrder(2, 107, 10, Side::Buy));  // higher bid wins
  EXPECT_EQ(book.bestBid()->head, hBetter);
}

TEST_F(OrderBookTest, RestOrderUpdatesBestAskOnBetterPrice) {
  OrderBook<16> book(100, 20);
  book.restOrder(makeOrder(1, 110, 10, Side::Sell));
  auto hBetter = book.restOrder(makeOrder(2, 106, 10, Side::Sell));  // lower ask wins
  EXPECT_EQ(book.bestAsk()->head, hBetter);
}

TEST_F(OrderBookTest, RestOrderWorsePriceDoesNotChangeBest) {
  OrderBook<16> book(100, 20);
  auto hBest = book.restOrder(makeOrder(1, 110, 10, Side::Buy));
  book.restOrder(makeOrder(2, 105, 10, Side::Buy));  // worse bid

  EXPECT_EQ(book.bestBid()->head, hBest);
}

TEST_F(OrderBookTest, RestOrderWhenPoolFullReturnsInvalidHandle) {
  OrderBook<2> book(100, 20);
  book.restOrder(makeOrder(1, 101, 10, Side::Buy));
  book.restOrder(makeOrder(2, 102, 10, Side::Buy));

  auto h = book.restOrder(makeOrder(3, 103, 10, Side::Buy));
  EXPECT_EQ(h, (decltype(book)::kInvalidHandle));
}

TEST_F(OrderBookTest, BestBidAndAskNullWhenEmpty) {
  OrderBook<16> book(100, 20);
  EXPECT_EQ(book.bestBid(), nullptr);
  EXPECT_EQ(book.bestAsk(), nullptr);
}

TEST_F(OrderBookTest, TimePriorityFifoOrderAtSamePrice) {
  OrderBook<16> book(100, 20);
  auto hA = book.restOrder(makeOrder(1, 105, 10, Side::Buy));
  book.restOrder(makeOrder(2, 105, 10, Side::Buy));
  auto hC = book.restOrder(makeOrder(3, 105, 10, Side::Buy));

  PriceLevel* level = book.bestBid();
  EXPECT_EQ(level->head, hA);  // oldest first
  EXPECT_EQ(level->tail, hC);  // newest last
  EXPECT_EQ(level->orderCount, 3u);
}

// ---- reduceQty ----

TEST_F(OrderBookTest, ReduceQtyPartialReducesVolumeKeepsOrderResting) {
  OrderBook<16> book(100, 20);
  auto h = book.restOrder(makeOrder(1, 105, 10, Side::Buy));

  ASSERT_TRUE(book.reduceQty(1, 4));

  PriceLevel* level = book.bestBid();
  EXPECT_EQ(level->head, h);  // order still resting
  EXPECT_EQ(level->totalVolume, 6u);
  EXPECT_EQ(level->orderCount, 1u);
}

TEST_F(OrderBookTest, ReduceQtyFullRemovesOrder) {
  OrderBook<16> book(100, 20);
  book.restOrder(makeOrder(1, 105, 10, Side::Buy));

  ASSERT_TRUE(book.reduceQty(1, 10));  // delta == full resting quantity

  EXPECT_EQ(book.bestBid(), nullptr);
}

TEST_F(OrderBookTest, ReduceQtyUnknownIdReturnsFalse) {
  OrderBook<16> book(100, 20);
  EXPECT_FALSE(book.reduceQty(999, 5));
}

// ---- removeOrder ----

TEST_F(OrderBookTest, RemoveOrderUnknownIdReturnsFalse) {
  OrderBook<16> book(100, 20);
  EXPECT_FALSE(book.removeOrder(999));
}

TEST_F(OrderBookTest, RemoveHeadOrderUpdatesLevelHead) {
  OrderBook<16> book(100, 20);
  book.restOrder(makeOrder(1, 105, 10, Side::Buy));
  auto hB = book.restOrder(makeOrder(2, 105, 10, Side::Buy));
  auto hC = book.restOrder(makeOrder(3, 105, 10, Side::Buy));

  ASSERT_TRUE(book.removeOrder(1));  // remove the head

  PriceLevel* level = book.bestBid();
  EXPECT_EQ(level->head, hB);
  EXPECT_EQ(level->tail, hC);  // tail untouched
  EXPECT_EQ(level->orderCount, 2u);
}

TEST_F(OrderBookTest, RemoveTailOrderUpdatesLevelTail) {
  OrderBook<16> book(100, 20);
  auto hA = book.restOrder(makeOrder(1, 105, 10, Side::Buy));
  auto hB = book.restOrder(makeOrder(2, 105, 10, Side::Buy));
  book.restOrder(makeOrder(3, 105, 10, Side::Buy));

  ASSERT_TRUE(book.removeOrder(3));  // remove the tail

  PriceLevel* level = book.bestBid();
  EXPECT_EQ(level->head, hA);  // head untouched
  EXPECT_EQ(level->tail, hB);
  EXPECT_EQ(level->orderCount, 2u);
}

// Removing B alone doesn't prove A.next/C.prev got relinked - head/tail
// don't move when a middle element goes. The proof is removing A next: if
// A.next wasn't repointed past B when B was unlinked, the level's head
// would dangle on B's now-freed handle instead of advancing to C.
TEST_F(OrderBookTest, RemovingMiddleOrderRelinksAroundIt) {
  OrderBook<16> book(100, 20);
  auto hA = book.restOrder(makeOrder(1, 105, 10, Side::Buy));
  book.restOrder(makeOrder(2, 105, 10, Side::Buy));
  auto hC = book.restOrder(makeOrder(3, 105, 10, Side::Buy));

  ASSERT_TRUE(book.removeOrder(2));  // remove the middle

  PriceLevel* level = book.bestBid();
  EXPECT_EQ(level->head, hA);  // unchanged - not yet proof of relinking
  EXPECT_EQ(level->tail, hC);  // unchanged either
  EXPECT_EQ(level->orderCount, 2u);

  ASSERT_TRUE(book.removeOrder(1));  // remove A - now head depends on A.next

  EXPECT_EQ(book.bestBid()->head, hC);  // only true if the relink worked
}

TEST_F(OrderBookTest, RemovingOnlyOrderEmptiesLevelAndClearsBestBid) {
  OrderBook<16> book(100, 20);
  book.restOrder(makeOrder(1, 105, 10, Side::Buy));

  ASSERT_TRUE(book.removeOrder(1));

  EXPECT_EQ(book.bestBid(), nullptr);
}

// The best-price scan must walk past every empty tick in between, not just
// one - a one-level-at-a-time bug would stop early and report the wrong
// best price (or never find the next occupied level at all).
TEST_F(OrderBookTest, RemovingBestBidAdvancesPastMultipleEmptyLevels) {
  OrderBook<16> book(100, 20);
  book.restOrder(makeOrder(1, 105, 10, Side::Buy));            // best bid
  auto hWorse = book.restOrder(makeOrder(2, 100, 10, Side::Buy));  // gap: ticks 101-104 empty

  ASSERT_TRUE(book.removeOrder(1));  // empty out the best level

  PriceLevel* level = book.bestBid();
  ASSERT_NE(level, nullptr);
  EXPECT_EQ(level->head, hWorse);
  EXPECT_EQ(level->totalVolume, 10u);
}

TEST_F(OrderBookTest, RemovingBestAskAdvancesPastMultipleEmptyLevels) {
  OrderBook<16> book(100, 20);
  book.restOrder(makeOrder(1, 105, 10, Side::Sell));            // best ask
  auto hWorse = book.restOrder(makeOrder(2, 110, 10, Side::Sell));  // gap: ticks 106-109 empty

  ASSERT_TRUE(book.removeOrder(1));  // empty out the best level

  PriceLevel* level = book.bestAsk();
  ASSERT_NE(level, nullptr);
  EXPECT_EQ(level->head, hWorse);
  EXPECT_EQ(level->totalVolume, 10u);
}

// Emptying a level that isn't the cached best shouldn't trigger a rescan at
// all - the best-price index should be left exactly as it was.
TEST_F(OrderBookTest, RemovingNonBestLevelDoesNotChangeBest) {
  OrderBook<16> book(100, 20);
  auto hBest = book.restOrder(makeOrder(1, 110, 10, Side::Buy));  // best bid
  book.restOrder(makeOrder(2, 105, 10, Side::Buy));               // worse, will be emptied

  ASSERT_TRUE(book.removeOrder(2));

  EXPECT_EQ(book.bestBid()->head, hBest);
}

#ifndef NDEBUG
TEST_F(OrderBookTest, DuplicateOrderIdTriggersAssertInDebugBuilds) {
  OrderBook<16> book(100, 20);
  book.restOrder(makeOrder(1, 105, 10, Side::Buy));
  EXPECT_DEATH(book.restOrder(makeOrder(1, 106, 5, Side::Sell)), "");
}
#endif
