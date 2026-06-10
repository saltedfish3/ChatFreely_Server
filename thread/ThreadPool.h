#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <functional>
#include "../global/GlobalVar.h"

#define ThreadPoolSize 5
class ThreadPool
{
public:
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool operator=(const ThreadPool&) = delete;
    ~ThreadPool();
    static ThreadPool* getThreadPool();

    void addTask(std::function<void()> func);


private:
    ThreadPool();
    void threadRun();

    std::mutex mutex_task;
    std::condition_variable cv;
    std::queue<std::function<void()>> list_task;

    std::vector<std::thread> list_thread;
};
