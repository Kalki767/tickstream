#include "tickstream/pg_writer.hpp"

#include <algorithm>
#include <stdexcept>

#include <pqxx/nontransaction>
#include <pqxx/params>
#include <pqxx/stream_to>
#include <pqxx/transaction>

#include "tickstream/sql.hpp"

namespace tickstream {

namespace {

constexpr const char* kInsertOne = "insert_one";
constexpr const char* kInsertBatch = "insert_batch";

void append_row(pqxx::params& params, const Trade& t) {
    params.append(t.symbol);
    params.append(t.trade_id);
    params.append(t.price);
    params.append(t.quantity);
    params.append(t.trade_time_ms);
    params.append(t.received_wall_ms);
    params.append(t.is_buyer_maker);
}

}  // namespace

PgWriter::PgWriter(const Config& config)
    : conn_(config.dsn), mode_(config.mode), batch_size_(config.batch_size) {
    if (!config.synchronous_commit) {
        // Diagnostic only: COMMIT returns before the WAL is flushed to disk,
        // trading durability of the last few hundred ms for speed.
        pqxx::nontransaction tx(conn_);
        tx.exec0("SET synchronous_commit = off");
    }
    switch (mode_) {
        case Mode::naive:
        case Mode::txn:
            conn_.prepare(kInsertOne, sql::insert_values(1));
            break;
        case Mode::multirow:
            if (batch_size_ == 0 || batch_size_ > sql::kMaxRowsPerInsert) {
                throw std::invalid_argument("multirow batch size must be 1.." +
                                            std::to_string(sql::kMaxRowsPerInsert));
            }
            conn_.prepare(kInsertBatch, sql::insert_values(batch_size_));
            break;
        case Mode::copy: {
            pqxx::nontransaction tx(conn_);
            tx.exec0(sql::create_staging_table());
            conn_.prepare(kInsertBatch, sql::insert_from_staging());
            break;
        }
    }
}

std::uint64_t PgWriter::write(std::span<const Trade> trades) {
    if (trades.empty()) {
        return 0;
    }
    switch (mode_) {
        case Mode::naive:
            return write_naive(trades);
        case Mode::txn:
            return write_txn(trades);
        case Mode::multirow:
            return write_multirow(trades);
        case Mode::copy:
            return write_copy(trades);
    }
    throw std::logic_error("unhandled PgWriter::Mode");
}

std::uint64_t PgWriter::write_naive(std::span<const Trade> trades) {
    // nontransaction: every statement is its own autocommit transaction, so
    // each row pays for its own COMMIT (and WAL flush).
    pqxx::nontransaction tx(conn_);
    std::uint64_t inserted = 0;
    for (const Trade& t : trades) {
        inserted += static_cast<std::uint64_t>(
            tx.exec_prepared(kInsertOne, t.symbol, t.trade_id, t.price, t.quantity,
                             t.trade_time_ms, t.received_wall_ms, t.is_buyer_maker)
                .affected_rows());
    }
    return inserted;
}

std::uint64_t PgWriter::write_txn(std::span<const Trade> trades) {
    // Same statements and round trips as naive; only the commit is shared.
    pqxx::work tx(conn_);
    std::uint64_t inserted = 0;
    for (const Trade& t : trades) {
        inserted += static_cast<std::uint64_t>(
            tx.exec_prepared(kInsertOne, t.symbol, t.trade_id, t.price, t.quantity,
                             t.trade_time_ms, t.received_wall_ms, t.is_buyer_maker)
                .affected_rows());
    }
    tx.commit();
    return inserted;
}

std::uint64_t PgWriter::write_multirow(std::span<const Trade> trades) {
    pqxx::work tx(conn_);
    std::uint64_t inserted = 0;
    // Full batches use the prepared statement; a short batch (time-triggered
    // flush or the final one at shutdown) is sent unprepared, built for its size.
    for (std::size_t start = 0; start < trades.size(); start += batch_size_) {
        const auto chunk = trades.subspan(start, std::min(batch_size_, trades.size() - start));
        pqxx::params params;
        params.reserve(chunk.size() * sql::kParamsPerRow);
        for (const Trade& t : chunk) {
            append_row(params, t);
        }
        const pqxx::result r = chunk.size() == batch_size_
                                   ? tx.exec_prepared(kInsertBatch, params)
                                   : tx.exec_params(sql::insert_values(chunk.size()), params);
        inserted += static_cast<std::uint64_t>(r.affected_rows());
    }
    tx.commit();
    return inserted;
}

std::uint64_t PgWriter::write_copy(std::span<const Trade> trades) {
    pqxx::work tx(conn_);
    {
        auto stream = pqxx::stream_to::table(
            tx, {"trades_staging"},
            {"symbol", "trade_id", "price", "quantity", "event_time_ms", "received_at_ms",
             "is_buyer_maker"});
        for (const Trade& t : trades) {
            stream.write_values(t.symbol, t.trade_id, t.price, t.quantity, t.trade_time_ms,
                                t.received_wall_ms, t.is_buyer_maker);
        }
        stream.complete();
    }
    const auto inserted =
        static_cast<std::uint64_t>(tx.exec_prepared(kInsertBatch).affected_rows());
    tx.commit();  // ON COMMIT DELETE ROWS empties the staging table
    return inserted;
}

}  // namespace tickstream
