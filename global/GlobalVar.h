#pragma once
#include <atomic>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <memory>
#include <optional>
#include "../third_party/snowflake.hpp"

extern std::atomic<bool> flag_shutdown;
using SnowflakeGen = snowflake<1704067200000LL, std::mutex>;
extern SnowflakeGen sfGen;
struct MySQL {
    int64_t uid;
    int64_t sid;
    std::string email;
    std::string password;
    std::string username;
};
struct userInfo {
    std::string username;
    std::string email;
};

extern std::unordered_map<std::string, std::shared_ptr<MySQL>> mysql;
extern std::unordered_map<int64_t, std::shared_ptr<userInfo>> onlineUser;
extern std::atomic<int64_t> sid;
