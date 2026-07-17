#pragma once
#include <atomic>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <memory>
#include <optional>
#include <jwt-cpp/jwt.h>
#include <sodium/crypto_hash_sha256.h>
#include <sodium/utils.h>
#include "../third_party/snowflake.hpp"

#define SPort 9000
#define UploadPort 9001
#define AppName "ChatFreely"

extern std::atomic<bool> flag_shutdown;
using SnowflakeGen = snowflake<1704067200000LL, std::mutex>;
extern SnowflakeGen sfGen;

struct userInfo {
    std::string username;
    std::string email;
};

extern std::unordered_map<int64_t, std::shared_ptr<userInfo>> onlineUser;
extern std::atomic<int64_t> sid;
extern std::string jwt_secret;
class GlobalTools
{
public:
    static std::string sha256(const std::string& input);
    static int64_t verifyAccessToken(const std::string& accessToken);
};
