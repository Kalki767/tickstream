#include "tickstream/sql.hpp"

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace sql = tickstream::sql;

namespace {
std::size_t count(const std::string& haystack, const std::string& needle) {
    std::size_t n = 0;
    for (auto pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + 1)) {
        ++n;
    }
    return n;
}
}  // namespace

TEST(Sql, SingleRowInsert) {
    EXPECT_EQ(sql::insert_values(1),
              "INSERT INTO trades (symbol, trade_id, price, quantity, event_time, received_at, "
              "is_buyer_maker) VALUES ($1, $2, $3::numeric, $4::numeric, "
              "'epoch'::timestamptz + $5::bigint * interval '1 millisecond', "
              "'epoch'::timestamptz + $6::bigint * interval '1 millisecond', $7) "
              "ON CONFLICT DO NOTHING");
}

TEST(Sql, MultiRowPlaceholdersContinueAcrossRows) {
    const std::string s = sql::insert_values(3);
    EXPECT_EQ(count(s, "), ("), 2u);  // 3 tuples
    EXPECT_NE(s.find("($8, $9, $10::numeric"), std::string::npos);
    EXPECT_NE(s.find("$21)"), std::string::npos);   // last placeholder = 3 * 7
    EXPECT_EQ(s.find("$22"), std::string::npos);
    EXPECT_EQ(count(s, "ON CONFLICT DO NOTHING"), 1u);
}

TEST(Sql, RejectsZeroAndTooManyRows) {
    EXPECT_THROW(sql::insert_values(0), std::invalid_argument);
    EXPECT_NO_THROW(sql::insert_values(sql::kMaxRowsPerInsert));
    EXPECT_THROW(sql::insert_values(sql::kMaxRowsPerInsert + 1), std::invalid_argument);
}

TEST(Sql, StagingInsertIsIdempotent) {
    EXPECT_NE(sql::insert_from_staging().find("ON CONFLICT DO NOTHING"), std::string::npos);
    EXPECT_NE(sql::create_staging_table().find("ON COMMIT DELETE ROWS"), std::string::npos);
}
