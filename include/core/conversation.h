#pragma once

#include "core/message.h"
#include <cstddef>

class Conversation {
public:
    Conversation() = default;
    ~Conversation();

    Conversation(const Conversation& other);
    Conversation& operator=(const Conversation& other);
    Conversation(Conversation&& other) noexcept;
    Conversation& operator=(Conversation&& other) noexcept;

    // A late System message goes first; a second one is rejected.
    void append(Message msg);

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return capacity_; }
    const Message& at(std::size_t i) const;

    const Message* begin() const noexcept { return data_; }
    const Message* end() const noexcept { return size_ ? data_ + size_ : data_; }

private:
    void grow();
    void swap(Conversation& other) noexcept;

    Message* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
};
