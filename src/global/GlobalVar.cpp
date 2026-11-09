#include "GlobalVar.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>

std::atomic<bool> flag_shutdown = false;
SnowflakeGen sfGen;

std::string g_node_id = "node_1";
int g_listen_port = 9002;
int g_upload_port = 9003;
int g_worker_num = 4;
int g_upload_worker_num = 2;
std::string g_jwt_secret_file = "./secret/jwt_secret.key";

int g_thread_pool_size = 8;

std::string g_redis_host = "127.0.0.1";
int g_redis_port = 6379;
std::string g_redis_password;
int g_redis_pool_size = 4;

std::string g_mysql_host = "127.0.0.1";
int g_mysql_port = 3306;
std::string g_mysql_username = "root";
std::string g_mysql_password;
std::string g_mysql_dbname = "ChatFreely";
size_t g_mysql_min_connection = 3;
size_t g_mysql_max_connection = 10;
size_t g_mysql_min_alive_minutes = 60;
size_t g_mysql_max_alive_minutes = 240;
size_t g_mysql_min_free_minutes = 20;
size_t g_mysql_max_free_minutes = 40;

std::string g_minio_endpoint = "http://127.0.0.1:9000";
std::string g_minio_access_key = "minioadmin";
std::string g_minio_secret_key = "minioadmin";
std::string g_minio_region = "us-east-1";
std::string g_minio_bucket = "chat";
bool g_minio_use_virtual_host = false;

std::string g_public_url_prefix = "http://127.0.0.1/bucket/";
std::string g_upload_public_host = "127.0.0.1:9003";

int g_mysql_connect_timeout = 3;
int g_redis_connect_timeout = 1000;
int g_redis_socket_timeout = 1000;
int g_http_connect_timeout = 2;
int g_http_total_timeout = 5;

std::shared_mutex mutex_local_sessions;
std::unordered_map<int64_t, std::weak_ptr<session>> local_sessions;
std::shared_mutex mutex_idempotentCache;
std::unordered_map<std::string, idempotentRecord> idempotentCache;
std::string jwt_secret;

static void trim(std::string& str)
{
    if(str.empty())
	return;

    size_t first = str.find_first_not_of(" \t\r\n");
    if(first == std::string::npos)
    {
	str.clear();
	return;
    }

    size_t last = str.find_last_not_of(" \t\r\n");
    str = str.substr(first, (last - first + 1));
}

static void writeDefaultConfig(const std::string& path)
{
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream ofs(path, std::ios::trunc);
    if(!ofs.is_open())
	throw std::runtime_error("Create ServerConfig Error:" + path);

    ofs << "# ========== Server ==========\n";
    ofs << "node_id = " << g_node_id << "\n";
    ofs << "listen_port = " << g_listen_port << "\n";
    ofs << "upload_port = " << g_upload_port << "\n";
    ofs << "worker_num = " << g_worker_num << "\n";
    ofs << "upload_worker_num = " << g_upload_worker_num << "\n";
    ofs << "jwt_secret_file = " << g_jwt_secret_file << "\n\n";

    ofs << "# ========== ThreadPool ==========\n";
    ofs << "thread_pool_size = " << g_thread_pool_size << "\n\n";

    ofs << "# ========== Redis ==========\n";
    ofs << "redis_host = " << g_redis_host << "\n";
    ofs << "redis_port = " << g_redis_port << "\n";
    ofs << "redis_password = " << g_redis_password << "\n";
    ofs << "redis_pool_size = " << g_redis_pool_size << "\n\n";

    ofs << "# ========== Mysql ==========\n";
    ofs << "mysql_host = " << g_mysql_host << "\n";
    ofs << "mysql_port = " << g_mysql_port << "\n";
    ofs << "mysql_username = " << g_mysql_username << "\n";
    ofs << "mysql_password = " << g_mysql_password << "\n";
    ofs << "mysql_dbname = " << g_mysql_dbname << "\n";
    ofs << "mysql_min_connection = " << g_mysql_min_connection << "\n";
    ofs << "mysql_max_connection = " << g_mysql_max_connection << "\n";
    ofs << "mysql_min_alive_minutes = " << g_mysql_min_alive_minutes << "\n";
    ofs << "mysql_max_alive_minutes = " << g_mysql_max_alive_minutes << "\n";
    ofs << "mysql_min_free_minutes = " << g_mysql_min_free_minutes << "\n";
    ofs << "mysql_max_free_minutes = " << g_mysql_max_free_minutes << "\n\n";

    ofs << "# ========= Minio ==========\n";
    ofs << "minio_endpoint = " << g_minio_endpoint << "\n";
    ofs << "minio_access_key = " << g_minio_access_key << "\n";
    ofs << "minio_secret_key = " << g_minio_secret_key << "\n";
    ofs << "minio_region = " << g_minio_region << "\n";
    ofs << "minio_bucket = " << g_minio_bucket << "\n";
    ofs << "# 使用云厂商通常为 true\n";
    ofs << "minio_use_virtual_host = " << g_minio_use_virtual_host << "\n";

    ofs << "# ========== Public Access ==========\n";
    ofs << "public_url_prefix = " << g_public_url_prefix << "\n";
    ofs << "upload_public_host = " << g_upload_public_host << "\n\n";

    ofs << "# ========== Timeouts ==========\n";
    ofs << "mysql_connect_timeout = " << g_mysql_connect_timeout << "\n";
    ofs << "redis_connect_timeout = " << g_redis_connect_timeout << "\n";
    ofs << "redis_socket_timeout = " << g_redis_socket_timeout << "\n";
    ofs << "http_connect_timeout = " << g_http_connect_timeout << "\n";
    ofs << "http_total_timeout = " << g_http_total_timeout << "\n";
}

void loadServerConfig(const std::string& configPath)
{
    if(!std::filesystem::exists(configPath))
    {
	writeDefaultConfig(configPath);
	throw std::runtime_error("Default Config Created Success, If no changes are needed, you can just restart the program");
    }

    std::ifstream ifs(configPath);
    if(!ifs.is_open())
	throw std::runtime_error("Open ServerConfig Failed:" + configPath);

    std::string line;
    while(std::getline(ifs, line))
    {
	auto comment = line.find('#');
	if(comment != std::string::npos)
	    line = line.substr(0, comment);

	trim(line);
	if(line.empty())
	    continue;

	auto equal = line.find('=');
	if(equal == std::string::npos)
	    continue;

	std::string key = line.substr(0, equal);
	std::string value = line.substr(equal + 1);
	trim(key);
	trim(value);

	try
	{
	    if(key == "node_id")
		g_node_id = value;
	    else if(key == "listen_port")
		g_listen_port = std::stoi(value);
	    else if(key == "upload_port")
		g_upload_port = std::stoi(value);
	    else if(key == "worker_num")
		g_worker_num = std::stoi(value);
	    else if(key == "upload_worker_num")
		g_upload_worker_num = std::stoi(value);
	    else if(key == "jwt_secret_file")
		g_jwt_secret_file = value;
	    else if(key == "thread_pool_size")
		g_thread_pool_size = std::stoi(value);
	    else if(key == "redis_host")
		g_redis_host = value;
	    else if(key == "redis_port")
		g_redis_port = std::stoi(value);
	    else if(key == "redis_password")
		g_redis_password = value;
	    else if(key == "redis_pool_size")
		g_redis_pool_size = std::stoi(value);
	    else if(key == "mysql_host")
		g_mysql_host = value;
	    else if(key == "mysql_port")
		g_mysql_port = std::stoi(value);
	    else if(key == "mysql_username")
		g_mysql_username = value;
	    else if(key == "mysql_password")
		g_mysql_password = value;
	    else if(key == "mysql_dbname")
		g_mysql_dbname = value;
	    else if(key == "mysql_min_connection")
		g_mysql_min_connection = std::stoul(value);
	    else if(key == "mysql_max_connection")
		g_mysql_max_connection = std::stoul(value);
	    else if(key == "mysql_min_alive_minutes")
		g_mysql_min_alive_minutes = std::stoul(value);
	    else if(key == "mysql_max_alive_minutes")
		g_mysql_max_alive_minutes = std::stoul(value);
	    else if(key == "mysql_min_free_minutes")
		g_mysql_min_free_minutes = std::stoul(value);
	    else if(key == "mysql_max_free_minutes")
		g_mysql_max_free_minutes = std::stoul(value);
	    else if(key == "minio_endpoint")
		g_minio_endpoint = value;
	    else if(key == "minio_access_key")
		g_minio_access_key = value;
	    else if(key == "minio_secret_key")
		g_minio_secret_key = value;
	    else if(key == "minio_region")
		g_minio_region = value;
	    else if(key == "minio_bucket")
		g_minio_bucket = value;
	    else if(key == "minio_use_virtual_host")
		g_minio_use_virtual_host = value == "true" || value == "1";
	    else if(key == "public_url_prefix")
		g_public_url_prefix = value;
	    else if(key == "upload_public_host")
		g_upload_public_host = value;
	    else if(key == "mysql_connect_timeout")
		g_mysql_connect_timeout = std::stoi(value);
	    else if(key == "redis_connect_timeout")
		g_redis_connect_timeout = std::stoi(value);
	    else if(key == "redis_socket_timeout")
		g_redis_socket_timeout = std::stoi(value);
	    else if(key == "http_connect_timeout")
		g_http_connect_timeout = std::stoi(value);
	    else if(key == "http_total_timeout")
		g_http_total_timeout = std::stoi(value);
	}catch(const std::exception& e)
	{
	    throw std::runtime_error("Config parse error: key =" + key + " value =" + value + "(" + e.what() + ")");
	}
    }
    ifs.close();

    if(g_node_id.empty())
	throw std::runtime_error("Config: node_id is empty");
    if(g_listen_port <= 0 || g_listen_port > 65535)
	throw std::runtime_error("Config: invalid listen_port");
    if(g_upload_port <= 0 || g_upload_port > 65535)
	throw std::runtime_error("Config: invalid upload_port");
    if(g_worker_num <= 0)
	throw std::runtime_error("Config: worker_num must be > 0");
    if(g_thread_pool_size <= 0)
	throw std::runtime_error("Config: thread_pool_size must be > 0");

    if(g_redis_host.empty() || g_redis_port <= 0)
	throw std::runtime_error("Config: invalid redis config");
    if(g_redis_pool_size <= 0)
	throw std::runtime_error("Config: redis_pool_size must be > 0");

    if(g_mysql_host.empty() || g_mysql_port <= 0 || g_mysql_username.empty() || g_mysql_dbname.empty())
	throw std::runtime_error("Config: invalid mysql config");

    if(g_mysql_min_connection == 0 || g_mysql_max_connection == 0 || g_mysql_max_connection < g_mysql_min_connection)
	throw std::runtime_error("Config: mysql connection range invalid");
    if(g_mysql_min_alive_minutes == 0 || g_mysql_max_alive_minutes == 0 || g_mysql_max_alive_minutes < g_mysql_min_alive_minutes)
	throw std::runtime_error("Config: mysql alive range invalid");

    if(g_mysql_min_free_minutes == 0 || g_mysql_max_free_minutes == 0 || g_mysql_max_free_minutes < g_mysql_min_free_minutes)
	throw std::runtime_error("Config: mysql free range invalid");

    if(g_minio_endpoint.empty() || g_minio_access_key.empty() || g_minio_secret_key.empty() || g_minio_bucket.empty())
	throw std::runtime_error("Config: invalid minio config");
    if(g_minio_endpoint.rfind("http://", 0) != 0 && g_minio_endpoint.rfind("https://", 0) != 0)
	throw std::runtime_error("Config: minio_endpoint must start with http:// or https://");

    while(!g_minio_endpoint.empty() && g_minio_endpoint.back() == '/')
	g_minio_endpoint.pop_back();

    if(g_public_url_prefix.empty() || g_upload_public_host.empty())
	throw std::runtime_error("Config: public_url_prefix / upload_public_host empty");

    if(g_public_url_prefix.back() != '/')
	g_public_url_prefix += '/';
}

int64_t GlobalTools::verifyAccessToken(const std::string& accessToken)
{
    try
    {
	auto decoded = jwt::decode( accessToken);

	auto verifyier = jwt::verify()
	    .allow_algorithm(jwt::algorithm::hs256{jwt_secret})
	    .with_issuer(AppName);
	    verifyier.verify(decoded);

	std::string uid_str = decoded.get_subject();
	return std::stoll(uid_str);
    }catch(const std::exception& e)
    {
	return -1;
    }
}

std::string GlobalTools::sha256(const std::string& input)
{
    unsigned char hash[crypto_hash_sha256_BYTES];
    if(crypto_hash_sha256(hash, reinterpret_cast<const unsigned char*>(input.data()), input.size()) != 0)
	return {};
    char hex[65];
    sodium_bin2hex(hex, sizeof(hex), hash, sizeof(hash));
    return std::string(hex, 64);
}
