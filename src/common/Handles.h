#pragma once
// RAII 句柄封装。禁止拷贝，允许移动。
#include <windows.h>
#include <utility>

namespace ul {

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) noexcept : h_(h) {}
    ~Handle() { reset(); }

    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& o) noexcept : h_(o.release()) {}
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) { reset(o.release()); }
        return *this;
    }

    HANDLE get() const noexcept { return h_; }
    HANDLE release() noexcept { HANDLE t = h_; h_ = nullptr; return t; }
    void reset(HANDLE h = nullptr) noexcept {
        if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = h;
    }
    bool valid() const noexcept { return h_ && h_ != INVALID_HANDLE_VALUE; }
    explicit operator bool() const noexcept { return valid(); }

private:
    HANDLE h_ = nullptr;
};

class FindHandle {
public:
    FindHandle() = default;
    explicit FindHandle(HANDLE h) noexcept : h_(h) {}
    ~FindHandle() { if (h_ && h_ != INVALID_HANDLE_VALUE) FindClose(h_); }
    FindHandle(const FindHandle&)            = delete;
    FindHandle& operator=(const FindHandle&) = delete;
    FindHandle(FindHandle&& o) noexcept : h_(o.release()) {}
    FindHandle& operator=(FindHandle&& o) noexcept { if (this != &o) { reset(o.release()); } return *this; }
    HANDLE get() const noexcept { return h_; }
    HANDLE release() noexcept { HANDLE t = h_; h_ = INVALID_HANDLE_VALUE; return t; }
    void reset(HANDLE h = INVALID_HANDLE_VALUE) noexcept {
        if (h_ && h_ != INVALID_HANDLE_VALUE) FindClose(h_);
        h_ = h;
    }
    bool valid() const noexcept { return h_ && h_ != INVALID_HANDLE_VALUE; }
    explicit operator bool() const noexcept { return valid(); }
private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

}  // namespace ul
