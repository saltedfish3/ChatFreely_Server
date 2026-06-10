#pragma once
#include <mysql/mysql.h>
#include <string>
#include <chrono>
#include <stdexcept>

class SQL
{
public:
    SQL(uint minutes_timeAlive, uint minutes_timeFree);
    ~SQL();
    SQL(const SQL&) = delete;
    SQL& operator=(const SQL&) = delete;

    bool connectServer(const char* host, unsigned short port, const char* username, const char* password, const char* db, unsigned long clientFlag);

    bool executeSql(std::string& sql, MYSQL_BIND* bind_param, MYSQL_BIND* bind_result = nullptr);

    int nextData();
    my_ulonglong getAffectRows();
    my_ulonglong getInsertID();
    
    bool ping();

    const char* getMysqlError();
    const char* getSQLError();

    void setLastTimeBack();
    bool isAlive();
    bool isOverFree();
private:
    MYSQL* mysql;
    MYSQL_STMT* stmt;
    MYSQL_RES* res;

    std::chrono::time_point<std::chrono::steady_clock> time_create;
    std::chrono::time_point<std::chrono::steady_clock> time_lastBack;
    std::chrono::time_point<std::chrono::steady_clock> time_alive;
    std::chrono::time_point<std::chrono::steady_clock> time_free;
    int freeTime;

    bool isConnect;

    void AllReInit();
    
};
