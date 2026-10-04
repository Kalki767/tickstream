#include "tickstream/gap_detector.hpp"

namespace tickstream {

std::optional<Gap> GapDetector::observe(std::string_view symbol, std::int64_t trade_id) {
    const auto it = last_id_.find(symbol);
    if (it == last_id_.end()) {
        last_id_.emplace(std::string(symbol), trade_id);
        return std::nullopt;
    }

    std::int64_t& last = it->second;
    if (trade_id <= last) {
        ++out_of_order_;
        return std::nullopt;
    }

    std::optional<Gap> gap;
    if (trade_id > last + 1) {
        gap = Gap{std::string(symbol), last, trade_id};
        ++gaps_;
        missing_trades_ += gap->missing();
    }
    last = trade_id;
    return gap;
}

}  // namespace tickstream
