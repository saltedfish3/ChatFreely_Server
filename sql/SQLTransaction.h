#pragma once
#include <mysql/mysql.h>

class SQLTransaction
{
public:
    SQLTransaction(MYSQL* mysql);
    ~SQLTransaction();
    void commit();
private:
    MYSQL* mysql;
    bool isCommit = false;

};
