#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "endgate/object.h"

namespace endgate {
template <class T>
class Nullable;

/**
 * @see endstone::NotNull
 */
template <class T>
class NotNull {
public:
    using pointer_type = std::shared_ptr<T>;
    using element_type = T;

    NotNull() = delete;
    NotNull(std::shared_ptr<T> ptr) : ptr_(std::move(ptr)) {}
    NotNull(const NotNull &other) = default;
    NotNull &operator=(const NotNull &other) = default;

    template <class U>
        requires(!std::is_same_v<U, T>) && std::is_convertible_v<U *, T *>
    NotNull(std::shared_ptr<U> ptr) : NotNull(std::shared_ptr<T>(std::move(ptr)))
    {
    }

    template <class U>
        requires(!std::is_same_v<U, T>) && std::is_convertible_v<U *, T *>
    NotNull(const NotNull<U> &other) : ptr_(other.get())
    {
    }

    NotNull(const Nullable<T> &other);

    const pointer_type &get() const noexcept { return ptr_; }
    T *operator->() const noexcept { return ptr_.get(); }
    T &operator*() const noexcept { return *get(); }

    template <class U>
        requires std::derived_from<T, Object> && std::derived_from<U, Object>
    [[nodiscard]] bool is() const
    {
        return ptr_->template is<U>();
    }

    template <class U>
        requires std::derived_from<T, Object> && std::derived_from<U, Object>
    [[nodiscard]] Nullable<U> as() const
    {
        if (auto *raw = ptr_->template as<U>()) {
            return std::shared_ptr<U>(ptr_, raw);
        }
        return nullptr;
    }

    template <class U>
    [[nodiscard]] NotNull<U> cast() const
    {
        return std::static_pointer_cast<U>(ptr_);
    }

    NotNull(std::nullptr_t) = delete;
    NotNull &operator=(std::nullptr_t) = delete;
    NotNull &operator++() = delete;
    NotNull &operator--() = delete;
    NotNull operator++(int) = delete;
    NotNull operator--(int) = delete;
    NotNull &operator+=(std::ptrdiff_t) = delete;
    NotNull &operator-=(std::ptrdiff_t) = delete;
    void operator[](std::ptrdiff_t) const = delete;
    void swap(NotNull &other) noexcept { std::swap(ptr_, other.ptr_); }

private:
    pointer_type ptr_;
};

/**
 * @see endstone::Nullable
 */
template <class T>
class Nullable {
public:
    using pointer_type = std::shared_ptr<T>;
    using element_type = T;

    constexpr Nullable() noexcept = default;
    constexpr Nullable(std::nullptr_t) noexcept {}
    Nullable(std::shared_ptr<T> ptr) : ptr_(std::move(ptr)) {}
    Nullable(const NotNull<T> &other) : ptr_(other.get()) {}

    template <class U>
        requires(!std::is_same_v<U, T>) && std::is_convertible_v<U *, T *>
    Nullable(std::shared_ptr<U> ptr) : ptr_(std::move(ptr))
    {
    }

    template <class U>
        requires(!std::is_same_v<U, T>) && std::is_convertible_v<U *, T *>
    Nullable(const Nullable<U> &other) : ptr_(other.get())
    {
    }

    template <class U>
        requires(!std::is_same_v<U, T>) && std::is_convertible_v<U *, T *>
    Nullable(const NotNull<U> &other) : ptr_(other.get())
    {
    }

    const pointer_type &get() const noexcept { return ptr_; }
    T *operator->() const noexcept { return ptr_.get(); }
    T &operator*() const noexcept { return *get(); }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    template <class U>
        requires std::derived_from<T, Object> && std::derived_from<U, Object>
    [[nodiscard]] bool is() const
    {
        return ptr_ != nullptr && ptr_->template is<U>();
    }

    template <class U>
        requires std::derived_from<T, Object> && std::derived_from<U, Object>
    [[nodiscard]] Nullable<U> as() const
    {
        if (ptr_) {
            if (auto *raw = ptr_->template as<U>()) {
                return std::shared_ptr<U>(ptr_, raw);
            }
        }
        return nullptr;
    }

    template <class U>
    [[nodiscard]] Nullable<U> cast() const
    {
        return std::static_pointer_cast<U>(ptr_);
    }

    T &value() const
    {
        if (ptr_ == nullptr) {
            throw std::runtime_error("Nullable holds no value.");
        }
        return *ptr_;
    }

    pointer_type value_or(pointer_type default_value) const { return ptr_ ? ptr_ : std::move(default_value); }

    bool operator==(std::nullptr_t) const noexcept { return ptr_ == nullptr; }

private:
    std::shared_ptr<T> ptr_;
};

template <class T>
NotNull<T>::NotNull(const Nullable<T> &other) : NotNull(other.get())
{
}

#define ENDGATE_DEFINE_PTR_COMPARISONS(Wrapper)                                    \
    template <class T, class U>                                                    \
    bool operator==(const Wrapper<T> &lhs, const Wrapper<U> &rhs) noexcept         \
    {                                                                              \
        return lhs.get() == rhs.get();                                             \
    }                                                                              \
    template <class T, class U>                                                    \
    bool operator!=(const Wrapper<T> &lhs, const Wrapper<U> &rhs) noexcept         \
    {                                                                              \
        return lhs.get() != rhs.get();                                             \
    }                                                                              \
    template <class T, class U>                                                    \
    bool operator<(const Wrapper<T> &lhs, const Wrapper<U> &rhs) noexcept          \
    {                                                                              \
        return lhs.get() < rhs.get();                                              \
    }                                                                              \
    template <class T, class U>                                                    \
    bool operator<=(const Wrapper<T> &lhs, const Wrapper<U> &rhs) noexcept         \
    {                                                                              \
        return lhs.get() <= rhs.get();                                             \
    }                                                                              \
    template <class T, class U>                                                    \
    bool operator>(const Wrapper<T> &lhs, const Wrapper<U> &rhs) noexcept          \
    {                                                                              \
        return lhs.get() > rhs.get();                                              \
    }                                                                              \
    template <class T, class U>                                                    \
    bool operator>=(const Wrapper<T> &lhs, const Wrapper<U> &rhs) noexcept         \
    {                                                                              \
        return lhs.get() >= rhs.get();                                             \
    }                                                                              \
    template <class T, class U>                                                    \
    bool operator==(const Wrapper<T> &lhs, const std::shared_ptr<U> &rhs) noexcept \
    {                                                                              \
        return lhs.get() == rhs;                                                   \
    }

ENDGATE_DEFINE_PTR_COMPARISONS(NotNull)
ENDGATE_DEFINE_PTR_COMPARISONS(Nullable)
#undef ENDGATE_DEFINE_PTR_COMPARISONS

template <class T, class U>
bool operator==(const NotNull<T> &lhs, const Nullable<U> &rhs) noexcept
{
    return lhs.get() == rhs.get();
}

template <class T, class U>
bool operator==(const Nullable<T> &lhs, const NotNull<U> &rhs) noexcept
{
    return lhs.get() == rhs.get();
}
}  // namespace endgate

template <class T>
struct std::hash<endgate::NotNull<T>> {
    std::size_t operator()(const endgate::NotNull<T> &value) const noexcept
    {
        return std::hash<std::shared_ptr<T>>{}(value.get());
    }
};

template <class T>
struct std::hash<endgate::Nullable<T>> {
    std::size_t operator()(const endgate::Nullable<T> &value) const noexcept
    {
        return std::hash<std::shared_ptr<T>>{}(value.get());
    }
};
