#include "tickstream/binance.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>

#include <nlohmann/json.hpp>

namespace tickstream::binance {

std::string combined_trade_stream_target(const std::vector<std::string>& symbols) {
    std::string target = "/stream?streams=";
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) {
            target += '/';
        }
        std::string symbol = symbols[i];
        std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        target += symbol + "@trade";
    }
    return target;
}

// SAX since Phase 6: 2,408 vs 4,507 ns and 7 vs 31 heap allocations per
// message (bench/results/phase6-micro.txt), identical output on the real
// capture (tests/parser_equivalence_test.cpp).
std::optional<Trade> parse_trade(std::string_view message) {
    return parse_trade_sax(message);
}

std::optional<Trade> parse_trade_dom(std::string_view message) {
    using nlohmann::json;

    // All nlohmann errors (parse_error, type_error, out_of_range) derive from
    // json::exception. Malformed messages are rare, so using exceptions here
    // keeps the happy path readable at no real cost; we convert them to
    // nullopt at this boundary so nothing escapes into the network code.
    try {
        const json doc = json::parse(message);

        // Combined streams wrap the payload: {"stream": "...", "data": {...}}.
        const json& data = doc.contains("data") ? doc.at("data") : doc;

        if (data.at("e").get<std::string>() != "trade") {
            return std::nullopt;
        }

        Trade trade;
        trade.symbol = data.at("s").get<std::string>();
        trade.price = data.at("p").get<std::string>();
        trade.quantity = data.at("q").get<std::string>();
        trade.trade_id = data.at("t").get<std::int64_t>();
        trade.trade_time_ms = data.at("T").get<std::int64_t>();
        trade.is_buyer_maker = data.at("m").get<bool>();
        return trade;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

namespace {

// The fields of a trade message we care about. Keys are mapped to this enum
// once, so the handler never stores key strings.
enum class Field : std::uint8_t { other, e, s, p, q, t, T, m, data };

Field field_of(std::string_view key) {
    if (key.size() == 1) {
        switch (key[0]) {
            case 'e': return Field::e;
            case 's': return Field::s;
            case 'p': return Field::p;
            case 'q': return Field::q;
            case 't': return Field::t;
            case 'T': return Field::T;
            case 'm': return Field::m;
            default: return Field::other;
        }
    }
    return key == "data" ? Field::data : Field::other;
}

constexpr unsigned bit(Field f) { return 1U << static_cast<unsigned>(f); }
constexpr unsigned kAllFields = bit(Field::e) | bit(Field::s) | bit(Field::p) | bit(Field::q) |
                                bit(Field::t) | bit(Field::T) | bit(Field::m);

// The trade fields found in one JSON object (the top level, or "data").
struct Partial {
    Trade trade;
    unsigned seen = 0;
    bool is_trade_event = false;
    bool bad = false;  // a field we need had the wrong type

    [[nodiscard]] bool complete() const {
        return !bad && is_trade_event && seen == kAllFields;
    }
};

// Collects trade fields from either the raw payload ({"e":"trade",...}) or
// the combined-stream envelope ({"stream":...,"data":{...}}). Values nested
// deeper than that are skipped. Mirrors parse_trade_dom(): when the top level
// has "data", only "data" counts.
class TradeSax final : public nlohmann::json_sax<nlohmann::json> {
public:
    bool null() override { return wrong_type(); }
    bool boolean(bool value) override {
        if (Partial* p = target(); p != nullptr && field_ == Field::m) {
            p->trade.is_buyer_maker = value;
            p->seen |= bit(Field::m);
            return true;
        }
        return wrong_type();
    }
    bool number_integer(number_integer_t value) override { return integer(value); }
    bool number_unsigned(number_unsigned_t value) override {
        if (value > static_cast<number_unsigned_t>(std::numeric_limits<std::int64_t>::max())) {
            return wrong_type();
        }
        return integer(static_cast<std::int64_t>(value));
    }
    bool number_float(number_float_t, const string_t&) override { return wrong_type(); }
    bool string(string_t& value) override {
        Partial* p = target();
        if (p == nullptr) {
            return wrong_type();
        }
        switch (field_) {
            case Field::e: p->is_trade_event = (value == "trade"); break;
            case Field::s: p->trade.symbol = value; break;
            case Field::p: p->trade.price = value; break;
            case Field::q: p->trade.quantity = value; break;
            default: return wrong_type();
        }
        p->seen |= bit(field_);
        return true;
    }
    bool binary(binary_t&) override { return wrong_type(); }

    bool start_object(std::size_t) override {
        if (depth_ == 0) {
            root_is_object_ = true;
        } else if (depth_ == 1 && top_field_ == Field::data) {
            data_is_object_ = true;
            in_data_ = true;
        } else {
            wrong_type();  // an object where we need a scalar
        }
        ++depth_;
        return true;
    }
    bool end_object() override {
        --depth_;
        if (depth_ == 1) {
            in_data_ = false;
        }
        return true;
    }
    bool start_array(std::size_t) override {
        if (depth_ > 0) {
            wrong_type();
        }
        ++depth_;
        return true;
    }
    bool end_array() override {
        --depth_;
        return true;
    }
    bool key(string_t& key) override {
        if (depth_ == 1) {
            top_field_ = field_of(key);
            data_seen_ = data_seen_ || top_field_ == Field::data;
        } else if (depth_ == 2 && in_data_) {
            data_field_ = field_of(key);
        }
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;  // malformed JSON: stop, sax_parse returns false
    }

    std::optional<Trade> result() {
        if (!root_is_object_) {
            return std::nullopt;
        }
        Partial& chosen = data_seen_ ? data_ : top_;
        if (data_seen_ && !data_is_object_) {
            return std::nullopt;
        }
        if (!chosen.complete()) {
            return std::nullopt;
        }
        return std::move(chosen.trade);
    }

private:
    // The object the current value belongs to, with its key in field_, or
    // nullptr if the value is somewhere we don't read.
    Partial* target() {
        if (depth_ == 1) {
            field_ = top_field_;
            return &top_;
        }
        if (depth_ == 2 && in_data_) {
            field_ = data_field_;
            return &data_;
        }
        return nullptr;
    }

    bool integer(std::int64_t value) {
        if (Partial* p = target(); p != nullptr && (field_ == Field::t || field_ == Field::T)) {
            (field_ == Field::t ? p->trade.trade_id : p->trade.trade_time_ms) = value;
            p->seen |= bit(field_);
            return true;
        }
        return wrong_type();
    }

    // A value of the wrong type (or a container) for a field we need marks
    // its object bad. Anything else (unknown keys, deeper levels) is ignored.
    bool wrong_type() {
        if (Partial* p = target(); p != nullptr && field_ != Field::other) {
            if (field_ != Field::data) {
                p->bad = true;
            }
        }
        return true;  // keep parsing: the other object may still be valid
    }

    Partial top_;
    Partial data_;
    Field field_ = Field::other;       // key of the value being handled
    Field top_field_ = Field::other;   // last key at depth 1
    Field data_field_ = Field::other;  // last key inside "data"
    int depth_ = 0;
    bool root_is_object_ = false;
    bool data_seen_ = false;
    bool data_is_object_ = false;
    bool in_data_ = false;
};

}  // namespace

std::optional<Trade> parse_trade_sax(std::string_view message) {
    TradeSax handler;
    try {
        if (!nlohmann::json::sax_parse(message.begin(), message.end(), &handler)) {
            return std::nullopt;
        }
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    return handler.result();
}

}  // namespace tickstream::binance
