#pragma once

#include <array>
#include <bitset>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

// Fixed-capacity, O(1) alloc/free object pool. Pre-allocates all storage up fornt and doesn't touch heap again. .
template <typename T, std::size_t Capacity>
class ObjectPool {
  static_assert(Capacity > 0, "ObjectPool capacity must be positive");

 public:
  
  // custom type representing the slot index 
  using Handle = std::uint32_t;

  // sentinel value for all to recognize that this slot is not free! -> (max val of a Handle)
  // note that static makes it so every ObjectPool object uses this instance rather than a bunch of copies 
  // constexpr -> resolved at compile time (this number will never change)
  static constexpr Handle kInvalidHandle =
      std::numeric_limits<Handle>::max();


  ObjectPool();
  ~ObjectPool();

  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;
  ObjectPool(ObjectPool&&) = delete;
  ObjectPool& operator=(ObjectPool&&) = delete;

  // TODO: placement-new construct T at a free slot, push/pop free list.
  // Returns kInvalidHandle if full.
  template <typename... Args>
  Handle allocate(Args&&... args);

  // TODO: destroy T at h, return its slot to the free list.
  void deallocate(Handle h);

  // TODO: return a reference to the live T at handle h.
  T& get(Handle h);
  const T& get(Handle h) const;

  std::size_t size() const;
  static constexpr std::size_t capacity() { return Capacity; }
  bool full() const;
  bool empty() const;

 private:
  // TODO: helpers for turning a Handle into a pointer into storage_.

  // TODO: storage_, free_indices_, free_count_, live_ member declarations.
};
