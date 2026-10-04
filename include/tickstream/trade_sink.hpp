#pragma once

#include <cstdint>
#include <span>

#include "tickstream/trade.hpp"

namespace tickstream {

// Where the writer thread sends trades. The real implementation is PgWriter
// (PostgreSQL); tests use fakes, and NullSink measures the pipeline without a
// database.
class TradeSink {
public:
    virtual ~TradeSink() = default;

    // Durably stores `trades` and returns once they are committed. Returns the
    // number of rows actually inserted (duplicates are skipped, not errors).
    // Throws on failure.
    virtual std::uint64_t write(std::span<const Trade> trades) = 0;
};

// Accepts and discards everything. Used for capture-only runs and to measure
// the parse -> queue ceiling with no database in the way.
class NullSink final : public TradeSink {
public:
    std::uint64_t write(std::span<const Trade> trades) override { return trades.size(); }
};

}  // namespace tickstream
