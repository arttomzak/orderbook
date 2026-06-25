#include "itch_adapter.hpp"
#include "replay_support.hpp"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
std::string levelSummary(PriceLevel* lvl) {
  if (lvl == nullptr) {
    return "none";
  }
  return std::to_string(lvl->totalVolume) + " sh / " +
         std::to_string(lvl->orderCount) + " orders";
}
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
