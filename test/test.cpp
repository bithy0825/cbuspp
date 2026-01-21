// Comprehensive test for optimized cbuspp modules
#include "cbuspp.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace cbuspp;
using namespace std::chrono_literals;

int passed = 0;
int failed = 0;

#define TEST(name) std::cout << "Testing: " << name << "... ";
#define PASS()                   \
    {                            \
        std::cout << "[PASS]\n"; \
        ++passed;                \
    }
#define FAIL(msg)                              \
    {                                          \
        std::cout << "[FAIL] " << msg << "\n"; \
        ++failed;                              \
    }

void test_basic_subscribe_publish()
{
    TEST("Basic subscribe/publish");

    bus my_bus;
    int received = 0;

    auto sub = my_bus.subscribe<int>("test/basic"_topic,
        [&received](const int& val, const context&) {
            received = val;
        });

    (void)my_bus.publish("test/basic"_topic, 42);

    if (received == 42) {
        PASS();
    } else {
        FAIL("received != 42");
    }
}

void test_custom_type()
{
    TEST("Custom type publish");

    struct MyEvent {
        int id;
        std::string name;
    };

    bus my_bus;
    std::string received_name;

    auto sub = my_bus.subscribe<MyEvent>("test/custom"_topic,
        [&received_name](const MyEvent& e, const context&) {
            received_name = e.name;
        });

    (void)my_bus.publish("test/custom"_topic, MyEvent { 1, "hello" });

    if (received_name == "hello") {
        PASS();
    } else {
        FAIL("name mismatch");
    }
}

void test_once_subscription()
{
    TEST("Once subscription");

    bus my_bus;
    int count = 0;

    auto sub = my_bus.once<int>("test/once"_topic,
        [&count](const int&, const context&) {
            ++count;
        });

    (void)my_bus.publish("test/once"_topic, 1);
    (void)my_bus.publish("test/once"_topic, 2);
    (void)my_bus.publish("test/once"_topic, 3);

    if (count == 1) {
        PASS();
    } else {
        FAIL("count != 1");
    }
}

void test_priority_ordering()
{
    TEST("Priority ordering");

    bus my_bus;
    std::vector<int> order;

    auto sub1 = my_bus.subscribe<int>("test/priority"_topic, [&order](const int&, const context&) { order.push_back(1); }, subscribe_options {}.with_priority(10));

    auto sub2 = my_bus.subscribe<int>("test/priority"_topic, [&order](const int&, const context&) { order.push_back(2); }, subscribe_options {}.with_priority(100)); // Higher priority

    auto sub3 = my_bus.subscribe<int>("test/priority"_topic, [&order](const int&, const context&) { order.push_back(3); }, subscribe_options {}.with_priority(50));

    (void)my_bus.publish("test/priority"_topic, 0);

    if (order.size() == 3 && order[0] == 2 && order[1] == 3 && order[2] == 1) {
        PASS();
    } else {
        FAIL("wrong priority order");
    }
}

void test_throttling()
{
    TEST("Throttling");

    bus my_bus;
    int count = 0;

    auto sub = my_bus.subscribe<int>("test/throttle"_topic, [&count](const int&, const context&) { ++count; }, subscribe_options {}.with_throttle(100)); // 100ms throttle

    (void)my_bus.publish("test/throttle"_topic, 1);
    (void)my_bus.publish("test/throttle"_topic, 2); // Should be throttled
    (void)my_bus.publish("test/throttle"_topic, 3); // Should be throttled

    if (count == 1) {
        PASS();
    } else {
        FAIL("throttle not working");
    }
}

void test_raii_unsubscribe()
{
    TEST("RAII unsubscribe");

    bus my_bus;
    int count = 0;

    {
        auto sub = my_bus.subscribe<int>("test/raii"_topic,
            [&count](const int&, const context&) { ++count; });

        (void)my_bus.publish("test/raii"_topic, 1);
        // sub goes out of scope here
    }

    (void)my_bus.publish("test/raii"_topic, 2); // Should not trigger callback

    if (count == 1) {
        PASS();
    } else {
        FAIL("RAII unsubscribe failed");
    }
}

void test_subscription_group()
{
    TEST("Subscription group");

    bus my_bus;
    int count = 0;

    {
        subscription_group group;
        group.add(my_bus.subscribe<int>("test/group1"_topic,
            [&count](const int&, const context&) { ++count; }));
        group += my_bus.subscribe<int>("test/group2"_topic,
            [&count](const int&, const context&) { ++count; });

        (void)my_bus.publish("test/group1"_topic, 1);
        (void)my_bus.publish("test/group2"_topic, 2);
        // group goes out of scope and unsubscribes all
    }

    (void)my_bus.publish("test/group1"_topic, 3); // Should not trigger
    (void)my_bus.publish("test/group2"_topic, 4); // Should not trigger

    if (count == 2) {
        PASS();
    } else {
        FAIL("subscription group failed");
    }
}

void test_builder_api()
{
    TEST("Builder API");

    bus my_bus;
    int received = 0;

    auto sub = with(my_bus)
                   .priority(50)
                   .throttle(0)
                   .to<int>("test/builder"_topic, [&received](const int& v, const context&) {
                       received = v;
                   });

    (void)my_bus.publish("test/builder"_topic, 99);

    if (received == 99) {
        PASS();
    } else {
        FAIL("builder API failed");
    }
}

void test_query_interface()
{
    TEST("Query interface");

    bus my_bus;

    auto sub1 = my_bus.subscribe<int>("test/query"_topic, [](const int&, const context&) { });
    auto sub2 = my_bus.subscribe<int>("test/query"_topic, [](const int&, const context&) { });
    auto sub3 = my_bus.subscribe<int>("test/query2"_topic, [](const int&, const context&) { });

    bool ok = my_bus.subscriber_count("test/query"_topic) == 2
        && my_bus.subscriber_count("test/query2"_topic) == 1
        && my_bus.total_subscribers() == 3
        && my_bus.topic_count() == 2;

    if (ok) {
        PASS();
    } else {
        FAIL("query interface mismatch");
    }
}

void test_context_passing()
{
    TEST("Context passing");

    bus my_bus;
    std::uint64_t received_trace = 0;

    auto sub = my_bus.subscribe<int>("test/context"_topic,
        [&received_trace](const int&, const context& ctx) {
            received_trace = ctx.get<ctx::trace_id_tag>();
        });

    context ctx;
    (void)ctx.put<ctx::trace_id_tag>(12345ULL);
    (void)my_bus.publish("test/context"_topic, 0, ctx);

    if (received_trace == 12345) {
        PASS();
    } else {
        FAIL("context not passed");
    }
}

void test_async_publish()
{
    TEST("Async publish");

    bus my_bus;
    std::atomic<int> received { 0 };

    auto sub = my_bus.subscribe<int>("test/async"_topic,
        [&received](const int& v, const context&) {
            received.store(v);
        });

    my_bus.publish_async("test/async"_topic, 77);

    // Wait for async execution
    std::this_thread::sleep_for(100ms);

    if (received.load() == 77) {
        PASS();
    } else {
        FAIL("async publish failed");
    }
}

void test_delayed_publish()
{
    TEST("Delayed publish");

    bus my_bus;
    std::atomic<int> received { 0 };
    auto start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point callback_time;

    auto sub = my_bus.subscribe<int>("test/delayed"_topic,
        [&received, &callback_time](const int& v, const context&) {
            received.store(v);
            callback_time = std::chrono::steady_clock::now();
        });

    my_bus.publish_delayed("test/delayed"_topic, 88, 50ms);

    // Should not be received yet
    std::this_thread::sleep_for(20ms);
    if (received.load() != 0) {
        FAIL("received too early");
        return;
    }

    // Wait for delayed execution
    std::this_thread::sleep_for(100ms);

    auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(callback_time - start);

    if (received.load() == 88 && delay.count() >= 40) {
        PASS();
    } else {
        FAIL("delayed publish failed");
    }
}

int main()
{
    std::cout << "=== CBUSPP Optimized Modules Tests ===\n\n";

    test_basic_subscribe_publish();
    test_custom_type();
    test_once_subscription();
    test_priority_ordering();
    test_throttling();
    test_raii_unsubscribe();
    test_subscription_group();
    test_builder_api();
    test_query_interface();
    test_context_passing();
    test_async_publish();
    test_delayed_publish();

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << passed << "\n";
    std::cout << "Failed: " << failed << "\n";

    return failed > 0 ? 1 : 0;
}
