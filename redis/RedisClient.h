#pragma once
#include <string>
#include <optional>
#include <memory>
#include <mutex>
#include <sw/redis++/connection.h>
#include <sw/redis++/connection_pool.h>
#include <sw/redis++/redis++.h>
#include <chrono>
#include <vector>
#include "../logger/Logger.h"

class RedisClient
{
using Redis = sw::redis::Redis;
using ConnectionOptions = sw::redis::ConnectionOptions;
using ConnectionPoolOptions = sw::redis::ConnectionPoolOptions;

static Logger logger;

public:
    static RedisClient& getClient();
    void init(const std::string& host = "127.0.0.1", int port = 6379, const std::string& password = "", int poolSize = 4);

    bool setWithExpired(const std::string& key, const std::string& value, int seconds);
    bool setIfNoExists(const std::string& key, const std::string& value, int seconds);
    bool refreshExpired(const std::string& key, int seconds);

    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);
    bool publish(const std::string& channel, const std::string& message);
    std::vector<std::optional<std::string>> mget(const std::vector<std::string>& keys);
    long long incr(const std::string& key);

private:
    RedisClient();
    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;

    std::unique_ptr<Redis> redis;
    std::mutex mutex_init;
    bool inited = false;

};
