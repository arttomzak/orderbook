#include "itch_adapter.hpp"

#include <cstddef>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
// Hardcoded generous price range for the smoke test: covers $0.0000 ..
// $500.0000 at ITCH's 1/10000-dollar tick. AAPL traded ~$185-190 in May 2018,
// comfortably inside. basePrice = 0 means the array index is just the raw tick.
// TODO: replace with measured min/max for the symbol before the real run.
constexpr Price kBasePrice = 0;
constexpr std::size_t kNumTicks = 5'000'000;
constexpr const char* kSymbol = "AAPL";

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

  std::ifstream file(argv[1], std::ios::binary);
  if (!file) {
    std::cerr << "Error: cannot open " << argv[1] << "\n";
    return 1;
  }

  // Heap-allocated: the engine embeds a ~44 MB inline object pool, far past the
  // ~8 MB stack limit, so it can't be a local. One-time startup allocation -
  // the hot-path no-alloc rule is about submit()/cancel(), not construction.
  auto engine = std::make_unique<Engine>(kBasePrice, kNumTicks);
  ItchAdapter adapter(*engine, kSymbol);

  try {
    adapter.run(file);
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
            << "  best ask: " << levelSummary(engine->bestAsk()) << "\n";

  const bool ok = engine->audit();
  std::cout << "  invariants: " << (ok ? "PASS" : "FAIL") << "\n";
  return ok ? 0 : 1;
}
