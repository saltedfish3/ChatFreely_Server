#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <optional>
#include <memory>
#include <mutex>
#include <sw/redis++/connection.h>
#include <sw/redis++/connection_pool.h>
#include <sw/redis++/redis++.h>
#include <sw/redis++/tls.h>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <vector>
#include "../logger/Logger.h"

class RedisClient
{
using Redis = sw::redis::Redis;
using ConnectionOptions = sw::redis::ConnectionOptions;
using ConnectionPoolOptions = sw::redis::ConnectionPoolOptions;
using TlsOptions = sw::redis::tls::TlsOptions;

static Logger logger;

public:
    static RedisClient& getClient();
    bool init(const std::string& host = "127.0.0.1", int port = 6379, const std::string& password = "", int poolSize = 4);

    bool setWithExpired(const std::string& key, const std::string& value, int seconds);
    bool setIfNoExists(const std::string& key, const std::string& value, int seconds);
    bool refreshExpired(const std::string& key, int seconds);

    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);
    bool publish(const std::string& channel, const std::string& message);
    std::vector<std::optional<std::string>> mget(const std::vector<std::string>& keys);
    long long incr(const std::string& key);

    void startSubscriber();
    void stopSubscriber();
    uint64_t addHandler(std::function<void(int64_t uid, const std::string& content)> callback);
    void removeHandler(uint64_t handlerID);

private:
    RedisClient();
    ~RedisClient();
    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;

    std::unique_ptr<Redis> redis;
    std::mutex mutex_init;
    bool inited = false;

    std::mutex mutex_subscribe;
    std::thread thread_subscribe;
    std::atomic<bool> subscribeStop = false;

    std::mutex mutex_handler;
    std::unordered_map<uint64_t, std::function<void(int64_t uid, const std::string& content)>> map_handlers;
    uint64_t nextHandlerID = 1;

    std::string host;
    int port = 6379;
    std::string password;
};
