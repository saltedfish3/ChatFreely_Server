#include "GlobalVar.h"

std::atomic<bool> flag_shutdown = false;
SnowflakeGen sfGen;
std::unordered_map<std::string, std::shared_ptr<MySQL>> mysql;
std::unordered_map<int64_t, std::shared_ptr<userInfo>> onlineUser;
std::atomic<int64_t> sid = 10000000000;
