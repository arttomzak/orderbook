#include "object_pool.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace {

// Local instrumented test type: counts constructions/destructions so tests
// can verify the pool actually constructs/destroys objects, not just moves
// indices around.
struct TrackedObject {
  int value;

  explicit TrackedObject(int v) : value(v) { ++constructCount; }
  ~TrackedObject() { ++destructCount; }

  static int constructCount;
  static int destructCount;
  static void resetCounts() {
    constructCount = 0;
    destructCount = 0;
  }
};

int TrackedObject::constructCount = 0;
int TrackedObject::destructCount = 0;

class ObjectPoolTest : public ::testing::Test {
 protected:
  void SetUp() override { TrackedObject::resetCounts(); }
};

}  // namespace

TEST_F(ObjectPoolTest, AllocateReturnsValidHandleAndConstructsObject) {
  ObjectPool<TrackedObject, 8> pool;
  auto h = pool.allocate(42);
  ASSERT_NE(h, decltype(pool)::kInvalidHandle);
  EXPECT_EQ(pool.get(h).value, 42);
}

TEST_F(ObjectPoolTest, AllocateToCapacityReturnsDistinctHandles) {
  constexpr std::size_t kCapacity = 8;
  ObjectPool<TrackedObject, kCapacity> pool;

  std::unordered_set<std::uint32_t> handles;
  for (std::size_t i = 0; i < kCapacity; ++i) {
    auto h = pool.allocate(static_cast<int>(i));
    ASSERT_NE(h, decltype(pool)::kInvalidHandle);
    handles.insert(h);
  }

  EXPECT_EQ(handles.size(), kCapacity);
  EXPECT_TRUE(pool.full());
  EXPECT_EQ(pool.size(), kCapacity);
}

TEST_F(ObjectPoolTest, AllocateWhenFullReturnsInvalidHandle) {
  ObjectPool<TrackedObject, 2> pool;
  pool.allocate(1);
  pool.allocate(2);
  ASSERT_TRUE(pool.full());

  auto h = pool.allocate(3);
  EXPECT_EQ(h, decltype(pool)::kInvalidHandle);
  EXPECT_EQ(TrackedObject::constructCount, 2);
}

TEST_F(ObjectPoolTest, DeallocateThenReallocateReusesSlot) {
  ObjectPool<TrackedObject, 2> pool;
  pool.allocate(1);
  auto h2 = pool.allocate(2);
  ASSERT_TRUE(pool.full());

  pool.deallocate(h2);
  EXPECT_FALSE(pool.full());
  EXPECT_EQ(pool.size(), 1u);

  auto h3 = pool.allocate(3);
  EXPECT_NE(h3, decltype(pool)::kInvalidHandle);
  EXPECT_TRUE(pool.full());
}

TEST_F(ObjectPoolTest, LifoOrderingIsObservable) {
  ObjectPool<TrackedObject, 3> pool;
  auto h0 = pool.allocate(0);
  auto h1 = pool.allocate(1);
  auto h2 = pool.allocate(2);

  pool.deallocate(h1);
  pool.deallocate(h2);

  // Most-recently-freed (h2) should come back first, then h1.
  EXPECT_EQ(pool.allocate(20), h2);
  EXPECT_EQ(pool.allocate(10), h1);
  (void)h0;
}

TEST_F(ObjectPoolTest, DeallocateInvokesDestructorExactlyOnce) {
  ObjectPool<TrackedObject, 4> pool;
  auto h = pool.allocate(1);
  ASSERT_EQ(TrackedObject::destructCount, 0);

  pool.deallocate(h);
  EXPECT_EQ(TrackedObject::destructCount, 1);
  EXPECT_EQ(TrackedObject::constructCount, 1);
}

TEST_F(ObjectPoolTest, ManyAllocDeallocCyclesBalance) {
  constexpr std::size_t kCapacity = 4;
  ObjectPool<TrackedObject, kCapacity> pool;

  for (int round = 0; round < 50; ++round) {
    std::vector<decltype(pool)::Handle> handles;
    for (std::size_t i = 0; i < kCapacity; ++i) {
      handles.push_back(pool.allocate(round));
    }
    EXPECT_TRUE(pool.full());
    for (auto h : handles) {
      pool.deallocate(h);
    }
    EXPECT_TRUE(pool.empty());
  }

  EXPECT_EQ(TrackedObject::constructCount, TrackedObject::destructCount);
  EXPECT_EQ(TrackedObject::constructCount, 50 * static_cast<int>(kCapacity));
}

TEST_F(ObjectPoolTest, PoolDestructorCleansUpStillLiveObjects) {
  {
    ObjectPool<TrackedObject, 4> pool;
    pool.allocate(1);
    pool.allocate(2);
    pool.allocate(3);
    EXPECT_EQ(TrackedObject::destructCount, 0);
  }
  // Pool went out of scope without explicit deallocate() calls - its
  // destructor must have cleaned up all 3 still-live objects.
  EXPECT_EQ(TrackedObject::destructCount, 3);
}

TEST_F(ObjectPoolTest, CapacityOneEdgeCase) {
  ObjectPool<TrackedObject, 1> pool;
  auto h = pool.allocate(7);
  ASSERT_NE(h, decltype(pool)::kInvalidHandle);
  EXPECT_TRUE(pool.full());

  EXPECT_EQ(pool.allocate(8), decltype(pool)::kInvalidHandle);

  pool.deallocate(h);
  EXPECT_TRUE(pool.empty());

  auto h2 = pool.allocate(9);
  EXPECT_NE(h2, decltype(pool)::kInvalidHandle);
  EXPECT_EQ(h2, h);
}

TEST_F(ObjectPoolTest, LargeCapacitySmokeTest) {
  constexpr std::size_t kCapacity = 100000;
  ObjectPool<TrackedObject, kCapacity> pool;

  std::vector<decltype(pool)::Handle> handles;
  handles.reserve(kCapacity);
  for (std::size_t i = 0; i < kCapacity; ++i) {
    handles.push_back(pool.allocate(static_cast<int>(i)));
  }
  EXPECT_TRUE(pool.full());
  EXPECT_EQ(pool.size(), kCapacity);

  for (auto h : handles) {
    pool.deallocate(h);
  }
  EXPECT_TRUE(pool.empty());
}

#ifndef NDEBUG
TEST_F(ObjectPoolTest, DoubleFreeTriggersAssertInDebugBuilds) {
  ObjectPool<TrackedObject, 2> pool;
  auto h = pool.allocate(1);
  pool.deallocate(h);
  EXPECT_DEATH(pool.deallocate(h), "");
}
#endif
