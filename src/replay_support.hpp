#pragma once

// Shared Layer-3 replay support: the price-band config and the mmap wrapper,
// used by both main.cpp (correctness replay) and bench/harness.cpp (benchmark),
// so the two drive the engine with identical sizing.

#include "types.hpp"

#include <cstddef>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// Price band for the flat array: $0.0000 .. $1000.0000 at ITCH's 1/10000-dollar
// tick. Measured from the data - AAPL's whole market for this day sits in
// $250-500, so $0-$1000 covers it ~2x over. A flat array can't span ITCH's full
// $200k ceiling (~64 GB), so out-of-band stub orders (~60 for this day) are
// filtered, not stored. basePrice = 0 means the array index is the raw tick.
inline constexpr Price kBasePrice = 0;
inline constexpr std::size_t kNumTicks = 10'000'000;
inline constexpr const char* kSymbol = "AAPL";

// RAII wrapper around a read-only mmap of an entire file. The decompressed ITCH
// day is far larger than RAM, so we never read it into memory - we map it and
// let the kernel page it in (and drop it behind us, via MADV_SEQUENTIAL) as the
// parser walks the buffer once, front to back.
class MappedFile {
 public:
  explicit MappedFile(const char* path) {
    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) {
      return;
    }
    struct stat st {};
    if (::fstat(fd_, &st) != 0 || st.st_size <= 0) {
      return;
    }
    size_ = static_cast<std::size_t>(st.st_size);
    void* addr = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (addr == MAP_FAILED) {
      addr_ = nullptr;
      return;
    }
    addr_ = addr;
    ::madvise(addr_, size_, MADV_SEQUENTIAL);
  }

  ~MappedFile() {
    if (addr_ != nullptr) {
      ::munmap(addr_, size_);
    }
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  bool valid() const { return addr_ != nullptr; }
  const char* data() const { return static_cast<const char*>(addr_); }
  std::size_t size() const { return size_; }

 private:
  int fd_ = -1;
  void* addr_ = nullptr;
  std::size_t size_ = 0;
};
