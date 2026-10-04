#pragma once

#include <cstddef>
#include <string>

namespace tickstream::sql {

// Bind parameters per trade row: symbol, trade_id, price, quantity,
// event_time_ms, received_at_ms, is_buyer_maker.
inline constexpr std::size_t kParamsPerRow = 7;

// PostgreSQL's wire protocol caps a statement at 65535 bind parameters.
inline constexpr std::size_t kMaxRowsPerInsert = 65535 / kParamsPerRow;

// INSERT ... VALUES (...), (...), ... ON CONFLICT DO NOTHING for `rows` rows,
// with $1..$N placeholders. Timestamps are integer milliseconds converted in
// SQL with exact interval arithmetic. rows == 1 is the naive single-row
// statement. Throws std::invalid_argument if rows is 0 or above
// kMaxRowsPerInsert.
std::string insert_values(std::size_t rows);

// For the COPY writer: a session-private staging table that empties itself
// at every commit, and the statement that moves its rows into trades. COPY
// itself has no ON CONFLICT, so this second step is what keeps it idempotent.
std::string create_staging_table();
std::string insert_from_staging();

}  // namespace tickstream::sql
