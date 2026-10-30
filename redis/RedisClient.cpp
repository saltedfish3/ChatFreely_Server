#include "RedisClient.h"
#include <chrono>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sw/redis++/command_options.h>
#include <vector>

Logger RedisClient::logger("RedisClient");

RedisClient& RedisClient::getClient()
{
    static RedisClient client;
    return client;
}

RedisClient::RedisClient()
{}

void RedisClient::init(const std::string& host, int port, const std::string& password, int poolSize)
{
    std::lock_guard<std::mutex> lock(this->mutex_init);
    if(this->inited)
	return;

    ConnectionOptions connOpts;
    connOpts.host = host;
    connOpts.port = port;
    if(!password.empty())
	connOpts.password = password;

    connOpts.connect_timeout = std::chrono::milliseconds(500);
    connOpts.socket_timeout = std::chrono::milliseconds(200);

    ConnectionPoolOptions poolOpts;
    poolOpts.size = poolSize;
    poolOpts.wait_timeout = std::chrono::milliseconds(100);

    this->redis = std::make_unique<Redis>(connOpts, poolOpts);
    this->inited = true;
}

bool RedisClient::setWithExpired(const std::string& key, const std::string& value, int seconds)
{
    if(!this->redis)
	return false;

    try
    {
	this->redis->set(key, value, std::chrono::seconds(seconds));
	return true;
    }catch(const sw::redis::Error&)
    {
	return false;
    }
}

bool RedisClient::setIfNoExists(const std::string& key, const std::string& value, int seconds)
{
    if(!this->redis)
	return false;
    
    try
    {
	return this->redis->set(key, value, std::chrono::seconds(seconds), sw::redis::UpdateType::NOT_EXIST);
    }catch(const sw::redis::Error&)
    {
	return false;
    }
}

bool RedisClient::refreshExpired(const std::string& key, int seconds)
{
    if(!this->redis)
	return false;

    try
    {
	return this->redis->expire(key, std::chrono::seconds(seconds));
    }catch(const sw::redis::Error&)
    {
	return false;
    }
}

std::optional<std::string> RedisClient::get(const std::string& key)
{
    if(!this->redis)
	return std::nullopt;
    try
    {
	auto value = this->redis->get(key);
	if(value)
	    return *value;
	return std::nullopt;
    }catch(const sw::redis::Error&)
    {
	return std::nullopt;
    }
}

bool RedisClient::del(const std::string& key)
{
    if(!this->redis)
	return false;
    try 
    {
	this->redis->del(key);
	return true;
    }catch(const sw::redis::Error&)
    {
	return false;
    }
}

bool RedisClient::publish(const std::string& channel, const std::string& message)
{
    if(!this->redis)
	return false;

    try
    {
	this->redis->publish(channel, message);
	return true;
    }catch(const sw::redis::Error&)
    {
	return false;
    }
}

std::vector<std::optional<std::string>> RedisClient::mget(const std::vector<std::string>& keys)
{
    std::vector<std::optional<std::string>> result;
    if(!this->redis || keys.empty())
	return result;

    try
    {
	this->redis->mget(keys.begin(), keys.end(), std::back_inserter(result));
    }catch(const sw::redis::Error& e)
    {
	logger.warn("Redis MEGE error:" + std::string(e.what()));
	result.clear();
    }
    return result;
}

long long RedisClient::incr(const std::string& key)
{
    if(!this->redis)
	return 0;

    try
    {
	return this->redis->incr(key);
    }catch(const sw::redis::Error&)
    {
	return 0;
    }
}
