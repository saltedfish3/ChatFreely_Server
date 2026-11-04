#include "RedisClient.h"
#include "GlobalVar.h"
#include <chrono>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <sw/redis++/command_options.h>
#include <sw/redis++/errors.h>
#include <thread>
#include <utility>
#include <vector>

Logger RedisClient::logger("RedisClient");

RedisClient& RedisClient::getClient()
{
    static RedisClient client;
    return client;
}

RedisClient::RedisClient()
{}

RedisClient::~RedisClient()
{
    stopSubscriber();
}

bool RedisClient::init(const std::string& host, int port, const std::string& password, int poolSize)
{
    std::lock_guard<std::mutex> lock(this->mutex_init);
    if(this->inited)
	return false;

    this->host = host;
    this->port = port;
    this->password = password;

    ConnectionOptions connOpts;
    connOpts.host = host;
    connOpts.port = port;
    if(!password.empty())
	connOpts.password = password;

    connOpts.connect_timeout = std::chrono::milliseconds(g_redis_connect_timeout);
    connOpts.socket_timeout = std::chrono::milliseconds(g_redis_socket_timeout);

    ConnectionPoolOptions poolOpts;
    poolOpts.size = poolSize;
    poolOpts.wait_timeout = std::chrono::milliseconds(100);

    this->redis = std::make_unique<Redis>(connOpts, poolOpts);

    this->redis->ping();
    
    this->inited = true;
    return true;
}

bool RedisClient::setWithExpired(const std::string& key, const std::string& value, int seconds)
{
    if(!this->redis)
	return false;

    thread_local std::mt19937 gen(std::random_device{}());
    int randomCount = seconds / 10;
    {
	std::uniform_int_distribution<int> dist(-randomCount, randomCount);
	seconds += dist(gen);
    }

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
	return this->redis->publish(channel, message) > 0;
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

void RedisClient::startSubscriber()
{
    std::lock_guard<std::mutex> lock(this->mutex_subscribe);
    if(thread_subscribe.joinable())
	return;

    subscribeStop.store(false);
    std::string channel = "channel:node:" + g_node_id;

    thread_subscribe = std::thread([this, channel](){
	while(!subscribeStop.load())
	{
	    try
	    {
		ConnectionOptions opts;
		opts.host = this->host;
		opts.port = this->port;
		if(!this->password.empty())
		    opts.password = this->password;
		opts.connect_timeout = std::chrono::milliseconds(g_redis_connect_timeout);
		opts.socket_timeout = std::chrono::milliseconds(g_redis_socket_timeout);

		auto redisSub = sw::redis::Redis(opts);
		auto sub = redisSub.subscriber();

		sub.on_message([this](std::string, std::string msg){
		    auto pos = msg.find('\n');
		    if(pos == std::string::npos)
			return;
		    
		    int64_t uid = 0;
		    try
		    {
			uid = std::stoll(msg.substr(0, pos));
		    }
		    catch(const std::exception& e)
		    {
			logger.warn("Subscribe handle msg failed: stoll failed[" + msg.substr(0, pos) + "]");
			return;
		    }

		    std::string content = msg.substr(pos + 1);

		    std::vector<std::function<void(int64_t uid, const std::string& content)>> snapshot;
		    {
			std::lock_guard<std::mutex> lock(this->mutex_handler);
			snapshot.reserve(this->map_handlers.size());
			for(auto& item : this->map_handlers)
			    snapshot.push_back(item.second);
		    }
		    
		    for(auto& handler : snapshot)
		    {
			try
			{
			    handler(uid, content);
			}catch(const std::exception& e)
			{
			    logger.warn(std::string("Execute subscriber callback failed:") + e.what());
			}
		    }
		});

		sub.subscribe(channel);

		while(!subscribeStop.load())
		{
		    try
		    {
			sub.consume();
		    }catch(const sw::redis::TimeoutError&)
		    {}

		}
		sub.unsubscribe();
	    }catch(const std::exception& e)
	    {
		if(subscribeStop.load())
		    break;
		std::this_thread::sleep_for(std::chrono::seconds(2));
	    }
	}
    });
}

void RedisClient::stopSubscriber()
{
    std::thread temp;
    {
	std::lock_guard<std::mutex> lock(this->mutex_subscribe);
	subscribeStop.store(true);
	temp = std::move(this->thread_subscribe);
    }

    if(temp.joinable())
	temp.join();

    std::lock_guard<std::mutex> lock(this->mutex_handler);
    this->map_handlers.clear();
}

uint64_t RedisClient::addHandler(std::function<void(int64_t uid, const std::string& content)> callback)
{
    std::lock_guard<std::mutex> lock(this->mutex_handler);
    this->map_handlers.emplace(this->nextHandlerID, std::move(callback));
    return this->nextHandlerID++;
}

void RedisClient::removeHandler(uint64_t handlerID)
{
    std::lock_guard<std::mutex> lock(this->mutex_handler);
    this->map_handlers.erase(handlerID);
}
