#include "SQLPool.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

Logger SQLPool::logger("sqlpool");

SQLPool::SQLPool()
    :maxconn_size(0),
    minconn_size(0),
    max_alive(0),
    min_alive(0),
    max_free(0),
    min_free(0)
{
    loadConfig("./config", 2);
    for(size_t i = 0;i < this->minconn_size; i++)
    {
	if(!createSQL(5))
	    throw std::runtime_error("createSQL Error exit");
    }
    this->thread_manager = std::thread(&SQLPool::manage,this);
}

SQLPool::~SQLPool()
{
    this->cv.notify_all();
    this->cv_manage.notify_all();

    if(this->thread_manager.joinable())
	this->thread_manager.join();

    std::unique_lock<std::mutex> lock(this->mutex_ListCon);
    if(!this->cv.wait_for(lock,std::chrono::seconds(10),[this](){
	return this->list_busyCon.empty();
	    }))
    {
	std::cerr << "~SQLPool error\n";
    }
    lock.unlock();

    while (!this->list_con.empty()) 
    {
	delete this->list_con.front();
	this->list_con.pop();
    }
    for (auto* conn : list_busyCon) {
        delete conn;
    }
    list_busyCon.clear();
}

void SQLPool::loadConfig(const char* filePath, size_t retryTime)
{
    std::ifstream file;
    while(!flag_shutdown)
    {
	file.open(std::string(filePath) + "/SQLPoolConfig.conf");
	if(file.is_open())
	    break;
	writeConfig(filePath);
	if(retryTime == 0 && !file.is_open())
	    throw std::runtime_error("Open SQLPool Config Error");
	else
	    std::this_thread::sleep_for(std::chrono::milliseconds(500));
	file.clear();
	retryTime--;
    }

    std::string line;
    while(std::getline(file,line))
    {
	size_t pos = line.find('=');
	if(pos == std::string::npos)
	    continue;
	std::string key = line.substr(0,pos);
	std::string value = line.substr(pos+1);
	trim(key);
	trim(value);
	if(key == "host")
	    this->host = value;
	else if(key == "port")
	    this->port = std::stoi(value);
	else if(key == "username")
	    this->username = value;
	else if(key == "password")
	    this->password = value;
	else if(key == "dbname")
	    this->dbname = value;
	else if(key == "min_connection")
	    this->minconn_size = std::stol(value);
	else if(key == "max_connection")
	    this->maxconn_size = std::stoul(value);
	else if(key == "max_aliveMinutes")
	    this->max_alive = std::stoul(value);
	else if(key == "min_aliveMinutes")
	    this->min_alive = std::stoul(value);
	else if(key == "max_freeTimeMinutes")
	    this->max_free = std::stoul(value);
	else if(key == "min_freeTimeMinutes")
	    this->min_free = std::stoul(value);
    }
    if(this->host.empty() || 
	    this->username.empty() || 
	    this->password.empty() || 
	    this->dbname.empty() || 
	    this->minconn_size == 0 || 
	    this->maxconn_size == 0 ||
	    this->max_alive == 0 ||
	    this->min_alive == 0 ||
	    this->max_free == 0 ||
	    this->min_free == 0)
	throw std::runtime_error("load config error");
    file.close();
    if(this->maxconn_size < this->minconn_size || this->max_alive < this->min_alive || this->max_free < this->min_free)
	throw std::runtime_error("config error");
}

void SQLPool::writeConfig(const char* filePath)
{
    std::filesystem::create_directories(filePath);
    std::ofstream ofs(std::string(filePath) + "/SQLPoolConfig.conf", std::ios::out | std::ios::trunc);
    if(!ofs.is_open())
    {
	throw std::runtime_error("Create SQLPool COnfig Error");
	return;
    }
    ofs << "host = 192.168.88.1\r\n"; 
    ofs << "port = 3306\r\n"; 
    ofs << "username = ChatFreely\r\n"; 
    ofs << "password = ChatFreely\r\n"; 
    ofs << "dbname = ChatFreely\r\n"; 
    ofs << "min_connection = 3\r\n"; 
    ofs << "max_connection = 10\r\n"; 
    ofs << "min_aliveMinutes = 60\r\n"; 
    ofs << "max_aliveMinutes = 240\r\n";
    ofs << "min_freeTimeMinutes = 20\r\n"; 
    ofs << "max_freeTimeMinutes = 40\r\n";
    ofs.flush();
    ofs.close();
}

void SQLPool::trim(std::string& str)
{
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
    SQL* con = new SQL(alive(gen), free_time(gen));
    uint i = 0;
    bool isNormal = false;
    for(i = 0; i < retryTime + 1; i++)
    {
	isNormal = con->connectServer(this->host.c_str(), this->port, this->username.c_str(), this->password.c_str(), this->dbname.c_str(), 0);
	if(!isNormal || !con->ping())
	{
	    std::this_thread::sleep_for(std::chrono::seconds(1));
	    continue;
	}
	break;
    }
    std::lock_guard<std::mutex> lock(this->mutex_ListCon);
    if((i >= retryTime && (!isNormal || !con->ping())) || 
	    this->list_busyCon.size() + this->list_con.size() >= this->maxconn_size)
    {
	delete con;
	return false;
    }

    this->list_con.push(con);
    return true;
}

SQLPool* SQLPool::getSQLPool()
{
    static SQLPool pool;
    return &pool;
}

SQL* SQLPool::getConn()
{
    while(!flag_shutdown)
    {
	std::unique_lock<std::mutex> lock(this->mutex_ListCon);
	while(!this->list_con.empty())
	{
	    if(!this->list_con.front()->isAlive() || !this->list_con.front()->ping())
	    {
		delete this->list_con.front();
		this->list_con.pop();
	    }
	    else
		break;
	}
    
	if(!this->list_con.empty())
	{
	    SQL* conn = this->list_con.front();
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
	continue;
    }
    return nullptr;
}

void SQLPool::backConn(SQL* conn)
{
    if(!conn->isAlive() || !conn->ping())
    {
	std::lock_guard<std::mutex> lock(this->mutex_ListCon);
	this->list_busyCon.erase(conn);
	delete conn;
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
	    while(!this->list_con.empty() && (!this->list_con.front()->isAlive() || this->list_con.front()->isOverFree() || !this->list_con.front()->ping()))
	    {
		delete this->list_con.front();
		this->list_con.pop();
	    }
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
