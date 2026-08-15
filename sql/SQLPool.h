#pragma once
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <stdexcept>
#include <string>
#include <system_error>
#include "../global/GlobalVar.h"
#include "SQL.h"
#include "../logger/Logger.h"

class SQLPool
{

    static Logger logger;
public:
    static SQLPool* getSQLPool();
    std::shared_ptr<SQL> getConn();
    void backConn(std::shared_ptr<SQL> conn);
    ~SQLPool();
private:
    SQLPool();
    void loadConfig(const char* filePath, size_t retryTime);
    void writeConfig(const char* filePath);
    void initGlobalSeq();
    bool createSQL(uint retryTime);
    void trim(std::string& str);
    void manage();

    std::string host;
    unsigned short port;
    std::string username;
    std::string password;
    std::string dbname;
    size_t maxconn_size;
    size_t minconn_size;
    size_t max_alive;
    size_t min_alive;
    size_t max_free;
    size_t min_free;

    std::queue<std::shared_ptr<SQL>> list_con;
    std::unordered_set<std::shared_ptr<SQL>> list_busyCon;
    std::mutex mutex_ListCon;
    std::condition_variable cv;
    std::condition_variable cv_manage;

    std::thread thread_manager;
};
