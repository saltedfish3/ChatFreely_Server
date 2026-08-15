#include "ChatServerWorker.h"
#include <bits/types/struct_timeval.h>
#include <chrono>
#include <cstring>
#include <ctime>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <event2/util.h>
#include <functional>
#include <jwt-cpp/jwt.h>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sodium/core.h>
#include <sodium/crypto_hash_sha256.h>
#include <sodium/crypto_pwhash.h>
#include <sodium/randombytes.h>
#include <sodium/utils.h>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

Logger ChatServerWorker::logger("worker");

ChatServerWorker::ChatServerWorker()
{
    if(evthread_use_pthreads() < 0)
	throw std::runtime_error("User evthread Error");
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
    sess->weak_self = sess;

    this->list_session.emplace(fd,sess);

    bufferevent_setcb(bev, cb_read, cb_write, cb_event, &sess->weak_self);
    timeval tv = {30,0};
    bufferevent_set_timeouts(bev, &tv, nullptr);
    bufferevent_enable(bev, EV_READ);
}

void ChatServerWorker::listen()
{
    if(this->base)
    {
	timeval tv = {1,0};
	auto lastClean = std::chrono::steady_clock::now();
	while(!flag_shutdown)
	{
	    event_base_loopexit(this->base, &tv);
	    event_base_dispatch(this->base);

	    auto now = std::chrono::steady_clock::now();
	    if(now - lastClean >= std::chrono::seconds(60))
	    {
		cleanExpiredCache();
		lastClean = now;
	    }
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
	logger.info("Client Exit");
    else if(events & BEV_EVENT_ERROR)
	logger.info(std::string("Connect Error:") + evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    else if(events & BEV_EVENT_TIMEOUT)
	logger.info("Client Timeout Exit");

    int64_t uid = -1;
    {
	std::lock_guard<std::shared_mutex> lock(mutex_onlineUser);
	if(sess->uid.has_value())
	{
	    uid = sess->uid.value();
	    onlineUser.erase(sess->uid.value());
	}
    }
    if(uid != -1)
	pushFriendStatus(uid, false);

    int fd = bufferevent_getfd(bev);
    if(bev)
	bufferevent_free(bev);

    sess->myself->list_session.erase(fd);	
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
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json no AccessToken");
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
	std::string verMsg;
	if(j.contains("VerMsg") && j["VerMsg"].is_string())
	    verMsg = j["VerMsg"];
	handleAddNewFriendRequest(sess, receiver_info, verMsg, j.value("AccessToken", ""), requests_id.str());
    }
    else if(type == "HandleNewFriendRequest")
    {
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json No AccessToken");
	    return;
	}
	if((!j.contains("HandleUID") || !j["HandleUID"].is_string()) && (!j.contains("Status") || j["Status"].is_boolean()))
	{
	    logger.warn("处理新好友申请缺少参数");
	    return;
	}
	std::string handle_uid = j.value("HandleUID", "");
	bool isAgree = j.value("Status", false);
	handleHandleNewFriendRequest(sess, handle_uid, isAgree, j.value("AccessToken", ""), requests_id.str());
	
    }
    else if(type == "GetNewFriendRequestsList")
    {
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json No AccessToken");
	    return;
	}
	handleGetNewFriendRequestsList(sess, j.value("AccessToken", ""), requests_id.str());
    }
    else if(type == "GetFriendList")
    {
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json No AccessToken");
	    return;
	}
	handleGetFriendList(sess, j.value("AccessToken", ""), requests_id.str());
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
    else if(type == "SendMessage")
    {
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json No AccessToken");
	    return;
	}
	if(!j.contains("ReceiverUID") || !j["ReceiverUID"].is_string() || !j.contains("Content") || !j["Content"].is_string() || !j.contains("TempMsgID") || !j["TempMsgID"].is_string())
	{
	    logger.warn("Json No Param");
	    return;
	}
	handleSendMessage(sess, j.value("AccessToken", ""), j.value("ReceiverUID", ""), j.value("Content", ""), j.value("TempMsgID", ""), requests_id.str());
    }
    else if(type == "GetConversationsFriendUIDListAndSeq")
    {
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json No AccessToken");
	    return;
	}
	if(!j.contains("SyncConvSeq") || !j["SyncConvSeq"].is_number())
	{
	    logger.warn("Json No Param");
	    return;
	}
	handleGetConversationsFriendUIDListAndSeq(sess, j.value("AccessToken", ""), j.value<int64_t>("SyncConvSeq", 0), requests_id.str());
    }
    else if(type == "SyncNewMessages")
    {
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json No AccessToken");
	    return;
	}
	if(!j.contains("FriendUID") || !j["FriendUID"].is_string() || !j.contains("StartConvSeq") || !j["StartConvSeq"].is_number() || !j.contains("Limit") || !j["Limit"].is_number())
	{
	    logger.warn("Json No Param");
	    return;
	}
	handleSyncMessages(sess, j.value("AccessToken", ""), j.value("FriendUID", ""), j.value<int64_t>("StartConvSeq", 0), j.value<int64_t>("Limit", 0), requests_id.str());
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
    ThreadPool::getThreadPool()->addTask([email = std::move(email),password = std::move(password),sessPtr, requests_id]() 
    {
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
	    GuardConnection con(SQLPool::getSQLPool());
	    if(!con)
		info = "登陆失败，请稍候重试";
	    else
	    {
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

		char buf_username[64*4];
		unsigned long len_username = 0;
		initStringParam(&result_param[2], buf_username, sizeof(buf_username), &len_username);

		char buf_password[128*4];
		unsigned long len_password = 0;
		initStringParam(&result_param[3], buf_password, sizeof(buf_password), &len_password);

		char buf_avatar[512*4];
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
			info = "登陆失败，请稍候重试";
			ret = -1;
		    }

		    if(ret == 1)
		    {
			g_uid = std::to_string(uid);
			g_sid = std::to_string(sid);
			g_username = std::string(buf_username, len_username);
			g_avatar_url = std::string(buf_avatar, len_avatar);
			std::string stored_password(buf_password,len_password);
			
			//have user
			{
			    std::shared_lock lock(mutex_onlineUser);
			    if(onlineUser.find(uid) != onlineUser.end())
			    {
				from = "Email";
				info = "该用户已在线";
			    }
			}
		    
			if(info.empty())
			{
			    //send success
			    if(verify_password(password, stored_password))
			    {
				g_accessToken = createAccessToken(uid);
				std::string refreshHashToken;
				auto ap = createRefreshToken();
				g_refreshPlainToken = ap.first;
				refreshHashToken = ap.second;

				GuardConnection con_guard(SQLPool::getSQLPool());
				if(!con_guard)
				    info = "登陆失败，请稍候重试";
				else
				{
				    SQL* con1 = con_guard.get();
				    SQLTransaction sqlTrans(con1->getMysql());
				    std::string sql1 = "INSERT INTO user_refresh_tokens(user_snowid, token_hash, expires_at) VALUES(?, ?, DATE_ADD(NOW(), INTERVAL 30 DAY)) ON DUPLICATE KEY UPDATE token_hash = VALUES(token_hash), expires_at = VALUES(expires_at);";
			
				    MYSQL_BIND bind_param[2];
				    initLongLongParam(&bind_param[0], &uid, sizeof(uid));
				    unsigned long len_hashToken = refreshHashToken.size();
				    initStringParam(&bind_param[1], (char*)refreshHashToken.data(), refreshHashToken.size(), &len_hashToken);
			
				    con1->executeSql(sql1, bind_param);
				    if(con1->getAffectRows() <= 0)
				    {
					logger.warn("插入长期Token失败");
					info = "登陆错误，请稍候再试";
				    }
				    else
				    {
					{
					    std::lock_guard<std::shared_mutex> lock(mutex_onlineUser);
					    if(onlineUser.find(uid) != onlineUser.end())
					    {
						from = "Email";
						info = "该用户已在线";
					    }
					    else
					    {
						auto user_info = std::make_shared<userInfo>();
						user_info->username = g_username;
						user_info->email = email;
						user_info->sid = g_sid;
						user_info->avatar_url = g_avatar_url;
						user_info->sessPtr = sessPtr;
						onlineUser[uid] = user_info;
						sessPtr->uid = uid;

						info = "登陆成功";
						is_login = true;

						sqlTrans.commit();
					    }
					}
					if(is_login)
					    pushFriendStatus(uid, true);
				    }
				}
			    }
			    else
			    {
				//send password wrong
				from = "Password";
				info = "密码错误";
			    }
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
			if(info.empty())
			    info = "登陆失败，请稍后重试!";
		    }
		}
	    }
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
	auto* func = new std::function<void()>([is_login, info = std::move(info), from = std::move(from), g_uid , g_sid, username = std::move(g_username), avatar_url = std::move(g_avatar_url), sessPtr, requests_id, g_accessToken, g_refreshPlainToken, email](){
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
	json j_Cache;
	bool isCache = sessPtr->myself->tryGetCache(requests_id, j_Cache);
	bool is_success = false;
	std::string from;
	std::string info;
	bool isEmailRules = isEmail(email);
	bool isPasswordRules = isPassword(password);
	bool isUsernameRules = !username.empty();

	if(isEmailRules && isPasswordRules && isUsernameRules && !isCache)
	{
	    std::string hashedPassword;
	    try
	    {
		hashedPassword = hash_password(password);
	    }catch(const std::runtime_error& e)
	    {
		logger.error(e.what());
		info = "注册失败，请稍候重试";
	    }
	    if(info.empty())
	    {
		GuardConnection con(SQLPool::getSQLPool());
		if(!con)
		    info = "注册失败，请稍候重试";
		else
		{
		    std::string sql = "INSERT INTO user(snowid, username, password, email) VALUES(?,?,?,?);";
	    
		    MYSQL_BIND bind_param[4];
		    memset(bind_param, 0, sizeof(bind_param));

		    int64_t uid = sfGen.nextid();
		    initLongLongParam(&bind_param[0], &uid, sizeof(uid));

		    unsigned long len_username = username.size();
		    initStringParam(&bind_param[1], (char*)username.data(), username.size(), &len_username);

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
		}
	    }
	}
	else if(!isCache) 
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
	auto* func = new std::function<void()>([is_success, info = std::move(info), from = std::move(from), sessPtr, requests_id, isCache, j_Cache](){
	    json j;
	    if(isCache)
		j = j_Cache;
	    else
	    {
		j["Requests_id"] = requests_id;
		j["Type"] = "RegisterResp";
		j["Result"] = is_success;
		j["Info"] = info;
		if(!is_success)
		    j["From"] = from;
		sessPtr->myself->setCache(requests_id, j, 600);
	    }
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

void ChatServerWorker::handleUploadAvatar(session* sess, std::string url, std::string accessToken, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, url = std::move(url), requests_id, accessToken](){
	bool result = false;
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	if(sessPtr->uid.has_value() && uid_token != -1 && uid_token == sessPtr->uid.value())
	{
	    int64_t uid = sessPtr->uid.value();
	    std::string old_avatarURL;
	    {
		GuardConnection con(SQLPool::getSQLPool());
		if(con)
		{
		    std::string sql = "SELECT avatar_url FROM user WHERE snowid = ?;";
		    MYSQL_BIND bind_param;
		    initLongLongParam(&bind_param, &uid, sizeof(uid));

		    MYSQL_BIND bind_result;
		    char buf_avatar[512*4];
		    unsigned long len_avatar = 0;
		    initStringParam(&bind_result, buf_avatar, sizeof(buf_avatar), &len_avatar);

		    con->executeSql(sql, &bind_param, &bind_result);
		    if(con->nextData() >= 0)
		    {
			old_avatarURL = std::string(buf_avatar, len_avatar);
		    }
		}
	    }
	    {
		GuardConnection con(SQLPool::getSQLPool());
		if(con)
		{
		    std::string sql = "UPDATE user SET avatar_url = ? WHERE snowid = ?;";
		    MYSQL_BIND bind_param1[2];

		    unsigned long len_url = url.size();
		    initStringParam(&bind_param1[0], (char*)url.data(), url.size(), &len_url);
		    initLongLongParam(&bind_param1[1], &uid, sizeof(uid)); 
		    if(!con->executeSql(sql, bind_param1))
			logger.warn(con->getSQLError());

		    if(con->getAffectRows() > 0)
		    {
			{
			    std::lock_guard<std::shared_mutex> lock(mutex_onlineUser);
			    auto it = onlineUser.find(sessPtr->uid.value());
			    if(it != onlineUser.end())
				it->second->avatar_url = url;
			}
			result = true;
			pushFriendAvatar(uid, url);
		    }
		}
	    }
	    if(!old_avatarURL.empty())
	    {
		size_t pos_up = old_avatarURL.find("/upload/");
		if(pos_up != std::string::npos)
		{
		    std::string delete_path = "." + old_avatarURL.substr(pos_up);
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
    json j_Cache;
    bool isCache = sessPtr->myself->tryGetCache(requests_id, j_Cache);
    if(!isCache && sessPtr->uid.has_value())
    {
	int64_t uid = GlobalTools::verifyAccessToken(accessToken);
	if(uid == sessPtr->uid.value())
	{
	    if(uid != -1)
		result = true;
	}

    }
	
    json j;
    if(isCache)
	j = j_Cache;
    else
    {
	j["Requests_id"] = requests_id;
	j["Type"] = "UnLoginResp";
	j["Result"] = result;
	if(!result)
	    j["AccessTokenExpired"] = true;
	sessPtr->myself->setCache(requests_id, j, 300);
    }

    std::string data = j.dump() + "\n";
    bufferevent_write(sessPtr->bev, data.c_str(), data.size());
    if(result)
    {
	int64_t uid = -1;
	{
	    std::lock_guard<std::shared_mutex> lock(mutex_onlineUser);
	    if(sessPtr->uid.has_value())
	    {
		uid = sessPtr->uid.value();
		onlineUser.erase(sessPtr->uid.value());
	    }
	    sessPtr->uid.reset();
	}
	if(uid != -1)
	    pushFriendStatus(uid, false);
    }

}

void ChatServerWorker::handleUpdateUsername(session* sess, std::string username, std::string accessToken, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, username, requests_id, accessToken](){
	json j_Cache;
	bool isCache = sessPtr->myself->tryGetCache(requests_id, j_Cache);
	bool result = false;
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);

	if(!isCache && sessPtr->uid.has_value() && !username.empty() && uid_token != -1 && uid_token == sessPtr->uid.value())
	{
	    GuardConnection con(SQLPool::getSQLPool());
	    if(!con)
	    {
		result = false;
	    }
	    else
	    {
		std::string sql = "Update user SET username = ? WHERE snowid = ?;";
		MYSQL_BIND bind_param[2];
		unsigned long len_username = username.size();
		initStringParam(&bind_param[0], (char*)username.data(), username.size(), &len_username);
		int64_t uid = sessPtr->uid.value();	
		initLongLongParam(&bind_param[1], &uid, sizeof(uid));

		con->executeSql(sql, bind_param);
		if(con->getAffectRows() > 0)
		{
		    {
			std::lock_guard<std::shared_mutex> lock(mutex_onlineUser);
			auto it = onlineUser.find(sessPtr->uid.value());
			if(it != onlineUser.end())
			    it->second->username = username;
		    }
		    result = true;
		    pushFriendUsername(uid, username);
		}
	    }
	}
	auto* func = new std::function<void()>([sessPtr, result, requests_id, uid_token, isCache, j_Cache](){
	    json j;
	    if(isCache)
		j = j_Cache;
	    else
	    {
		j["Requests_id"] = requests_id;
		j["Type"] = "UpdateUsernameResp";
		j["Result"] = result;
		bool tokenValid = uid_token != -1 && sessPtr->uid.has_value() && uid_token == sessPtr->uid.value();
		if(!tokenValid)
		{
		    j["Result"] = false;
		    j["AccessTokenExpired"] = true;
		}
		sessPtr->myself->setCache(requests_id, j, 300);
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

void ChatServerWorker::handleAddNewFriendRequest(session* sess, std::string receiver_info, std::string verification_msg, std::string accessToken, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, receiver_info, verification_msg, requests_id, accessToken](){
	json j_Cache;
	bool isCache = sessPtr->myself->tryGetCache(requests_id, j_Cache);
	bool result = false;
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	bool uidValid = sessPtr->uid.has_value() && sessPtr->uid.value() == uid_token && uid_token != -1;

	std::string info;
	int64_t userb_uid = 0;

	if(!isCache && uidValid)
	{
	    if(!isSID(receiver_info) && !isEmail(receiver_info))
		info = "申请添加好友ID错误";
	    else
	    {
		std::optional<int64_t> sid;
		if(isSID(receiver_info))
		    sid = std::stoll(receiver_info);
		
		{
		    GuardConnection con(SQLPool::getSQLPool());
		    if(!con)
			info = "申请添加好友失败，请稍候重试";
		    else
		    {
			std::string sql = "SELECT snowid FROM user WHERE ";

			MYSQL_BIND bind_param;
			unsigned long len_email = 0;
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
			initLongLongParam(&result_param, &userb_uid, sizeof(userb_uid));

			if(!con->executeSql(sql, &bind_param, &result_param))
			{
			    logger.error("executeSQL failed:" + std::string(con->getSQLError()));
			    info = "申请失败，请稍后重试";
			}
			else
			{
			    int ret = con->nextData();
			    if(ret < 0)
			    {
				logger.warn("MySQL Nextdata < 0:In AddNewFriendRequest");
				info = "申请失败，请稍后重试";
			    }
			    else if(ret == 0)
			    {
				info = "未找到该账户";
			    }
			    else if(userb_uid == sessPtr->uid.value())
			    {
				info = "不能和自己成为好友";
			    }
			}
		    }   
		}
	    
		if(info.empty())
		{
		    int64_t record_id = sfGen.nextid();
		    int64_t usera_uid = sessPtr->uid.value();

		    {
			GuardConnection con(SQLPool::getSQLPool());
			if(!con)
			    info = "申请添加好友错误，请稍候重试";
			else
			{
			    std::string sql1 = "INSERT INTO friendships(id, user_a, user_b, lastaction_uid, verification_msg) VALUES(?, LEAST(?, ?), GREATEST(?, ?), ?, ?) ON DUPLICATE KEY UPDATE lastaction_uid = IF(status = 2, VALUES(lastaction_uid), lastaction_uid), verification_msg = IF(status = 2, VALUES(verification_msg), verification_msg), status = IF(status = 2, 0, status);";
			    MYSQL_BIND bind_param1[7];
			    initLongLongParam(&bind_param1[0], &record_id, sizeof(record_id));
			    int64_t usera_uid = sessPtr->uid.value();
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
			    {
				result = true;
				pushNewFriendRequests(sessPtr->uid.value(), userb_uid, verification_msg);
			    }
			    else if(affectRows == 0)
			    {
			    }
			    else
			    {
				logger.warn("SQL Con State Error");
				info = "申请添加好友失败，请稍候重试";
			    }
			}
		    }
		    if(!result && info.empty())
		    {
			GuardConnection con(SQLPool::getSQLPool());
			if(!con)
			    info = "申请添加好友失败，请稍候重试";
			else
			{
			    std::string sql2 = "SELECT status, lastaction_uid FROM friendships WHERE user_a = LEAST(?, ?) AND user_b = GREATEST(?, ?);";

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
		
			    if(con->executeSql(sql2, bind_param2, result_param1) && con->nextData() > 0)
			    {
				if(status == 0)
				    info = "已发送好友申请，请等待通过申请";
				else if(status == 1)
				    info = "您已和对方是好友";
				else if(status == 3)
				{
				    if(lastaction_uid == sessPtr->uid.value())
					info = "您已经给对方拉黑";
				    else
					info = "对方已经给您拉黑";
				}
				else
				{
				    info = "申请失败，请稍后重试";
				}
			    }
			    else
			    {
				logger.warn("SQL Con State Error");
				info = "申请失败，请稍候重试";
			    }
			}
		    }
		}
	    }
	}
	auto* func = new std::function<void()>([result, info, sessPtr, requests_id, uidValid, isCache, j_Cache](){
	    json j;
	    if(isCache)
		j = j_Cache;
	    else
	    {
		j["Requests_id"] = requests_id;
		j["Type"] = "AddNewFriendRequestResp";
		j["Result"] = result;
		if(!result)
		    j["Info"] = info;
		if(!uidValid)
		{
		    j["Result"] = false;
		    j["AccessTokenExpired"] = true;
		}
		sessPtr->myself->setCache(requests_id, j, 600);
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

void ChatServerWorker::handleHandleNewFriendRequest(session* sess, std::string handle_uid, bool isAgree, std::string accessToken, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, handle_uid, isAgree, requests_id, accessToken](){
	json j_Cache;
	bool isCache = sessPtr->myself->tryGetCache(requests_id, j_Cache);
	bool result = false;
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	bool uidValid = sessPtr->uid.has_value() && uid_token != -1 && sessPtr->uid.value() == uid_token;

	if(!isCache && isUID(handle_uid) && uidValid)
	{
	    std::string sql = "UPDATE friendships SET status = ?, lastaction_uid = ? WHERE user_a = LEAST(?, ?) AND user_b = GREATEST(?, ?);";
	    int8_t status = 0;
	    if(isAgree)
		status = 1;
	    else
		status = 2;
	    int64_t uid_ = sessPtr->uid.value();
	    int64_t handle_uid_ = std::stoll(handle_uid);

	    MYSQL_BIND bind_param[6];
	    initTinyIntParam(&bind_param[0], &status, sizeof(status));
	    initLongLongParam(&bind_param[1], &uid_, sizeof(uid_));
	    initLongLongParam(&bind_param[2], &uid_, sizeof(uid_));
	    initLongLongParam(&bind_param[3], &handle_uid_, sizeof(handle_uid_));
	    initLongLongParam(&bind_param[4], &uid_, sizeof(uid_));
	    initLongLongParam(&bind_param[5], &handle_uid_, sizeof(handle_uid_));
	    
	    GuardConnection con(SQLPool::getSQLPool());
	    if(con)
	    {
		con->executeSql(sql, bind_param);
		int affectRows = con->getAffectRows();
		if(affectRows > 0)
		{
		    result = true;
		    pushNewFriend(sessPtr->uid.value(), handle_uid_);
		}
	    }
	    else
		logger.error("handleHandleNewFriendRequest 获取数据库连接失败");
	}
	auto* func = new std::function<void()>([result, sessPtr, requests_id, uidValid, isCache, j_Cache](){
	    json j;
	    if(isCache)
		j = j_Cache;
	    else
	    {
		j["Requests_id"] = requests_id;
		j["Type"] = "HandleNewFriendRequestResp";
		j["Result"] = result;
		if(!uidValid)
		    j["AccessTokenExpired"] = true;
		sessPtr->myself->setCache(requests_id, j, 600);
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

void ChatServerWorker::handleGetNewFriendRequestsList(session* sess, std::string accessToken, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, requests_id, accessToken](){
	bool result = false;
	json arr = json::array();
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	bool uidValid = sessPtr->uid.has_value() && sessPtr->uid.value() == uid_token && uid_token != -1;

	if(uidValid)
	{
	    int64_t uid_ = sessPtr->uid.value();
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

	    GuardConnection con(SQLPool::getSQLPool());
	    if(con)
	    {
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
		    len_username = 0;
		    len_avatar_url = 0;
		    len_verification_msg = 0;
		    arr.push_back(item);
		    count++;
		}
		if(ret != 0)
		{
		    logger.warn("获取申请好友列表错误");
		}
		else
		{
		    if(count > 0)
			result = true;
		}
	    }
	    else
		logger.error("handleGetNewFriendRequestsList 获取数据库连接失败");
	}
	auto* func = new std::function<void()>([result, sessPtr, requests_id, arr, uidValid](){
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "GetNewFriendRequestsListResp";
	    j["Result"] = result;
	    if(result)
		j["List"] = arr;
	    if(!uidValid)
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

void ChatServerWorker::handleGetFriendList(session* sess, std::string accessToken, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, accessToken, requests_id](){
	bool result = false;
	json arr = json::array();
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	bool uidValid = sessPtr->uid.has_value() && sessPtr->uid.value() == uid_token && uid_token != -1;

	if(uidValid)
	{
	    std::string sql = "SELECT user.id, user.snowid, user.username, user.email, user.avatar_url FROM friendships JOIN user ON user.snowid = friendships.user_a WHERE friendships.status = 1 AND friendships.user_b = ? UNION ALL SELECT user.id, user.snowid, user.username, user.email, user.avatar_url FROM friendships JOIN user ON user.snowid = friendships.user_b WHERE friendships.status = 1 AND friendships.user_a = ?;";
	    int64_t uid = sessPtr->uid.value();
	    MYSQL_BIND bind_param[2];
	    initLongLongParam(&bind_param[0], &uid, sizeof(uid));
	    initLongLongParam(&bind_param[1], &uid, sizeof(uid));

	    int64_t user_sid;
	    int64_t user_uid;
	    char buf_username[64*4];
	    char buf_email[128*4];
	    char buf_avatar[512*4];
	    MYSQL_BIND result_param[5];
	    initLongLongParam(&result_param[0], &user_sid, sizeof(user_sid));
	    initLongLongParam(&result_param[1], &user_uid, sizeof(user_uid));
	    unsigned long len_username = 0;
	    initStringParam(&result_param[2], buf_username, sizeof(buf_username), &len_username);
	    unsigned long len_email = 0;
	    initStringParam(&result_param[3], buf_email, sizeof(buf_email), &len_email);
	    unsigned long len_avatar = 0;
	    initStringParam(&result_param[4], buf_avatar, sizeof(buf_avatar), &len_avatar);

	    GuardConnection con(SQLPool::getSQLPool());
	    if(con)
	    {
		con->executeSql(sql, bind_param, result_param);
		int ret, count = 0;
		while((ret = con->nextData()) == 1)
		{
		    json item;
		    item["SID"] = std::to_string(user_sid);
		    item["UID"] = std::to_string(user_uid);
		    item["Username"] = std::string(buf_username, len_username);
		    item["Email"] = std::string(buf_email, len_email);
		    item["Avatar_Url"] = std::string(buf_avatar, len_avatar);
		    len_username = 0;
		    len_email = 0;
		    len_avatar = 0;
		    bool isOnline = false;
		    {
			std::shared_lock lock(mutex_onlineUser);
			if(onlineUser.find(user_uid) != onlineUser.end())
			    isOnline = true;
		    }
		    item["IsOnline"] = isOnline;
		    arr.push_back(item);
		    count++;
		}
		if(ret != 0)
		    logger.warn("获取好友列表错误");
		else
		{
		    if(count > 0)
			result = true;
		}
	    }
	    else
	    {
		logger.error("获取数据库连接失败");
	    }
	}
	auto* func = new std::function<void()>([result, sessPtr, requests_id, arr, uidValid](){
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "GetFriendListResp";
	    j["Result"] = result;
	    if(result)
		j["List"] = arr;
	    if(!uidValid)
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

void ChatServerWorker::handleRefreshToken(session* sess, std::string refreshToken, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, refreshToken, requests_id](){
	bool result = false;
	std::string refreshHashToken = GlobalTools::sha256(refreshToken);
	std::string accessToken;
	int64_t uid = 0;
	if(!refreshHashToken.empty())
	{
	    std::string sql = "SELECT user_snowid FROM user_refresh_tokens WHERE token_hash = ? AND expires_at > NOW();"; 
	    
	    GuardConnection con(SQLPool::getSQLPool());
	    if(con)
	    {
		MYSQL_BIND bind_param;
		unsigned long len_refreshHashToken;
		initStringParam(&bind_param, (char*)refreshHashToken.data(), refreshHashToken.size(), &len_refreshHashToken);

		MYSQL_BIND result_param;
		initLongLongParam(&result_param, &uid, sizeof(uid));
		con->executeSql(sql, &bind_param, &result_param);
		if(con->nextData() == 1)
		{
		    result = true;
		    accessToken = createAccessToken(uid);
		}
	    }
	    else
		logger.warn("RefreshToken:: Failed to get SQLConnection");
	}
	else
	{
	    logger.warn("sha256 refreshToken Error");
	}

	auto* func = new std::function<void()>([result, sessPtr, requests_id, accessToken, uid](){
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "RefreshTokenResp";
	    j["Result"] = result;
	    if(result)
	    {
		j["AccessToken"] = accessToken;
		if(!sessPtr->uid.has_value())
		    sessPtr->uid = uid;
	    }
	    else
	    {
		j["RefreshTokenExpired"] = true;
		if(sessPtr->uid.has_value())
		{
		    int64_t uid = sessPtr->uid.value();
		    {
			std::lock_guard<std::shared_mutex> lock(mutex_onlineUser);
			onlineUser.erase(uid);
		    }   
		    pushFriendStatus(uid, false);
		}
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
    });
}

void ChatServerWorker::handleAccessTokenLogin(session* sess, std::string accessToken, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, accessToken, requests_id](){
	bool result = false;
	bool isAccessTokenExpired = false;
	int64_t uid = GlobalTools::verifyAccessToken(accessToken);

	if(uid != -1)
	{
	    std::string sql = "SELECT id, username, avatar_url, email FROM user WHERE snowid = ?;";
	    MYSQL_BIND bind_param;
	    int64_t uid_ = uid;
	    initLongLongParam(&bind_param, &uid_, sizeof(uid_));

	    MYSQL_BIND result_param[4];
	    int64_t sid;
	    initLongLongParam(&result_param[0], &sid, sizeof(sid));
	    char buf_username[64*4];
	    unsigned long len_username = 0;
	    initStringParam(&result_param[1], buf_username, sizeof(buf_username), &len_username);
	    char buf_avatarUrl[512*4];
	    unsigned long len_avatarUrl = 0;
	    initStringParam(&result_param[2], buf_avatarUrl, sizeof(buf_avatarUrl), &len_avatarUrl);
	    char buf_email[128*4];
	    unsigned long len_email = 0;
	    initStringParam(&result_param[3], buf_email, sizeof(buf_email), &len_email);

	    GuardConnection con(SQLPool::getSQLPool());
	    if(con)
	    {
		con->executeSql(sql, &bind_param, result_param);
		if(con->nextData() > 0)
		{
		    //result = true;
		    {
			std::lock_guard<std::shared_mutex> lock(mutex_onlineUser);
			onlineUser.erase(uid);
			auto user_info = std::make_shared<userInfo>();
			user_info->sessPtr = sessPtr;
			user_info->sid = std::to_string(sid);
			user_info->username = std::string(buf_username, len_username);
			user_info->email = std::string(buf_email, len_email);
			user_info->avatar_url = std::string(buf_avatarUrl, len_avatarUrl);
			onlineUser[uid] = user_info;
		    }   	
		    sessPtr->uid = uid;
		    result = true;
		    pushFriendStatus(sessPtr->uid.value(), true);
		}
	    }
	    else
		logger.error("handleAccessTokenLogin 获取数据库连接失败");
	}
	else
	    isAccessTokenExpired = true;

	auto* func = new std::function<void()>([result, sessPtr, requests_id, isAccessTokenExpired](){
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "AccessTokenLoginResp";
	    j["Result"] = result;
		if(isAccessTokenExpired)
		    j["AccessTokenExpired"] = true;

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

void ChatServerWorker::pushNewFriendRequests(int64_t sendUID, int64_t pushUID, const std::string& verMsg)
{
    std::shared_ptr<session> pushSessPtr;
    std::shared_ptr<userInfo> sendInfoPtr;
    {
	std::shared_lock lock(mutex_onlineUser);
	auto it_push = onlineUser.find(pushUID);
	if(it_push == onlineUser.end())
	    return;
	pushSessPtr = it_push->second->sessPtr.lock();
	if(!pushSessPtr)
	    return;
	auto it_send = onlineUser.find(sendUID);
	if(it_send != onlineUser.end())
	    sendInfoPtr = it_send->second;
    }
    std::string SID;
    std::string username;
    std::string avatar_url;
    if(!sendInfoPtr)
    {
	GuardConnection con(SQLPool::getSQLPool());
	if(!con)
	{
	    logger.error("PushNewFriendRequests 获取数据库连接失败");
	    return;
	}

	std::string sql = "SELECT id, username, avatar_url FROM user WHERE snowid = ?;";
	MYSQL_BIND bind_param;
	initLongLongParam(&bind_param, &sendUID, sizeof(sendUID));
	int64_t sid;
	char buf_username[64*4];
	unsigned long len_username = 0;
	char buf_avatar[512*4];
	unsigned long len_avatar = 0;
	MYSQL_BIND result_param[3];
	initLongLongParam(&result_param[0], &sid, sizeof(sid));
	initStringParam(&result_param[1], buf_username, sizeof(buf_username), &len_username);
	initStringParam(&result_param[2], buf_avatar, sizeof(buf_avatar), &len_avatar);
	con->executeSql(sql, &bind_param, result_param);
	if(con->nextData() <= 0)
	    return;

	username = std::string(buf_username, len_username);
	avatar_url = std::string(buf_avatar, len_avatar);
	SID = std::to_string(sid);
    }
    else
    {
	username = sendInfoPtr->username;
	avatar_url = sendInfoPtr->avatar_url;
	SID = sendInfoPtr->sid;
    }

    if(pushSessPtr)
    {
	auto* func = new std::function<void()>([pushSessPtr, sendUID, verMsg, username, avatar_url, SID](){
	    json j;
	    j["Requests_id"] = "125433701";
	    j["Type"] = "PushNewFriendRequests";
	    j["SID"] = SID; 
	    j["UID"] = std::to_string(sendUID);
	    j["Username"] = username;
	    j["Avatar_Url"] = avatar_url;
	    j["VerMsg"] = verMsg; 
	    std::string data = j.dump() + "\n";
	    
	    bufferevent_write(pushSessPtr->bev, data.c_str(), data.size());
	    });
	    timeval tv{0, 0};
	    event_base_once(pushSessPtr->myself->base, -1, EV_TIMEOUT, [](evutil_socket_t, short, void* arg){
		auto* func = static_cast<std::function<void()>*>(arg);
		(*func)();
		delete func;
	}, func, &tv);
    }
}

void ChatServerWorker::pushNewFriend(int64_t sendUID, int64_t pushUID)
{
    std::shared_ptr<userInfo> pushInfoPtr;
    std::shared_ptr<userInfo> sendInfoPtr;

    std::string sendUsername;
    std::string sendSID;
    std::string sendEmail;
    std::string sendAvatar;
    bool sendIsOnline = false;
    std::string pushUsername;
    std::string pushSID;
    std::string pushEmail;
    std::string pushAvatar;
    bool pushIsOnline = false;
    {
	std::shared_lock lock(mutex_onlineUser);
	auto it_push = onlineUser.find(pushUID);
	auto it_send = onlineUser.find(sendUID);
	if(it_push == onlineUser.end() && it_send == onlineUser.end())
	    return;
	if(it_push != onlineUser.end())
	{
	    pushInfoPtr = it_push->second;
	    pushUsername = pushInfoPtr->username;
	    pushAvatar = pushInfoPtr->avatar_url;
	    pushEmail = pushInfoPtr->email;
	    pushSID = pushInfoPtr->sid;
	    pushIsOnline = true;
	}
	if(it_send != onlineUser.end())
	{
	    sendInfoPtr = it_send->second;
	    sendUsername = sendInfoPtr->username;
	    sendAvatar = sendInfoPtr->avatar_url;
	    sendEmail = sendInfoPtr->email;
	    sendSID = sendInfoPtr->sid;
	    sendIsOnline = true;
	}
    }

    if(!sendInfoPtr || !pushInfoPtr)
    {
	std::string sql = "SELECT id, username, email, avatar_url FROM user WHERE snowid = ?;";
	MYSQL_BIND bind_param;
	if(!sendInfoPtr && pushInfoPtr)
	{
	    initLongLongParam(&bind_param, &sendUID, sizeof(sendUID));
	}
	else if(sendInfoPtr && !pushInfoPtr)
	{
	    initLongLongParam(&bind_param, &pushUID, sizeof(pushUID));
	}
	else
	    return;

	MYSQL_BIND result_param[4];
	int64_t sid;
	unsigned long len_username = 0;
	char buf_username[64*4];
	unsigned long len_email = 0;
	char buf_email[128*4];
	unsigned long len_avatar = 0;
	char buf_avatar[512*4];
	initLongLongParam(&result_param[0], &sid, sizeof(sid));
	initStringParam(&result_param[1], buf_username, sizeof(buf_username), &len_username);
	initStringParam(&result_param[2], buf_email, sizeof(buf_email), &len_email);
	initStringParam(&result_param[3], buf_avatar, sizeof(buf_avatar), &len_avatar);

	GuardConnection con(SQLPool::getSQLPool());
	if(!con)
	{
	    logger.error("pushNewFriend 获取数据库连接失败");
	    return;
	}
	con->executeSql(sql, &bind_param, result_param);
	if(con->nextData() != 1)
	{
	    logger.warn("Push New Friend Search DB Error/No Data");
	    return;
	}

	if(!sendInfoPtr)
	{
	    sendUsername = std::string(buf_username, len_username);
	    sendEmail = std::string(buf_email, len_email);
	    sendAvatar = std::string(buf_avatar, len_avatar);
	    sendSID = std::to_string(sid);
	    sendIsOnline = false;
	}
	if(!pushInfoPtr)
	{
	    pushUsername = std::string(buf_username, len_username);
	    pushEmail = std::string(buf_email, len_email);
	    pushAvatar = std::string(buf_avatar, len_avatar);
	    pushSID = std::to_string(sid);
	    pushIsOnline = false;
	}
    }
    std::shared_ptr<session> sendSessPtr;
    std::shared_ptr<session> pushSessPtr;
    {
	std::shared_lock lock(mutex_onlineUser);
	auto it_push = onlineUser.find(pushUID);
	auto it_send = onlineUser.find(sendUID);
	if(it_push == onlineUser.end() && it_send == onlineUser.end())
	    return;
	if(it_push != onlineUser.end())
	    pushSessPtr = it_push->second->sessPtr.lock();
	if(it_send != onlineUser.end())
	    sendSessPtr = it_send->second->sessPtr.lock();
    }

    if(pushSessPtr)
    {
	auto* func = new std::function<void()>([pushSessPtr, sendUID, sendUsername, sendEmail, sendAvatar, sendSID, sendIsOnline](){
	    json j;
	    j["Requests_id"] = "125433702";
	    j["Type"] = "PushNewFriend";
	    j["SID"] = sendSID; 
	    j["UID"] = std::to_string(sendUID);
	    j["Username"] = sendUsername;
	    j["Avatar_Url"] = sendAvatar;
	    j["Email"] = sendEmail;
	    j["IsOnline"] = sendIsOnline;
	     
	    std::string data = j.dump() + "\n";
	    
	    bufferevent_write(pushSessPtr->bev, data.c_str(), data.size());
	    });
	    timeval tv{0, 0};
	    event_base_once(pushSessPtr->myself->base, -1, EV_TIMEOUT, [](evutil_socket_t, short, void* arg){
		auto* func = static_cast<std::function<void()>*>(arg);
		(*func)();
		delete func;
	}, func, &tv);
    }

    if(sendSessPtr)
    {
	auto* func = new std::function<void()>([sendSessPtr, pushUID, pushUsername, pushEmail, pushAvatar, pushSID, pushIsOnline](){
	    json j;
	    j["Requests_id"] = "125433702";
	    j["Type"] = "PushNewFriend";
	    j["SID"] = pushSID; 
	    j["UID"] = std::to_string(pushUID);
	    j["Username"] = pushUsername;
	    j["Avatar_Url"] = pushAvatar;
	    j["Email"] = pushEmail;
	    j["IsOnline"] = pushIsOnline;
	     
	    std::string data = j.dump() + "\n";
	    
	    bufferevent_write(sendSessPtr->bev, data.c_str(), data.size());
	    });
	    timeval tv{0, 0};
	    event_base_once(sendSessPtr->myself->base, -1, EV_TIMEOUT, [](evutil_socket_t, short, void* arg){
		auto* func = static_cast<std::function<void()>*>(arg);
		(*func)();
		delete func;
	}, func, &tv);
    }
}

void ChatServerWorker::pushFriendStatus(int64_t changeUID, bool isOnline)
{
    ThreadPool::getThreadPool()->addTask([changeUID, isOnline](){
	if(!isOnline)
	{
	    std::shared_lock lock(mutex_onlineUser);
	    if(onlineUser.find(changeUID) != onlineUser.end())
		return;
	}

	std::string sql = "SELECT user.snowid FROM friendships JOIN user ON user.snowid = friendships.user_a WHERE friendships.status = 1 AND friendships.user_b = ? UNION ALL SELECT user.snowid FROM friendships JOIN user ON user.snowid = friendships.user_b WHERE friendships.status = 1 AND friendships.user_a = ?;";
	MYSQL_BIND bind_param[2];
	int64_t changeUID_ = changeUID;
	initLongLongParam(&bind_param[0], &changeUID_, sizeof(changeUID_));
	initLongLongParam(&bind_param[1], &changeUID_, sizeof(changeUID_));
	
	int64_t pushUID;
	MYSQL_BIND result_param;
	initLongLongParam(&result_param, &pushUID, sizeof(pushUID));

	GuardConnection con(SQLPool::getSQLPool());
	if(!con)
	{
	    logger.error("pushFriendStatus 获取数据库连接失败");
	    return;
	}
	con->executeSql(sql, bind_param, &result_param);
	int ret = 0;
	while((ret = con->nextData()) == 1)
	{
	    std::shared_lock lock(mutex_onlineUser);
	    auto it_push = onlineUser.find(pushUID);
	    if(it_push != onlineUser.end())
	    {
		std::shared_ptr<session> pushSessPtr = it_push->second->sessPtr.lock();
		if(pushSessPtr)
		{
		    auto* func = new std::function<void()>([changeUID, isOnline, pushSessPtr](){
		    json j;
		    j["Requests_id"] = "125433703";
		    j["Type"] = "PushFriendStatus";
		    j["UID"] = std::to_string(changeUID);
		    j["IsOnline"] = isOnline;
	     
		    std::string data = j.dump() + "\n";
		    bufferevent_write(pushSessPtr->bev, data.c_str(), data.size());
		    });
		
		    timeval tv{0, 0};
		    event_base_once(pushSessPtr->myself->base, -1, EV_TIMEOUT, [](evutil_socket_t, short, void* arg){
			auto* func = static_cast<std::function<void()>*>(arg);
			(*func)();
			delete func;
		    }, func, &tv);
		}
	    }
	}
	if(ret < 0)
	    logger.warn("PushFriendStatus Select DB Error");
    });
}

void ChatServerWorker::pushFriendUsername(int64_t changeUID, const std::string& username)
{
    ThreadPool::getThreadPool()->addTask([changeUID, username](){
	std::string sql = "SELECT user.snowid FROM friendships JOIN user ON user.snowid = friendships.user_a WHERE friendships.status = 1 AND friendships.user_b = ? UNION ALL SELECT user.snowid FROM friendships JOIN user ON user.snowid = friendships.user_b WHERE friendships.status = 1 AND friendships.user_a = ?;";
	MYSQL_BIND bind_param[2];
	int64_t changeUID_ = changeUID;
	initLongLongParam(&bind_param[0], &changeUID_, sizeof(changeUID_));
	initLongLongParam(&bind_param[1], &changeUID_, sizeof(changeUID_));
	
	int64_t pushUID;
	MYSQL_BIND result_param;
	initLongLongParam(&result_param, &pushUID, sizeof(pushUID));

	GuardConnection con(SQLPool::getSQLPool());
	if(!con)
	{
	    logger.error("pushFriendUsername 获取数据库连接失败");
	    return;
	}
	con->executeSql(sql, bind_param, &result_param);
	int ret = 0;
	while((ret = con->nextData()) == 1)
	{
	    std::shared_lock lock(mutex_onlineUser);
	    auto it_push = onlineUser.find(pushUID);
	    if(it_push != onlineUser.end())
	    {
		std::shared_ptr<session> pushSessPtr = it_push->second->sessPtr.lock();
		if(pushSessPtr)
		{
		    auto* func = new std::function<void()>([changeUID, username, pushSessPtr](){
		    json j;
		    j["Requests_id"] = "125433705";
		    j["Type"] = "PushFriendUsername";
		    j["UID"] = std::to_string(changeUID);
		    j["Username"] = username;
	     
		    std::string data = j.dump() + "\n";
		    bufferevent_write(pushSessPtr->bev, data.c_str(), data.size());
		    });
		
		    timeval tv{0, 0};
		    event_base_once(pushSessPtr->myself->base, -1, EV_TIMEOUT, [](evutil_socket_t, short, void* arg){
			auto* func = static_cast<std::function<void()>*>(arg);
			(*func)();
			delete func;
		    }, func, &tv);
		}
	    }
	}
	if(ret < 0)
	    logger.warn("PushFriendUsername Select DB Error");
    });
}

void ChatServerWorker::pushFriendAvatar(int64_t changeUID, const std::string& avatar)
{
    ThreadPool::getThreadPool()->addTask([changeUID, avatar](){
	std::string sql = "SELECT user.snowid FROM friendships JOIN user ON user.snowid = friendships.user_a WHERE friendships.status = 1 AND friendships.user_b = ? UNION ALL SELECT user.snowid FROM friendships JOIN user ON user.snowid = friendships.user_b WHERE friendships.status = 1 AND friendships.user_a = ?;";
	MYSQL_BIND bind_param[2];
	int64_t changeUID_ = changeUID;
	initLongLongParam(&bind_param[0], &changeUID_, sizeof(changeUID_));
	initLongLongParam(&bind_param[1], &changeUID_, sizeof(changeUID_));
	
	int64_t pushUID;
	MYSQL_BIND result_param;
	initLongLongParam(&result_param, &pushUID, sizeof(pushUID));

	GuardConnection con(SQLPool::getSQLPool());
	if(!con)
	{
	    logger.error("pushFriendAvatar 获取数据库连接失败");
	    return;
	}
	con->executeSql(sql, bind_param, &result_param);
	int ret = 0;
	while((ret = con->nextData()) == 1)
	{
	    std::shared_lock lock(mutex_onlineUser);
	    auto it_push = onlineUser.find(pushUID);
	    if(it_push != onlineUser.end())
	    {
		std::shared_ptr<session> pushSessPtr = it_push->second->sessPtr.lock();
		if(pushSessPtr)
		{
		    auto* func = new std::function<void()>([changeUID, avatar, pushSessPtr](){
		    json j;
		    j["Requests_id"] = "125433706";
		    j["Type"] = "PushFriendAvatar";
		    j["UID"] = std::to_string(changeUID);
		    j["Avatar"] = avatar;
	     
		    std::string data = j.dump() + "\n";
		    bufferevent_write(pushSessPtr->bev, data.c_str(), data.size());
		    });
		
		    timeval tv{0, 0};
		    event_base_once(pushSessPtr->myself->base, -1, EV_TIMEOUT, [](evutil_socket_t, short, void* arg){
			auto* func = static_cast<std::function<void()>*>(arg);
			(*func)();
			delete func;
		    }, func, &tv);
		}
	    }
	}
	if(ret < 0)
	    logger.warn("PushFriendAvatar Select DB Error");
    });
}

void ChatServerWorker::pushNewMessage(int64_t sendUID, int64_t pushUID, int64_t messageID, int64_t timestamp, const std::string& content, int64_t convSeq)
{
    std::shared_ptr<session> pushSessPtr;
    {
	std::shared_lock lock(mutex_onlineUser);
	auto it = onlineUser.find(pushUID);
	if(it == onlineUser.end())
	    return;
	pushSessPtr = it->second->sessPtr.lock();
    }
    if(pushSessPtr)
    {
	auto* func = new std::function<void()>([sendUID, messageID, timestamp, content, pushSessPtr, convSeq](){
	    json j;
	    j["Requests_id"] = "125433704";
	    j["Type"] = "PushNewMessage";
	    j["SenderUID"] = std::to_string(sendUID);
	    j["MessageID"] = std::to_string(messageID);
	    j["Content"] = content;
	    j["TimeStamp"] = std::to_string(timestamp);
	    j["ConvSeq"] = std::to_string(convSeq);
	     
	    std::string data = j.dump() + "\n";
	    
	    bufferevent_write(pushSessPtr->bev, data.c_str(), data.size());
	    });
	    timeval tv{0, 0};
	    event_base_once(pushSessPtr->myself->base, -1, EV_TIMEOUT, [](evutil_socket_t, short, void* arg){
		auto* func = static_cast<std::function<void()>*>(arg);
		(*func)();
		delete func;
	}, func, &tv);
    }
}

void ChatServerWorker::handleSendMessage(session* sess, std::string accessToken, std::string receiverUID, const std::string content, const std::string tempmsgid, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([accessToken, receiverUID, content, requests_id, sessPtr, tempmsgid](){
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	bool result = false;
	int64_t timestamp = 0;
	std::string info;
	bool uidValid = sessPtr->uid.has_value() && sessPtr->uid.value() == uid_token && uid_token != -1;
	int64_t id = sfGen.nextid();
	int64_t nextSeq = 0;

	std::string idempotentKey = requests_id + "_" + tempmsgid;
	json j_Cache;
	bool isCache = sessPtr->myself->tryGetCache(idempotentKey, j_Cache);

	if(!isCache && uidValid && isUID(receiverUID))
	{
	    GuardConnection con(SQLPool::getSQLPool());
	    if(!con)
		info = "发送消息失败";
	    else
	    {
		SQL* con_ = con.get();
		{
		    SQLTransaction sqlTrans(con_->getMysql());

		    std::string sql = "SELECT id, status, lastaction_uid FROM friendships WHERE user_a = LEAST(?, ?) AND user_b = GREATEST(?, ?);";
		    int64_t friendUID = std::stoll(receiverUID);
		    int64_t senderUID = sessPtr->uid.value();

		    MYSQL_BIND bind_param[4];
		    initLongLongParam(&bind_param[0], &senderUID, sizeof(senderUID));
		    initLongLongParam(&bind_param[1], &friendUID, sizeof(friendUID));
		    initLongLongParam(&bind_param[2], &senderUID, sizeof(senderUID));
		    initLongLongParam(&bind_param[3], &friendUID, sizeof(friendUID));

		    int64_t conversationID;
		    int8_t status;
		    int64_t lastaction_uid;
		    MYSQL_BIND result_param[3];
		    initLongLongParam(&result_param[0], &conversationID, sizeof(conversationID));
		    initTinyIntParam(&result_param[1], &status, sizeof(status));
		    initLongLongParam(&result_param[2], &lastaction_uid, sizeof(lastaction_uid));
	    
		    con->executeSql(sql, bind_param, result_param);

		    if(con->nextData() <= 0 || status != 1)
		    {
			if(status == 3)
			{
			    if(lastaction_uid == senderUID)
				info = "您已经拉黑对方";
			    else
				info = "对方已经把您拉黑";
			}
			else
			    info = "您和对方还不是好友";
		    }
		    else
		    {  
			int64_t uid = sessPtr->uid.value();
			std::string sql_insert = "INSERT IGNORE INTO conversation_seq(conversation_id, last_conv_seq) VALUES(?, 0);";
			std::string sql1 = "UPDATE conversation_seq SET last_conv_seq = LAST_INSERT_ID(last_conv_seq + 1) WHERE conversation_id = ?;";
			MYSQL_BIND bind_param1;
			initLongLongParam(&bind_param1, &conversationID, sizeof(conversationID));
		
			int64_t conv_seq = 0;
			
			if(!con->executeSql(sql_insert, &bind_param1) || !con->executeSql(sql1, &bind_param1) || con->getAffectRows() <= 0)
			{
			    logger.warn("Send Message SELECT Error");
			    info = "发送失败，请稍后重试";
			}
			else
			{
			    conv_seq = con->getInsertID();
			    std::string sql_updateGlobalSeq = "UPDATE global_seq SET current_value = LAST_INSERT_ID(current_value + 1) WHERE seq_name = 'conv_sync';";
			    if(con->executeSql(sql_updateGlobalSeq, nullptr) && con->getAffectRows() > 0)
			    {
				int64_t newGlobalSeq = con->getInsertID();

				std::string sql_insertConversations_myself = "INSERT INTO conversations(uid, conversation_id, last_seq) VALUES(?, ?, ?) ON DUPLICATE KEY UPDATE last_seq = VALUES(last_seq);";
				MYSQL_BIND bind_param_insertConversations_myself[3];
				initLongLongParam(&bind_param_insertConversations_myself[0], &uid, sizeof(uid));
				initLongLongParam(&bind_param_insertConversations_myself[1], &conversationID, sizeof(conversationID));
				initLongLongParam(&bind_param_insertConversations_myself[2], &newGlobalSeq, sizeof(newGlobalSeq));
				    
				std::string sql_insertConversations_friend = "INSERT INTO conversations(uid, conversation_id, last_seq) VALUES(?, ?, ?) ON DUPLICATE KEY UPDATE last_seq = VALUES(last_seq);";
				MYSQL_BIND bind_param_insertConversations_friend[3];
				initLongLongParam(&bind_param_insertConversations_friend[0], &friendUID, sizeof(friendUID));
				initLongLongParam(&bind_param_insertConversations_friend[1], &conversationID, sizeof(conversationID));
				initLongLongParam(&bind_param_insertConversations_friend[2], &newGlobalSeq, sizeof(newGlobalSeq));
				    
				if(con->executeSql(sql_insertConversations_myself, bind_param_insertConversations_myself) && con->getAffectRows() > 0 && con->executeSql(sql_insertConversations_friend, bind_param_insertConversations_friend) && con->getAffectRows() > 0)
				{
				    nextSeq = conv_seq;
				    std::string sql2 = "INSERT INTO messages(snowid, conversation_id, conv_seq, sender_id, content, idempotent_key, timestamp) VALUES(?, ?, ?, ?, ?, ?, ?);";
				    MYSQL_BIND bind_param2[7];
				    initLongLongParam(&bind_param2[0], &id, sizeof(id));
				    initLongLongParam(&bind_param2[1], &conversationID, sizeof(conversationID));
				    initLongLongParam(&bind_param2[2], &nextSeq, sizeof(nextSeq));
				    initLongLongParam(&bind_param2[3], &senderUID, sizeof(senderUID));

				    std::string truncContent;
				    if(content.size() > 10000)
					truncContent = content.substr(0, 10000);
				    else
					truncContent = content;
				    unsigned long len_message = truncContent.size();
				    initStringParam(&bind_param2[4], (char*)truncContent.data(), truncContent.size(), &len_message);
				    unsigned long len_idempotent = idempotentKey.size();
				    initStringParam(&bind_param2[5], (char*)idempotentKey.data(), idempotentKey.size(), &len_idempotent);
				    timestamp = ((id >> 22) + 1704067200000LL) / 1000; 
				    initLongLongParam(&bind_param2[6], &timestamp, sizeof(timestamp));

				    con->executeSql(sql2, bind_param2);
				    if(con->getAffectRows() > 0)
				    {
					sqlTrans.commit();
					result = true;
					pushNewMessage(senderUID, friendUID, id, timestamp, truncContent, nextSeq);
				    }
				    else
				    {
					logger.warn("SendMessage INSERT Error");
					info = "发送失败，请稍后重试";
				    }
				}
				else
				{
				    logger.warn("Send Message INSERT conversations Error");
				    info = "发送失败，请稍后重试";
				}
			    }
			    else
			    {
				logger.warn("Send Message UPDATE global_seq Error");
				info = "发送失败，请稍后重试";
			    }
			}
		    }
		}
	    }
	}

	auto* func = new std::function<void()>([sessPtr, result, requests_id, id, uidValid, info, timestamp, tempmsgid, isCache, j_Cache, idempotentKey, nextSeq, receiverUID](){
	    json j;
	    if(isCache)
		j = j_Cache;
	    else if(sessPtr->myself->tryGetCache(idempotentKey, j))
	    {}
	    else
	    {
		j["Requests_id"] = requests_id;
		j["Type"] = "SendMessageResp";
		j["Result"] = result;
		j["ReceiverUID"] = receiverUID;
		if(result)
		{
		    j["MessageID"] = std::to_string(id);
		    j["TimeStamp"] = std::to_string(timestamp);
		    j["ConvSeq"] = std::to_string(nextSeq);
		}
		else
		{
		    j["Info"] = info;
		    if(!uidValid)
		    {
			j["AccessTokenExpired"] = true;
		    }
		}
		j["TempMsgID"] = tempmsgid;
		
		if(result)
		    sessPtr->myself->setCache(idempotentKey, j, 180);
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

void ChatServerWorker::handleGetConversationsFriendUIDListAndSeq(session* sess, std::string accessToken, int64_t lastConvSyncSeq, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([accessToken, requests_id, sessPtr, lastConvSyncSeq](){
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	bool uidValid = sessPtr->uid.has_value() && sessPtr->uid.value() == uid_token && uid_token != -1;
	json arr = json::array();
	
	bool result = false;
	int64_t current_value = 0;
	if(uidValid)
	{
	    std::string sql_selectGlobalSeq = "SELECT current_value FROM global_seq WHERE seq_name = 'conv_sync';";
	    GuardConnection con(SQLPool::getSQLPool());
	    MYSQL_BIND bind_selectGlobalSeq;
	    initLongLongParam(&bind_selectGlobalSeq, &current_value, sizeof(current_value));

	    if(con->executeSql(sql_selectGlobalSeq, nullptr, &bind_selectGlobalSeq) && con->nextData() > 0)
	    {
		int64_t uid = sessPtr->uid.value();
		std::string sql_selectFriendUID = "SELECT IF(f.user_a = ?, f.user_b, f.user_a) AS friend_uid FROM conversations c JOIN friendships f ON f.id = c.conversation_id WHERE uid = ? AND last_seq > ? ORDER BY last_seq DESC;";

		MYSQL_BIND bind_selectFriendUID[3];
		initLongLongParam(&bind_selectFriendUID[0], &uid, sizeof(uid));
		initLongLongParam(&bind_selectFriendUID[1], &uid, sizeof(uid));
		int64_t lastConvSyncSeq_copy = lastConvSyncSeq;
		initLongLongParam(&bind_selectFriendUID[2], &lastConvSyncSeq_copy, sizeof(lastConvSyncSeq_copy));

		MYSQL_BIND result_selectFriendUID;
		int64_t conversationID = 0;
		initLongLongParam(&result_selectFriendUID, &conversationID, sizeof(conversationID));
		
		if(con->executeSql(sql_selectFriendUID, bind_selectFriendUID, &result_selectFriendUID))
		{
		    int ret = 0;
		    while((ret = con->nextData()) > 0)
			arr.push_back(std::to_string(conversationID));

		    result = true;
		    if(ret == -1)
		    {
			logger.warn("Select最新会话ID错误");
			result = false;
		    }
		}
	    }
	}
	
	auto* func = new std::function<void()>([sessPtr, result, requests_id, uidValid, current_value, arr](){
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "GetConversationsFriendUIDListAndSeqResp";
	    j["Result"] = result;
	    if(result)
	    {
		j["NewGlobalSeq"] = std::to_string(current_value);
		j["FriendUIDArray"] = arr;
	    }
	    else
	    {
		if(!uidValid)
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

void ChatServerWorker::handleSyncMessages(session* sess, std::string accessToken, const std::string friendUID, int64_t startConvSeq, int limit, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([accessToken, requests_id, sessPtr, friendUID, startConvSeq, limit](){
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	bool uidValid = sessPtr->uid.has_value() && sessPtr->uid.value() == uid_token && uid_token != -1;
	bool result = false;
	json arr = json::array();
	if(uidValid)
	{
	    std::string sql_selectFriendConversation = "SELECT id FROM friendships WHERE user_a = LEAST(?, ?) AND user_b = GREATEST(?, ?) AND status = 1;";
	    int64_t friendUID_int = std::stoll(friendUID);
	    int64_t uid = sessPtr->uid.value();
	    int64_t conversationID = 0;
	    std::string sql = "SELECT snowid, conv_seq, sender_id, content, timestamp FROM messages WHERE conversation_id = ? AND conv_seq > ? ORDER BY conv_seq ASC LIMIT ?;";
	    
	    MYSQL_BIND bind_selectFriendUID[4];
	    initLongLongParam(&bind_selectFriendUID[0], &friendUID_int, sizeof(friendUID_int));
	    initLongLongParam(&bind_selectFriendUID[1], &uid, sizeof(uid));
	    initLongLongParam(&bind_selectFriendUID[2], &friendUID_int, sizeof(friendUID_int));
	    initLongLongParam(&bind_selectFriendUID[3], &uid, sizeof(uid));

	    MYSQL_BIND result_selectFriendUID;
	    initLongLongParam(&result_selectFriendUID, &conversationID, sizeof(conversationID));
	    GuardConnection con(SQLPool::getSQLPool());

	    if(con->executeSql(sql_selectFriendConversation, bind_selectFriendUID, &result_selectFriendUID) && con->nextData() > 0)
	    {
		MYSQL_BIND bind_selectMessage[3];
		int64_t startConvSeq_copy = startConvSeq;
		int limit_copy = limit;
		initLongLongParam(&bind_selectMessage[0], &conversationID, sizeof(conversationID));
		initLongLongParam(&bind_selectMessage[1], &startConvSeq_copy, sizeof(startConvSeq_copy));
		initIntParam(&bind_selectMessage[2], &limit_copy, sizeof(limit_copy));
	    
		MYSQL_BIND result_selectMessage[5];
		int64_t result_messageID = 0;
		int64_t result_convSeq = 0;
		int64_t result_senderID = 0;
		char result_buf_content[10000 * 4 + 1];
		unsigned long len_content = 0;
		int64_t result_timestamp = 0;
		initLongLongParam(&result_selectMessage[0], &result_messageID, sizeof(result_messageID));
		initLongLongParam(&result_selectMessage[1], &result_convSeq, sizeof(result_convSeq));
		initLongLongParam(&result_selectMessage[2], &result_senderID, sizeof(result_senderID));
		initStringParam(&result_selectMessage[3], result_buf_content, sizeof(result_buf_content), &len_content);
		initLongLongParam(&result_selectMessage[4], &result_timestamp, sizeof(result_timestamp));

		if(con->executeSql(sql, bind_selectMessage, result_selectMessage))
		{
		    int count = 0;
		    int ret = 0;
		    while((ret = con->nextData()) > 0)
		    {
			std::string content(result_buf_content, len_content);
			len_content = 0;
			json item;
			item["MessageID"] = std::to_string(result_messageID);
			item["ConvSeq"] = std::to_string(result_convSeq);
			item["SenderUID"] = std::to_string(result_senderID);
			item["Content"] = content;
			item["Timestamp"] = std::to_string(result_timestamp);
			arr.push_back(item);
			count++;
			if(count >= limit)
			{
			    json j;
			    j["Requests_id"] = requests_id;
			    j["Type"] = "SyncNewMessagesResp";
			    j["Result"] = true;
			    j["FriendUID"] = friendUID;
			    j["Messages"] = arr;
		    
			    {
				std::lock_guard<std::mutex> lock(sessPtr->mutex_sendQueue);
				sessPtr->sendQueue.push(j.dump() + "\n");
			    }
			    sessPtr->myself->handleSendQueueTask(sessPtr);
			
			    arr.clear();
			    count = 0;

			}
			result = true;
		    }

		    if(ret < 0)
		    {
			result = false;
			logger.warn("查询消息错误");
		    }
		    else
		    {
			result = true;
			json j;
			j["Requests_id"] = requests_id;
			j["Type"] = "SyncNewMessagesResp";
			j["Result"] = true;
			j["FriendUID"] = friendUID;
			j["Messages"] = arr;
			{
			    std::lock_guard<std::mutex> lock(sessPtr->mutex_sendQueue);
			    sessPtr->sendQueue.push(j.dump() + "\n");
			}
			sessPtr->myself->handleSendQueueTask(sessPtr);
		    }
		}
		else
		    logger.warn("同步消息执行sql失败");
	    }
	    else
		logger.warn("同步消息SELECT 好友会话ID失败");
	    if(result)
	    {
		json j;
		j["Requests_id"] = requests_id;
		j["Type"] = "SyncNewMessagesResp";
		j["Result"] = true;
		j["FriendUID"] = friendUID;
		arr.clear();
		j["Messages"] = arr;
		{
		    std::lock_guard<std::mutex> lock(sessPtr->mutex_sendQueue);
		    sessPtr->sendQueue.push(j.dump() + "\n");
		}
		sessPtr->myself->handleSendQueueTask(sessPtr);
		return;
	    }
	}

	auto* func = new std::function<void()>([sessPtr, result, requests_id, uidValid, friendUID](){
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "SyncNewMessagesResp";
	    j["Result"] = result;
	    j["FriendUID"] = friendUID;
	    if(!result)
	    {
		if(!uidValid)
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

void ChatServerWorker::handleSendQueueTask(std::shared_ptr<session> sessPtr)
{
    bool isRunning = false;
    if(!sessPtr->isQueueRunning.compare_exchange_strong(isRunning, true))
	return;

    auto* func = new std::function<void()>([sessPtr](){
	while(true)
	{
	    std::string data;
	    {
		std::lock_guard<std::mutex> lock(sessPtr->mutex_sendQueue);
		if(sessPtr->sendQueue.empty())
		{
		    sessPtr->isQueueRunning = false;
		    break;
		}
		data = sessPtr->sendQueue.front();
		sessPtr->sendQueue.pop();
	    }
	    bufferevent_write(sessPtr->bev, data.c_str(), data.size());
	}
    });
    timeval tv{0, 0};
    event_base_once(sessPtr->myself->base, -1, EV_TIMEOUT, [](evutil_socket_t, short, void* arg){
    auto* func = static_cast<std::function<void()>*>(arg);
	(*func)();
	delete func;
    }, func, &tv);
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
    if(buffer_length == 0 || buf_string == nullptr)
    {
	pargma->buffer = nullptr;
	pargma->buffer_length = 0;
	if(length)
	{
	    *length = 0;
	    pargma->length = length;
	}
    }
    else
    {
	pargma->buffer = buf_string;
	pargma->buffer_length = buffer_length;
	if(length)
	    pargma->length = length;
    }
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

void ChatServerWorker::initIntParam(MYSQL_BIND* pargma, int* buf_int, size_t buffer_length)
{
    memset(pargma, 0, sizeof(*pargma));
    pargma->buffer_type = MYSQL_TYPE_LONG;
    pargma->buffer = buf_int;
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

void ChatServerWorker::setCache(const std::string &requests_id, const json &responseJson, int seconds)
{
    std::unique_lock lock(this->mutex_idempotentCache);
    idempotentRecord idr;
    idr.j = responseJson;
    idr.expiredTime = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    this->idempotentCache[requests_id] = idr;
    if(this->idempotentCache.size() > MAX_CACHE_SIZE)
    {
	lock.unlock();
	cleanExpiredCache();
    }
}

bool ChatServerWorker::tryGetCache(const std::string &requests_id, json &outJson)
{
    std::shared_lock lock(this->mutex_idempotentCache);
    auto it = this->idempotentCache.find(requests_id);
    if(it != this->idempotentCache.end() && std::chrono::steady_clock::now() < it->second.expiredTime)
    {
	outJson = it->second.j;
	return true;
    }
    return false;
}

void ChatServerWorker::cleanExpiredCache()
{
    std::unique_lock lock(this->mutex_idempotentCache);
    auto now = std::chrono::steady_clock::now();
    for(auto it = this->idempotentCache.begin(); it != this->idempotentCache.end();)
    {
	if(now >= it->second.expiredTime)
	    it = this->idempotentCache.erase(it);
	else
	    it++;
    }

    if(idempotentCache.size() > MAX_CACHE_SIZE)
    {
	logger.warn("Idempotent Cache Size limit, Clean ALL");
	this->idempotentCache.clear();
    }

}
