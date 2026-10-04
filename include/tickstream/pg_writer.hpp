#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <pqxx/connection>

#include "tickstream/trade.hpp"
#include "tickstream/trade_sink.hpp"

namespace tickstream {

// Writes trades to PostgreSQL over one persistent connection. Every mode is
// idempotent (ON CONFLICT DO NOTHING on the (symbol, trade_id) key).
//
//   naive:    one prepared single-row INSERT per trade, each its own autocommit
//             transaction. The honest baseline: persistent connection and a
//             prepared statement, so batching can only remove per-row round
//             trips and per-row commits.
//   txn:      the same per-row prepared INSERTs, but one transaction per batch.
//             Isolates the cost of the commit (WAL flush) from the round trips.
//   multirow: one INSERT ... VALUES (..),(..),... per batch.
//   copy:     COPY the batch into a temp staging table, then
//             INSERT ... SELECT ... ON CONFLICT DO NOTHING, in one transaction.
//             COPY can't skip conflicts itself; the staging step buys
//             idempotency at the cost of writing each row twice.
class PgWriter final : public TradeSink {
public:
    enum class Mode { naive, txn, multirow, copy };

    struct Config {
        std::string dsn;                 // libpq connection string, e.g. "dbname=tickstream"
        Mode mode = Mode::naive;
        std::size_t batch_size = 1;      // full-batch size; multirow prepares this one
        bool synchronous_commit = true;  // false only for the WAL-flush diagnostic
    };

    explicit PgWriter(const Config& config);

    std::uint64_t write(std::span<const Trade> trades) override;

private:
    std::uint64_t write_naive(std::span<const Trade> trades);
    std::uint64_t write_txn(std::span<const Trade> trades);
    std::uint64_t write_multirow(std::span<const Trade> trades);
    std::uint64_t write_copy(std::span<const Trade> trades);

    pqxx::connection conn_;
    Mode mode_;
    std::size_t batch_size_;
};

}  // namespace tickstream
