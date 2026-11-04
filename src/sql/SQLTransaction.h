#pragma once
#include <mysql/mysql.h>
#include "../logger/Logger.h"

class SQLTransaction
{
    static Logger logger;
public:
    SQLTransaction(MYSQL* mysql);
    ~SQLTransaction();
    void commit();
private:
    MYSQL* mysql;
    bool isCommit = false;

};
