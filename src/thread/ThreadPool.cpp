#include "ThreadPool.h"

ThreadPool::ThreadPool()
{
    int n = g_thread_pool_size > 0 ? g_thread_pool_size : 4;
    this->list_thread.reserve(n);
    for(int i = 0; i < n; i++)
    {
	this->list_thread.emplace_back(std::thread(&ThreadPool::threadRun, this));
    }
}

ThreadPool::~ThreadPool()
{
    this->cv.notify_all();
    for(auto& threadItem : this->list_thread)
    {
	if(threadItem.joinable())
	    threadItem.join();
    }
}

ThreadPool* ThreadPool::getThreadPool()
{
    static ThreadPool pool;
    return &pool;
}

void ThreadPool::addTask(std::function<void()> func)
{
    {
	std::lock_guard<std::mutex> lock(this->mutex_task);
	this->list_task.push(std::move(func));
    }
    cv.notify_one();
}

void ThreadPool::threadRun()
{
    while(!flag_shutdown)
    {
	std::unique_lock<std::mutex> lock(this->mutex_task);
	this->cv.wait(lock, [this](){
	    return !this->list_task.empty() || flag_shutdown;
		});

	if(flag_shutdown && this->list_task.empty())
	    break;

	std::function<void()> func = std::move(this->list_task.front());
	this->list_task.pop();
	lock.unlock();

	func();
    }
}
