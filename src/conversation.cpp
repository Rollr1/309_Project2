#include "core/conversation.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

static_assert(std::is_nothrow_move_assignable_v<Message>,
              "Message move-assignment must be noexcept");

Conversation::~Conversation() {
    delete[] data_;
}

Conversation::Conversation(const Conversation& other) {
    if (other.size_ == 0) return;
    Message* buf = new Message[other.size_];
    // If copying throws, this object never reaches its destructor.
    try {
        for (std::size_t i = 0; i < other.size_; ++i) buf[i] = other.data_[i];
    } catch (...) {
        delete[] buf;
        throw;
    }
    data_ = buf;
    size_ = other.size_;
    capacity_ = other.size_;
}

Conversation& Conversation::operator=(const Conversation& other) {
    if (this == &other) return *this;

    Conversation tmp(other);
    swap(tmp);
    return *this;
}

Conversation::Conversation(Conversation&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      capacity_(std::exchange(other.capacity_, 0)) {}

Conversation& Conversation::operator=(Conversation&& other) noexcept {
    if (this == &other) return *this;

    delete[] data_;
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
    capacity_ = std::exchange(other.capacity_, 0);
    return *this;
}

void Conversation::swap(Conversation& other) noexcept {
    std::swap(data_, other.data_);
    std::swap(size_, other.size_);
    std::swap(capacity_, other.capacity_);
}

void Conversation::grow() {
    const auto max_slots = std::numeric_limits<std::size_t>::max() / sizeof(Message);
    if (capacity_ > max_slots / 2) {
        throw std::length_error("Conversation capacity is too large");
    }

    const std::size_t next_cap = capacity_ == 0 ? 1 : capacity_ * 2;
    Message* buf = new Message[next_cap];
    for (std::size_t i = 0; i < size_; ++i) buf[i] = std::move(data_[i]);

    delete[] data_;
    data_ = buf;
    capacity_ = next_cap;
}

// Take a copy before growing: msg may come from this conversation.
void Conversation::append(Message msg) {
    const bool pin = msg.role() == Role::System && size_ > 0;
    if (pin && data_[0].role() == Role::System) {
        throw std::logic_error(
            "Conversation::append: a System message is already pinned at index 0");
    }

    if (size_ == capacity_) grow();

    if (pin) {
        // Shift backward so unread messages are not overwritten.
        for (std::size_t i = size_; i > 0; --i) data_[i] = std::move(data_[i - 1]);
        data_[0] = std::move(msg);
    } else {
        data_[size_] = std::move(msg);
    }
    ++size_;
}

const Message& Conversation::at(std::size_t i) const {
    if (i >= size_) {
        throw std::out_of_range("Conversation::at: index " + std::to_string(i) +
                                " out of range (size " + std::to_string(size_) + ")");
    }
    return data_[i];
}
