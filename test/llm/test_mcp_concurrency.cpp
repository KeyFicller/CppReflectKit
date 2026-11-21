#include <exception>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/thread_pool.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <coroutine>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "mcp_test_http.h"

namespace {

using namespace std::chrono_literals;
using mcp_test::json;

constexpr int kParallelReads = 4;
constexpr auto kWait = 3s;
constexpr auto kGateTimeout = 10s;

struct RunOn {
    asio::thread_pool &pool;

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> _continuation) const {
        asio::post(pool, [_continuation] { _continuation.resume(); });
    }
    void await_resume() const noexcept {}
};

class IoServer : public mcp::Server<IoServer> {
public:
    IoServer(std::filesystem::path _directory, int _upstream_port)
        : Server(2), m_directory(std::move(_directory)),
          m_origin("http://127.0.0.1:" + std::to_string(_upstream_port)) {}

    std::string_view server_name() const override { return "io-test"; }
    std::string_view server_version() const override { return "1"; }

    std::atomic<int> opened_files{0};
    std::promise<void> release_reads;

    [[= mcp::tool{}]] mcp::awaitable<std::string> read_file(int record) {
        co_await RunOn{m_io};
        std::ifstream file(m_directory / std::to_string(record),
                           std::ios::binary);
        if (!file) {
            throw std::runtime_error("could not open test file");
        }
        ++opened_files;
        if (m_read_gate.wait_for(kGateTimeout) != std::future_status::ready) {
            throw std::runtime_error("timed out waiting to read test file");
        }
        co_return std::string(std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>());
    }

    [[= mcp::tool{}]] mcp::awaitable<std::string> fetch_url(int record) {
        co_await mcp::this_coro::report_progress("waiting for HTTP");
        co_await RunOn{m_io};
        httplib::Client client(m_origin);
        client.set_connection_timeout(kWait);
        client.set_read_timeout(kGateTimeout);
        auto response = client.Get("/data?record=" + std::to_string(record));
        if (!response || response->status != 200) {
            throw std::runtime_error("could not fetch test URL");
        }
        co_return std::move(response->body);
    }

private:
    const std::filesystem::path m_directory;
    const std::string m_origin;
    std::shared_future<void> m_read_gate = release_reads.get_future().share();
    asio::thread_pool m_io{2 * kParallelReads};
};

using IoHttpServer = mcp_test::HttpServerOf<IoServer>;

class TimerServer : public mcp::Server<TimerServer> {
public:
    explicit TimerServer(asio::io_context &_timers)
        : Server(1), m_timers(_timers) {}

    std::string_view server_name() const override { return "timer-test"; }
    std::string_view server_version() const override { return "1"; }

    [[= mcp::tool{}]] mcp::awaitable<std::string> sleep_for(int seconds) {
        struct WaitForTimer {
            asio::steady_timer &timer;
            asio::error_code error{};

            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> _continuation) {
                timer.async_wait(
                    [this, _continuation](asio::error_code _error) {
                        error = _error;
                        _continuation.resume();
                    });
            }
            void await_resume() const {
                if (error) {
                    throw std::system_error(error);
                }
            }
        };

        EXPECT_FALSE(m_timers.get_executor().running_in_this_thread());
        asio::steady_timer timer(m_timers, std::chrono::seconds(seconds));
        co_await mcp::this_coro::report_progress("waiting for timer");
        co_await WaitForTimer{timer};
        EXPECT_TRUE(m_timers.get_executor().running_in_this_thread());
        co_return std::to_string(seconds) + "s";
    }

private:
    asio::io_context &m_timers;
};

json call(auto &_http, std::string_view _method, json _params,
          std::string_view _name = {}) {
    _params.update(mcp_test::meta_version());
    _params["_meta"]["io.modelcontextprotocol/clientCapabilities"]["extensions"]
           ["io.modelcontextprotocol/tasks"] = json::object();
    httplib::Headers headers;
    if (!_name.empty()) {
        headers.emplace("Mcp-Name", _name);
    }
    const auto response = _http.post(
        mcp_test::request(_method, std::move(_params)), std::move(headers));
    if (!response || response->status != 200) {
        throw std::runtime_error("MCP HTTP request failed");
    }
    return json::parse(response->body).at("result");
}

std::string start(auto &_http, std::string_view _name, json _arguments) {
    const json task =
        call(_http, "tools/call",
             {{"name", _name}, {"arguments", std::move(_arguments)}}, _name);
    EXPECT_EQ(task.at("resultType"), "task");
    return task.at("taskId").get<std::string>();
}

json poll(auto &_http, const std::string &_id) {
    return call(_http, "tasks/get", {{"taskId", _id}}, _id);
}

bool wait_until(const auto &_condition,
                std::chrono::milliseconds _timeout = kWait) {
    const auto deadline = std::chrono::steady_clock::now() + _timeout;
    do {
        if (_condition()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

void expect_text(auto &_http, const std::string &_id,
                 const std::string &_expected,
                 std::chrono::milliseconds _timeout = kWait) {
    json task;
    ASSERT_TRUE(wait_until(
        [&] {
            task = poll(_http, _id);
            return task.at("status") != "working";
        },
        _timeout))
        << _id;
    ASSERT_EQ(task.at("status"), "completed") << task.dump();
    const auto &result = task.at("result");
    ASSERT_EQ(result.at("isError"), false) << result.dump();
    EXPECT_EQ(result.at("content").at(0).at("text"), _expected);
}

struct TestFiles {
    const std::filesystem::path directory;

    explicit TestFiles(int _port)
        : directory(std::filesystem::temp_directory_path() /
                    ("cppreflect-mcp-io-" + std::to_string(_port))) {
        if (!std::filesystem::create_directory(directory)) {
            throw std::runtime_error("test directory already exists");
        }
    }

    ~TestFiles() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};

TEST(McpConcurrency, file_reads_complete_while_url_requests_are_waiting) {
    std::atomic<int> requests_received{0};
    std::promise<void> release_responses;
    const auto response_gate = release_responses.get_future().share();
    httplib::Server upstream;
    upstream.new_task_queue = [] {
        return new httplib::ThreadPool(kParallelReads);
    };
    upstream.Get("/data", [&](const httplib::Request &_request,
                              httplib::Response &_response) {
        ++requests_received;
        if (response_gate.wait_for(kGateTimeout) != std::future_status::ready) {
            _response.status = 503;
            return;
        }
        _response.set_content("url-" + _request.get_param_value("record"),
                              "text/plain");
    });
    const int port = upstream.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    std::jthread listener([&](std::stop_token _stop) {
        std::stop_callback stop(_stop, [&] { upstream.stop(); });
        upstream.listen_after_bind();
    });
    upstream.wait_until_ready();

    TestFiles files(port);
    IoHttpServer mcp(files.directory, port);
    ASSERT_GT(mcp.server.http_port(), 0);
    std::vector<std::string> url_ids;
    for (int i = 0; i < kParallelReads; ++i) {
        url_ids.push_back(start(mcp, "fetch_url", {{"record", i}}));
    }
    EXPECT_TRUE(
        wait_until([&] { return requests_received == kParallelReads; }));

    std::vector<std::string> file_ids;
    for (int i = 0; i < kParallelReads; ++i) {
        const auto record = std::to_string(i);
        const auto path = files.directory / record;
        std::ofstream file(path, std::ios::binary);
        file << "file-" << record;
        file.close();
        ASSERT_TRUE(file);
        file_ids.push_back(start(mcp, "read_file", {{"record", i}}));
    }
    EXPECT_TRUE(
        wait_until([&] { return mcp.server.opened_files == kParallelReads; }));
    mcp.server.release_reads.set_value();
    for (int i = 0; i < kParallelReads; ++i) {
        expect_text(mcp, file_ids.at(i), "file-" + std::to_string(i));
        EXPECT_EQ(poll(mcp, url_ids.at(i)).at("status"), "working");
    }
    EXPECT_TRUE(call(mcp, "ping", json::object()).is_object());

    release_responses.set_value();
    for (int i = 0; i < kParallelReads; ++i) {
        expect_text(mcp, url_ids.at(i), "url-" + std::to_string(i));
    }
}

TEST(McpConcurrency, three_and_five_second_timers_overlap_with_one_mcp_worker) {
    asio::io_context timers;
    const auto work = asio::make_work_guard(timers);
    std::jthread timer_thread([&timers](std::stop_token _stop) {
        std::stop_callback stop(_stop, [&timers] { timers.stop(); });
        timers.run();
    });
    mcp_test::HttpServerOf<TimerServer> http(timers);
    ASSERT_GT(http.server.http_port(), 0);

    const auto started = std::chrono::steady_clock::now();
    const auto short_task = start(http, "sleep_for", {{"seconds", 3}});
    const auto long_task = start(http, "sleep_for", {{"seconds", 5}});
    ASSERT_TRUE(wait_until([&] {
        return poll(http, long_task).value("statusMessage", "") ==
                   "waiting for timer" &&
               poll(http, short_task).value("statusMessage", "") ==
                   "waiting for timer";
    }));
    expect_text(http, short_task, "3s", 8s);
    expect_text(http, long_task, "5s", 8s);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    RecordProperty("elapsed_ms", std::to_string(elapsed.count()));
}

class DirectServer : public mcp::Server<DirectServer> {
public:
    std::string_view server_name() const override { return "direct-test"; }
    std::string_view server_version() const override { return "1"; }

    std::atomic<int> entered{0};
    std::promise<void> release;

    [[= mcp::tool{}]] int hold(int value) {
        ++entered;
        if (m_release.wait_for(kGateTimeout) != std::future_status::ready) {
            throw std::runtime_error("timed out waiting to release calls");
        }
        return value;
    }

private:
    std::shared_future<void> m_release = release.get_future().share();
};

TEST(McpConcurrency, ordinary_http_calls_overlap_and_leave_room_for_ping) {
    mcp_test::HttpServerOf<DirectServer> http;
    ASSERT_GT(http.server.http_port(), 0);

    const auto invoke = [&](int value) {
        auto params = mcp_test::meta_version();
        params["name"] = "hold";
        params["arguments"] = {{"value", value}};
        auto request = mcp_test::request("tools/call", std::move(params));
        request["id"] = value;
        return http.post(request, {{"Mcp-Name", "hold"}});
    };
    constexpr int concurrent_calls = 4;
    std::vector<std::future<httplib::Result>> calls;
    for (int value = 0; value < concurrent_calls; ++value) {
        calls.push_back(std::async(std::launch::async, invoke, value));
    }
    if (!wait_until([&] { return http.server.entered == concurrent_calls; })) {
        http.server.release.set_value();
        FAIL() << "Only " << http.server.entered.load() << " tools entered";
    }

    const auto ping =
        http.post(mcp_test::request("ping", mcp_test::meta_version()), {});
    EXPECT_TRUE(ping && ping->status == 200);
    const auto overload = invoke(concurrent_calls);
    EXPECT_TRUE(overload && overload->status == 503);
    EXPECT_EQ(http.server.entered.load(), concurrent_calls);
    http.server.release.set_value();

    for (int value = 0; value < concurrent_calls; ++value) {
        const auto reply = calls.at(value).get();
        ASSERT_TRUE(reply);
        ASSERT_EQ(reply->status, 200);
        const auto rpc = json::parse(reply->body);
        EXPECT_EQ(rpc.at("id"), value);
        const auto &result = rpc.at("result");
        EXPECT_EQ(result.at("resultType"), "complete");
        EXPECT_EQ(result.at("content").at(0).at("text"), std::to_string(value));
    }
}

}

namespace mcp_test {

int run_io_http(const char *_directory, const char *_upstream_port) {
    const std::string_view port_text(_upstream_port);
    const char *end = port_text.data() + port_text.size();
    int port = 0;
    const auto [ptr, ec] = std::from_chars(port_text.data(), end, port);
    if (ec != std::errc{} || ptr != end || port < 1 || port > 65535) {
        return 1;
    }
    IoServer server(_directory, port);
    server.release_reads.set_value();
    return server.serve_http("127.0.0.1", 0);
}

}
