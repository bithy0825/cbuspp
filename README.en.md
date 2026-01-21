# CBUSPP - C++23 High-Performance Zero-Overhead Event Bus

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Header Only](https://img.shields.io/badge/header--only-yes-green.svg)]()
[![License](https://img.shields.io/badge/license-MIT-brightgreen.svg)]()

**CBUSPP** is a high-performance, zero-overhead, cross-platform event bus library designed for C++23. It features compile-time topic hashing, lock-free read paths, and type-safe payload design, achieving a perfect balance between extreme performance and ease of use.

## ✨ Core Features

| Feature | Description |
|---------|-------------|
| 🚀 **Zero-Overhead Abstraction** | Compile-time topic hash (FNV-1a), only compares `uint64_t` at runtime |
| 🔒 **Thread-Safe** | Lock-free read path, wait-free publish, fine-grained lock on write path |
| 🎯 **Type-Safe** | Templated payload, compile-time type checking |
| ⚡ **Async Execution** | Supports sync/async/deferred publish modes |
| 🔌 **Executor Abstraction** | Supports external thread pool injection or built-in thread pool |
| 🎛️ **Rich Features** | Priority, one-shot subscription, throttling, deduplication, filtering |
| 📦 **Header Only** | No third-party dependencies, single header include |
| 🖥️ **Cross-Platform** | Linux/GCC, Windows/MSVC, macOS/Clang |

## ⚡ Performance Optimizations

### v2.0 Optimizations

| Optimization | Before | After | Benefit |
|--------------|--------|-------|---------|
| **Sync Publish Allocation** | Always `make_shared<payload>` | Lazy allocation (only when async subscribers exist) | ~40% faster for sync-only paths |
| **Callback Dispatch** | Virtual `callback_base::invoke()` | `std::move_only_function` with SBO | Eliminates vtable lookup |
| **Executor Type Erasure** | `shared_ptr<concept_t>` | Small Buffer Optimization (24 bytes inline) | No heap allocation for small executors |
| **Filter Dispatch** | Virtual `match_filter()` | Function pointer `bool(*)(uint64_t, uint64_t)` | Direct call, no indirection |

### When to Use Async

```cpp
// Sync publish (fastest) - use when all subscribers are sync
bus.publish(topic, data);  // No shared_ptr allocation

// Async publish - only creates shared_ptr when needed
bus.publish_async(topic, data);  // shared_ptr created for async queue
```

## 📁 File Structure

```
cbuspp/
├── cbuspp.hpp       # Core header (contains bus, payload, topic, etc.)
├── context.hpp      # Publish context (metadata, deadline, mode, etc.)
├── executor.hpp     # Executor abstraction and built-in thread pool
├── filter.hpp       # Compile-time filter predicates
├── payload.hpp      # Type-safe message payload
├── subscription.hpp # Subscription handle and callback wrapper
├── topic.hpp        # Topic ID and compile-time hash
├── wildcard.hpp     # Topic wildcard matching
└── README.md        # This document
```

## 🚀 Quick Start

### Basic Usage

```cpp
#include "bus.hpp"
using namespace cbuspp;

// Create bus instance
bus my_bus;

// Define message type
struct user_logged_in {
    std::string username;
    std::chrono::system_clock::time_point timestamp;
};

// Subscribe to event
auto sub = my_bus.subscribe<user_logged_in>(
    "auth/login"_topic,
    [](const user_logged_in& event, const context& ctx) {
        std::cout << "User logged in: " << event.username << "\n";
    }
);

// Publish event
my_bus.publish("auth/login"_topic, user_logged_in{
    .username = "alice",
    .timestamp = std::chrono::system_clock::now()
});

// sub automatically unsubscribes on destruction (RAII)
```

### Topic Definition

```cpp
// Method 1: String literal (recommended, compile-time hash)
constexpr auto topic1 = "orders/created"_topic;

// Method 2: consteval function
constexpr auto topic2 = topic_hash("orders/updated");

// Method 3: Runtime string (use only when necessary)
std::string dynamic_topic = "orders/" + order_type;
auto topic3 = topic_hash(dynamic_topic);
```

## 📖 Detailed Features

### 1. Publish Modes

#### Sync Publish (Default)
```cpp
// Execute all callbacks immediately on current thread
auto result = my_bus.publish(topic, data);
std::cout << "Delivered to " << result.delivered_count << " subscribers\n";
```

#### Async Publish
```cpp
// Enqueue message, execute callbacks via thread pool
my_bus.publish_async(topic, data);
```

#### Async Publish with Pre-existing shared_ptr (Zero-Copy)

When you already have data managed by `shared_ptr`, use `publish_async_shared` to avoid double-wrapping overhead:

```cpp
// ❌ Suboptimal: data is copied into a new shared_ptr
auto data = std::make_shared<MyData>(...);
my_bus.publish_async(topic, *data);  // Creates another shared_ptr internally

// ✅ Optimal: uses your existing shared_ptr directly
auto data = std::make_shared<MyData>(...);
my_bus.publish_async_shared(topic, data);  // No extra allocation

// Also works with delayed publish
my_bus.publish_delayed_shared(topic, data, 100ms);
```

**When to use each method:**

| Method | Use Case | Overhead |
|--------|----------|----------|
| `publish(topic, data)` | Sync-only subscribers | Zero (no allocation) |
| `publish_async(topic, data)` | Data not in shared_ptr | One allocation |
| `publish_async_shared(topic, shared_ptr)` | Data already in shared_ptr | Zero extra allocation |

#### Delayed Publish
```cpp
using namespace std::chrono_literals;

// Publish after 100ms
my_bus.publish_delayed(topic, data, 100ms);

// Publish at specified time point
auto fire_time = std::chrono::steady_clock::now() + 1s;
my_bus.publish_at(topic, data, fire_time);
```

#### Using Context to Control Publish
```cpp
context ctx;
ctx.put<ctx::mode_tag>(publish_mode::async);
ctx.put<ctx::priority_tag>(10);
ctx.put<ctx::user_tag>(user_id);

my_bus.publish(topic, data, ctx);
```

### 2. Subscription Options

#### Priority Subscription
```cpp
subscribe_options opts;
opts.priority = 100;  // High priority (higher value executes first)

auto sub = my_bus.subscribe<MyEvent>(topic, callback, opts);
```

#### One-Shot Subscription
```cpp
// Method 1: Using once()
auto sub = my_bus.once<MyEvent>(topic, [](const MyEvent& e, const context&) {
    // Called only once, then automatically unsubscribed
});

// Method 2: Using options
subscribe_options opts;
opts.once = true;
auto sub = my_bus.subscribe<MyEvent>(topic, callback, opts);
```

#### Throttle
```cpp
subscribe_options opts;
opts.throttle_ms = 100;  // Trigger at most once per 100ms

auto sub = my_bus.subscribe<MouseMove>(topic, callback, opts);
```

#### Filter
```cpp
subscribe_options opts;
opts.filter_tag = 42;  // Only receive messages with event_tag == 42
opts.has_filter = true;

auto sub = my_bus.subscribe<MyEvent>(topic, callback, opts);

// Set tag when publishing
context ctx;
ctx.put<ctx::event_tag_tag>(42);
my_bus.publish(topic, data, ctx);
```

### 3. Subscription Builder (Fluent API)

```cpp
auto sub = with(my_bus)
    .priority(100)
    .throttle(50)
    .filter(42)
    .once()
    .executor(my_executor_id)
    .to<MyEvent>(topic, [](const MyEvent& e, const context&) {
        // Handle event
    });
```

### 4. Executor

#### Using Built-in Thread Pool
```cpp
bus_config config;

// Option 1: Specify exact thread count
config.max_async_threads = 8;

// Option 2: Use auto-limited default (recommended)
// Default: min(4, hardware_concurrency) - avoids consuming too many resources
config.max_async_threads = 0;        // Auto mode
config.default_pool_limit = 4;       // Limit to 4 threads max

// Option 3: Use full hardware concurrency
config.max_async_threads = 0;
config.default_pool_limit = 0;       // No limit

bus my_bus(config);
```

**Thread Pool Configuration Guide:**

| Scenario | Recommended Config | Rationale |
|----------|-------------------|-----------|
| Desktop app with other tasks | `default_pool_limit = 2-4` | Leave resources for UI/other work |
| Server with dedicated cores | `max_async_threads = N` | Full utilization of allocated cores |
| Library embedded in larger app | `default_pool_limit = 2` | Minimal footprint |
| High-throughput message processing | `max_async_threads = hardware_concurrency` | Maximum parallelism |

#### Register Custom Executor
```cpp
// Create executor
single_thread_executor my_executor;

// Register with bus
auto executor_id = my_bus.register_executor(executor(my_executor));

// Specify executor when subscribing
subscribe_options opts;
opts.executor_id = executor_id;
auto sub = my_bus.subscribe<MyEvent>(topic, callback, opts);
```

#### Implement Custom Executor
```cpp
class my_custom_executor {
public:
    // Must implement execute method
    void execute(std::move_only_function<void()> task) {
        // Your scheduling logic
        my_thread_pool.submit(std::move(task));
    }
    
    // Optional: check if empty
    bool empty() const noexcept { return false; }
};

// Usage
my_custom_executor custom;
my_bus.set_default_executor(executor(custom));
```

### 5. Message Deduplication

```cpp
// Enable deduplication (enabled by default)
bus_config config;
config.enable_dedup = true;
bus my_bus(config);

// Set dedup window when publishing
context ctx;
ctx.put<ctx::dedup_ms_tag>(100);  // Dedup same topic within 100ms

my_bus.publish(topic, data, ctx);
my_bus.publish(topic, data, ctx);  // Deduplicated, callback not triggered
```

### 6. Payload Types

#### Any Type
```cpp
// Basic types
my_bus.publish(topic, 42);
my_bus.publish(topic, std::string("hello"));

// Custom types
struct MyData { int x; std::string name; };
my_bus.publish(topic, MyData{.x = 1, .name = "test"});
```

#### In-Place Construction
```cpp
// Avoid extra copies
my_bus.emplace<MyData>(topic, context{}, 42, "test");
```

#### Payload Callback (Generic Handling)
```cpp
// Using templated subscription (recommended, type-safe)
my_bus.subscribe<MyData>(topic, [](const MyData& data, const context& ctx) {
    // Process MyData directly
});

// Or using generic payload subscription (requires knowing type)
my_bus.subscribe(topic, [](const payload& p, const context& ctx) {
    // Note: payload is type-erased, as<T>() requires correct type
    auto& data = p.as<MyData>();
    // Process data
});
```

### 7. Wildcard Matching

```cpp
#include "wildcard.hpp"
using namespace cbuspp::wildcard;

// Single-level wildcard '*'
matches("sensors/*/temperature", "sensors/room1/temperature");  // true
matches("sensors/*/temperature", "sensors/room1/sub/temperature");  // false

// Multi-level wildcard '#' (must be at end)
matches("sensors/#", "sensors/room1/temperature");  // true
matches("sensors/#", "sensors/room1/sub/deep/value");  // true

// Compile-time pattern
constexpr auto pattern = static_pattern<"events/*">();
static_assert(pattern.matches("events/click"));
```

### 8. Compile-Time Filters

```cpp
#include "filter.hpp"
using namespace cbuspp::filters;

// Priority filter
auto f1 = priority_at_least(10);

// User filter
auto f2 = user_equals(12345);

// Tag filter
auto f3 = tag_equals(42);

// Combined filter (compile-time operations)
auto combined = (f1 && f2) || !f3;

// Usage
context ctx;
ctx.put<ctx::priority_tag>(15);
ctx.put<ctx::user_tag>(12345);

if (combined(ctx)) {
    // Passed filter
}
```

## 🔧 Context Field Reference

| Tag | Type | Description |
|-----|------|-------------|
| `mode_tag` | `publish_mode` | Publish mode (sync/async/deferred) |
| `priority_tag` | `std::int32_t` | Message priority |
| `user_tag` | `std::uint64_t` | User identifier |
| `seq_tag` | `std::uint64_t` | Sequence number |
| `deadline_tag` | `time_point` | Deadline |
| `trace_id_tag` | `std::uint64_t` | Trace ID |
| `throttle_ms_tag` | `std::uint32_t` | Throttle window (ms) |
| `dedup_ms_tag` | `std::uint32_t` | Dedup window (ms) |
| `event_tag_tag` | `std::uint64_t` | Event tag (for filtering) |
| `executor_hint_tag` | `std::uint32_t` | Executor hint |

## 📊 Performance Characteristics

### Compile-Time Optimizations

- **Topic Hash**: FNV-1a consteval hash, zero runtime overhead
- **Callback Dispatch**: Template instantiation, no virtual function calls (except at type erasure boundary)
- **Filters**: constexpr predicates, compile-time composition

### Runtime Optimizations

- **Lock-Free Read Path**: Uses `shared_lock` for publishing, minimizes contention
- **Priority Heap**: Delayed messages managed via `std::make_heap`
- **Scheduling Separation**: Scheduler thread only schedules, does not execute callbacks

### Memory Optimizations

- **Context**: Bit-masked optional fields, only stores used fields
- **Payload**: Small Buffer Optimization (SBO), avoids heap allocation for small data
- **Subscription**: RAII automatic management, no memory leaks

## 🖥️ Platform Support

| Platform | Compiler | Version Requirement |
|----------|----------|---------------------|
| Linux | GCC | 13+ |
| Linux | Clang | 16+ |
| Windows | MSVC | VS 2022 17.6+ |
| macOS | Clang | 16+ |

## ⚙️ Compiler Options

```cmake
# CMake example
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GCC/Clang
add_compile_options(-std=c++23 -O3)

# MSVC
add_compile_options(/std:c++latest /O2)
```

## 📋 API Quick Reference

### bus class

```cpp
class bus {
public:
    // Construction
    explicit bus(bus_config config = {});
    ~bus();
    
    // Subscribe
    template<typename T, typename F>
    subscription subscribe(topic_id, F&&, subscribe_options = {});
    
    template<typename T, typename F>
    subscription once(topic_id, F&&, subscribe_options = {});
    
    // Publish
    publish_result publish(topic_id, payload, context = {});
    template<typename T>
    publish_result publish(topic_id, T&&, context = {});
    
    void publish_async(topic_id, payload, context = {});
    void publish_delayed(topic_id, payload, duration, context = {});
    void publish_at(topic_id, payload, time_point, context = {});
    
    // Executor
    std::uint32_t register_executor(executor);
    void set_default_executor(executor);
    
    // Query
    std::size_t subscriber_count(topic_id) const;
    std::size_t total_subscribers() const;
    std::size_t topic_count() const;
    std::size_t pending_count() const;
    
    // Lifecycle
    void shutdown();
    bool is_running() const;
    void clear();
};
```

### subscription class

```cpp
class subscription {
public:
    subscription();
    ~subscription();  // Auto unsubscribe
    
    subscription(subscription&&);
    subscription& operator=(subscription&&);
    
    void unsubscribe();
    void release();  // Release ownership (no unsubscribe)
    
    bool valid() const;
    explicit operator bool() const;
};
```

### subscription_group class

```cpp
class subscription_group {
public:
    subscription_group& add(subscription);       // Add subscription
    subscription_group& operator+=(subscription); // Same as add()
    
    void unsubscribe_all() noexcept;  // Unsubscribe all and clear
    void clear() noexcept;            // Release ownership (no unsubscribe)
    
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    
    // Destructor automatically calls unsubscribe_all()
};
```

## 📝 Example Projects

### Simple Chat Room

```cpp
#include "cbuspp.hpp"
using namespace cbuspp;

struct chat_message {
    std::string user;
    std::string text;
    std::chrono::system_clock::time_point time;
};

int main() {
    bus chat_bus;
    
    // Subscribe to all messages
    auto sub = chat_bus.subscribe<chat_message>(
        "chat/messages"_topic,
        [](const chat_message& msg, const context&) {
            std::cout << "[" << msg.user << "]: " << msg.text << "\n";
        }
    );
    
    // Send message
    chat_bus.publish("chat/messages"_topic, chat_message{
        .user = "Alice",
        .text = "Hello, world!",
        .time = std::chrono::system_clock::now()
    });
    
    return 0;
}
```

### Sensor Data Processing

```cpp
#include "cbuspp.hpp"
using namespace cbuspp;

struct sensor_reading {
    std::string sensor_id;
    double value;
    std::string unit;
};

int main() {
    bus sensor_bus;
    subscription_group subs;
    
    // Temperature monitoring (throttled to 1 second)
    subs.add(with(sensor_bus)
        .throttle(1000)
        .to<sensor_reading>("sensors/temperature"_topic,
            [](const sensor_reading& r, const context&) {
                if (r.value > 30.0) {
                    std::cout << "Warning: High temperature!\n";
                }
            }));
    
    // High priority alerts
    subs.add(with(sensor_bus)
        .priority(100)
        .to<sensor_reading>("sensors/alerts"_topic,
            [](const sensor_reading& r, const context&) {
                std::cout << "ALERT: " << r.sensor_id << "\n";
            }));
    
    // Async logging to database
    subs.add(sensor_bus.subscribe<sensor_reading>(
        "sensors/#"_topic,
        [](const sensor_reading& r, const context&) {
            // database.insert(r);
        }));
    
    return 0;
}
```

## 📄 License

MIT License

---

**CBUSPP** - Event bus built for modern C++ 🚀
