#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <queue>
#include <random>
#include <shared_mutex>
#include <unordered_map>
#include <memory>
#include <optional>
#include <jwt-cpp/jwt.h>
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <sodium/crypto_hash_sha256.h>
#include <sodium/utils.h>
#include "../third_party/snowflake.hpp"
#include "../third_party/json.hpp"

#define SPort 9000
#define UploadPort 9001
#define AppName "ChatFreely"
#define MAX_CACHE_SIZE 10000

extern std::atomic<bool> flag_shutdown;
using SnowflakeGen = snowflake<1704067200000LL, std::mutex>;
extern SnowflakeGen sfGen;

class ChatServerWorker;

struct session : public std::enable_shared_from_this<session>
{
   ChatServerWorker* myself;
   bufferevent* bev;
   std::optional<int64_t> uid;
   std::weak_ptr<session> weak_self;

   std::queue<std::string> sendQueue;
   std::mutex mutex_sendQueue;
   std::atomic<bool> isQueueRunning = false;
};

struct userInfo {
    std::string username;
    std::string email;
    std::string sid;
    std::string avatar_url;
    std::weak_ptr<session> sessPtr;
};

struct idempotentRecord
{
    nlohmann::json j;
    std::chrono::steady_clock::time_point expiredTime;
};

extern std::shared_mutex mutex_onlineUser;
extern std::unordered_map<int64_t, std::shared_ptr<userInfo>> onlineUser;
extern std::string jwt_secret;
class GlobalTools
{
public:
    static std::string sha256(const std::string& input);
    static int64_t verifyAccessToken(const std::string& accessToken);
};
