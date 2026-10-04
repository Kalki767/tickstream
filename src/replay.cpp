#include "tickstream/replay.hpp"

#include <chrono>
#include <fstream>
#include <stdexcept>

#include "tickstream/binance.hpp"

namespace tickstream {

std::vector<std::string> load_capture(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open capture file '" + path + "'");
    }
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
    }
    return lines;
}

ReplayResult replay(const std::vector<std::string>& lines, int copies,
                    BoundedQueue<Trade>& queue) {
    using namespace std::chrono;
    ReplayResult result;
    for (int copy = 0; copy < copies; ++copy) {
        const std::int64_t id_offset = copy * kReplayIdOffset;
        for (const std::string& line : lines) {
            const auto received_steady = steady_clock::now();
            const auto received_wall = system_clock::now();
            ++result.messages;

            auto trade = binance::parse_trade(line);
            if (!trade) {
                ++result.unparseable;
                continue;
            }
            trade->trade_id += id_offset;
            trade->received_steady_ns =
                duration_cast<nanoseconds>(received_steady.time_since_epoch()).count();
            trade->received_wall_ms =
                duration_cast<milliseconds>(received_wall.time_since_epoch()).count();

            if (result.pushed == 0) {
                result.first_push_steady_ns = trade->received_steady_ns;
            }
            if (!queue.push(std::move(*trade))) {
                return result;  // queue closed: shutting down
            }
            ++result.pushed;
        }
    }
    return result;
}

}  // namespace tickstream
