#pragma once

#include "mcp_awaitable.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace mcp::detail {

inline constexpr std::string_view kTasksExtension =
    "io.modelcontextprotocol/tasks";

struct TaskUnavailable : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class TaskRuntime {
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;
    using WallClock = std::chrono::system_clock;
    using BlockingWork = std::function<Json(std::stop_token)>;
    using Job = std::variant<BlockingWork, awaitable<Json>>;

    static constexpr auto kRetention = std::chrono::minutes(1);
    static constexpr auto kAdmissionTimeout = std::chrono::milliseconds(500);
    static constexpr std::size_t kMaxInFlight = 32;
    static constexpr std::size_t kMaxRecords = 1024;
    static constexpr std::size_t kMaxWaiters = 2;
    static constexpr std::size_t kMaxResultBytes = 128 * 1024;
    enum class Status { working, completed, failed, cancelled };

    struct Task {
        std::string id;
        const Clock::time_point started = Clock::now();
        const WallClock::time_point created = WallClock::now();
        WallClock::time_point updated = created;
        std::optional<Clock::time_point> expires;
        Status status = Status::working;
        bool running = false;
        std::stop_source stop;
        bool cancel_requested = false;
        std::optional<Job> job;
        std::optional<std::promise<Json>> reply;
        std::string payload;
    };

public:
    explicit TaskRuntime(std::size_t _workers, bool _stdio = false)
        : m_worker_count(std::min(_workers, kMaxInFlight)), m_stdio(_stdio) {}

    ~TaskRuntime() {
        for (auto &stop : mark_stopping()) {
            stop.request_stop();
        }
        m_workers.clear();
        std::unique_lock lock(m_mutex);
        m_admission.wait(lock, [this] { return m_in_flight == 0; });
    }

    std::vector<std::stop_source> mark_stopping() {
        std::lock_guard lock(m_mutex);
        if (m_stopping) {
            return {};
        }
        m_stopping = true;
        m_admission.notify_all();
        m_work_ready.notify_all();
        std::vector<std::stop_source> stops;
        for (auto it = m_tasks.begin(); it != m_tasks.end();) {
            const auto task = (it++)->second;
            if (auto stop = cancel_locked(task)) {
                stops.push_back(std::move(*stop));
            }
        }
        return stops;
    }

    Json run(Job _work) {
        std::promise<Json> reply;
        auto result = reply.get_future();
        submit(std::move(_work), std::move(reply));
        return result.get();
    }

    Json submit(Job _work, std::optional<std::promise<Json>> _reply = {}) {
        std::unique_lock lock(m_mutex);
        await_admission(lock, m_stdio && _reply.has_value());
        while (m_workers.size() < m_worker_count) {
            m_workers.emplace_back([this] { worker(); });
        }
        auto task = std::make_shared<Task>();
        task->reply = std::move(_reply);
        task->job.emplace(std::move(_work));
        return submit_locked(task);
    }

    Json get(const std::string &_id) {
        Json result;
        std::string payload;
        {
            std::lock_guard lock(m_mutex);
            prune_locked();
            const auto it = m_tasks.find(_id);
            if (it == m_tasks.end()) {
                return {};
            }
            result = snapshot(*it->second, false);
            payload = it->second->payload;
        }
        if (!payload.empty()) {
            result[result["status"] == "completed" ? "result" : "error"] =
                Json::parse(payload);
        }
        return result;
    }

    bool contains(const std::string &_id) {
        std::lock_guard lock(m_mutex);
        prune_locked();
        return m_tasks.contains(_id);
    }

    bool cancel(const std::string &_id) {
        std::optional<std::stop_source> stop;
        {
            std::lock_guard lock(m_mutex);
            prune_locked();
            const auto it = m_tasks.find(_id);
            if (it == m_tasks.end()) {
                return false;
            }
            const auto task = it->second;
            stop = cancel_locked(task);
        }
        if (stop) {
            stop->request_stop();
        }
        return true;
    }

private:
    void worker() {
        for (;;) {
            std::shared_ptr<Task> task;
            {
                std::unique_lock lock(m_mutex);
                m_work_ready.wait(
                    lock, [this] { return m_stopping || !m_pending.empty(); });
                if (m_stopping) {
                    return;
                }
                task = std::move(m_pending.front());
                m_pending.pop_front();
                task->running = true;
                ++m_active_workers;
            }
            auto job = std::move(*task->job);
            task->job.reset();
            execute(task, job);
            std::lock_guard lock(m_mutex);
            --m_active_workers;
            m_admission.notify_all();
        }
    }

    void execute(const std::shared_ptr<Task> &_task, Job &_job) {
        if (auto *blocking = std::get_if<BlockingWork>(&_job)) {
            Json result;
            std::exception_ptr error;
            try {
                result = (*blocking)(_task->stop.get_token());
            } catch (...) {
                error = std::current_exception();
            }
            complete(_task, error, std::move(result));
            return;
        }
        try {
            start_coroutine(
                std::get<awaitable<Json>>(std::move(_job)),
                _task->stop.get_token(),
                [this, task = _task](std::exception_ptr _error, Json _result) {
                    complete(task, _error, std::move(_result));
                });
        } catch (...) {
            complete(_task, std::current_exception(), {});
        }
    }

    void await_admission(std::unique_lock<std::mutex> &_lock,
                         bool _reserve_worker) {
        prune_locked();
        const auto has_capacity = [this, _reserve_worker] {
            return m_in_flight < kMaxInFlight && m_tasks.size() < kMaxRecords &&
                   (!_reserve_worker ||
                    m_active_workers + m_pending.size() < m_worker_count);
        };
        if (!m_stopping && !has_capacity()) {
            if (m_waiters == kMaxWaiters) {
                throw TaskUnavailable("MCP task admission is busy");
            }
            ++m_waiters;
            const auto deadline = Clock::now() + kAdmissionTimeout;
            while (!m_stopping && !has_capacity() && Clock::now() < deadline) {
                const auto next =
                    m_finished.empty()
                        ? deadline
                        : std::min(deadline, *m_finished.front()->expires);
                m_admission.wait_until(_lock, next);
                prune_locked();
            }
            --m_waiters;
        }
        if (m_stopping) {
            throw TaskUnavailable("MCP task runtime is stopping");
        }
        if (!has_capacity()) {
            throw TaskUnavailable(
                _reserve_worker ? "Timed out waiting for a task worker"
                                : "Timed out waiting for MCP task capacity");
        }
    }

    void release_slot_locked() {
        --m_in_flight;
        m_admission.notify_all();
    }

    std::string new_task_id_locked() const {
        std::string id;
        do {
            id.clear();
            std::random_device random;
            constexpr std::string_view hex = "0123456789abcdef";
            for (int i = 0; i < 4; ++i) {
                auto word = random();
                for (int digit = 0; digit < 8; ++digit) {
                    id += hex[word & 15];
                    word >>= 4;
                }
            }
        } while (m_tasks.contains(id));
        return id;
    }

    Json submit_locked(const std::shared_ptr<Task> &_task) {
        prune_locked();
        const std::string id = new_task_id_locked();
        _task->id = id;
        Json result = _task->reply ? Json() : snapshot(*_task, true);
        m_tasks.emplace(id, _task);
        try {
            m_pending.push_back(_task);
        } catch (...) {
            m_tasks.erase(id);
            throw;
        }
        ++m_in_flight;
        m_work_ready.notify_one();
        return result;
    }

    static std::string timestamp(WallClock::time_point _time) {
        return std::format(
            "{:%FT%T}Z", std::chrono::floor<std::chrono::milliseconds>(_time));
    }

    static Json snapshot(const Task &_task, bool _created) {
        Json result{{"resultType", _created ? "task" : "complete"},
                    {"taskId", _task.id},
                    {"status", "working"},
                    {"createdAt", timestamp(_task.created)},
                    {"lastUpdatedAt", timestamp(_task.updated)},
                    {"ttlMs", nullptr},
                    {"pollIntervalMs", 250}};
        if (_task.expires) {
            result["ttlMs"] =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    *_task.expires - _task.started)
                    .count();
        }
        switch (_task.status) {
        case Status::working:
            break;
        case Status::completed:
            result["status"] = "completed";
            break;
        case Status::failed:
            result["status"] = "failed";
            break;
        case Status::cancelled:
            result["status"] = "cancelled";
            break;
        }
        return result;
    }

    void finish(const std::shared_ptr<Task> &_task, Status _status) {
        _task->updated = WallClock::now();
        _task->expires = Clock::now() + kRetention;
        _task->status = _status;
        m_finished.push_back(_task);
    }

    std::optional<std::stop_source>
    cancel_locked(const std::shared_ptr<Task> &_task) {
        if (_task->status != Status::working || _task->cancel_requested) {
            return {};
        }
        _task->cancel_requested = true;
        if (_task->running) {
            return _task->stop;
        }
        std::erase(m_pending, _task);
        release_slot_locked();
        _task->job.reset();
        if (_task->reply) {
            reply_direct_locked(_task, {}, {});
        } else {
            finish(_task, Status::cancelled);
        }
        return {};
    }

    void reply_direct_locked(const std::shared_ptr<Task> &_task,
                             std::exception_ptr _error, Json _result) {
        if (_task->cancel_requested) {
            _error = std::make_exception_ptr(
                TaskUnavailable("MCP server is stopping"));
        }
        if (_error) {
            _task->reply->set_exception(_error);
        } else {
            _task->reply->set_value(std::move(_result));
        }
        m_tasks.erase(_task->id);
    }

    static std::string encode_failure(std::exception_ptr _error) {
        std::string_view message = "Unknown task failure";
        try {
            std::rethrow_exception(_error);
        } catch (const std::exception &_e) {
            message = _e.what();
        } catch (...) {
        }
        return Json{{"code", -32603}, {"message", message.substr(0, 1024)}}
            .dump(-1, ' ', false, Json::error_handler_t::replace);
    }

    void complete(const std::shared_ptr<Task> &_task, std::exception_ptr _error,
                  Json _result) {
        if (_task->reply) {
            std::lock_guard lock(m_mutex);
            reply_direct_locked(_task, _error, std::move(_result));
            release_slot_locked();
            return;
        }
        std::string payload;
        if (!_error && !_task->stop.stop_requested()) {
            try {
                payload = _result.dump();
                if (payload.size() > kMaxResultBytes) {
                    throw std::length_error("MCP task result exceeds 128 KiB");
                }
            } catch (...) {
                _error = std::current_exception();
            }
        }
        if (_error && !_task->stop.stop_requested()) {
            std::string().swap(payload);
            payload = encode_failure(_error);
        }
        std::lock_guard lock(m_mutex);
        if (_task->cancel_requested) {
            finish(_task, Status::cancelled);
        } else {
            _task->payload = std::move(payload);
            finish(_task, _error ? Status::failed : Status::completed);
        }
        release_slot_locked();
    }

    void prune_locked() {
        const auto now = Clock::now();
        while (!m_finished.empty() && *m_finished.front()->expires <= now) {
            m_tasks.erase(m_finished.front()->id);
            m_finished.pop_front();
        }
    }

    const std::size_t m_worker_count;
    const bool m_stdio;
    std::mutex m_mutex;
    std::condition_variable m_admission;
    std::size_t m_in_flight = 0;
    std::size_t m_waiters = 0;
    bool m_stopping = false;
    std::unordered_map<std::string, std::shared_ptr<Task>> m_tasks;
    std::deque<std::shared_ptr<Task>> m_finished;
    std::condition_variable m_work_ready;
    std::deque<std::shared_ptr<Task>> m_pending;
    std::size_t m_active_workers = 0;
    std::vector<std::jthread> m_workers;
};

}
