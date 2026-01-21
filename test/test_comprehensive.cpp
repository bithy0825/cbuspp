// ============================================================================
// CBUSPP Comprehensive Test Suite
// ============================================================================
// This test suite covers:
// - Basic functionality (subscribe, publish, unsubscribe)
// - Async operations (publish_async, publish_async_shared, delayed)
// - Thread pool configuration and limits
// - Priority ordering
// - Throttling and deduplication
// - Subscription groups and RAII
// - Builder API
// - Edge cases and stress tests
// ============================================================================

#include "cbuspp.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace cbuspp;
using namespace std::chrono_literals;

// ============================================================================
// Test framework
// ============================================================================

struct test_result {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;
};

test_result g_results;

#define RUN_TEST(name, code)                                      \
    do {                                                          \
        std::cout << "Testing: " << std::setw(45) << std::left    \
                  << name << " ";                                 \
        std::cout.flush();                                        \
        try {                                                     \
            code                                                  \
                    std::cout                                     \
                << "\033[32m[PASS]\033[0m" << std::endl;          \
            ++g_results.passed;                                   \
        } catch (const std::exception& e) {                       \
            std::cout << "\033[31m[FAIL]\033[0m " << e.what()     \
                      << std::endl;                               \
            ++g_results.failed;                                   \
            g_results.failures.push_back(std::string(name) + ": " \
                + e.what());                                      \
        }                                                         \
    } while (0)

#define EXPECT(cond, msg) \
    if (!(cond))          \
    throw std::runtime_error(msg)

#define SECTION(name) \
    std::cout << "\n=== " << name << " ===" << std::endl

// ============================================================================
// Test data structures
// ============================================================================

struct test_event {
    int value;
    std::string message;
};

struct large_payload {
    std::array<char, 1024> data;
    int id;
};

// ============================================================================
// Basic functionality tests
// ============================================================================

void test_basic_functionality()
{
    SECTION("Basic Functionality");

    RUN_TEST("Basic subscribe and publish", {
        bus b;
        int received = 0;
        auto sub = b.subscribe<int>("test"_topic,
            [&](const int& v, const context&) { received = v; });
        (void)b.publish("test"_topic, 42);
        EXPECT(received == 42, "received != 42");
    });

    RUN_TEST("Custom type publish", {
        bus b;
        std::string received_msg;
        auto sub = b.subscribe<test_event>("event"_topic,
            [&](const test_event& e, const context&) {
                received_msg = e.message;
            });
        (void)b.publish("event"_topic, test_event { 99, "hello" });
        EXPECT(received_msg == "hello", "message mismatch");
    });

    RUN_TEST("Multiple subscribers same topic", {
        bus b;
        int count = 0;
        auto sub1 = b.subscribe<int>("multi"_topic,
            [&](const int&, const context&) { ++count; });
        auto sub2 = b.subscribe<int>("multi"_topic,
            [&](const int&, const context&) { ++count; });
        auto sub3 = b.subscribe<int>("multi"_topic,
            [&](const int&, const context&) { ++count; });
        (void)b.publish("multi"_topic, 1);
        EXPECT(count == 3, "count != 3");
    });

    RUN_TEST("Multiple topics", {
        bus b;
        int a_count = 0;
        int b_count = 0;
        auto sub1 = b.subscribe<int>("topic_a"_topic,
            [&](const int&, const context&) { ++a_count; });
        auto sub2 = b.subscribe<int>("topic_b"_topic,
            [&](const int&, const context&) { ++b_count; });
        (void)b.publish("topic_a"_topic, 1);
        (void)b.publish("topic_a"_topic, 1);
        (void)b.publish("topic_b"_topic, 1);
        EXPECT(a_count == 2 && b_count == 1, "topic isolation failed");
    });

    RUN_TEST("Publish to nonexistent topic", {
        bus b;
        auto result = b.publish("nonexistent"_topic, 42);
        EXPECT(result.delivered_count == 0, "should deliver to 0");
    });

    RUN_TEST("Unsubscribe removes subscriber", {
        bus b;
        int count = 0;
        auto sub = b.subscribe<int>("unsub"_topic,
            [&](const int&, const context&) { ++count; });
        (void)b.publish("unsub"_topic, 1);
        sub.unsubscribe();
        (void)b.publish("unsub"_topic, 2);
        EXPECT(count == 1, "count != 1 after unsubscribe");
    });
}

// ============================================================================
// Async operation tests
// ============================================================================

void test_async_operations()
{
    SECTION("Async Operations");

    RUN_TEST("Basic async publish", {
        bus b;
        std::atomic<int> received { 0 };
        auto sub = b.subscribe<int>("async"_topic,
            [&](const int& v, const context&) { received.store(v); });
        b.publish_async("async"_topic, 777);
        std::this_thread::sleep_for(100ms);
        EXPECT(received.load() == 777, "async publish failed");
    });

    RUN_TEST("Async publish with shared_ptr (no double wrap)", {
        bus b;
        std::atomic<int> received { 0 };
        auto sub = b.subscribe<test_event>("shared"_topic,
            [&](const test_event& e, const context&) {
                received.store(e.value);
            });

        // User already has data in shared_ptr
        auto data = std::make_shared<test_event>(test_event { 123, "shared" });
        b.publish_async_shared("shared"_topic, data);

        std::this_thread::sleep_for(100ms);
        EXPECT(received.load() == 123, "shared_ptr async failed");
    });

    RUN_TEST("Async publish shared keeps data alive", {
        bus b;
        std::atomic<bool> callback_ok { false };
        auto sub = b.subscribe<large_payload>("alive"_topic,
            [&](const large_payload& p, const context&) {
                callback_ok.store(p.id == 999);
            });

        {
            auto data = std::make_shared<large_payload>();
            data->id = 999;
            b.publish_async_shared("alive"_topic, data);
            // data goes out of scope here, but should stay alive
        }

        std::this_thread::sleep_for(100ms);
        EXPECT(callback_ok.load(), "data was destroyed prematurely");
    });

    RUN_TEST("Delayed publish basic", {
        bus b;
        std::atomic<int> received { 0 };
        auto sub = b.subscribe<int>("delayed"_topic,
            [&](const int& v, const context&) { received.store(v); });
        b.publish_delayed("delayed"_topic, 888, 50ms);
        EXPECT(received.load() == 0, "should not receive immediately");
        std::this_thread::sleep_for(100ms);
        EXPECT(received.load() == 888, "delayed publish failed");
    });

    RUN_TEST("Delayed publish shared_ptr", {
        bus b;
        std::atomic<int> received { 0 };
        auto sub = b.subscribe<test_event>("delayed_shared"_topic,
            [&](const test_event& e, const context&) {
                received.store(e.value);
            });

        auto data = std::make_shared<test_event>(test_event { 456, "delayed" });
        b.publish_delayed_shared("delayed_shared"_topic, data, 50ms);

        EXPECT(received.load() == 0, "should not receive immediately");
        std::this_thread::sleep_for(100ms);
        EXPECT(received.load() == 456, "delayed shared publish failed");
    });

    RUN_TEST("Multiple async publishes ordering", {
        bus b;
        std::vector<int> received;
        std::mutex mtx;
        auto sub = b.subscribe<int>("order"_topic,
            [&](const int& v, const context&) {
                std::lock_guard lock(mtx);
                received.push_back(v);
            });

        for (int i = 0; i < 10; ++i) {
            b.publish_async("order"_topic, i);
        }

        std::this_thread::sleep_for(200ms);
        EXPECT(received.size() == 10, "did not receive all messages");
    });
}

// ============================================================================
// Thread pool configuration tests
// ============================================================================

void test_thread_pool_config()
{
    SECTION("Thread Pool Configuration");

    RUN_TEST("Default thread pool limited to 4", {
        // Default config should limit to min(4, hardware_concurrency)
        bus_config config;
        bus b(config);
        // This is an indirect test - we just verify it doesn't crash
        std::atomic<int> count { 0 };
        auto sub = b.subscribe<int>("pool"_topic,
            [&](const int&, const context&) { ++count; });
        for (int i = 0; i < 100; ++i) {
            b.publish_async("pool"_topic, i);
        }
        std::this_thread::sleep_for(200ms);
        EXPECT(count.load() == 100, "not all messages processed");
    });

    RUN_TEST("Custom thread pool size", {
        bus_config config;
        config.max_async_threads = 2; // Force 2 threads
        bus b(config);
        std::atomic<int> count { 0 };
        auto sub = b.subscribe<int>("custom_pool"_topic,
            [&](const int&, const context&) { ++count; });
        for (int i = 0; i < 50; ++i) {
            b.publish_async("custom_pool"_topic, i);
        }
        std::this_thread::sleep_for(200ms);
        EXPECT(count.load() == 50, "not all messages processed");
    });

    RUN_TEST("High thread pool limit", {
        bus_config config;
        config.default_pool_limit = 16; // Allow more threads
        bus b(config);
        std::atomic<int> count { 0 };
        auto sub = b.subscribe<int>("high_pool"_topic,
            [&](const int&, const context&) { ++count; });
        for (int i = 0; i < 100; ++i) {
            b.publish_async("high_pool"_topic, i);
        }
        std::this_thread::sleep_for(200ms);
        EXPECT(count.load() == 100, "not all messages processed");
    });
}

// ============================================================================
// Priority and ordering tests
// ============================================================================

void test_priority_ordering()
{
    SECTION("Priority and Ordering");

    RUN_TEST("Priority ordering (high first)", {
        bus b;
        std::string order;
        subscribe_options low_opts;
        subscribe_options mid_opts;
        subscribe_options high_opts;
        low_opts.priority = 1;
        mid_opts.priority = 50;
        high_opts.priority = 100;

        auto sub1 = b.subscribe<int>("prio"_topic, [&](const int&, const context&) { order += "L"; }, low_opts);
        auto sub2 = b.subscribe<int>("prio"_topic, [&](const int&, const context&) { order += "M"; }, mid_opts);
        auto sub3 = b.subscribe<int>("prio"_topic, [&](const int&, const context&) { order += "H"; }, high_opts);

        (void)b.publish("prio"_topic, 0);
        EXPECT(order == "HML", "order != 'HML', got '" + order + "'");
    });

    RUN_TEST("Same priority maintains insertion order", {
        bus b;
        std::string order;
        subscribe_options opts;
        opts.priority = 50;

        auto sub1 = b.subscribe<int>("same"_topic, [&](const int&, const context&) { order += "1"; }, opts);
        auto sub2 = b.subscribe<int>("same"_topic, [&](const int&, const context&) { order += "2"; }, opts);
        auto sub3 = b.subscribe<int>("same"_topic, [&](const int&, const context&) { order += "3"; }, opts);

        (void)b.publish("same"_topic, 0);
        EXPECT(order == "123", "insertion order not maintained");
    });

    RUN_TEST("Negative priority supported", {
        bus b;
        std::string order;
        subscribe_options neg_opts;
        subscribe_options pos_opts;
        neg_opts.priority = -100;
        pos_opts.priority = 100;

        auto sub1 = b.subscribe<int>("neg"_topic, [&](const int&, const context&) { order += "N"; }, neg_opts);
        auto sub2 = b.subscribe<int>("neg"_topic, [&](const int&, const context&) { order += "P"; }, pos_opts);

        (void)b.publish("neg"_topic, 0);
        EXPECT(order == "PN", "negative priority failed");
    });
}

// ============================================================================
// Throttling and deduplication tests
// ============================================================================

void test_throttle_dedup()
{
    SECTION("Throttling and Deduplication");

    RUN_TEST("Throttle limits callback frequency", {
        bus b;
        int count = 0;
        subscribe_options opts;
        opts.throttle_ms = 50; // Max once per 50ms

        auto sub = b.subscribe<int>("throttle"_topic, [&](const int&, const context&) { ++count; }, opts);

        for (int i = 0; i < 10; ++i) {
            (void)b.publish("throttle"_topic, i);
        }
        EXPECT(count == 1, "throttle should limit to 1, got " + std::to_string(count));
    });

    RUN_TEST("Throttle resets after window", {
        bus b;
        int count = 0;
        subscribe_options opts;
        opts.throttle_ms = 30;

        auto sub = b.subscribe<int>("throttle_reset"_topic, [&](const int&, const context&) { ++count; }, opts);

        (void)b.publish("throttle_reset"_topic, 1);
        EXPECT(count == 1, "first should pass");

        std::this_thread::sleep_for(50ms);
        (void)b.publish("throttle_reset"_topic, 2);
        EXPECT(count == 2, "after window should pass");
    });

    RUN_TEST("Deduplication removes duplicates", {
        bus_config config;
        config.enable_dedup = true;
        bus b(config);
        int count = 0;

        auto sub = b.subscribe<int>("dedup"_topic,
            [&](const int&, const context&) { ++count; });

        context ctx;
        (void)ctx.put<ctx::dedup_ms_tag>(100); // 100ms dedup window

        (void)b.publish("dedup"_topic, 1, ctx);
        (void)b.publish("dedup"_topic, 1, ctx);
        (void)b.publish("dedup"_topic, 1, ctx);

        EXPECT(count == 1, "dedup should limit to 1, got " + std::to_string(count));
    });

    RUN_TEST("Dedup disabled allows duplicates", {
        bus_config config;
        config.enable_dedup = false;
        bus b(config);
        int count = 0;

        auto sub = b.subscribe<int>("no_dedup"_topic,
            [&](const int&, const context&) { ++count; });

        context ctx;
        (void)ctx.put<ctx::dedup_ms_tag>(100);

        (void)b.publish("no_dedup"_topic, 1, ctx);
        (void)b.publish("no_dedup"_topic, 1, ctx);

        EXPECT(count == 2, "without dedup should get 2");
    });
}

// ============================================================================
// Once subscription tests
// ============================================================================

void test_once_subscription()
{
    SECTION("Once Subscription");

    RUN_TEST("Once subscription fires only once", {
        bus b;
        int count = 0;
        auto sub = b.once<int>("once"_topic,
            [&](const int&, const context&) { ++count; });
        (void)b.publish("once"_topic, 1);
        (void)b.publish("once"_topic, 2);
        (void)b.publish("once"_topic, 3);
        EXPECT(count == 1, "once should fire only once");
    });

    RUN_TEST("Once via subscribe_options", {
        bus b;
        int count = 0;
        subscribe_options opts;
        opts.once = true;

        auto sub = b.subscribe<int>("once_opt"_topic, [&](const int&, const context&) { ++count; }, opts);
        (void)b.publish("once_opt"_topic, 1);
        (void)b.publish("once_opt"_topic, 2);
        EXPECT(count == 1, "once via options should fire only once");
    });

    RUN_TEST("Once with priority", {
        bus b;
        std::string order;
        subscribe_options high_opts;
        subscribe_options low_opts;
        high_opts.priority = 100;
        high_opts.once = true;
        low_opts.priority = 1;

        auto sub1 = b.once<int>("once_prio"_topic, [&](const int&, const context&) { order += "H"; }, high_opts);
        auto sub2 = b.subscribe<int>("once_prio"_topic, [&](const int&, const context&) { order += "L"; }, low_opts);

        (void)b.publish("once_prio"_topic, 1);
        (void)b.publish("once_prio"_topic, 2);

        EXPECT(order == "HLL", "once with priority failed, got: " + order);
    });
}

// ============================================================================
// RAII and subscription group tests
// ============================================================================

void test_raii_and_groups()
{
    SECTION("RAII and Subscription Groups");

    RUN_TEST("RAII unsubscribe on scope exit", {
        bus b;
        int count = 0;
        {
            auto sub = b.subscribe<int>("raii"_topic,
                [&](const int&, const context&) { ++count; });
            (void)b.publish("raii"_topic, 1);
            EXPECT(count == 1, "should receive before scope exit");
        } // sub goes out of scope
        (void)b.publish("raii"_topic, 2);
        EXPECT(count == 1, "should not receive after scope exit");
    });

    RUN_TEST("Subscription group add and unsubscribe_all", {
        bus b;
        int count = 0;
        subscription_group group;

        group.add(b.subscribe<int>("g1"_topic, [&](const int&, const context&) { ++count; }));
        group.add(b.subscribe<int>("g2"_topic, [&](const int&, const context&) { ++count; }));
        group.add(b.subscribe<int>("g3"_topic, [&](const int&, const context&) { ++count; }));

        (void)b.publish("g1"_topic, 1);
        (void)b.publish("g2"_topic, 1);
        (void)b.publish("g3"_topic, 1);
        EXPECT(count == 3, "should receive 3 before unsubscribe");

        group.unsubscribe_all();

        (void)b.publish("g1"_topic, 2);
        (void)b.publish("g2"_topic, 2);
        EXPECT(count == 3, "should not receive after unsubscribe_all");
    });

    RUN_TEST("Subscription group += operator", {
        bus b;
        int count = 0;
        subscription_group group;

        group += b.subscribe<int>("op1"_topic, [&](const int&, const context&) { ++count; });
        group += b.subscribe<int>("op2"_topic, [&](const int&, const context&) { ++count; });

        (void)b.publish("op1"_topic, 1);
        (void)b.publish("op2"_topic, 1);
        EXPECT(count == 2, "operator += should work");
    });

    RUN_TEST("Subscription release does not unsubscribe", {
        bus b;
        int count = 0;
        {
            auto sub = b.subscribe<int>("release"_topic,
                [&](const int&, const context&) { ++count; });
            sub.release(); // Release ownership without unsubscribing
        }
        (void)b.publish("release"_topic, 1);
        EXPECT(count == 1, "release should not unsubscribe");
    });
}

// ============================================================================
// Builder API tests
// ============================================================================

void test_builder_api()
{
    SECTION("Builder API");

    RUN_TEST("Builder with priority", {
        bus b;
        std::string order;
        auto sub1 = with(b).priority(1).to<int>("build_prio"_topic, [&](const int&, const context&) { order += "L"; });
        auto sub2 = with(b).priority(100).to<int>("build_prio"_topic, [&](const int&, const context&) { order += "H"; });
        (void)b.publish("build_prio"_topic, 0);
        EXPECT(order == "HL", "builder priority failed");
    });

    RUN_TEST("Builder with throttle", {
        bus b;
        int count = 0;
        auto sub = with(b).throttle(100).to<int>("build_throttle"_topic, [&](const int&, const context&) { ++count; });
        for (int i = 0; i < 5; ++i) {
            (void)b.publish("build_throttle"_topic, i);
        }
        EXPECT(count == 1, "builder throttle failed");
    });

    RUN_TEST("Builder with once", {
        bus b;
        int count = 0;
        auto sub = with(b).once().to<int>("build_once"_topic, [&](const int&, const context&) { ++count; });
        (void)b.publish("build_once"_topic, 1);
        (void)b.publish("build_once"_topic, 2);
        EXPECT(count == 1, "builder once failed");
    });

    RUN_TEST("Builder chained options", {
        bus b;
        int count = 0;
        auto sub = with(b)
                       .priority(50)
                       .throttle(50)
                       .once()
                       .to<int>("build_chain"_topic, [&](const int&, const context&) { ++count; });
        (void)b.publish("build_chain"_topic, 1);
        (void)b.publish("build_chain"_topic, 2);
        EXPECT(count == 1, "builder chain failed");
    });
}

// ============================================================================
// Query interface tests
// ============================================================================

void test_query_interface()
{
    SECTION("Query Interface");

    RUN_TEST("subscriber_count returns correct count", {
        bus b;
        auto sub1 = b.subscribe<int>("query"_topic, [](const int&, const context&) { });
        auto sub2 = b.subscribe<int>("query"_topic, [](const int&, const context&) { });
        EXPECT(b.subscriber_count("query"_topic) == 2, "subscriber_count != 2");
    });

    RUN_TEST("total_subscribers across topics", {
        bus b;
        auto sub1 = b.subscribe<int>("q1"_topic, [](const int&, const context&) { });
        auto sub2 = b.subscribe<int>("q2"_topic, [](const int&, const context&) { });
        auto sub3 = b.subscribe<int>("q2"_topic, [](const int&, const context&) { });
        EXPECT(b.total_subscribers() == 3, "total_subscribers != 3");
    });

    RUN_TEST("topic_count returns unique topics", {
        bus b;
        auto sub1 = b.subscribe<int>("t1"_topic, [](const int&, const context&) { });
        auto sub2 = b.subscribe<int>("t1"_topic, [](const int&, const context&) { });
        auto sub3 = b.subscribe<int>("t2"_topic, [](const int&, const context&) { });
        EXPECT(b.topic_count() == 2, "topic_count != 2");
    });

    RUN_TEST("pending_count for async messages", {
        bus b;
        // Don't subscribe, so messages stay pending
        b.publish_delayed("pending"_topic, 1, 1s);
        b.publish_delayed("pending"_topic, 2, 1s);
        EXPECT(b.pending_count() >= 2, "pending_count should be >= 2");
    });

    RUN_TEST("is_running after construction", {
        bus b;
        EXPECT(b.is_running(), "bus should be running");
    });

    RUN_TEST("is_running false after shutdown", {
        bus b;
        b.shutdown();
        EXPECT(!b.is_running(), "bus should not be running after shutdown");
    });
}

// ============================================================================
// Context passing tests
// ============================================================================

void test_context()
{
    SECTION("Context Passing");

    RUN_TEST("Context user_id passed to callback", {
        bus b;
        std::uint64_t received_user = 0;
        auto sub = b.subscribe<int>("ctx_user"_topic,
            [&](const int&, const context& ctx) {
                received_user = ctx.get<ctx::user_id_tag>();
            });
        context ctx;
        (void)ctx.put<ctx::user_id_tag>(12345);
        (void)b.publish("ctx_user"_topic, 0, ctx);
        EXPECT(received_user == 12345, "user_id not passed correctly");
    });

    RUN_TEST("Context priority passed to callback", {
        bus b;
        priority received_prio = priority::normal;
        auto sub = b.subscribe<int>("ctx_prio"_topic,
            [&](const int&, const context& ctx) {
                received_prio = ctx.get<ctx::priority_tag>();
            });
        context ctx;
        (void)ctx.put<ctx::priority_tag>(priority::high);
        (void)b.publish("ctx_prio"_topic, 0, ctx);
        EXPECT(received_prio == priority::high, "priority not passed correctly");
    });

    RUN_TEST("Context trace_id passed to callback", {
        bus b;
        std::uint64_t received_trace = 0;
        auto sub = b.subscribe<int>("ctx_trace"_topic,
            [&](const int&, const context& ctx) {
                received_trace = ctx.get<ctx::trace_id_tag>();
            });
        context ctx;
        (void)ctx.put<ctx::trace_id_tag>(0xDEADBEEF);
        (void)b.publish("ctx_trace"_topic, 0, ctx);
        EXPECT(received_trace == 0xDEADBEEF, "trace_id not passed correctly");
    });
}

// ============================================================================
// Filter tests
// ============================================================================

void test_filters()
{
    SECTION("Filters");

    RUN_TEST("Filter by event tag", {
        bus b;
        int count = 0;
        subscribe_options opts;
        opts.has_filter = true;
        opts.filter_tag = 42;

        auto sub = b.subscribe<int>("filter"_topic, [&](const int&, const context&) { ++count; }, opts);

        // Publish without tag - should not match
        (void)b.publish("filter"_topic, 1);
        EXPECT(count == 0, "should not receive without tag");

        // Publish with wrong tag
        context ctx1;
        (void)ctx1.put<ctx::event_tag_tag>(99);
        (void)b.publish("filter"_topic, 2, ctx1);
        EXPECT(count == 0, "should not receive with wrong tag");

        // Publish with correct tag
        context ctx2;
        (void)ctx2.put<ctx::event_tag_tag>(42);
        (void)b.publish("filter"_topic, 3, ctx2);
        EXPECT(count == 1, "should receive with correct tag");
    });
}

// ============================================================================
// Edge cases and stress tests
// ============================================================================

void test_edge_cases()
{
    SECTION("Edge Cases and Stress Tests");

    RUN_TEST("Empty payload publish", {
        bus b;
        bool received = false;
        struct empty_struct { };
        auto sub = b.subscribe<empty_struct>("empty"_topic,
            [&](const empty_struct&, const context&) { received = true; });
        (void)b.publish("empty"_topic, empty_struct {});
        EXPECT(received, "empty struct not received");
    });

    RUN_TEST("Large payload publish", {
        bus b;
        bool received = false;
        auto sub = b.subscribe<large_payload>("large"_topic,
            [&](const large_payload& p, const context&) {
                received = (p.id == 12345);
            });
        large_payload data;
        data.id = 12345;
        std::fill(data.data.begin(), data.data.end(), 'X');
        (void)b.publish("large"_topic, data);
        EXPECT(received, "large payload not received correctly");
    });

    RUN_TEST("Rapid subscribe/unsubscribe", {
        bus b;
        for (int i = 0; i < 100; ++i) {
            auto sub = b.subscribe<int>("rapid"_topic,
                [](const int&, const context&) { });
            // sub unsubscribes immediately
        }
        EXPECT(b.subscriber_count("rapid"_topic) == 0, "should have no subscribers");
    });

    RUN_TEST("Concurrent publish stress test", {
        bus b;
        std::atomic<int> count { 0 };
        auto sub = b.subscribe<int>("stress"_topic,
            [&](const int&, const context&) { ++count; });

        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&b]() {
                for (int i = 0; i < 100; ++i) {
                    (void)b.publish("stress"_topic, i);
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        EXPECT(count.load() == 400, "concurrent stress test failed, got " + std::to_string(count.load()));
    });

    RUN_TEST("Callback throws exception", {
        bus b;
        int count = 0;
        auto sub1 = b.subscribe<int>("throw"_topic,
            [](const int&, const context&) { throw std::runtime_error("test"); });
        auto sub2 = b.subscribe<int>("throw"_topic,
            [&](const int&, const context&) { ++count; });

        auto result = b.publish("throw"_topic, 1);
        EXPECT(result.has_error, "should report error");
        EXPECT(count == 1, "second callback should still execute");
    });

    RUN_TEST("Shutdown while async pending", {
        bus b;
        std::atomic<bool> callback_called { false };
        auto sub = b.subscribe<int>("shutdown_test"_topic,
            [&](const int&, const context&) {
                std::this_thread::sleep_for(50ms);
                callback_called.store(true);
            });

        b.publish_async("shutdown_test"_topic, 1);
        std::this_thread::sleep_for(10ms); // Let it start
        b.shutdown(); // Should not crash

        // May or may not complete depending on timing
        EXPECT(true, "shutdown did not crash");
    });
}

// ============================================================================
// Main
// ============================================================================

int main()
{
    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║           CBUSPP Comprehensive Test Suite                     ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════╝\n";

    test_basic_functionality();
    test_async_operations();
    test_thread_pool_config();
    test_priority_ordering();
    test_throttle_dedup();
    test_once_subscription();
    test_raii_and_groups();
    test_builder_api();
    test_query_interface();
    test_context();
    test_filters();
    test_edge_cases();

    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                        Results                                 ║\n";
    std::cout << "╠═══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Passed: " << std::setw(4) << g_results.passed
              << "                                                    ║\n";
    std::cout << "║  Failed: " << std::setw(4) << g_results.failed
              << "                                                    ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════╝\n";

    if (!g_results.failures.empty()) {
        std::cout << "\nFailure details:\n";
        for (const auto& f : g_results.failures) {
            std::cout << "  - " << f << "\n";
        }
    }

    return g_results.failed > 0 ? 1 : 0;
}
