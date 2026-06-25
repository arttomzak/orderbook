#include "itch_adapter.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
// Price band for the flat array: $0.0000 .. $1000.0000 at ITCH's 1/10000-dollar
// tick. Measured from the data - AAPL's whole market for this day sits in
// $250-500, so $0-$1000 covers it ~2x over. A flat array can't span ITCH's full
// $200k ceiling (~64 GB), so the adapter filters the handful of stub orders
// above $1000 (~60 for this day); see ItchAdapter's band filter.
constexpr Price kBasePrice = 0;
constexpr std::size_t kNumTicks = 10'000'000;
constexpr const char* kSymbol = "AAPL";

std::string levelSummary(PriceLevel* lvl) {
  if (lvl == nullptr) {
    return "none";
  }
  return std::to_string(lvl->totalVolume) + " sh / " +
         std::to_string(lvl->orderCount) + " orders";
}

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
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <decompressed-itch-file>\n";
    return 1;
  }

  MappedFile file(argv[1]);
  if (!file.valid()) {
    std::perror("mmap input file");
    return 1;
  }

  // Heap-allocated: the engine embeds a ~44 MB inline object pool, far past the
  // ~8 MB stack limit, so it can't be a local. One-time startup allocation -
  // the hot-path no-alloc rule is about submit()/cancel(), not construction.
  auto engine = std::make_unique<Engine>(kBasePrice, kNumTicks);
  ItchAdapter adapter(*engine, kSymbol);

  try {
    adapter.run(file.data(), file.size());
  } catch (const std::exception& e) {
    // Expected when the input is a byte-sliced prefix: the final message is
    // truncated and the parser throws once it runs off the end. Every message
    // before that point was already replayed into the book.
    std::cerr << "[note] parse stopped: " << e.what()
              << " (expected for a sliced sample)\n";
  }

  const ItchAdapter::Stats& s = adapter.stats();
  std::cout << "Symbol: " << kSymbol << "\n"
            << "  adds:     " << s.adds << "\n"
            << "  deletes:  " << s.deletes << "\n"
            << "  cancels:  " << s.cancels << "\n"
            << "  executes: " << s.executes << "\n"
            << "  replaces: " << s.replaces << "\n"
            << "  best bid: " << levelSummary(engine->bestBid()) << "\n"
            << "  best ask: " << levelSummary(engine->bestAsk()) << "\n"
            << "  in-band price range (ticks): " << s.minPrice << " .. "
            << s.maxPrice << "\n"
            << "  excluded out-of-band: " << s.excludedOutOfBand << " orders\n";

  // Sizing diagnostic - how big the pool needs to be, vs. its capacity.
  std::cout << "  peak live: " << engine->peakLiveOrders() << " / "
            << kPoolCapacity << " capacity\n";

  // Drops void the run: from the first one the book is missing an order the
  // real book had, so the audit below would be checking a corrupted book.
  const std::uint64_t drops = engine->droppedOrders();
  if (drops != 0) {
    std::cout << "  drops: " << drops << " (pool-full "
              << engine->droppedPoolFull() << ", price-range "
              << engine->droppedPriceRange()
              << ") -- run is INVALID, raise capacity/range and rerun\n";
  }

  // PASS requires both: the book is internally consistent AND nothing was ever
  // dropped (so the audit ran against a book that could be correct).
  const bool ok = engine->audit() && drops == 0;
  std::cout << "  invariants: " << (ok ? "PASS" : "FAIL") << "\n";
  return ok ? 0 : 1;
}
