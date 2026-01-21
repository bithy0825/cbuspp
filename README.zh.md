# CBUSPP - C++23 事件总线

**CBUSPP** 是一个用于 C++23 的跨平台事件总线库。它基于 Topic 提供了发布/订阅机制，支持编译期哈希和无锁读路径。

## 特性

- **Topic 哈希**：使用 FNV-1a 算法进行编译期字符串哈希。
- **并发支持**：支持同步、异步以及延迟消息发布。
- **类型安全**：使用模板处理消息载荷（Payload）。
- **Header Only**：纯头文件库，无外部依赖。
- **跨平台**：支持 Linux, Windows 和 macOS。

## 文件结构

```
cbuspp/
├── cbuspp.hpp       # 主头文件
├── context.hpp      # 发布上下文定义
├── executor.hpp     # 执行器与线程池实现
├── filter.hpp       # 过滤器谓词
├── payload.hpp      # 消息载荷封装
├── subscription.hpp # 订阅管理
├── topic.hpp        # Topic ID 与哈希
└── wildcard.hpp     # Topic 通配符匹配工具
```

## 快速开始

### 基本用法

```cpp
#include "cbuspp.hpp"
using namespace cbuspp;

// 创建 bus 实例
bus my_bus;

// 定义消息类型
struct user_logged_in {
    std::string username;
    std::chrono::system_clock::time_point timestamp;
};

// 订阅事件
auto sub = my_bus.subscribe<user_logged_in>(
    "auth/login"_topic,
    [](const user_logged_in& event, const context& ctx) {
        std::cout << "User logged in: " << event.username << "\n";
    }
);

// 发布事件
my_bus.publish("auth/login"_topic, user_logged_in{
    .username = "alice",
    .timestamp = std::chrono::system_clock::now()
});
```

### Topic 定义

Topic 可以使用字符串字面量（编译期计算哈希）或运行时字符串定义。

```cpp
// 编译期哈希
constexpr auto topic1 = "orders/created"_topic;

// 运行时哈希
std::string dynamic_topic = "orders/updated";
auto topic3 = topic_hash(dynamic_topic);
```

## 功能说明

### 发布模式

**同步发布（默认）**
在调用线程立即执行回调。
```cpp
my_bus.publish(topic, data);
```

**异步发布**
将消息入队，由执行器处理。
```cpp
my_bus.publish_async(topic, data);
```

**延迟发布**
在指定时间段后或指定时间点发布消息。
```cpp
using namespace std::chrono_literals;
my_bus.publish_delayed(topic, data, 100ms);
```

### 使用智能指针发布

如果数据已由 `std::shared_ptr` 管理，可以使用 `publish_async_shared` 直接传递，避免重复封装。

```cpp
auto data = std::make_shared<MyData>(...);
my_bus.publish_async_shared(topic, data);
```

### 订阅选项

可以通过 `subscribe_options` 结构体控制订阅行为。

**优先级**
```cpp
subscribe_options opts;
opts.priority = 100;
auto sub = my_bus.subscribe<MyEvent>(topic, callback, opts);
```

**一次性订阅**
执行一次后自动取消订阅。
```cpp
auto sub = my_bus.once<MyEvent>(topic, callback);
```

**节流**
限制特定订阅的回调执行频率。
```cpp
subscribe_options opts;
opts.throttle_ms = 100;
auto sub = my_bus.subscribe<MyEvent>(topic, callback, opts);
```

### 订阅构建器

提供流式接口用于构建订阅。

```cpp
auto sub = with(my_bus)
    .priority(100)
    .once()
    .to<MyEvent>(topic, [](const MyEvent& e, const context&) {
        // 处理事件
    });
```

### 执行器

库内部包含默认线程池，也允许使用自定义执行器。

**配置**
```cpp
bus_config config;
config.max_async_threads = 4; // 限制线程池大小
bus my_bus(config);
```

**自定义执行器**
执行器需实现接受 `std::move_only_function<void()>` 的 `execute` 方法。

### 消息去重

如果在配置中启用并通过发布上下文指定，去重功能可以阻止在时间窗口内处理相同的消息。

```cpp
bus_config config;
config.enable_dedup = true;
bus my_bus(config);

context ctx;
ctx.put<ctx::dedup_ms_tag>(100);
my_bus.publish(topic, data, ctx);
```

### 通配符

支持在 Topic 匹配中使用单层 (`*`) 和多层 (`#`) 通配符。

```cpp
#include "wildcard.hpp"
using namespace cbuspp::wildcard;

matches("sensors/*/temperature", "sensors/room1/temperature"); // true
```

### 过滤器

编译期过滤器可用于基于上下文标签的筛选。

```cpp
#include "filter.hpp"
using namespace cbuspp::filters;

auto f = priority_at_least(10);
```

## 要求

- 支持 C++23 的编译器
- CMake

## 说明

本库是在 AI 工具辅助下编写的。

## 许可证

MIT License
