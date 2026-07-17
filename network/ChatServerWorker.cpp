#include "ChatServerWorker.h"
#include <algorithm>
#include <bits/types/struct_timeval.h>
#include <chrono>
#include <cstring>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/util.h>
#include <exception>
#include <functional>
#include <jwt-cpp/jwt.h>
#include <memory>
#include <sodium/core.h>
#include <sodium/crypto_hash_sha256.h>
#include <sodium/crypto_pwhash.h>
#include <sodium/randombytes.h>
#include <sodium/utils.h>
#include <stdexcept>
#include <string>

Logger ChatServerWorker::logger("worker");

ChatServerWorker::ChatServerWorker()
{
    this->base = event_base_new();
    if(!base)
	throw std::runtime_error("Worker Create Base Error\n");
    this->is_run = false;
    if(sodium_init() < 0)
	throw std::runtime_error("init sodium error");
}

ChatServerWorker::~ChatServerWorker()
{
    this->stop();
    if(this->base)
    {
	event_base_free(this->base);
	this->base = nullptr;
    }
}

bool ChatServerWorker::isRun()
{
    return this->is_run;
}

void ChatServerWorker::run()
{
    if(!is_run)
    {
	this->is_run = true;
	this->myThread = std::thread(&ChatServerWorker::listen,this);
    }
}

void ChatServerWorker::stop()
{
    if(this->is_run && this->base != nullptr)
    {
	event_base_loopbreak(base);
	if(this->myThread.joinable())
	{
	    this->myThread.join();
	    this->is_run = false;
	}
    }
}

void ChatServerWorker::addConnect(int fd)
{
    if(!base)
    {
	logger.error("Base Error");
	exit(1);
    }
    evutil_make_socket_nonblocking(fd);
    bufferevent* bev = bufferevent_socket_new(this->base, fd, BEV_OPT_CLOSE_ON_FREE);
    if(!bev)
    {
	logger.error("addConnect Error");
	evutil_closesocket(fd);
	return;
    }
    std::shared_ptr<session> sess = std::make_shared<session>();
    sess->myself = this;
    sess->bev = bev;
    this->list_session.emplace(fd,sess);
    auto* weak_sess = new std::weak_ptr<session>(sess);
    bufferevent_setcb(bev, cb_read, cb_write, cb_event, weak_sess);
    timeval tv = {30,0};
    bufferevent_set_timeouts(bev, &tv, nullptr);
    bufferevent_enable(bev, EV_READ);
}

void ChatServerWorker::listen()
{
    if(this->base)
    {
	timeval tv = {1,0};
	while(!flag_shutdown)
	{
	    event_base_loopexit(this->base, &tv);
	    event_base_dispatch(this->base);
	}
	event_base_loop(base, EVLOOP_NONBLOCK);
    }
}

void ChatServerWorker::cb_read(bufferevent* bev, void* arg)
{
    //auto sess = (static_cast<session*>(arg))->shared_from_this();
    auto* weak_sess = static_cast<std::weak_ptr<session>*>(arg);
    auto sess = weak_sess->lock();
    if(!sess)
	return;
    evbuffer* evb = bufferevent_get_input(bev);
    char* line;
    while((line = evbuffer_readln(evb,nullptr,EVBUFFER_EOL_LF)))
    {
	unPack(line,sess.get());
	free(line);
    }
}

void ChatServerWorker::cb_write(bufferevent* bev, void* arg)
{
    //auto sess = (static_cast<session*>(arg))->shared_from_this();
    auto* weak_sess = static_cast<std::weak_ptr<session>*>(arg);
    auto sess = weak_sess->lock();
    if(!sess)
	return;
}

void ChatServerWorker::cb_event(bufferevent* bev, short events, void* arg)
{
    //auto sess = (static_cast<session*>(arg))->shared_from_this();
    auto* weak_sess = static_cast<std::weak_ptr<session>*>(arg);
    auto sess = weak_sess->lock();
    if(!sess)
	return;
    if(events & BEV_EVENT_EOF)
    {
	logger.info("Client Exit");
	if(sess->uid.has_value())
	    onlineUser.erase(sess->uid.value());
	int fd = bufferevent_getfd(bev);
	if(bev)
	    bufferevent_free(bev);
	sess->myself->list_session.erase(fd);	
	delete weak_sess;
    }
    else if(events & BEV_EVENT_ERROR)
    {
	logger.info(std::string("Connect Error:") + evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
	if(sess->uid.has_value())
	    onlineUser.erase(sess->uid.value());
	int fd = bufferevent_getfd(bev);
	if(bev)
	    bufferevent_free(bev);
	sess->myself->list_session.erase(fd);	
	delete weak_sess;
    }
    else if(events & BEV_EVENT_TIMEOUT)
    {
	logger.info("Client Timeout Exit");
	if(sess->uid.has_value())
	    onlineUser.erase(sess->uid.value());
	int fd = bufferevent_getfd(bev);
	if(bev)
	    bufferevent_free(bev);
	sess->myself->list_session.erase(fd);	
	delete weak_sess;
    }
}

void ChatServerWorker::unPack(const char* line_json, session* sess)
{
    auto sessPtr = sess->shared_from_this();
    json j = json::parse(line_json);
    
    if(!j.contains("Requests_id") || !j["Requests_id"].is_string())
    {
	logger.warn("Pack Requests_id Error");
	return;
    }
    std::ostringstream requests_id;
    requests_id << j.value("Requests_id", "");

    if(!j.contains("Type") || !j["Type"].is_string())
    {
	logger.warn("Pack Type Error");
	return;
    }
    std::string type = j.value("Type","");
    if(type == "Hello")
	handleHello(sessPtr->bev, requests_id.str());
    else if(type == "Login")
    {
	if(!j.contains("Email") || !j["Email"].is_string() || !j.contains("Password") || !j["Password"].is_string())
	{
	    logger.warn("Json No Email Or Password");
	    return;
	}
	std::string email = j.value("Email","");
	std::string password = j.value("Password","");
	if(email.empty() || password.empty())
	{
	    logger.warn("Json parse Email Or Password Empty");
	    return;
	}
	
	handleLogin(sess, email, password, requests_id.str());
    }
    else if(type == "Register")
    {
	//Register handle
	//
	if(!j.contains("Email") || !j["Email"].is_string() || !j.contains("Password") || !j["Password"].is_string() || !j.contains("Username") || !j["Username"].is_string())
	{
	    logger.warn("Json no Email Or Password Or Username");
	    return;
	}
	std::string email = j.value("Email","");
	std::string password = j.value("Password","");
	std::string username = j.value("Username","");
	if(email.empty() || password.empty() || username.empty())
	{
	    logger.warn("Json parse Email Or Password Or username Empty");
	    return;
	}
	handleRegister(sess, email, password, username, requests_id.str());
    }
    else if(type == "Heartbeat")
	handleHeartbeat(sessPtr->bev, requests_id.str());
    else if(type == "Send")
	//send handle
	;
    else if(type == "Update_Avatar")
    {
	if(!j.contains("Url") || !j["Url"].is_string())
	{
	    logger.warn("Json no Url");
	    return;
	}
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json no AccessToken");
	    return;
	}
	handleUploadAvatar(sess, j.value("Url", ""), j.value("AccessToken", ""), requests_id.str());
    }
    else if(type == "UnLogin")
    {
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json no AccessToken");
	    return;
	}
	handleUnLogin(sess, j.value("AccessToken", ""), requests_id.str());
    }
    else if(type == "UpdateUsername")
    {
	if(!j.contains("Username") || !j["Username"].is_string())
	{
	    logger.warn("Json no Username");
	    return;
	}
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json no AccessToken");
	    return;
	}
	handleUpdateUsername(sess, j.value("Username", ""), j.value("AccessToken", ""), requests_id.str());
    }
    else if(type == "AddNewFriendRequest")
    {
	if(!j.contains("UID") || !j["UID"].is_string())
	{
	    logger.warn("Json no UID");
	    return;
	}
	std::string receiver_info;
	if(!j.contains("Receiver_SID") || !j["Receiver_SID"].is_string())
	{
	    if(!j.contains("Receiver_Email") || !j["Receiver_Email"].is_string())
	    {
		logger.warn("Json no SID And Email");
		return;
	    }
	    else
		receiver_info = j.value("Receiver_Email", "");
	}
	else
	    receiver_info = j.value("Receiver_SID", "");
	std::string uid = j.value("UID", "");
	std::string verMsg;
	if(j.contains("VerMsg") && j["VerMsg"].is_string())
	    verMsg = j["VerMsg"];
	handleAddNewFriendRequest(sess, uid, receiver_info, verMsg, requests_id.str());
    }
    else if(type == "HandleNewFriendRequest")
    {
	if((!j.contains("UID") || !j["UID"].is_string()) && (!j.contains("HandleUID") || !j["HandleUID"].is_string()) && (!j.contains("Status") || j["Status"].is_boolean()))
	{
	    logger.warn("处理新好友申请缺少参数");
	    return;
	}
	std::string uid = j.value("UID", "");
	std::string handle_uid = j.value("HandleUID", "");
	bool isAgree = j.value("Status", false);
	handleHandleNewFriendRequest(sess, uid, handle_uid, isAgree, requests_id.str());
	
    }
    else if(type == "GetNewFriendRequestsList")
    {
	if(!j.contains("UID") || !j["UID"].is_string())
	{
	    logger.warn("获取新好友申请缺少参数");
	    return;
	}
	std::string uid = j.value("UID", "");
	handleGetNewFriendRequestsList(sess, uid, requests_id.str());
    }
    else if(type == "RefreshToken")
    {
	if(!j.contains("RefreshToken") || !j["RefreshToken"].is_string())
	{
	    logger.warn("Json No RefreshToken");
	    return;
	}
	handleRefreshToken(sess, j.value("RefreshToken", ""), requests_id.str());
    }
    else if(type == "AccessTokenLogin")
    {
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json No AccessToken");
	    return;
	}
	handleAccessTokenLogin(sess, j.value("AccessToken", ""), requests_id.str());
    }
    else
    {
	logger.warn("Pack Type Error");
	return;
    }
}

void ChatServerWorker::handleHello(bufferevent* bev, const std::string requests_id)
{
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(20,120);
    int heartbeatNum = dist(gen);

    json j;
    j["Requests_id"] = requests_id;
    j["Type"] = "HelloResp";
    j["HeartbeatNum"] = heartbeatNum;
    std::string data = j.dump() + '\n';

    timeval tv;
    tv.tv_sec = heartbeatNum * 1.5;
    tv.tv_usec = 0;
    bufferevent_set_timeouts(bev, &tv, nullptr);

    bufferevent_write(bev, data.c_str(), data.size());
}

void ChatServerWorker::handleHeartbeat(bufferevent* bev, const std::string requests_id)
{
    json j;
    j["Requests_id"] = requests_id;
    j["Type"] = "HeartbeatResp";
    std::string data = j.dump() + '\n';
    bufferevent_write(bev, data.c_str(), data.size());
}

void ChatServerWorker::handleLogin(session* sess, std::string email, std::string password, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    //get uid and judge is online?
    ThreadPool::getThreadPool()->addTask([email = std::move(email),password = std::move(password),sess, requests_id]() 
    {
	auto sessPtr = sess->shared_from_this();
	std::string from;
	std::string info;
	bool is_login = false;
	bool isEmailRules = isEmail(email);
	bool isPasswordRules = isPassword(password);

	//global 
	std::string g_username;
	std::string g_uid;
	std::string g_sid;
	std::string g_avatar_url;
	std::string g_accessToken;
	std::string g_refreshPlainToken;

	//判断合规性
	if(isEmailRules && isPasswordRules)
	{
	    //get uid and judge is online?
	    SQL* con = SQLPool::getSQLPool()->getConn();
	    //bind param
	    MYSQL_BIND bind_param;
	    memset(&bind_param, 0, sizeof(bind_param));

	    unsigned long len = email.size();
	    initStringParam(&bind_param,(char*)email.data(),email.size(),&len);
 
	    //result param
	    MYSQL_BIND result_param[5];
	    memset(result_param, 0, sizeof(result_param));

	    int64_t uid = 0,sid = 0;
	    initLongLongParam(&result_param[0], &uid, sizeof(uid));
	    initLongLongParam(&result_param[1], &sid, sizeof(sid));

	    char buf_username[64];
	    unsigned long len_username = 0;
	    initStringParam(&result_param[2], buf_username, sizeof(buf_username), &len_username);

	    char buf_password[128];
	    unsigned long len_password = 0;
	    initStringParam(&result_param[3], buf_password, sizeof(buf_password), &len_password);

	    char buf_avatar[512];
	    unsigned long len_avatar = 0;
	    initStringParam(&result_param[4], buf_avatar, sizeof(buf_avatar), &len_avatar);

	    std::string sql = "SELECT snowid, id, username, password, avatar_url FROM user WHERE email = ? AND status = 0;";
	    if(!con->executeSql(sql, &bind_param, result_param))
	    {
		logger.error("executesql login error");
		info = "登陆失败，请稍后重试!";
	    }
	    else
	    {
		int ret = 0;
		try
		{
		    ret = con->nextData();
		}catch(const std::runtime_error& e)
		{
		    logger.error(e.what());
		    return;
		}
		g_uid = std::to_string(uid);
		g_sid = std::to_string(sid);
		g_username = std::string(buf_username, len_username);
		g_avatar_url = std::string(buf_avatar, len_avatar);

		std::string stored_password(buf_password,len_password);
		if(ret == 1)
		{
		    //have user
		    if(onlineUser.find(uid) != onlineUser.end())
		    {
			from = "Email";
			info = "该用户已在线";
		    }
		    else if(verify_password(password, stored_password))
		    {
			//send success

			g_accessToken = createAccessToken(uid);
			std::string refreshHashToken;
			auto ap = createRefreshToken();
			g_refreshPlainToken = ap.first;
			refreshHashToken = ap.second;

			SQL* con1 = SQLPool::getSQLPool()->getConn();
			std::string sql1 = "INSERT INTO user_refresh_tokens(user_snowid, token_hash, expires_at) VALUES(?, ?, DATE_ADD(NOW(), INTERVAL 30 DAY)) ON DUPLICATE KEY UPDATE token_hash = VALUES(token_hash), expires_at = VALUES(expires_at);";
			
			MYSQL_BIND bind_param[2];
			initLongLongParam(&bind_param[0], &uid, sizeof(uid));
			unsigned long len_hashToken = refreshHashToken.size();
			initStringParam(&bind_param[1], (char*)refreshHashToken.data(), refreshHashToken.size(), &len_hashToken);
			
			con1->executeSql(sql1, bind_param);
			int ret = con1->getAffectRows();
			if(ret <= 0)
			{
			    logger.warn("插入长期Token失败");
			    info = "登陆错误，请稍候再试";
			}
			else
			{
			    info = "登陆成功";
			    is_login = true;
			}
			SQLPool::getSQLPool()->backConn(con1);
		    }
		    else
		    {
			//send password wrong
			from = "Password";
			info = "密码错误";
		    }
		}
		else if(ret == 0)
		{
		    //send no this account
		    from = "Email";
		    info = "该用户未注册";
		}
		else
		{
		    logger.error("Login Nextdata Error");
		    info = "登陆失败，请稍后重试!";
		}
	    }
	    SQLPool::getSQLPool()->backConn(con);
	}
	else
	{
	    if(!isEmailRules)
	    {
		from = "Email";
		info = "邮箱不合规";
	    }
	    else if(!isPasswordRules)
	    {
		from = "Password";
		info = "密码不合规";
	    }
	}
	auto* func = new std::function<void()>([is_login, info = std::move(info), from = std::move(from), g_uid , g_sid, username = std::move(g_username), avatar_url = std::move(g_avatar_url), sess, requests_id, g_accessToken, g_refreshPlainToken, email](){
	    auto sessPtr = sess->shared_from_this();
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "LoginResp";
	    j["Result"] = is_login;
	    j["Info"] = info;
	    if(is_login)
	    {
		j["UID"] = g_uid;
		j["SID"] = g_sid;
		j["Username"] = username;
		j["Avatar_Url"] = avatar_url;
		j["AccessToken"] = g_accessToken;
		j["RefreshToken"] = g_refreshPlainToken;
		sessPtr->uid = std::stoll(g_uid);
		auto user_info = std::make_shared<userInfo>();
		user_info->username = username;
		user_info->email = email;
		onlineUser[std::stoll(g_uid)] = user_info;
	    }
	    else
	    {
		j["From"] = from;
	    }
	    std::string data = j.dump() + '\n';
	    bufferevent_write(sessPtr->bev, data.c_str(), data.size());
	});
	timeval tv = {0,0};
	event_base_once(sessPtr->myself->base, -1, EV_TIMEOUT, [](evutil_socket_t, short, void *arg){
	    auto* func = static_cast<std::function<void()>*>(arg);
	    (*func)();
	    delete func;
		}, func, &tv);
    });
}

void ChatServerWorker::handleRegister(session* sess, std::string email, std::string password, std::string username, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, email = std::move(email), password = std::move(password), username = std::move(username), requests_id]()
    {
	bool is_success = false;
	std::string from;
	std::string info;
	bool isEmailRules = isEmail(email);
	bool isPasswordRules = isPassword(password);
	bool isUsernameRules = !username.empty();
	if(isEmailRules && isPasswordRules && isUsernameRules)
	{
	    
	    SQL* con = SQLPool::getSQLPool()->getConn();
	    std::string sql = "INSERT INTO user(snowid, username, password, email) VALUES(?,?,?,?);";
	    

	    MYSQL_BIND bind_param[4];
	    memset(bind_param, 0, sizeof(bind_param));

	    int64_t uid = sfGen.nextid();
	    initLongLongParam(&bind_param[0], &uid, sizeof(uid));

	    unsigned long len_username = username.size();
	    initStringParam(&bind_param[1], (char*)username.data(), username.size(), &len_username);

	    std::string hashedPassword;
	    try
	    {
		hashedPassword = hash_password(password);
	    }catch(const std::runtime_error& e)
	    {
		logger.error(e.what());
		info = "注册失败，请稍后重试!";
	    }
	    unsigned long len_password = hashedPassword.size();
	    initStringParam(&bind_param[2], (char*)hashedPassword.data(), hashedPassword.size(), &len_password);

	    unsigned long len_email = email.size();
	    initStringParam(&bind_param[3], (char*)email.data(), email.size(), &len_email);
	    
	    con->executeSql(sql, bind_param);
	    if(con->getAffectRows() <= 0)
	    {
		info = "该邮箱无法注册";
		from = "Email";
	    }
	    else
	    {
		info = "注册成功";
		is_success = true;
	    }

	    SQLPool::getSQLPool()->backConn(con);
	}
	else 
	{
	    if(!isEmailRules)
	    {
		from = "Email";
		info = "邮箱不合规";
	   }
	    else if(!isPasswordRules)
	    {
		from = "Password";
		info = "密码不合规";
	    }
	    else if(!isUsernameRules)
	    {
		from = "Username";
		info = "用户名不能为空";
	    }
	}
	auto* func = new std::function<void()>([is_success, info = std::move(info), from = std::move(from), sessPtr, requests_id](){
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "RegisterResp";
	    j["Result"] = is_success;
	    j["Info"] = info;
	    if(!is_success)
		j["From"] = from;
	    std::string data = j.dump() + '\n';
	    bufferevent_write(sessPtr->bev, data.c_str(), data.size());
	    });
	timeval tv = {0,0};
	event_base_once(sessPtr->myself->base, -1, EV_TIMEOUT, [](evutil_socket_t, short, void* arg){
	    auto* func = static_cast<std::function<void()>*>(arg);
	    (*func)();
	    delete func;
		}, func, &tv);	
    });
}

void ChatServerWorker::handleSend(bufferevent* bev, const std::string requests_id)
{}

void ChatServerWorker::handleUploadAvatar(session* sess, std::string url, std::string accessToken, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, url = std::move(url), requests_id, accessToken](){
	bool result = false;
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	if(sessPtr->uid.has_value() && uid_token != -1 && uid_token == sessPtr->uid.value())
	{
	    SQL* con = SQLPool::getSQLPool()->getConn();
	    std::string sql = "SELECT avatar_url FROM user WHERE snowid = ?;";
	    MYSQL_BIND bind_param;
	    int64_t uid;
	    uid = sessPtr->uid.value();

	    initLongLongParam(&bind_param, &uid, sizeof(uid));

	    MYSQL_BIND bind_result;
	    char buf_avatar[512];
	    unsigned long len_avatar = 0;
	    initStringParam(&bind_result, buf_avatar, sizeof(buf_avatar), &len_avatar);
	    con->executeSql(sql, &bind_param, &bind_result);
	    std::string old_avatarURL;
	    if(con->nextData() >= 0)
	    {
		old_avatarURL = std::string(buf_avatar, len_avatar);
	    }
	    SQLPool::getSQLPool()->backConn(con);

	    SQL* con1 = SQLPool::getSQLPool()->getConn();
	    sql = "UPDATE user SET avatar_url = ? WHERE snowid = ?;";
	    MYSQL_BIND bind_param1[2];

	    unsigned long len_url = url.size();
	    initStringParam(&bind_param1[0], (char*)url.data(), url.size(), &len_url);
	    initLongLongParam(&bind_param1[1], &uid, sizeof(uid)); 
	    if(!con1->executeSql(sql, bind_param1))
		std::cout << con1->getSQLError() << "\n";

	    if(con1->getAffectRows() > 0)
	    {
		result = true;
	    }
	    std::string delete_avatar(buf_avatar, len_avatar);
	    size_t pos_up = delete_avatar.find("/upload/");
	    if(pos_up != std::string::npos)
	    {
		std::string delete_path = "." + delete_avatar.substr(pos_up);
		std::ifstream ifs(delete_path);
		if(ifs.is_open())
		{
		    ifs.close();
		    std::remove(delete_path.c_str());
		}
		else
		    ifs.close();
	    }
	}
	//send back base and send json
	auto* func = new std::function<void()>([result, requests_id, sessPtr, uid_token](){
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "UpdateAvatarResp";
	    j["Result"] = result;
	    if(uid_token == -1 || !sessPtr->uid.has_value() || uid_token != sessPtr->uid.value())
	    {
		j["Result"] = false;
		j["AccessTokenExpired"] = true;
	    }
	    std::string data = j.dump() + "\n";
	    bufferevent_write(sessPtr->bev, data.c_str(), data.size());
	});

	    timeval tv = {0,0};
	    event_base_once(sessPtr->myself->base, -1, EV_TIMEOUT, [](evutil_socket_t, short, void* arg){
		auto* func = static_cast<std::function<void()>*>(arg);
		(*func)();
		delete func;
	    }, func, &tv);
    });

}

void ChatServerWorker::handleUnLogin(session* sess, std::string accessToken, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    bool result = false;
    if(sessPtr->uid.has_value())
    {
	int64_t uid = GlobalTools::verifyAccessToken(accessToken);
	if(uid == sessPtr->uid.value())
	{
	    if(uid != -1)
		result = true;
	}

    }
	
    json j;
    j["Requests_id"] = requests_id;
    j["Type"] = "UnLoginResp";
    j["Result"] = result;
    if(!result)
	j["AccessTokenExpired"] = true;

    std::string data = j.dump() + "\n";
    bufferevent_write(sessPtr->bev, data.c_str(), data.size());
    if(result)
    {
	if(sessPtr->uid.has_value())
	    onlineUser.erase(sessPtr->uid.value());
	sessPtr->uid.reset();
    }
}

void ChatServerWorker::handleUpdateUsername(session* sess, std::string username, std::string accessToken, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, username, requests_id, accessToken](){
	bool result = false;
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);

	if(sessPtr->uid.has_value() && !username.empty() && uid_token != -1 && uid_token == sessPtr->uid.value())
	{
	    SQL* con = SQLPool::getSQLPool()->getConn();
	    std::string sql = "Update user SET username = ? WHERE snowid = ?;";
	    MYSQL_BIND bind_param[2];
	    unsigned long len_username = username.size();
	    initStringParam(&bind_param[0], (char*)username.data(), username.size(), &len_username);
	    int64_t uid = sessPtr->uid.value();	
	    initLongLongParam(&bind_param[1], &uid, sizeof(uid));

	    con->executeSql(sql, bind_param);
	    if(con->getAffectRows() > 0)
		result = true;
	}
	auto* func = new std::function<void()>([sessPtr, result, requests_id, uid_token](){
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "UpdateUsernameResp";
	    j["Result"] = result;
	    bool tokenValid = uid_token != -1 && sessPtr->uid.has_value() && uid_token == sessPtr->uid.value();
	    if(!tokenValid)
	    {
		j["Result"] = false;
		j["AccessTokenExpired"] = true;
	    }
	    std::string data = j.dump() + "\n";
	    
	    bufferevent_write(sessPtr->bev, data.c_str(), data.size());
	});
	timeval tv{0, 0};
	event_base_once(sessPtr->myself->base, -1, EV_TIMEOUT, [](evutil_socket_t, short, void* arg){
	    auto* func = static_cast<std::function<void()>*>(arg);
	    (*func)();
	    delete func;
	}, func, &tv);
    });
}

void ChatServerWorker::handleAddNewFriendRequest(session* sess, std::string UID, std::string receiver_info, std::string verification_msg, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, UID, receiver_info, verification_msg, requests_id](){
	bool result = false;
	std::string info;
	if(!isSID(receiver_info) && !isEmail(receiver_info))
	{
	    info = "申请添加好友ID错误";
	}
	else
	{
	    std::optional<int64_t> sid;
	    if(isSID(receiver_info))
		sid = std::stoll(receiver_info);
	    SQL* con = SQLPool::getSQLPool()->getConn();
	    int64_t record_id = sfGen.nextid();
	    std::string sql = "SELECT snowid FROM user WHERE ";

	    MYSQL_BIND bind_param;
	    unsigned long len_email;
	    if(sid.has_value())
	    {
		sql += "id = ?;";
		initLongLongParam(&bind_param, &sid.value(), sizeof(sid.value()));
	    }
	    else
	    {
		sql += "email = ?;";
		len_email = receiver_info.size();
		initStringParam(&bind_param, (char*)receiver_info.data(), receiver_info.size(), &len_email);
	    }
	    MYSQL_BIND result_param;
	    int64_t userb_uid;
	    initLongLongParam(&result_param, &userb_uid, sizeof(userb_uid));
	    if(!con->executeSql(sql, &bind_param, &result_param))
	    {
		logger.error("executeSQL failed:" + std::string(con->getSQLError()));
		SQLPool::getSQLPool()->backConn(con);
		return;
	    }
	    int ret = con->nextData();
	    if(ret < 0)
	    {
		logger.warn("MySQL Nextdata < 0:In AddNewFriendRequest");
		SQLPool::getSQLPool()->backConn(con);
		return;
	    }
	    else if(ret == 0)
	    {
		info = "未找到该账户";
	    }
	    else if(userb_uid == std::stoll(UID))
	    {
		info = "不能和自己成为好友";
	    }
	    else
	    {
		SQLPool::getSQLPool()->backConn(con);
		std::string sql1 = "INSERT INTO friendships(id, user_a, user_b, lastaction_uid, verification_msg) VALUES(?, LEAST(?, ?), GREATEST(?, ?), ?, ?) ON DUPLICATE KEY UPDATE status = IF(status = 2, 0, status), verification_msg = VALUES(verification_msg)";
		con = SQLPool::getSQLPool()->getConn();
		MYSQL_BIND bind_param1[7];
		initLongLongParam(&bind_param1[0], &record_id, sizeof(record_id));
		int64_t usera_uid = std::stoll(UID);
		initLongLongParam(&bind_param1[1], &usera_uid, sizeof(usera_uid));
		initLongLongParam(&bind_param1[2], &userb_uid, sizeof(userb_uid));
		initLongLongParam(&bind_param1[3], &usera_uid, sizeof(usera_uid));
		initLongLongParam(&bind_param1[4], &userb_uid, sizeof(userb_uid));
		initLongLongParam(&bind_param1[5], &usera_uid, sizeof(usera_uid));
		unsigned long len_verMsg = verification_msg.size();
		initStringParam(&bind_param1[6], (char*)verification_msg.data(), verification_msg.size(), &len_verMsg);

		con->executeSql(sql1, bind_param1);
		uint64_t affectRows = con->getAffectRows();
		if(affectRows == 1 || affectRows == 2)
		    result = true;
		else if(affectRows == (uint64_t)-2)
		{
		    SQLPool::getSQLPool()->backConn(con);
		    logger.warn("SQL Con State Error");
		    return;
		}
		else if(affectRows == 0)
		{
		    SQLPool::getSQLPool()->backConn(con);
		    std::string sql2 = "SELECT status, lastaction_uid FROM friendships WHERE user_a = LEAST(?, ?) AND user_b = GREATEST(?, ?);";
		    con = SQLPool::getSQLPool()->getConn();

		    MYSQL_BIND bind_param2[4];
		    initLongLongParam(&bind_param2[0], &usera_uid, sizeof(usera_uid));
		    initLongLongParam(&bind_param2[1], &userb_uid, sizeof(userb_uid));
		    initLongLongParam(&bind_param2[2], &usera_uid, sizeof(usera_uid));
		    initLongLongParam(&bind_param2[3], &userb_uid, sizeof(userb_uid));

		    MYSQL_BIND result_param1[2];
		    int8_t status = -1;
		    initTinyIntParam(&result_param1[0], &status, sizeof(status));
		    int64_t lastaction_uid;
		    initLongLongParam(&result_param1[1], &lastaction_uid, sizeof(lastaction_uid));
		
		    con->executeSql(sql2, bind_param2, result_param1);
		    if(con->nextData() <= 0)
		    {
			logger.warn("MySQL Nextdata != 0:In AddNewFriendRequest");
			SQLPool::getSQLPool()->backConn(con);
			return;
		    }
		    if(status == 0)
			info = "已发送好友申请，请等待通过申请";
		    else if(status == 1)
			info = "您已和对方是好友";
		    else if(status == 3)
		    {
			if(lastaction_uid == std::stoll(UID))
			    info = "您已经给对方拉黑";
			else
			    info = "对方已经给您拉黑";
		    }
		    else
		    {
			SQLPool::getSQLPool()->backConn(con);
			return;
		    }
		}
	    }
	    SQLPool::getSQLPool()->backConn(con);

	    auto* func = new std::function<void()>([result, info, sessPtr, requests_id](){
		json j;
		j["Requests_id"] = requests_id;
		j["Type"] = "AddNewFriendRequestResp";
		j["Result"] = result;
		if(!result)
		    j["Info"] = info;
		std::string data = j.dump() + "\n";
	    
		bufferevent_write(sessPtr->bev, data.c_str(), data.size());
	    });
	    timeval tv{0, 0};
	    event_base_once(sessPtr->myself->base, -1, EV_TIMEOUT, [](evutil_socket_t, short, void* arg){
		auto* func = static_cast<std::function<void()>*>(arg);
		(*func)();
		delete func;
	    }, func, &tv);
	}
    });

}

void ChatServerWorker::handleHandleNewFriendRequest(session* sess, std::string uid, std::string handle_uid, bool isAgree, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, uid, handle_uid, isAgree, requests_id](){
	bool result = false;

	if(isUID(uid) && isUID(handle_uid))
	{
	    std::string sql = "UPDATE friendships SET status = ?, lastaction_uid = ? WHERE user_a = LEAST(?, ?) AND user_b = GREATEST(?, ?);";
	    int8_t status = 0;
	    if(isAgree)
		status = 1;
	    else
		status = 2;
	    int64_t uid_ = std::stoll(uid);
	    int64_t handle_uid_ = std::stoll(handle_uid);

	    MYSQL_BIND bind_param[6];
	    initTinyIntParam(&bind_param[0], &status, sizeof(status));
	    initLongLongParam(&bind_param[1], &uid_, sizeof(uid_));
	    initLongLongParam(&bind_param[2], &uid_, sizeof(uid_));
	    initLongLongParam(&bind_param[3], &handle_uid_, sizeof(handle_uid_));
	    initLongLongParam(&bind_param[4], &uid_, sizeof(uid_));
	    initLongLongParam(&bind_param[5], &handle_uid_, sizeof(handle_uid_));
	    
	    SQL* con = SQLPool::getSQLPool()->getConn();
	    con->executeSql(sql, bind_param);
	    int affectRows = con->getAffectRows();
	    if(affectRows > 0)
		result = true;
	    SQLPool::getSQLPool()->backConn(con);
	    auto* func = new std::function<void()>([result, sessPtr, requests_id](){
		json j;
		j["Requests_id"] = requests_id;
		j["Type"] = "HandleNewFriendRequestResp";
		j["Result"] = result;
		
		std::string data = j.dump() + "\n";
	    
		bufferevent_write(sessPtr->bev, data.c_str(), data.size());
	    });
	    timeval tv{0, 0};
	    event_base_once(sessPtr->myself->base, -1, EV_TIMEOUT, [](evutil_socket_t, short, void* arg){
		auto* func = static_cast<std::function<void()>*>(arg);
		(*func)();
		delete func;
	    }, func, &tv);

	}
    });
}

void ChatServerWorker::handleGetNewFriendRequestsList(session* sess, std::string uid, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, uid, requests_id](){
	bool result = false;
	json arr = json::array();
	if(isUID(uid))
	{
	    int64_t uid_;
	    uid_ = std::stoll(uid);
	    std::string sql = "SELECT user.id, user.snowid, user.username, user.avatar_url, friendships.verification_msg FROM friendships JOIN user ON user.snowid = friendships.lastaction_uid WHERE friendships.status = 0 AND friendships.user_a = ? AND lastaction_uid != ? UNION ALL SELECT user.id, user.snowid, user.username, user.avatar_url, friendships.verification_msg FROM friendships JOIN user ON user.snowid = friendships.lastaction_uid WHERE friendships.status = 0 AND friendships.user_b = ? AND lastaction_uid != ?;";

	    MYSQL_BIND bind_param[4];
	    initLongLongParam(&bind_param[0], &uid_, sizeof(uid_));
	    initLongLongParam(&bind_param[1], &uid_, sizeof(uid_));
	    initLongLongParam(&bind_param[2], &uid_, sizeof(uid_));
	    initLongLongParam(&bind_param[3], &uid_, sizeof(uid_));

	    MYSQL_BIND result_param[5];
	    int64_t sid, uid;
	    char buf_username[64*4];
	    char buf_avatar_url[512*4];
	    char buf_verification_msg[30*4];
	    initLongLongParam(&result_param[0], &sid, sizeof(sid));
	    initLongLongParam(&result_param[1], &uid, sizeof(uid));
	    unsigned long len_username = 0;
	    initStringParam(&result_param[2], buf_username, sizeof(buf_username),&len_username);
	    unsigned long len_avatar_url = 0;
	    initStringParam(&result_param[3], buf_avatar_url, sizeof(buf_avatar_url), &len_avatar_url);
	    unsigned long len_verification_msg = 0;
	    initStringParam(&result_param[4], buf_verification_msg, sizeof(buf_verification_msg), &len_verification_msg);
	    SQL* con = SQLPool::getSQLPool()->getConn();
	    con->executeSql(sql, bind_param, result_param);
	    int ret, count = 0;
	    while((ret = con->nextData()) == 1)//have data
	    {
		json item;
		item["SID"] = std::to_string(sid);
		item["UID"] = std::to_string(uid);
		item["Username"] = std::string(buf_username, len_username);
		item["Avatar_Url"] = std::string(buf_avatar_url, len_avatar_url);
		item["VerMsg"] = std::string(buf_verification_msg, len_verification_msg);
		arr.push_back(item);
		count++;
	    }
	    if(ret != 0)
	    {
		logger.warn("获取申请好友列表错误");
		SQLPool::getSQLPool()->backConn(con);
		return;
	    }
	    else
	    {
		if(count > 0)
		    result = true;
	    }
	    SQLPool::getSQLPool()->backConn(con);
	}
	auto* func = new std::function<void()>([result, sessPtr, requests_id, arr](){
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "GetNewFriendRequestsListResp";
	    j["Result"] = result;
	    if(result)
		j["List"] = arr;
	    std::string data = j.dump() + "\n";
	    
	    bufferevent_write(sessPtr->bev, data.c_str(), data.size());
	    });
	    timeval tv{0, 0};
	    event_base_once(sessPtr->myself->base, -1, EV_TIMEOUT, [](evutil_socket_t, short, void* arg){
		auto* func = static_cast<std::function<void()>*>(arg);
		(*func)();
		delete func;
	}, func, &tv);
    });
}

void ChatServerWorker::handleRefreshToken(session* sess, std::string refreshToken, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, refreshToken, requests_id](){
	bool result = false;
	std::string refreshHashToken = GlobalTools::sha256(refreshToken);
	std::string accessToken;
	if(!refreshHashToken.empty())
	{
	    std::string sql = "SELECT user_snowid FROM user_refresh_tokens WHERE token_hash = ? AND expires_at > NOW();"; 
	    SQL* con = SQLPool::getSQLPool()->getConn();
	    MYSQL_BIND bind_param;
	    unsigned long len_refreshHashToken;
	    initStringParam(&bind_param, (char*)refreshHashToken.data(), refreshHashToken.size(), &len_refreshHashToken);

	    MYSQL_BIND result_param;
	    int64_t uid;
	    initLongLongParam(&result_param, &uid, sizeof(uid));
	    con->executeSql(sql, &bind_param, &result_param);
	    if(con->nextData() == 1)
	    {
		result = true;
		accessToken = createAccessToken(uid);
	    }
	    SQLPool::getSQLPool()->backConn(con);
	    
	    auto* func = new std::function<void()>([result, sessPtr, requests_id, accessToken, uid](){
		json j;
		j["Requests_id"] = requests_id;
		j["Type"] = "RefreshTokenResp";
		j["Result"] = result;
		if(result)
		{
		    j["AccessToken"] = accessToken;
		    if(!sessPtr->uid.has_value())
		    {
			sessPtr->uid = uid;
			auto user_info = std::make_shared<userInfo>();
			onlineUser[uid] = user_info;
		    }
		}
		else
		{
		    j["RefreshTokenExpired"] = true;
		    sessPtr->uid.reset();
		}
		std::string data = j.dump() + "\n";
	    
		bufferevent_write(sessPtr->bev, data.c_str(), data.size());
	    });
	    timeval tv{0, 0};
	    event_base_once(sessPtr->myself->base, -1, EV_TIMEOUT, [](evutil_socket_t, short, void* arg){
		auto* func = static_cast<std::function<void()>*>(arg);
		(*func)();
		delete func;
	    }, func, &tv);
	}
	else
	{
	    logger.warn("sha256 refreshToken Error");
	    return;
	}

    });
}

void ChatServerWorker::handleAccessTokenLogin(session* sess, std::string accessToken, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    bool result = false;
    int64_t uid = GlobalTools::verifyAccessToken(accessToken);
    if(uid != -1)
    {
	result = true;
	sessPtr->uid = uid;
	onlineUser.erase(uid);
	auto user_info = std::make_shared<userInfo>();
	onlineUser[uid] = user_info;
    }
    json j;
    j["Requests_id"] = requests_id;
    j["Type"] = "AccessTokenLoginResp";
    j["Result"] = result;
    if(!result)
	j["AccessTokenExpired"] = true;

    std::string data = j.dump() + "\n";
	    
		bufferevent_write(sessPtr->bev, data.c_str(), data.size());
}

bool ChatServerWorker::isEmail(const std::string& email)
{
    std::regex email_pattern(R"(^[0-9a-zA-Z._%+-]+@[0-9a-zA-Z.-]+\.[a-zA-Z]{2,}$)");
    return std::regex_match(email, email_pattern);
}

bool ChatServerWorker::isPassword(const std::string& password)
{
    std::regex password_pattern(R"(^(?=.*[a-z])(?=.*[A-Z])(?=.*\d)(?=.*[!@#$%^&*])[a-zA-Z\d!@#$%^&*]{8,30}$)");
    return std::regex_match(password, password_pattern);
}

bool ChatServerWorker::isSID(const std::string& sid)
{
    std::regex sid_pattern(R"(^\d{10}$)");
    return std::regex_match(sid, sid_pattern);
}

bool ChatServerWorker::isUID(const std::string& uid)
{
    std::regex uid_pattern(R"(^\d{18}$)");
    return std::regex_match(uid, uid_pattern);
}

std::string ChatServerWorker::hash_password(const std::string& password)
{
    char hashed[crypto_pwhash_STRBYTES];
    if(crypto_pwhash_str(hashed, password.c_str(), password.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
    {
	throw std::runtime_error("Password Hashing Failed");
    }
    return std::string(hashed);
}

bool ChatServerWorker::verify_password(const std::string& password, const std::string& stored_hash)
{
    return crypto_pwhash_str_verify(stored_hash.c_str(), password.c_str(), password.size()) == 0;
}

void ChatServerWorker::initStringParam(MYSQL_BIND* pargma, char* buf_string, size_t buffer_length, unsigned long* length)
{
    memset(pargma, 0, sizeof(*pargma));
    pargma->buffer_type = MYSQL_TYPE_STRING;
    pargma->buffer = buf_string;
    pargma->buffer_length = buffer_length;
    if(length)
	pargma->length = length;
}

void ChatServerWorker::initLongLongParam(MYSQL_BIND* pargma, int64_t* buf_longlong, size_t buffer_length)
{
    memset(pargma, 0, sizeof(*pargma));
    pargma->buffer_type = MYSQL_TYPE_LONGLONG;
    pargma->buffer = buf_longlong;
    pargma->buffer_length = buffer_length;
}

void ChatServerWorker::initTinyIntParam(MYSQL_BIND* pargma, int8_t* buf_tinyint, size_t buffer_length)
{
    memset(pargma, 0, sizeof(*pargma));
    pargma->buffer_type = MYSQL_TYPE_TINY;
    pargma->buffer = buf_tinyint;
    pargma->buffer_length = buffer_length;
}

std::string ChatServerWorker::createAccessToken(int64_t uid, const std::chrono::seconds& expire_duration)
{
    auto now = std::chrono::system_clock::now();
    return jwt::create()
	.set_issuer(AppName)
	.set_subject(std::to_string(uid))
	.set_issued_at(now)
	.set_expires_at(now + expire_duration)
	.sign(jwt::algorithm::hs256{jwt_secret});
}

std::pair<std::string, std::string> ChatServerWorker::createRefreshToken()
{
    unsigned char random_bytes[64];
    randombytes_buf(random_bytes, sizeof(random_bytes));

    char hex[129];
    sodium_bin2hex(hex, sizeof(hex), random_bytes, sizeof(random_bytes));
    std::string plain_token(hex, 128);

    unsigned char hash[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(hash, random_bytes, sizeof(random_bytes));

    char hash_hex[65];
    sodium_bin2hex(hash_hex, sizeof(hash_hex), hash, sizeof(hash));
    std::string hash_token(hash_hex, 64);

    return {plain_token, hash_token};
}
