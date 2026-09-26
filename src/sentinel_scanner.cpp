#include "core/sentinel_scanner.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

SentinelScanner::SentinelScanner(std::string sentinel)
    : sentinel_(std::move(sentinel)) {
    if (sentinel_.empty()) {
        throw std::invalid_argument("SentinelScanner: sentinel must be non-empty");
    }
}

std::size_t SentinelScanner::held_back_length(std::string_view text) const noexcept {
    const std::string_view sentinel(sentinel_);
    // Full matches are handled by feed(); keep only a proper prefix.
    const std::size_t max_k = std::min(text.size(), sentinel.size() - 1);
    for (std::size_t k = max_k; k > 0; --k) {
        const std::size_t start = text.size() - k;
        if (text[start] != sentinel[0]) continue;
        if (text.substr(start) == sentinel.substr(0, k)) return k;
    }
    return 0;
}

SentinelScanner::Out SentinelScanner::feed(std::string_view chunk) {
    if (found_) return {"", true};
    if (chunk.empty()) return {"", false};

    std::string window = pending_;
    window.append(chunk);

    const std::size_t pos = window.find(sentinel_);
    if (pos != std::string::npos) {
        found_ = true;
        pending_.clear();
        window.resize(pos);
        return {std::move(window), true};
    }

    const std::size_t keep = held_back_length(window);
    pending_.assign(window, window.size() - keep, keep);
    window.resize(window.size() - keep);
    return {std::move(window), false};
}

SentinelScanner::Out SentinelScanner::flush() {
    if (found_) return {"", true};
    // At EOF, an incomplete sentinel is ordinary text.
    Out out{std::move(pending_), false};
    pending_.clear();
    return out;
}
