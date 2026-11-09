#include "curl/curl.h"
#include "miniocpp/args.h"
#include "miniocpp/client.h"
#include "miniocpp/providers.h"
#include "miniocpp/request.h"
#include "network/ChatServerListener.h"
#include "global/GlobalVar.h"
#include "redis/RedisClient.h"
#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <sodium/randombytes.h>
#include <sodium/utils.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>

struct HostPort
{
    std::string host;
    int port = 0;
    bool https = false;
};

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

static bool checkTcpPort(const std::string& host, int port, int timeout_sec)
{
    addrinfo info;
    info.ai_family = AF_UNSPEC;
    info.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if(getaddrinfo(host.c_str(), portStr.c_str(), &info, &res) != 0 || !res)
	return false;

    bool ok = false;
    for(addrinfo* p = res; p != nullptr; p = p->ai_next)
    {
	int sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
	if(sock < 0)
	    continue;

	int flag = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flag | O_NONBLOCK);

	int r = connect(sock, p->ai_addr, p->ai_addrlen);
	if(r < 0 && errno == EINPROGRESS)
	{
	    fd_set fdset;
	    FD_ZERO(&fdset);
	    FD_SET(sock, &fdset);

	    timeval tv;
	    tv.tv_sec = timeout_sec;
	    tv.tv_usec = 0;

	    r = select(sock + 1, NULL, &fdset, NULL, &tv);
	    if(r > 0)
	    {
		int so_error;
		socklen_t len = sizeof(so_error);
		getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
		if(so_error == 0)
		    ok = true;
	    }
	}
	else if(r == 0)
	    ok = true;

	close(sock);
    }

    freeaddrinfo(res);
    return ok;
}

static HostPort parseUrlToHostPort(const std::string& url, int defaultPort = 0)
{
    HostPort hp;
    std::string url_copy = url;

    size_t pos_pre = url_copy.find("://");
    if(pos_pre != std::string::npos)
    {
	std::string preStr = url_copy.substr(0, pos_pre);
	for(char& c : preStr)
	    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

	hp.https = (preStr == "https");
	url_copy = url_copy.substr(pos_pre + 3);
    }

    size_t pos_path = url_copy.find('/');
    if(pos_path != std::string::npos)
	url_copy = url_copy.substr(0, pos_path);

    size_t pos_at = url_copy.find('@');
    if(pos_at != std::string::npos)
	url_copy = url_copy.substr(pos_at + 1);

    if(!url_copy.empty() && url_copy.front() == '[')
    {
	size_t end = url_copy.find(']');
	if(end != std::string::npos)
	{
	    hp.host = url_copy.substr(1, end - 1);
	    if(end + 1 < url_copy.size() && url_copy[end + 1] == ':')
		hp.port = std::stoi(url_copy.substr(end + 2));
	}
    }
    else
    {
	size_t pos_colon = url_copy.find(':');
	if(pos_colon != std::string::npos && url_copy.find(':') == pos_colon)
	{
	    hp.host = url_copy.substr(0, pos_colon);
	    hp.port = std::stoi(url_copy.substr(pos_colon + 1));
	}
	else
	    hp.host = url_copy;
    }

    if(hp.port == 0)
	hp.port = defaultPort > 0 ? defaultPort : (hp.https ? 443 : 80);

    return hp;
}

static bool checkMinioReady(const std::string& endpoint, const std::string& accessKey, const std::string& secretKey, const std::string& bucket, std::string& outErrMsg)
{
    try
    {
	std::string virtualEndpoint = endpoint;
	while(!virtualEndpoint.empty() && virtualEndpoint.back() == '/')
	    virtualEndpoint.pop_back();

	bool isHttps = (virtualEndpoint.rfind("https://", 0) == 0) ? true : false;

	minio::s3::BaseUrl BaseUrl(virtualEndpoint, isHttps, g_minio_region);
	BaseUrl.virtual_style = g_minio_use_virtual_host;

	minio::creds::StaticProvider provider(accessKey, secretKey);
	minio::s3::Client client(BaseUrl, &provider);

	minio::s3::BucketExistsArgs args;
	args.bucket = bucket;
	if(!g_minio_region.empty())
	    args.region = g_minio_region;

	auto resp = client.BucketExists(args);

	if(resp)
	    return true;

	const int http = resp.status_code;
	const std::string& code = resp.code;

	if(http == 404 || code == "NoSuchBucket")
	    return true;

	if(http == 403 || code == "AccessDenied")
	{
	    outErrMsg = "AccessDenied: endpoint reachable but credentials lack permission";
	    return false;
	}

	if(http == 301 || http == 307 || code == "PermanentRedirect" || code == "AuthorizationHeaderMalformed")
	{
	    outErrMsg = "region/endpoint mismatch (HTTP " + std::to_string(http) + ", " + code + ")";
	    return false;
	}

	if(http == 0)
	    outErrMsg = resp.Error().String();
	else
	    outErrMsg = (code.empty() ? std::string("UnknownError") : code) + " (HTTP " + std::to_string(http) + "): " + resp.message;

	return false;

    }catch(const std::exception& e)
    {
	outErrMsg = e.what();
	return false;
    }
}

int main(int argc, char* argv[])
{
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    std::string configPath = ec ? "./config/ServerConfig.conf" : (exe.parent_path() / "config" / "ServerConfig.conf").lexically_normal().string();
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

    try
    {
	if(g_minio_endpoint.empty())
	{
	    logger.error("Start failed: minio_endpoint is empty");
	    return 1;
	}

	HostPort hp = parseUrlToHostPort(g_minio_endpoint);

	if(!checkTcpPort(hp.host, hp.port, g_http_connect_timeout))
	{
	    logger.error("Start failed: MinIO (" + hp.host + ":" + std::to_string(hp.port) + ")");
	    return 1;
	}

	std::string err;
	if(!checkMinioReady(g_minio_endpoint, g_minio_access_key, g_minio_secret_key, g_minio_bucket, err))
	{
	    logger.error("Start failed: MinIO check failed (" + g_minio_endpoint + ", bucket = " + g_minio_bucket + "):" + err);
	    return 1;
	}
    }catch(const std::exception& e)
    {
	logger.error(std::string("Start failed: MinIO endpoint config parse failed: ") + e.what() + std::string("; g_minio_endpoint = ") + g_minio_endpoint);
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
