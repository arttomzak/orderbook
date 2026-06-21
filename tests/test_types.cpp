#include "types.hpp"

#include <gtest/gtest.h>

#include <type_traits>

TEST(Types, SideHasTwoDistinctEnumerators) {
  EXPECT_NE(Side::Buy, Side::Sell);
}

TEST(Types, SideUnderlyingTypeIsOneByte) {
  static_assert(std::is_same_v<std::underlying_type_t<Side>, std::uint8_t>);
  EXPECT_EQ(sizeof(Side), 1u);
}

TEST(Types, PriceIsSigned64Bit) {
  static_assert(std::is_signed_v<Price>);
  EXPECT_EQ(sizeof(Price), 8u);
}

TEST(Types, QuantityIsUnsigned32Bit) {
  static_assert(std::is_unsigned_v<Quantity>);
  EXPECT_EQ(sizeof(Quantity), 4u);
}

TEST(Types, OrderIdIsUnsigned64Bit) {
  static_assert(std::is_unsigned_v<OrderId>);
  EXPECT_EQ(sizeof(OrderId), 8u);
}

TEST(Types, FillRoundTripsAllFields) {
  Fill fill{/*restingOrderId=*/1, /*aggressorOrderId=*/2, /*price=*/30000,
            /*quantity=*/100, /*aggressorSide=*/Side::Buy};

  EXPECT_EQ(fill.restingOrderId, 1u);
  EXPECT_EQ(fill.aggressorOrderId, 2u);
  EXPECT_EQ(fill.price, 30000);
  EXPECT_EQ(fill.quantity, 100u);
  EXPECT_EQ(fill.aggressorSide, Side::Buy);
}

TEST(Types, FillIsTriviallyCopyable) {
  EXPECT_TRUE(std::is_trivially_copyable_v<Fill>);
}

TEST(Types, FillStaysCompact) {
  // 2x OrderId (8) + Price (8) + Quantity (4) + Side (1) = 29, likely
  // padded to 32 - keep an eye on it staying small on the hot path.
  EXPECT_LE(sizeof(Fill), 32u);
}
