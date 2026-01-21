# CBUSPP - C++23 高性能零开销事件总线（中文文档）

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Header Only](https://img.shields.io/badge/header--only-yes-green.svg)]()
[![License](https://img.shields.io/badge/license-MIT-brightgreen.svg)]()

**CBUSPP** 是一个为 C++23 设计的高性能、低开销、跨平台事件总线库。它使用编译期 topic 哈希、类型安全的 `payload` 设计以及多项运行时优化，在性能与易用性之间做了细致权衡。

## ✨ 核心特性

| 特性 | 描述 |
|------|------|
| 🚀 **零开销抽象** | 编译期的 Topic 哈希（FNV-1a），运行时只比较 `uint64_t` |
| 🔒 **线程安全** | 无锁读路径、写路径采用细粒度锁 |
| 🎯 **类型安全** | 模板化 `payload` 与编译期签名检查 |
| ⚡ **异步/延迟/同步发布** | 支持多种发布模式与执行器抽象 |
| 🔌 **执行器抽象** | 支持注入自定义执行器或使用内置线程池 |
| 🎛️ **丰富订阅选项** | 优先级、一次性订阅、节流、过滤、去重 |
| 📦 **Header Only** | 仅基于头文件，无第三方依赖 |
| 🖥️ **跨平台** | 支持 Linux/macOS/Windows 常见编译器 |

## 📁 文件结构

```
cbuspp/
├── cbuspp.hpp       # 核心实现（包含 bus、payload、topic 等）
├── context.hpp      # 发布上下文（元数据、deadline、发布模式等）
├── executor.hpp     # 执行器抽象与内置线程池实现
├── filter.hpp       # 编译期过滤器谓词
├── payload.hpp      # 类型安全的消息载荷与 SBO
├── subscription.hpp # 订阅句柄、回调包装与类型擦除
├── topic.hpp        # Topic ID 与编译期哈希工具
├── wildcard.hpp     # Topic 通配符匹配工具
└── README.md        # 英文原始文档
```

## 🚀 快速开始

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

// 订阅事件（lambda）
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

// subscription 析构时会自动取消订阅（RAII）
```

### Topic 定义方式

- 字面量（推荐，编译期哈希）：`constexpr auto topic1 = "orders/created"_topic;`
- consteval 函数：`constexpr auto topic2 = topic_hash("orders/updated");`
- 运行时字符串（仅在必要时）：`auto topic3 = runtime_topic_hash(dynamic_string);`

## 📖 详细功能

### 1. 发布模式

#### 同步发布（默认）
```cpp
// 在当前线程立即执行所有回调
auto result = my_bus.publish(topic, data);
std::cout << "Delivered to " << result.delivered_count << " subscribers\n";
```

#### 异步发布
```cpp
// 将消息入队，由线程池或执行器异步执行回调
my_bus.publish_async(topic, data);
```

#### 延迟发布
```cpp
using namespace std::chrono_literals;

// 100ms 后发布
my_bus.publish_delayed(topic, data, 100ms);

// 在指定时间点发布
auto fire_time = std::chrono::steady_clock::now() + 1s;
my_bus.publish_at(topic, data, fire_time);
```

#### 使用 Context 控制发布
```cpp
context ctx;
ctx.put<ctx::mode_tag>(publish_mode::async);
ctx.put<ctx::priority_tag>(10);
ctx.put<ctx::user_tag>(user_id);

my_bus.publish(topic, data, ctx);
```

### 2. 订阅选项

#### 优先级订阅
```cpp
subscribe_options opts;
opts.priority = 100;  // 值越大优先级越高

auto sub = my_bus.subscribe<MyEvent>(topic, callback, opts);
```

#### 一次性订阅
```cpp
// 方式一：使用 once()
auto sub = my_bus.once<MyEvent>(topic, [](const MyEvent& e, const context&) {
    // 只会被调用一次
});

// 方式二：通过 options
subscribe_options opts;
opts.once = true;
auto sub = my_bus.subscribe<MyEvent>(topic, callback, opts);
```

#### 节流（Throttle）
```cpp
subscribe_options opts;
opts.throttle_ms = 100;  // 每 100ms 最多触发一次

auto sub = my_bus.subscribe<MouseMove>(topic, callback, opts);
```

#### 过滤（Filter）
```cpp
subscribe_options opts;
opts.filter_tag = 42;  // 只接收 event_tag == 42 的消息
opts.has_filter = true;

auto sub = my_bus.subscribe<MyEvent>(topic, callback, opts);

// 发布时设置 tag
context ctx;
ctx.put<ctx::event_tag_tag>(42);
my_bus.publish(topic, data, ctx);
```

### 3. 订阅构建器（Fluent API）

```cpp
auto sub = with(my_bus)
    .priority(100)
    .throttle(50)
    .filter(42)
    .once()
    .executor(my_executor_id)
    .to<MyEvent>(topic, [](const MyEvent& e, const context& ctx) {
        // 处理事件
    });
```

### 4. 执行器（Executor）

#### 使用内置线程池
```cpp
bus_config config;
config.max_async_threads = 8;  // 0 = 使用硬件并发数
bus my_bus(config);
```

#### 注册自定义执行器
```cpp
// 创建执行器
single_thread_executor my_executor;

// 注册到 bus
auto executor_id = my_bus.register_executor(executor(my_executor));

// 订阅时指定执行器
subscribe_options opts;
opts.executor_id = executor_id;
auto sub = my_bus.subscribe<MyEvent>(topic, callback, opts);
```

#### 自定义执行器示例
```cpp
class my_custom_executor {
public:
    // 必须实现 execute
    void execute(std::move_only_function<void()> task) {
        my_thread_pool.submit(std::move(task));
    }

    // 可选：检查是否为空
    bool empty() const noexcept { return false; }
};

my_custom_executor custom;
my_bus.set_default_executor(executor(custom));
```

### 5. 消息去重（Deduplication）

```cpp
// 启用去重（默认已启用）
bus_config config;
config.enable_dedup = true;
bus my_bus(config);

// 发布时设置去重窗口
context ctx;
ctx.put<ctx::dedup_ms_tag>(100);  // 100ms 内相同 topic 去重

my_bus.publish(topic, data, ctx);
my_bus.publish(topic, data, ctx);  // 被去重，不会触发回调
```

### 6. Payload 类型

#### 支持任意可移动类型
```cpp
// 基础类型
my_bus.publish(topic, 42);
my_bus.publish(topic, std::string("hello"));

// 自定义类型
struct MyData { int x; std::string name; };
my_bus.publish(topic, MyData{.x = 1, .name = "test"});
```

#### 原地构造（避免拷贝）
```cpp
// 原地构造，避免额外拷贝
my_bus.emplace<MyData>(topic, context{}, 42, "test");
```

#### Payload 回调（通用处理）
```cpp
// 使用模板化订阅（类型安全）
my_bus.subscribe<MyData>(topic, [](const MyData& data, const context& ctx) {
    // 直接处理 MyData
});

// 使用通用 payload 订阅（需要知道类型）
my_bus.subscribe(topic, [](const payload& p, const context& ctx) {
    auto& data = p.as<MyData>();
    // 处理 data
});
```

### 7. 通配符匹配

```cpp
#include "wildcard.hpp"
using namespace cbuspp::wildcard;

// 单层通配符 '*'
matches("sensors/*/temperature", "sensors/room1/temperature");  // true

// 多层通配符 '#'
matches("sensors/#", "sensors/room1/temperature");  // true
```

### 8. 编译期过滤器

```cpp
#include "filter.hpp"
using namespace cbuspp::filters;

// 组合过滤器示例
auto f1 = priority_at_least(10);
auto f2 = user_equals(12345);
auto combined = (f1 && f2) || !tag_equals(42);

context ctx;
ctx.put<ctx::priority_tag>(15);
ctx.put<ctx::user_tag>(12345);

if (combined(ctx)) {
    // 通过过滤
}
```

## 🔧 性能特性

### 编译期优化

- Topic 哈希使用 consteval FNV-1a，零运行时开销。
- 回调分发通过模板实例化（在类型擦除边界外可避免虚调用）。
- 过滤器支持 constexpr 组合。

### 运行时优化

- 无锁读路径（`shared_lock`），最小化发布时的竞态。
- 延迟消息使用优先队列（heap）管理。
- 调度线程仅负责调度，回调在执行器线程中执行以分离职责。

### 内存优化

- `context` 采用位掩码的可选字段，仅存储被使用的字段。
- `payload` 支持 Small Object Optimization（SBO），避免小数据的堆分配。
- `subscription` 使用 RAII 管理订阅生命周期，避免内存泄漏。

## 🖥️ 平台支持

| 平台 | 编译器 | 版本要求 |
|------|--------|----------|
| Linux | GCC | 13+ |
| Linux | Clang | 16+ |
| Windows | MSVC | VS 2022 17.6+ |
| macOS | Clang | 16+ |

## ⚙️ 编译选项

```cmake
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GCC/Clang
add_compile_options(-std=c++23 -O3)

# MSVC
add_compile_options(/std:c++latest /O2)
```

## 📋 API 速查（节选）

```cpp
class bus {
public:
    // 构造
    explicit bus(bus_config config = {});
    ~bus();
    
    // 订阅
    template<typename T, typename F>
    subscription subscribe(topic_id, F&&, subscribe_options = {});
    
    template<typename T, typename F>
    subscription once(topic_id, F&&, subscribe_options = {});
    
    // 发布
    publish_result publish(topic_id, payload, context = {});
    template<typename T>
    publish_result publish(topic_id, T&&, context = {});
```

### subscription_group 类

```cpp
class subscription_group {
public:
    subscription_group& add(subscription);       // 添加订阅
    subscription_group& operator+=(subscription); // 等同于 add()

    void unsubscribe_all() noexcept;  // 取消所有订阅并清空
    void clear() noexcept;            // 释放所有权（不取消订阅）

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    // 析构函数会自动调用 unsubscribe_all()
};
```

## 📝 示例项目

### 简单聊天室

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
    
    // 订阅所有消息
    auto sub = chat_bus.subscribe<chat_message>(
        "chat/messages"_topic,
        [](const chat_message& msg, const context&) {
            std::cout << "[" << msg.user << "]: " << msg.text << "\n";
        }
    );
    
    // 发送消息
    chat_bus.publish("chat/messages"_topic, chat_message{
        .user = "Alice",
        .text = "Hello, world!",
        .time = std::chrono::system_clock::now()
    });
    
    return 0;
}
```

### 传感器数据处理

```cpp
#include "bus.hpp"
using namespace cbuspp;

struct sensor_reading {
    std::string sensor_id;
    double value;
    std::string unit;
};

int main() {
    bus sensor_bus;
    subscription_group subs;
    
    // 温度监控（节流到 1 秒）
    subs.add(with(sensor_bus)
        .throttle(1000)
        .to<sensor_reading>("sensors/temperature"_topic,
            [](const sensor_reading& r, const context&) {
                if (r.value > 30.0) {
                    std::cout << "Warning: High temperature!\n";
                }
            }));
    
    // 高优先级告警
    subs.add(with(sensor_bus)
        .priority(100)
        .to<sensor_reading>("sensors/alerts"_topic,
            [](const sensor_reading& r, const context&) {
                std::cout << "ALERT: " << r.sensor_id << "\n";
            }));
    
    // 异步记录到数据库
    subs.add(sensor_bus.subscribe<sensor_reading>(
        "sensors/#"_topic,
        [](const sensor_reading& r, const context&) {
            // database.insert(r);
        }));
    
    return 0;
}
```

## 📄 许可证

MIT License

---
