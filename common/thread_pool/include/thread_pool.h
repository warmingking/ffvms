#ifndef __THREAD_POOL_H__
#define __THREAD_POOL_H__

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace common
{

class ThreadPool
{
public:
    explicit ThreadPool(size_t);
    template <typename F, typename... Args>
    auto submit(F &&f, Args &&...args) -> std::future<typename std::result_of<F(Args...)>::type>;
    ~ThreadPool();

private:
    std::vector<std::thread> mWorks;
    std::queue<std::function<void()>> mTasks;
    bool mStop;
    std::mutex mMutex;
    std::condition_variable mCv;
};

inline ThreadPool::ThreadPool(size_t capacity) : mStop(false)
{
    for (size_t i = 0; i < capacity; ++i)
    {
        mWorks.emplace_back([this]() {
            for (;;)
            {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> _(mMutex);
                    mCv.wait(_, [this] { return mStop || !mTasks.empty(); });
                    if (mStop)
                    { // TODO: 直接终止线程 or 继续执行完剩下的任务
                        break;
                    }
                    task = std::move(mTasks.front());
                    mTasks.pop();
                }
                task();
            }
        });
    }
}

template <class F, class... Args>
auto ThreadPool::submit(F &&f, Args &&...args)
    -> std::future<typename std::result_of<F(Args...)>::type>
{
    using ReturnType = typename std::result_of<F(Args...)>::type;
    auto pacTask = std::make_shared<std::packaged_task<ReturnType()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    std::future<ReturnType> res = std::move(pacTask->get_future());
    {
        std::unique_lock<std::mutex> _(mMutex);
        if (mStop)
        {
            throw std::runtime_error("submit to stopped ThreadPool");
        }
        mTasks.emplace([pacTask]() { (*pacTask)(); });
    }
    mCv.notify_one();
    return std::move(res);
}

inline ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> _(mMutex);
        mStop = true;
    }
    mCv.notify_all();
    for (auto &t : mWorks)
    {
        t.join();
    }
}
} // namespace common

#endif // __THREAD_POOL_H__