#pragma once

#include <string>
#include <utility>

enum class Role { System, User, Assistant };

class Message {
public:
    Message() = default;

    Message(Role role, std::string content)
        : role_(role), content_(std::move(content)) {}

    Role role() const noexcept { return role_; }
    const std::string& content() const noexcept { return content_; }

private:
    Role role_ = Role::System;
    std::string content_;
};
