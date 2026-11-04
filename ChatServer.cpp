#include "curl/curl.h"
#include "network/ChatServerListener.h"
#include "global/GlobalVar.h"
#include "redis/RedisClient.h"
#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <netinet/in.h>
#include <sodium/randombytes.h>
#include <sodium/utils.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>

Logger logger("main");

static void printHelp(const char* argv0)
{
    std::ostringstream oss;
    oss << "Help:\n  " << argv0 << " [-c configPath]\n  "
	<< argv0 << " --gen-jwt-secret <path>\n\nOptions:\n"
	<< "  -c <path> \t\t指定配置文件(默认./config/ServerConfig.conf)\n" << "  --gen-jwt-secret <p> \t\t生成 JWT secret 文件并退出\n"
	<< "  -h, --help \t\t显示帮助\n";

    logger.info(oss.str());
}

static void generateJwtSecret(const std::string& filePath)
{
    if(std::filesystem::exists(filePath))
	throw std::runtime_error("File already exists: " + filePath);

    unsigned char buf[32];
    randombytes_buf(buf, sizeof(buf));
    char hex[65];
    sodium_bin2hex(hex, sizeof(hex), buf, sizeof(buf));
    std::string secret(hex, 64);

    std::filesystem::create_directories(std::filesystem::path(filePath).parent_path());
    std::ofstream ofs(filePath, std::ios::trunc);
    if(!ofs.is_open())
	throw std::runtime_error("Create JWT secret failed: " + filePath);
    ofs << secret;
    ofs.close();

    chmod(filePath.c_str(), S_IRUSR | S_IWUSR);

    logger.info("JWT secret generated: " + filePath);
}

static bool checkTcpPort(const std::string& ip, int port, int timeout_sec)
{
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0)
	return false;

    int flag = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flag | O_NONBLOCK);

    int res = connect(sock, (sockaddr*)&addr, sizeof(addr));
    if(res < 0 && errno == EINPROGRESS)
    {
	fd_set fdset;
	FD_ZERO(&fdset);
	FD_SET(sock, &fdset);

	timeval tv;
	tv.tv_sec = timeout_sec;
	tv.tv_usec = 0;

	res = select(sock + 1, NULL, &fdset, NULL, &tv);
	if(res <= 0)
	{
	    close(sock);
	    return false;
	}
	else
	{
	    int so_error;
	    socklen_t len = sizeof(so_error);
	    getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
	    if(so_error != 0)
	    {
		close(sock);
		return false;
	    }
	}
    }
    else if(res < 0)
    {
	close(sock);
	return false;
    }
    close(sock);
    return true;
}

int main(int argc, char* argv[])
{
    std::string configPath = "./config/ServerConfig.conf";
    std::string genSecretPath;

    for(int i = 1; i < argc; i++)
    {
	std::string arg = argv[i];
	if(arg == "-c" && i + 1 < argc)
	    configPath = argv[++i];
	else if(arg == "--gen-jwt-secret" && i + 1 < argc)
	    genSecretPath = argv[++i];
	else if(arg == "-h" || arg == "--help")
	{
	    printHelp(argv[0]);
	    return 0;
	}
	else
	{
	    logger.error("Unknown argument: " + arg);
	    printHelp(argv[0]);
	    return 1;
	}
    }

    if(!genSecretPath.empty())
    {
	try
	{
	    generateJwtSecret(genSecretPath);
	    return 0;
	}catch(const std::exception& e)
	{
	    logger.error(std::string("Generate JWT secret failed: ") + e.what());
	    return 1;
	}
    }

    try
    {
	loadServerConfig(configPath);
    }catch(const std::exception& e)
    {
	logger.error(std::string("Load config failed: ") + e.what());
	return 1;
    }

    if(!checkTcpPort(g_mysql_host, g_mysql_port, g_mysql_connect_timeout))
    {
	logger.error("Start failed: MySQL (" + g_mysql_host + ":" + std::to_string(g_mysql_port) + ") is unreachable");
	return 1;
    }

    if(!checkTcpPort(g_redis_host, g_redis_port, g_redis_connect_timeout / 1000))
    {
	logger.error("Start failed: Redis (" + g_redis_host + ":" + std::to_string(g_redis_port) + ") is unreachable");
	return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    try
    {
	if(!RedisClient::getClient().init(g_redis_host, g_redis_port, g_redis_password, g_redis_pool_size))
	{
	    logger.error("Redis Init failed");
	    curl_global_cleanup();
	    return 1;
	}
	RedisClient::getClient().startSubscriber();
    }catch(const std::exception& e)
    {
	logger.error(std::string("Redis init exception: ") + e.what());
	curl_global_cleanup();
	return 1;
    }

    int ret = 0;
    try
    {
	ChatServerListener csl(g_listen_port, g_worker_num, g_upload_worker_num);
	csl.start();
    }catch(const std::exception& e)
    {
	logger.error(std::string("Start Failed: ") + e.what());
	ret = 1;
    }

    RedisClient::getClient().stopSubscriber();
    curl_global_cleanup();
    return ret;
}
