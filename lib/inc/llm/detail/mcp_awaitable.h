#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace mcp {

template <typename T> class awaitable;

namespace detail {
struct StopToken {};
}

namespace this_coro {
/**
 * @brief Await to obtain the current task's cooperative cancellation token.
 */
inline constexpr detail::StopToken stop_token{};
}

namespace detail {

template <typename T> struct CoroutineResult {
    std::optional<T> value;
    void return_value(T _value) { value.emplace(std::move(_value)); }
    T take() { return std::move(*value); }
};

template <> struct CoroutineResult<void> {
    void return_void() noexcept {}
    void take() noexcept {}
};

struct ReadyStopToken {
    std::stop_token stop;
    bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    std::stop_token await_resume() const noexcept { return stop; }
};

template <typename T> struct AwaitablePromise : CoroutineResult<T> {
    std::stop_token stop;
    std::coroutine_handle<> continuation = std::noop_coroutine();
    std::exception_ptr error;

    awaitable<T> get_return_object() noexcept {
        return awaitable<T>(
            std::coroutine_handle<AwaitablePromise>::from_promise(*this));
    }
    std::suspend_always initial_suspend() const noexcept { return {}; }
    auto final_suspend() const noexcept {
        struct ResumeParent {
            bool await_ready() const noexcept { return false; }
            std::coroutine_handle<>
            await_suspend(std::coroutine_handle<AwaitablePromise> _handle)
                const noexcept {
                return _handle.promise().continuation;
            }
            void await_resume() const noexcept {}
        };
        return ResumeParent{};
    }
    void unhandled_exception() noexcept { error = std::current_exception(); }

    ReadyStopToken await_transform(StopToken) const noexcept { return {stop}; }
    template <typename A> A &&await_transform(A &&_awaiter) const noexcept {
        return std::forward<A>(_awaiter);
    }

    T result() {
        if (error) {
            std::rethrow_exception(error);
        }
        return this->take();
    }
};

}

/**
 * @brief Owns a lazy coroutine returned by a tool or another coroutine.
 * @tparam T Result type, or void for a coroutine with no result.
 * @details Awaiters determine when and where the coroutine resumes.
 */
template <typename T> class awaitable {
public:
    using promise_type = detail::AwaitablePromise<T>;

    /**
     * @brief Transfers ownership of a coroutine frame.
     * @param _other The awaitable to move from.
     */
    awaitable(awaitable &&_other) noexcept
        : m_handle(std::exchange(_other.m_handle, {})) {}
    /**
     * @brief Coroutine ownership is move-only.
     */
    awaitable(const awaitable &) = delete;
    /**
     * @brief Copy assignment is disabled.
     */
    awaitable &operator=(const awaitable &) = delete;
    /**
     * @brief Destroys the coroutine frame if this awaitable still owns it.
     */
    ~awaitable() {
        if (m_handle) {
            m_handle.destroy();
        }
    }

    /**
     * @brief Transfers the coroutine to an awaiter for co_await.
     * @throws std::invalid_argument If the awaitable has no coroutine frame.
     */
    auto operator co_await() && {
        if (!m_handle) {
            throw std::invalid_argument("Empty MCP awaitable");
        }
        return ChildAwaiter(std::exchange(m_handle, {}));
    }

private:
    friend promise_type;

    using Handle = std::coroutine_handle<promise_type>;

    struct ChildAwaiter {
        explicit ChildAwaiter(Handle _handle) noexcept : handle(_handle) {}
        ChildAwaiter(const ChildAwaiter &) = delete;
        ~ChildAwaiter() { handle.destroy(); }

        bool await_ready() const noexcept { return false; }
        template <typename Parent>
        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<Parent> _parent) noexcept {
            auto &child = handle.promise();
            if constexpr (requires { child.stop = _parent.promise().stop; }) {
                child.stop = _parent.promise().stop;
            }
            child.continuation = _parent;
            return handle;
        }
        T await_resume() { return handle.promise().result(); }

        Handle handle;
    };

    explicit awaitable(Handle _handle) noexcept : m_handle(_handle) {}
    Handle m_handle;
};

namespace detail {

struct StartedCoroutine {
    struct promise_type {
        std::stop_token stop;

        template <typename Work, typename Complete>
        promise_type(Work &, const std::stop_token &_stop, Complete &) noexcept
            : stop(_stop) {}

        StartedCoroutine get_return_object() const noexcept { return {}; }
        std::suspend_never initial_suspend() const noexcept { return {}; }
        std::suspend_never final_suspend() const noexcept { return {}; }
        void return_void() const noexcept {}
        void unhandled_exception() const noexcept { std::terminate(); }
    };
};

template <typename T, typename Complete>
StartedCoroutine start_coroutine(awaitable<T> _work,
                                 [[maybe_unused]] std::stop_token _stop,
                                 Complete _complete) {
    std::optional<T> result;
    std::exception_ptr error;
    try {
        result.emplace(co_await std::move(_work));
    } catch (...) {
        error = std::current_exception();
    }
    _complete(error, result ? std::move(*result) : T{});
}

}
}
