# CBUSPP - C++23 Event Bus

**CBUSPP** is a cross-platform event bus library for C++23. It provides mechanisms for topic-based publish/subscribe interactions using compile-time hashing and lock-free read paths.

## Features

- **Topic Hashing**: Uses FNV-1a for compile-time string hashing.
- **Concurrency**: Supports synchronous, asynchronous, and delayed message publishing.
- **Type Safety**: Utilizes templates for payload handling.
- **Header Only**: Provided as a header-only library with no external dependencies.
- **Cross-Platform**: Compatible with Linux, Windows, and macOS.

## File Structure

```
cbuspp/
├── cbuspp.hpp       # Main header
├── context.hpp      # Publishing context definitions
├── executor.hpp     # Executor and thread pool implementation
├── filter.hpp       # Filter predicates
├── payload.hpp      # Message payload wrapper
├── subscription.hpp # Subscription management
├── topic.hpp        # Topic ID and hashing
└── wildcard.hpp     # Topic wildcard matching
```

## Quick Start

### Basic Usage

```cpp
#include "cbuspp.hpp"
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
```

### Topic Definition

Topics can be defined using string literals (hashed at compile-time) or runtime strings.

```cpp
// Compile-time hash
constexpr auto topic1 = "orders/created"_topic;

// Runtime hash
std::string dynamic_topic = "orders/updated";
auto topic3 = topic_hash(dynamic_topic); // Note: topic_hash can be runtime or compile-time
```

## Functionality

### Publish Modes

**Synchronous (Default)**
Executes callbacks on the calling thread immediately.
```cpp
my_bus.publish(topic, data);
```

**Asynchronous**
Enqueues the message to be processed by an executor.
```cpp
my_bus.publish_async(topic, data);
```

**Delayed**
Schedules the message to be published after a duration or at a specific time.
```cpp
using namespace std::chrono_literals;
my_bus.publish_delayed(topic, data, 100ms);
```

### Publishing with Shared Pointers

If data is already managed by a `std::shared_ptr`, `publish_async_shared` can be used to pass it directly.

```cpp
auto data = std::make_shared<MyData>(...);
my_bus.publish_async_shared(topic, data);
```

### Subscription Options

Options can be controlled via the `subscribe_options` struct.

**Priority**
```cpp
subscribe_options opts;
opts.priority = 100;
auto sub = my_bus.subscribe<MyEvent>(topic, callback, opts);
```

**One-Shot**
Automatically unsubscribes after the first execution.
```cpp
auto sub = my_bus.once<MyEvent>(topic, callback);
```

**Throttling**
Limits the rate of callback execution for a specific subscription.
```cpp
subscribe_options opts;
opts.throttle_ms = 100;
auto sub = my_bus.subscribe<MyEvent>(topic, callback, opts);
```

### Subscription Builder

A fluent interface is available for constructing subscriptions.

```cpp
auto sub = with(my_bus)
    .priority(100)
    .once()
    .to<MyEvent>(topic, [](const MyEvent& e, const context&) {
        // Handle event
    });
```

### Executors

The library includes a default thread pool but allows custom executors.

**Configuration**
```cpp
bus_config config;
config.max_async_threads = 4; // limit thread pool size
bus my_bus(config);
```

**Custom Executor**
Executors must implement the `execute` method accepting a `std::move_only_function<void()>`.

### Message Deduplication

Deduplication prevents processing of identical messages within a time window if enabled in the configuration and specified in the publication context.

```cpp
bus_config config;
config.enable_dedup = true;
bus my_bus(config);

context ctx;
ctx.put<ctx::dedup_ms_tag>(100);
my_bus.publish(topic, data, ctx);
```

### Wildcards

Support for single-level (`*`) and multi-level (`#`) wildcards in topic matching.

```cpp
#include "wildcard.hpp"
using namespace cbuspp::wildcard;

matches("sensors/*/temperature", "sensors/room1/temperature"); // true
```

### Filters

Compile-time filters can correspond to context tags.

```cpp
#include "filter.hpp"
using namespace cbuspp::filters;

auto f = priority_at_least(10);
// Used internally for dispatching
```

## Requirements

- C++23 compliant compiler
- CMake

## Note

This library was written with the assistance of AI.

## License

MIT License
