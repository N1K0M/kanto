#pragma once
#include <iostream>
#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <future>

const int TASK_MAX_THRESHHOLD = 2;//UINT32_MAX;
const int THREAD_MAX_THRESHHOLD = 100;
const int THREAD_MAX_IDLE_TIME = 5; // 单位：秒

enum class PoolMode
{
    MODE_FIXED,
    MODE_CACHED,
};

// 线程类型
class Thread
{
public:
    // 线程函数对象类型
    using ThreadFunc = std::function<void(int)>;

    // 线程构造
    Thread(ThreadFunc func)
        : func_(func)
        , threadId_(generateId_++)
    {
    }

    // 线程析构
    ~Thread() = default;

    // 启动线程
    void start()
    {
        // 创建一个线程来执行线程函数
        std::thread t(func_, threadId_);
        t.detach(); // 设置分离线程
    }

    int getId() const
    {
        return threadId_;
    }

private:
    ThreadFunc func_;
    static int generateId_;
    int threadId_; // 保存线程id
};

int Thread::generateId_ = 0;

// 线程池
class ThreadPool
{
public:
    ThreadPool()
        : initThreadSize_(4)
        , taskSize_(0)
        , taskQueMaxThreshold_(TASK_MAX_THRESHHOLD)
        , threadSizeThreshold_(THREAD_MAX_THRESHHOLD)
        , poolMode_(PoolMode::MODE_FIXED)
        , isPoolRunning_(false)
        , idleThreadSize_(0)
        , curThreadSize_(0)
    {
    }

    // 设置线程池模式
    void setMode(PoolMode mode)
    {
        if (checkRunningState())
            return;
        poolMode_ = mode;
    }

    // 设置task任务队列上限阈值
    void setTaskQueMaxThreshold(int threshold)
    {
        if (checkRunningState())
            return;
        taskQueMaxThreshold_ = threshold;
    }

    // 设置线程池cached模式下线程阈值
    void setThreadSizeThreshold(int threshold)
    {
        if (checkRunningState())
            return;
        if (poolMode_ == PoolMode::MODE_CACHED)
        {
            threadSizeThreshold_ = threshold;
        }
    }

    // 开始线程池
    void start(int initThreadSize = std::thread::hardware_concurrency())
    {
        isPoolRunning_ = true;
        initThreadSize_ = initThreadSize;
        curThreadSize_ = initThreadSize;

        // 创建线程对象
        for (int i = 0; i < initThreadSize_; ++i)
        {
            // 创建Thread对象的时候，把线程函数给到Thread线程对象
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
        }

        // 启动所有线程
        for (int i = 0; i < initThreadSize_; ++i)
        {
            threads_[i]->start(); // 需要去执行一个线程函数
            idleThreadSize_++;    // 记录空闲线程的数量
        }
    }

    // 提交任务
    template <typename Func, typename... Args>
    auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
    {
        using RType = decltype(func(args...));
        auto task = std::make_shared<std::packaged_task<RType()>>(
            std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
        std::future<RType> result = task->get_future();

        // 获取锁
        std::unique_lock<std::mutex> lock(taskQueMtx_);

        // 线程的通信  等待任务队列有空余    等待notFull_条件
        // 用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败
        if (!notFull_.wait_for(lock, std::chrono::seconds(1),
            [&]() -> bool { return taskQue_.size() < (size_t)taskQueMaxThreshold_; }))
        {
            // 表示等待1s，条件依然没有满足
            std::cout << " submit tasks failed " << std::endl;
            auto task = std::make_shared<std::packaged_task<RType()>>(
                []() -> RType { return RType(); });
            (*task)(); // 执行一次避免未初始化的问题
            return task->get_future();
        }

        // 如果有空余，把任务放入任务队列中
        taskQue_.emplace([task]() { (*task)(); });
        taskSize_++;

        // 因为新放了任务，任务队列肯定不空了，在notEmpty_上进行通知
        notEmpty_.notify_all();

        // cached模式 任务处理比较紧急 场景：小而快的任务
        if (poolMode_ == PoolMode::MODE_CACHED
            && taskSize_ > idleThreadSize_
            && curThreadSize_ < threadSizeThreshold_)
        {
            std::cout << "create new thread..." << std::endl;
            // 创建新线程
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
            // 启动线程
            threads_[threadId]->start();
            curThreadSize_++;
            idleThreadSize_++;
        }

        return result;
    }

    // 禁止拷贝和赋值
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    ~ThreadPool()
    {
        isPoolRunning_ = false;

        // 等待线程池所有的线程返回
        // 有两种状态：正在执行任务中、正在等待任务
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        notEmpty_.notify_all();
        exitCond_.wait(lock, [&]() -> bool { return threads_.empty(); });
    }

private:
    // 检查线程池是否在运行
    bool checkRunningState() const
    {
        return isPoolRunning_;
    }

    // 线程函数
    void threadFunc(int threadid)
    {
        auto lastTime = std::chrono::high_resolution_clock::now();

        for (;;)
        {
            Task task;
            {
                // 先获取锁
                std::unique_lock<std::mutex> lock(taskQueMtx_);

                std::cout << "tid: " << std::this_thread::get_id() << "尝试获取任务..." << std::endl;

                // cached模式下，有可能已经创建了很多线程，需要回收
                while (taskQue_.size() == 0)
                {
                    // 线程池要结束，回收线程资源
                    if (!isPoolRunning_)
                    {
                        threads_.erase(threadid);
                        std::cout << "threadid: " << std::this_thread::get_id() << " exit!" << std::endl;
                        exitCond_.notify_all();
                        return;
                    }

                    if (poolMode_ == PoolMode::MODE_CACHED)
                    {
                        // 条件变量超时返回
                        if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                        {
                            auto now = std::chrono::high_resolution_clock::now();
                            auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                            if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_)
                            {
                                // 开始回收当前线程
                                threads_.erase(threadid);
                                curThreadSize_--;
                                idleThreadSize_--;
                                std::cout << "threadid: " << std::this_thread::get_id() << " exit!" << std::endl;
                                return;
                            }
                        }
                    }
                    else
                    {
                        // 固定模式，等待任务
                        notEmpty_.wait(lock);
                    }
                }

                idleThreadSize_--;

                // 从任务队列中取一个任务
                task = taskQue_.front();
                taskQue_.pop();
                taskSize_--;

                // 如果依然有剩余任务，继续通知其他线程执行任务
                if (taskQue_.size() > 0)
                {
                    notEmpty_.notify_all();
                }

                // 通知可以继续提交生产任务
                notFull_.notify_all();
            } // 释放锁

            // 当前线程执行这个任务
            if (task != nullptr)
            {
                task(); // 执行function<void()>
            }

            idleThreadSize_++;
            lastTime = std::chrono::high_resolution_clock::now(); // 更新线程执行完任务的时间
        }
    }

private:
    std::unordered_map<int, std::unique_ptr<Thread>> threads_;

    size_t initThreadSize_; // 初始线程数量
    int threadSizeThreshold_; // 线程数量上限阈值
    std::atomic_int curThreadSize_; // 当前线程池中线程的总数量
    std::atomic_int idleThreadSize_; // 空闲线程的数量

    using Task = std::function<void()>;
    std::queue<Task> taskQue_; // 任务队列
    std::atomic_int taskSize_; // 任务数量
    int taskQueMaxThreshold_; // 任务队列数量上限阈值

    std::mutex taskQueMtx_; // 保证任务队列的线程安全
    std::condition_variable notFull_; // 表示任务队列不满
    std::condition_variable notEmpty_; // 表示任务队列不空
    std::condition_variable exitCond_; // 等待线程退出

    PoolMode poolMode_; // 线程池模式
    std::atomic_bool isPoolRunning_; // 线程池运行状态
};