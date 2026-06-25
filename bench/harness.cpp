// Benchmark harness (Layer 3). Measures the matching engine's hot-path latency
// and throughput on a full day of real ITCH order flow.
//
// Two passes, deliberately separated so the timed loop is PURE engine - no
// parsing, no symbol/band filtering, no map lookups polluting the measurement:
//
//   Pass 1 (untimed): parse the ITCH stream once, apply the same symbol filter,
//     price-band filter, and id->side tracking the production adapter uses, and
//     record the resulting sequence of engine operations into a flat vector.
//   Pass 2 (timed): replay that op vector into a fresh engine, wrapping
//     clock_gettime(CLOCK_MONOTONIC) around each submit/cancel/reduceQty.
//
// Pass 2 then runs the same Tier-1 audit as the correctness harness, so the
// benchmark self-validates that it drove the engine into a correct book (and
// that pass 1's dispatch hasn't drifted from the adapter's).

#include "itch_adapter.hpp"   // Engine, kPoolCapacity
#include "replay_support.hpp"  // MappedFile, kBasePrice/kNumTicks/kSymbol

#include "itch/parser.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace {

struct Op {
  enum class Kind : std::uint8_t { Submit, Cancel, Reduce };
  Kind kind;
  Side side;       // Submit
  OrderId id;      // all
  Price price;     // Submit
  Quantity qty;    // Submit, Reduce
};

inline std::uint64_t nowNs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

inline bool inBand(Price p) {
  const std::int64_t idx = p - kBasePrice;
  return idx >= 0 && static_cast<std::size_t>(idx) < kNumTicks;
}

// Pass 1: parse + filter + translate into the engine-op stream. Mirrors the
// production adapter's dispatch exactly (symbol filter, band filter, Option-A
// id->side tracking) so the ops are the same ones production would execute.
std::vector<Op> buildOps(const char* data, std::size_t size,
                         std::uint64_t& excludedOutOfBand) {
  std::vector<Op> ops;
  ops.reserve(8'000'000);
  std::unordered_map<OrderId, Side> tracked;
  excludedOutOfBand = 0;

  itch::Parser parser;
  parser.parse(data, size, [&](const itch::Message& msg) {
    std::visit(
        [&](const auto& m) {
          using T = std::decay_t<decltype(m)>;

          if constexpr (std::is_same_v<T, itch::AddOrderMessage> ||
                        std::is_same_v<T, itch::AddOrderMPIDAttributionMessage>) {
            if (itch::to_string(m.stock, itch::STOCK_LEN) != kSymbol) return;
            const Side side =
                (m.buy_sell_indicator == 'B') ? Side::Buy : Side::Sell;
            const auto price = static_cast<Price>(m.price);
            if (!inBand(price)) {
              ++excludedOutOfBand;
              return;
            }
            ops.push_back({Op::Kind::Submit, side, m.order_reference_number,
                           price, m.shares});
            tracked[m.order_reference_number] = side;

          } else if constexpr (std::is_same_v<T, itch::OrderDeleteMessage>) {
            auto it = tracked.find(m.order_reference_number);
            if (it == tracked.end()) return;
            ops.push_back({Op::Kind::Cancel, Side::Buy,
                           m.order_reference_number, 0, 0});
            tracked.erase(it);

          } else if constexpr (std::is_same_v<T, itch::OrderCancelMessage>) {
            if (!tracked.count(m.order_reference_number)) return;
            ops.push_back({Op::Kind::Reduce, Side::Buy,
                           m.order_reference_number, 0, m.cancelled_shares});

          } else if constexpr (std::is_same_v<T, itch::OrderExecutedMessage>) {
            if (!tracked.count(m.order_reference_number)) return;
            ops.push_back({Op::Kind::Reduce, Side::Buy,
                           m.order_reference_number, 0, m.executed_shares});

          } else if constexpr (std::is_same_v<
                                   T, itch::OrderExecutedWithPriceMessage>) {
            if (!tracked.count(m.order_reference_number)) return;
            ops.push_back({Op::Kind::Reduce, Side::Buy,
                           m.order_reference_number, 0, m.executed_shares});

          } else if constexpr (std::is_same_v<T, itch::OrderReplaceMessage>) {
            auto it = tracked.find(m.original_order_reference_number);
            if (it == tracked.end()) return;
            const Side side = it->second;
            ops.push_back({Op::Kind::Cancel, Side::Buy,
                           m.original_order_reference_number, 0, 0});
            tracked.erase(it);
            const auto price = static_cast<Price>(m.price);
            if (!inBand(price)) {
              ++excludedOutOfBand;
              return;
            }
            ops.push_back({Op::Kind::Submit, side, m.new_order_reference_number,
                           price, m.shares});
            tracked[m.new_order_reference_number] = side;
          }
        },
        msg);
  });
  return ops;
}

// Median back-to-back clock_gettime pair cost - the measurement noise floor.
std::uint64_t measureClockOverhead() {
  constexpr int kN = 200'000;
  std::vector<std::uint64_t> ov;
  ov.reserve(kN);
  for (int i = 0; i < kN; ++i) {
    const std::uint64_t a = nowNs();
    const std::uint64_t b = nowNs();
    ov.push_back(b - a);
  }
  std::sort(ov.begin(), ov.end());
  return ov[ov.size() / 2];
}

struct Pct {
  std::uint64_t p50, p99, p999, max, count;
};

Pct percentiles(std::vector<std::uint32_t>& v) {
  if (v.empty()) return {0, 0, 0, 0, 0};
  std::sort(v.begin(), v.end());
  const auto at = [&](double p) {
    return static_cast<std::uint64_t>(
        v[static_cast<std::size_t>(p * (v.size() - 1))]);
  };
  return {at(0.50), at(0.99), at(0.999), v.back(), v.size()};
}

void report(const char* label, Pct p) {
  std::printf("  %-8s n=%-9llu  p50=%-5llu p99=%-6llu p99.9=%-7llu max=%llu ns\n",
              label, static_cast<unsigned long long>(p.count),
              static_cast<unsigned long long>(p.p50),
              static_cast<unsigned long long>(p.p99),
              static_cast<unsigned long long>(p.p999),
              static_cast<unsigned long long>(p.max));
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

  // -- Pass 1: build the op stream (untimed) -------------------------------
  std::uint64_t excluded = 0;
  std::vector<Op> ops = buildOps(file.data(), file.size(), excluded);

  std::uint64_t nSubmit = 0, nCancel = 0, nReduce = 0;
  for (const Op& op : ops) {
    nSubmit += op.kind == Op::Kind::Submit;
    nCancel += op.kind == Op::Kind::Cancel;
    nReduce += op.kind == Op::Kind::Reduce;
  }
  std::cout << "pass 1 (op stream):\n"
            << "  submits: " << nSubmit << "  cancels: " << nCancel
            << "  reduces: " << nReduce << "  total: " << ops.size() << "\n"
            << "  excluded out-of-band: " << excluded << "\n";

  const std::uint64_t clockOverhead = measureClockOverhead();

  // -- Pass 2: timed replay into a fresh engine ----------------------------
  auto engine = std::make_unique<Engine>(kBasePrice, kNumTicks);
  std::vector<Fill> fills;
  fills.reserve(64);

  std::vector<std::uint32_t> submitNs, cancelNs, reduceNs, allNs;
  submitNs.reserve(nSubmit);
  cancelNs.reserve(nCancel);
  reduceNs.reserve(nReduce);
  allNs.reserve(ops.size());

  for (const Op& op : ops) {
    if (op.kind == Op::Kind::Submit) {
      Order o{.id = op.id,
              .price = op.price,
              .quantity = op.qty,
              .side = op.side};
      fills.clear();
      const std::uint64_t t0 = nowNs();
      engine->submit(o, fills);
      const std::uint64_t dt = nowNs() - t0;
      submitNs.push_back(static_cast<std::uint32_t>(dt));
      allNs.push_back(static_cast<std::uint32_t>(dt));
    } else if (op.kind == Op::Kind::Cancel) {
      const std::uint64_t t0 = nowNs();
      engine->cancel(op.id);
      const std::uint64_t dt = nowNs() - t0;
      cancelNs.push_back(static_cast<std::uint32_t>(dt));
      allNs.push_back(static_cast<std::uint32_t>(dt));
    } else {
      const std::uint64_t t0 = nowNs();
      engine->reduceQty(op.id, op.qty);
      const std::uint64_t dt = nowNs() - t0;
      reduceNs.push_back(static_cast<std::uint32_t>(dt));
      allNs.push_back(static_cast<std::uint32_t>(dt));
    }
  }

  // -- Pass 3: clean throughput (untimed per-op) ---------------------------
  // Replay the same ops into a FRESH engine (the engine is stateful - the op
  // stream can't be replayed twice on the same one) with a single timer around
  // the whole loop, so the ~19 ns/op clock cost doesn't inflate the rate.
  auto engine2 = std::make_unique<Engine>(kBasePrice, kNumTicks);
  std::vector<Fill> fills2;
  fills2.reserve(64);
  const std::uint64_t wall0 = nowNs();
  for (const Op& op : ops) {
    if (op.kind == Op::Kind::Submit) {
      Order o{.id = op.id,
              .price = op.price,
              .quantity = op.qty,
              .side = op.side};
      fills2.clear();
      engine2->submit(o, fills2);
    } else if (op.kind == Op::Kind::Cancel) {
      engine2->cancel(op.id);
    } else {
      engine2->reduceQty(op.id, op.qty);
    }
  }
  const std::uint64_t wallNs = nowNs() - wall0;

  // -- Report --------------------------------------------------------------
  std::cout << "pass 2 (timed engine replay):\n";
  std::cout << "  clock_gettime pair overhead (median): " << clockOverhead
            << " ns  (included in the latencies below; true latency is lower)\n";
  report("submit", percentiles(submitNs));
  report("cancel", percentiles(cancelNs));
  report("reduce", percentiles(reduceNs));
  report("all", percentiles(allNs));

  const double totalOps = static_cast<double>(ops.size());
  std::printf(
      "  throughput: %.2f M ops/sec  (%.0f ops in %.1f ms, untimed per-op)\n",
      totalOps / static_cast<double>(wallNs) * 1000.0, totalOps,
      static_cast<double>(wallNs) / 1'000'000.0);

  const bool ok = engine->audit() && engine->droppedOrders() == 0;
  std::cout << "  invariants: " << (ok ? "PASS" : "FAIL")
            << " (peak live " << engine->peakLiveOrders() << ")\n";
  return ok ? 0 : 1;
}
