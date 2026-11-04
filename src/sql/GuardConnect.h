#pragma once
#include "SQLPool.h"
#include "SQL.h"
#include <memory>

class GuardConnection
{
public:
    GuardConnection();
    GuardConnection(SQLPool* pool);
    GuardConnection(GuardConnection&& other);
    GuardConnection& operator=(GuardConnection&& other);

    GuardConnection(const GuardConnection&) = delete;
    GuardConnection& operator=(const GuardConnection&) = delete;

    ~GuardConnection();
    SQL* get() const;
    SQL* operator->() const;
    SQL& operator*() const;

    explicit operator bool() const;
    void release();

private:
    std::shared_ptr<SQL> con;

};
