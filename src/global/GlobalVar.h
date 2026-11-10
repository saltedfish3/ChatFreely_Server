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
   std::optional<std::string> clientSessionID;
   std::weak_ptr<session> weak_self;

   std::string client_ip;

   std::queue<std::string> sendQueue;
   std::mutex mutex_sendQueue;
   std::atomic<bool> isQueueRunning = false;
};

struct idempotentRecord
{
    nlohmann::json j;
    std::chrono::steady_clock::time_point expiredTime;
};

//server
extern std::string g_node_id;
extern int g_listen_port;
extern int g_upload_port;
extern int g_worker_num;
extern int g_upload_worker_num;
extern std::string g_jwt_secret_file;

//ThreadPool
extern int g_thread_pool_size;

//Redis
extern std::string g_redis_host;
extern int g_redis_port;
extern std::string g_redis_password;
extern int g_redis_pool_size;
extern bool g_redis_use_ssl;
extern std::string g_redis_ssl_cacert;

//Mysql
extern std::string g_mysql_host;
extern int g_mysql_port;
extern std::string g_mysql_username;
extern std::string g_mysql_password;
extern std::string g_mysql_dbname;
extern size_t g_mysql_min_connection;
extern size_t g_mysql_max_connection;
extern size_t g_mysql_min_alive_minutes;
extern size_t g_mysql_max_alive_minutes;
extern size_t g_mysql_min_free_minutes;
extern size_t g_mysql_max_free_minutes;

//Minio
extern std::string g_minio_endpoint;
extern std::string g_minio_access_key;
extern std::string g_minio_secret_key;
extern std::string g_minio_region;
extern std::string g_minio_bucket;
extern bool g_minio_use_virtual_host;

//Public Access
extern std::string g_public_url_prefix;
extern std::string g_upload_public_host;

//Timeout
extern int g_mysql_connect_timeout;
extern int g_redis_connect_timeout;
extern int g_redis_socket_timeout;
extern int g_http_connect_timeout;
extern int g_http_total_timeout;

extern std::shared_mutex mutex_local_sessions;
extern std::unordered_map<int64_t, std::weak_ptr<session>> local_sessions;
extern std::string jwt_secret;

void loadServerConfig(const std::string& configPath = "./config/ServerConfig.conf");

class GlobalTools
{
public:
    static std::string sha256(const std::string& input);
    static int64_t verifyAccessToken(const std::string& accessToken);
};
