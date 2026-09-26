#pragma once

#include <cstddef>
#include <string>
#include <string_view>

class SentinelScanner {
public:
    explicit SentinelScanner(std::string sentinel);

    struct Out {
        std::string safe_text;
        bool sentinel_found;
    };

    Out feed(std::string_view chunk);
    Out flush();
    std::size_t pending_size() const noexcept { return pending_.size(); }

private:
    std::size_t held_back_length(std::string_view text) const noexcept;

    std::string sentinel_;
    // Longest suffix that could still complete the sentinel.
    std::string pending_;
    bool found_ = false;
};
