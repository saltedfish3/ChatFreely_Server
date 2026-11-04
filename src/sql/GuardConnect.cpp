#include "GuardConnect.h"

GuardConnection::GuardConnection()
    :con(nullptr)
{

}

GuardConnection::GuardConnection(SQLPool* pool)
{
    if(pool)
	this->con = pool->getConn();
}

GuardConnection::GuardConnection(GuardConnection&& other)
    :con(other.con)
{
    other.con = nullptr;
}

GuardConnection& GuardConnection::operator=(GuardConnection&& other)
{
    if(this != &other)
    {
	release();
	this->con = other.con;
	other.con = nullptr;
    }
    return *this;
}

GuardConnection::~GuardConnection()
{
    release();
}

GuardConnection::operator bool() const
{
    return this->con != nullptr;
}

void GuardConnection::release()
{
    if(this->con)
    {
	SQLPool::getSQLPool()->backConn(this->con);
	this->con = nullptr;
    }
}

SQL* GuardConnection::get() const
{
    return this->con.get();
}

SQL* GuardConnection::operator->() const
{
    return this->con.get();
}

SQL& GuardConnection::operator*() const
{
    return *(this->con);
}
