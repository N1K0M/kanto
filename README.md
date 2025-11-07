# 基于C++17可变参模板实现的线程池

一个高性能、跨平台的C++17线程池实现，支持任务提交、异步结果获取和动态线程管理。

## 项目特点

-  基于C++17可变参模板和引用折叠，支持任意任务函数和参数
-  支持fixed和cached两种线程池模式
-  使用`std::future`获取任务执行结果
-  基于条件变量和互斥锁实现线程间通信
-  跨平台兼容性测试（Windows/Linux）

## 技术栈

- **编程语言**: C++17
- **开发环境**: Visual Studio 2022
- **编译环境**: CentOS 7
- **调试工具**: GDB, EDB
- **核心特性**: 可变参模板、引用折叠、智能指针、RAII

## 项目架构

### 核心接口

```cpp
// 提交任务接口
template<typename Func, typename... Args>
auto submitTask(Func&& func, Args&&... args) 
    -> std::future<decltype(func(args...))>;

// 线程池模式
enum class PoolMode {
    FIXED,   // 固定线程数
    CACHED   // 动态线程管理
};
任务队列管理 - 使用std::queue管理待执行任务
线程资源管理 - 使用std::unordered_map管理线程对象
线程通信机制 - 基于std::condition_variable和std::mutex
资源生命周期管理 - 智能指针和RAII设计

1. 可变参模板任务提交
template<typename Func, typename... Args>
auto ThreadPool::submitTask(Func&& func, Args&&... args) 
    -> std::future<decltype(func(args...))> {
    
    using ReturnType = decltype(func(args...));
    auto task = std::make_shared<std::packaged_task<ReturnType()>>(
        [func = std::forward<Func>(func), 
         args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            return std::apply(func, std::move(args));
        }
    );
    
    std::future<ReturnType> result = task->get_future();
    
    // 任务入队逻辑
    {
        std::unique_lock<std::mutex> lock(mtx_);
        tasks_.emplace([task](){ (*task)(); });
    }
    
    cond_.notify_one();
    return result;
}

2. 双模式线程管理
FIXED模式: 固定线程数量，适用于CPU密集型任务
CACHED模式: 动态创建和回收线程，适用于IO密集型任务

项目挑战与解决方案
问题：跨平台死锁问题
现象: Windows平台运行正常，Linux平台出现死锁,线程池析构时进程无法正常退出

分析工具
# GDB调试分析
gdb -p <pid>
(gdb) info threads          # 查看所有线程状态
(gdb) thread <tid>          # 切换到指定线程
(gdb) bt                    # 查看调用堆栈
(gdb) frame <frame_num>     # 查看具体帧信息

通过EDB附加进程分析，发现死锁发生在：
1.主线程持有线程池锁，等待工作线程退出
2.工作线程在等待条件变量通知，但无法获取锁
3.Linux与Windows线程调度差异导致死锁场景不同

Windows (VS2022)
#include "ThreadPool.h"
ThreadPool pool(4, PoolMode::FIXED);
auto result = pool.submitTask([](int a, int b) { return a + b; }, 10, 20);
std::cout << "Result: " << result.get() << std::endl;

Linux (CentOS 7)
g++ -std=c++17 -pthread -O2 -o thread_pool_test main.cpp ThreadPool.cpp
./thread_pool_test

项目收获
深入理解C++17现代特性在并发编程中的应用
掌握跨平台开发中的兼容性问题和解决方案
熟练使用调试工具定位和解决复杂的并发问题
实践软件工程设计原则，创建可维护的高质量代码

@kanto
email : 1152178427@qq.com
