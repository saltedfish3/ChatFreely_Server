#include "SQLPool.h"
#include "GlobalVar.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <queue>
#include <stdexcept>
#include <thread>

Logger SQLPool::logger("sqlpool");

SQLPool::SQLPool()
    :host(g_mysql_host),
    port(g_mysql_port),
    username(g_mysql_username),
    password(g_mysql_password),
    dbname(g_mysql_dbname),
    maxconn_size(g_mysql_max_connection),
    minconn_size(g_mysql_min_connection),
    max_alive(g_mysql_max_alive_minutes),
    min_alive(g_mysql_min_alive_minutes),
    max_free(g_mysql_max_free_minutes),
    min_free(g_mysql_min_free_minutes)
{
    for(size_t i = 0;i < this->minconn_size; i++)
    {
	if(!createSQL(1))
	    throw std::runtime_error("createSQL Error");
    }
    initGlobalSeq();
    this->thread_manager = std::thread(&SQLPool::manage,this);
}

SQLPool::~SQLPool()
{
    flag_shutdown.store(true, std::memory_order_release);

    this->cv.notify_all();
    this->cv_manage.notify_all();

    if(this->thread_manager.joinable())
	this->thread_manager.join();

    {
	std::unique_lock<std::mutex> lock(this->mutex_ListCon);
	while(!this->list_con.empty())
	{
	    this->list_con.pop();
	}
    }
    this->list_busyCon.clear();
}

void SQLPool::initGlobalSeq()
{
    std::shared_ptr<SQL> con = getConn();
    std::string sql = "INSERT IGNORE INTO global_seq(seq_name, current_value) VALUES('conv_sync', 0);";
    if(!con->executeSql(sql, nullptr))
    {
	this->backConn(con);
	throw std::runtime_error("初始化global_seq失败");
	return;
    }
    this->backConn(con);
}

void SQLPool::trim(std::string& str)
{
    if(str.empty())
	return;

    str.erase(str.begin(),std::find_if(str.begin(),str.end(),[](unsigned char ch){
	return !std::isspace(ch);
		}));

    str.erase(std::find_if(str.rbegin(),str.rend(),[](unsigned char ch){
	return !std::isspace(ch);
		}).base(), str.end());
}

bool SQLPool::createSQL(uint retryTime)
{
    thread_local static std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<int> alive(this->min_alive,this->max_alive);
    std::uniform_int_distribution<int> free_time(this->min_free,this->max_free);

    auto con = std::make_shared<SQL>(alive(gen), free_time(gen));

    bool isNormal = con->connectServer(this->host.c_str(), this->port, this->username.c_str(), this->password.c_str(), this->dbname.c_str(), 0);
    if(!isNormal || !con->ping())
	return false;

    std::lock_guard<std::mutex> lock(this->mutex_ListCon);
    if(this->list_busyCon.size() + this->list_con.size() >= this->maxconn_size)
	return false;

    this->list_con.push(con);
    return true;
}

SQLPool* SQLPool::getSQLPool()
{
    static SQLPool pool;
    return &pool;
}

std::shared_ptr<SQL> SQLPool::getConn()
{
    while(!flag_shutdown)
    {
	std::unique_lock<std::mutex> lock(this->mutex_ListCon);
	std::queue<std::shared_ptr<SQL>> valid_connections;

	while(!this->list_con.empty())
	{
	    auto conn =  this->list_con.front();
	    this->list_con.pop();

	    if(conn->isAlive() && conn->ping())
		valid_connections.push(conn);
	}
	this->list_con = std::move(valid_connections);
    
	if(!this->list_con.empty())
	{
	    auto conn = this->list_con.front();
	    this->list_con.pop();
	    this->list_busyCon.insert(conn);
	    return conn;
	}

	if(this->list_busyCon.size() + this->list_con.size() < this->maxconn_size)
	{
	    lock.unlock();
	    bool iscreate = createSQL(5);
	    lock.lock();
	    if(iscreate && !this->list_con.empty())
		continue;
	    else if(!iscreate)
		return nullptr;
	    continue;
	}

	this->cv.wait(lock, [this](){
	    return !this->list_con.empty() || (this->list_con.size() + this->list_busyCon.size()) < this->maxconn_size || flag_shutdown;
	    });
    }
    return nullptr;
}

void SQLPool::backConn(std::shared_ptr<SQL> conn)
{
    if(!conn->isAlive() || !conn->ping())
    {
	std::lock_guard<std::mutex> lock(this->mutex_ListCon);
	this->list_busyCon.erase(conn);
	cv.notify_one();
	return;
    }
    std::lock_guard<std::mutex> lock(this->mutex_ListCon);
    conn->setLastTimeBack();
    this->list_busyCon.erase(conn);
    this->list_con.push(conn);
    cv.notify_one();
}

void SQLPool::manage()
{
    while(!flag_shutdown)
    {
	{
            std::unique_lock<std::mutex> lock(this->mutex_ListCon);

            this->cv_manage.wait_for(lock, std::chrono::seconds(10), [this](){
                return flag_shutdown.load();
            });
        }
        if(flag_shutdown) 
	    break;
	//check alive/free time
	{
	    std::lock_guard<std::mutex> lock(this->mutex_ListCon);
	    std::queue<std::shared_ptr<SQL>> valid_connections;
	    while(!this->list_con.empty())
	    {
		auto conn = this->list_con.front();
		this->list_con.pop();

		if(conn->isAlive() && !conn->isOverFree() && conn->ping())
		    valid_connections.push(conn);
	    }
	    this->list_con = std::move(valid_connections);
	}

	//check < min_conn
	{
	    std::lock_guard<std::mutex> lock(this->mutex_ListCon);
	    if(this->list_con.size() >= this->minconn_size ||
		this->list_con.size() + this->list_busyCon.size() >= this->maxconn_size)
		continue;
	}
	if(!createSQL(5))
	    logger.warn("manager createSQL error");
    }
}
