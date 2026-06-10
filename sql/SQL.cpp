#include "SQL.h"

SQL::SQL(uint minutes_timeAlive, uint minutes_timeFree)
    :res(nullptr),
    time_create(std::chrono::steady_clock::now()),
    time_lastBack(std::chrono::steady_clock::now()),
    time_alive(time_create + std::chrono::minutes(minutes_timeAlive)),
    time_free(time_lastBack + std::chrono::minutes(minutes_timeFree)),
    freeTime(minutes_timeFree),
    isConnect(false)
{
    this->mysql = mysql_init(nullptr);
    if(!this->mysql)
	throw std::runtime_error("Init Mysql Error");
    this->stmt = mysql_stmt_init(this->mysql);
    if(!this->stmt)
	throw std::runtime_error("Init Stmt Error");
    mysql_autocommit(this->mysql, 1);
}

SQL::~SQL()
{
    if(this->res)
    {
	mysql_free_result(this->res);
	this->res = nullptr;
    }
    if(this->stmt)
    {
	mysql_stmt_close(this->stmt);
	this->stmt = nullptr;
    }
    if(this->mysql)
    {
	mysql_close(this->mysql);
	this->mysql = nullptr;
    }
}


void SQL::AllReInit()
{
    if(this->stmt)
    {
	mysql_stmt_close(this->stmt);
	this->stmt = nullptr;
    }
    if(this->mysql)
    {
	mysql_close(this->mysql);
	this->mysql = nullptr;
    }
    this->mysql = mysql_init(nullptr);
    if(!this->mysql)
	throw std::runtime_error("Init Mysql Error");

    this->stmt = mysql_stmt_init(this->mysql);
    if(!this->stmt)
	throw std::runtime_error("Init Stmt Error");
    this->isConnect = false; 
}

bool SQL::connectServer(const char* host, unsigned short port, const char* username, const char* password, const char* db, unsigned long clientFlag)
{
    try
    {
	if(this->isConnect)
	    AllReInit();

	if(mysql_real_connect(this->mysql, host, username, password, db, port, nullptr, clientFlag) == nullptr)
	{
	    AllReInit();
	    return false;
	}
    }catch(const std::runtime_error&)
    {
	this->isConnect = false;
	return false;
    }
    this->isConnect = true;
    return true;
}

bool SQL::executeSql(std::string& sql, MYSQL_BIND* bind_param, MYSQL_BIND* bind_result)
{
    try
    {
	mysql_stmt_free_result(this->stmt);
	if(mysql_stmt_reset(this->stmt) != 0)
	{
	    AllReInit();
	    return false;
	}
	if(mysql_stmt_prepare(this->stmt, sql.c_str(), sql.size()) != 0)
	{
	    if(mysql_stmt_reset(this->stmt) != 0)
		AllReInit();
	    return false;
	}
	if(bind_param)
	{
	    if(mysql_stmt_bind_param(this->stmt, bind_param) != 0)
	    {
		if(mysql_stmt_reset(this->stmt) != 0)
		    AllReInit();
		return false;
	    }
	}
	if(mysql_stmt_execute(this->stmt) != 0)
	{
	    if(mysql_stmt_reset(this->stmt) != 0)
		AllReInit();
	    return false;
	}
	
	//handle data
	if(bind_result && mysql_stmt_field_count(this->stmt) > 0)
	{
	    if(mysql_stmt_bind_result(this->stmt, bind_result) != 0)
	    {
		if(mysql_stmt_reset(this->stmt) != 0)
		    AllReInit();
		return false;
	    }
	}
    }catch(const std::runtime_error&)
    {
	this->isConnect = false;
	return false;
    }
    return true;
}

int SQL::nextData()
{
    if(!this->isConnect || !this->stmt || !this->mysql)
    {
	return -1;
    }
    int status = mysql_stmt_fetch(this->stmt);
    if(status == 0)
    {
	return 1;
    }
    else if(status == MYSQL_NO_DATA)
    {
	return 0;
    }
    else if(status == 1)
    {
	throw std::runtime_error("Mysql Next Data status Error:" + std::to_string(status));
	return -1;
    }
    return -1;
}

my_ulonglong SQL::getAffectRows()
{
    if(!this->isConnect || !this->stmt || !this->mysql)
	return (my_ulonglong)-2;
    return mysql_stmt_affected_rows(this->stmt);
}

my_ulonglong SQL::getInsertID()
{
    if(!this->isConnect || !this->stmt || !this->mysql)
	return (my_ulonglong)-2;
    return mysql_stmt_insert_id(this->stmt);
}

const char* SQL::getMysqlError()
{
    if(!this->isConnect || !this->mysql)
	return "";
    return mysql_error(this->mysql);
}

const char* SQL::getSQLError()
{
    if(!this->isConnect || !this->mysql || !this->stmt)
	return "";
    return mysql_stmt_error(this->stmt);
}

void SQL::setLastTimeBack()
{
    this->time_lastBack = std::chrono::steady_clock::now();
    this->time_free = std::chrono::steady_clock::now() + std::chrono::minutes(this->freeTime);
}

bool SQL::isAlive()
{
    return std::chrono::steady_clock::now() < this->time_alive;
}

bool SQL::isOverFree()
{
    return std::chrono::steady_clock::now() > this->time_free;
}

bool SQL::ping()
{
    if(!this->mysql)
	return false;
    return !mysql_ping(this->mysql);
}
