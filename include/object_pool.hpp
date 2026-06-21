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

  // sentinel meaning "no handle" - returned by allocate() when the pool is full, not a marker on any real slot -> (max val of a Handle)
  // note that static makes it so every ObjectPool object uses this instance rather than a bunch of copies 
  // constexpr -> resolved at compile time (this number will never change)
  static constexpr Handle kInvalidHandle =
      std::numeric_limits<Handle>::max();


  // every slot starts free 
  ObjectPool() : free_count_(Capacity) {
    for (std::size_t i = 0; i < Capacity; ++i) {
      free_indices_[i] = static_cast<Handle>(Capacity - 1 - i);
    }
  }

  // Anything still live when the pool itself is destroyed (e.g. orders
  // still resting at the end of a benchmark run) needs its destructor run
  // explicitly - storage_ is just bytes, nothing does this automatically.
  ~ObjectPool() {
    for (Handle h = 0; h < Capacity; ++h) {
      if (live_.test(h)) {
        slot_ptr(h)->~T();
      }
    }
  }

  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;
  ObjectPool(ObjectPool&&) = delete;
  ObjectPool& operator=(ObjectPool&&) = delete;

  // Returns kInvalidHandle if full - an expected, caller-checked outcome,
  // not an assert/throw.
  template <typename... Args>
  Handle allocate(Args&&... args) {
    if (free_count_ == 0) {
      return kInvalidHandle;
    }

    const Handle h = free_indices_[--free_count_]; // in one line "pops" from our stack of free indices 
    
    // :: is the global new, making sure we know what it's doing 
    // slot_address uses our helper to find exactly where the slot is from our handle index 
    // then calls a constructor for T using the args we passed through (from our market data)
    ::new (slot_address(h)) T(std::forward<Args>(args)...);
    live_.set(h); // mark slot as allocated 
    return h;
  }

  // Bad handle (never allocated / already freed) is a programmer error,
  // not a runtime condition - defended with debug-only asserts.
  void deallocate(Handle h) {
    // make sure that we are under the cap and by the bitset have an object to dealloc 
    assert(h < Capacity);  
    assert(live_.test(h));

    slot_ptr(h)->~T(); // call the destructor of the object at the handle's pointer 
    live_.reset(h); // bitset method, clears the bit back to 0 or available mode  
    free_indices_[free_count_++] = h; // "push"" to the stack of free indices
  }

  // Same narrow-contract pattern as deallocate(): bad handle is a
  // programmer error, debug-only asserts, zero cost in Release.
  T& get(Handle h) {
    assert(h < Capacity);
    assert(live_.test(h));
    return *slot_ptr(h);
  }
  const T& get(Handle h) const {
    assert(h < Capacity);
    assert(live_.test(h));
    return *slot_ptr(h);
  }

  // Live count is just total minus however many are sitting free.
  std::size_t size() const { return Capacity - free_count_; }
  static constexpr std::size_t capacity() { return Capacity; }
  bool full() const { return free_count_ == 0; }
  bool empty() const { return free_count_ == Capacity; }

 private:
  // Raw address of slot h, for placement-new'ing a T into a slot that has
  // no live object yet - no claims about what's there.
  void* slot_address(Handle h) { return storage_[h].data(); }

  // Pointer to the already-constructed T living in slot h. std::launder
  // re-derives the real object pointer, since the compiler doesn't
  // otherwise know a T now lives inside what it still sees as raw bytes.
  T* slot_ptr(Handle h) {
    return std::launder(reinterpret_cast<T*>(storage_[h].data()));
  }
  const T* slot_ptr(Handle h) const {
    return std::launder(reinterpret_cast<const T*>(storage_[h].data()));
  }

  // Raw, uninitialized storage for Capacity slots, each big enough and
  // correctly aligned to hold one T (alignas enables this)
  alignas(T) std::array<std::array<std::byte, sizeof(T)>, Capacity> storage_;

  // LIFO stack of slot indices that are currently free.
  // Only the first free_count_ entries are meaningful at any given time -
  // the rest is leftover/stale data from past pops, ignored.
  std::array<Handle, Capacity> free_indices_;

  // How many entries at the front of free_indices_ are currently valid -
  // i.e. how many slots are free right now. Also doubles as the stack's
  // "top" pointer: the next free slot to hand out is free_indices_[free_count_ - 1].
  std::size_t free_count_;

  // One bit per slot: true if that slot currently holds a real, constructed
  // T. Used to catch double-free (asserting a slot was actually live before
  // destroying it again) and so the destructor knows exactly which slots
  // still need their T destroyed when the pool itself goes away.
  std::bitset<Capacity> live_;
};
