#pragma once

#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "common/error/error_code.hpp"

namespace duckemu {

struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string message;
};

// Result<T>: either a T or an Error. No exceptions cross an API boundary.
template <typename T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
    Result(Error error) : storage_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept {
        return std::holds_alternative<T>(storage_);
    }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] T& value() & { return std::get<T>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(storage_)); }

    [[nodiscard]] const Error& error() const& { return std::get<Error>(storage_); }

private:
    std::variant<T, Error> storage_;
};

// For call sites that do not care whether they produced a value.
template <>
class Result<void> {
public:
    Result() noexcept = default;
    Result(Error error) : error_(std::move(error)) {}        // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const Error& error() const& { return *error_; }

private:
    std::optional<Error> error_;
};

} // namespace duckemu
