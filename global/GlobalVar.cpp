#include "GlobalVar.h"

std::atomic<bool> flag_shutdown = false;
SnowflakeGen sfGen;
std::unordered_map<int64_t, std::shared_ptr<userInfo>> onlineUser;
std::atomic<int64_t> sid = 10000000000;
std::string jwt_secret;

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

