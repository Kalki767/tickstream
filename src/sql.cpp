#include "tickstream/sql.hpp"

#include <stdexcept>

namespace tickstream::sql {

namespace {

constexpr const char* kColumns =
    "INSERT INTO trades (symbol, trade_id, price, quantity, event_time, received_at, "
    "is_buyer_maker) VALUES ";

}  // namespace

std::string insert_values(std::size_t rows) {
    if (rows == 0 || rows > kMaxRowsPerInsert) {
        throw std::invalid_argument("insert_values: rows must be 1.." +
                                    std::to_string(kMaxRowsPerInsert));
    }
    std::string sql = kColumns;
    sql.reserve(sql.size() + rows * 140 + 32);
    for (std::size_t r = 0; r < rows; ++r) {
        const std::size_t p = r * kParamsPerRow;  // last placeholder used so far
        if (r > 0) {
            sql += ", ";
        }
        sql += "($" + std::to_string(p + 1) + ", $" + std::to_string(p + 2) + ", $" +
               std::to_string(p + 3) + "::numeric, $" + std::to_string(p + 4) +
               "::numeric, 'epoch'::timestamptz + $" + std::to_string(p + 5) +
               "::bigint * interval '1 millisecond', 'epoch'::timestamptz + $" +
               std::to_string(p + 6) + "::bigint * interval '1 millisecond', $" +
               std::to_string(p + 7) + ")";
    }
    sql += " ON CONFLICT DO NOTHING";
    return sql;
}

std::string create_staging_table() {
    return "CREATE TEMP TABLE trades_staging ("
           "symbol TEXT, trade_id BIGINT, price NUMERIC, quantity NUMERIC, "
           "event_time_ms BIGINT, received_at_ms BIGINT, is_buyer_maker BOOLEAN"
           ") ON COMMIT DELETE ROWS";
}

std::string insert_from_staging() {
    return "INSERT INTO trades (symbol, trade_id, price, quantity, event_time, received_at, "
           "is_buyer_maker) SELECT symbol, trade_id, price, quantity, "
           "'epoch'::timestamptz + event_time_ms * interval '1 millisecond', "
           "'epoch'::timestamptz + received_at_ms * interval '1 millisecond', "
           "is_buyer_maker FROM trades_staging ON CONFLICT DO NOTHING";
}

}  // namespace tickstream::sql
