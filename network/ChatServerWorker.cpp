#include "ChatServerWorker.h"
#include "GlobalVar.h"
#include "GuardConnect.h"
#include "SQLPool.h"
#include "SQLTransaction.h"
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
#include <mysql/mysql.h>
#include <set>
#include <shared_mutex>
#include <sodium/core.h>
#include <sodium/crypto_hash_sha256.h>
#include <sodium/crypto_pwhash.h>
#include <sodium/randombytes.h>
#include <sodium/utils.h>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
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
    else if(type == "UpdateGroupNameRequest")
    {
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json no AccessToken");
	    return;
	}
	if(!j.contains("ConversationID") || !j["ConversationID"].is_string() || !j.contains("GroupName") || !j["GroupName"].is_string())
	{
	    logger.warn("Json no param");
	    return;
	}
	handleUpdateGroupNameRequest(sess, j.value("AccessToken", ""), j.value("ConversationID", ""), j.value("GroupName", ""), requests_id.str());
    }
    else if(type == "UpdateGroupAvatarRequest")
    {
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json no AccessToken");
	    return;
	}
	if(!j.contains("ConversationID") || !j["ConversationID"].is_string() || !j.contains("AvatarUrl") || !j["AvatarUrl"].is_string())
	{
	    logger.warn("Json no param");
	    return;
	}
	handleUpdateGroupAvatarRequest(sess, j.value("AccessToken", ""), j.value("ConversationID", ""), j.value("AvatarUrl", ""), requests_id.str());
    }
    else if(type == "DeleteFriend")
    {
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json no AccessToken");
	    return;
	}
	if(!j.contains("ConversationID") || !j["ConversationID"].is_string())
	{
	    logger.warn("Json no param");
	    return;
	}
	handleDeleteFriendRequest(sess, j.value("AccessToken", ""), j.value("ConversationID", ""), requests_id.str());
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
    else if(type == "HandleNewGroupJoinRequest")
    {
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json No AccessToken");
	    return;
	}
	if(!j.contains("HandleRequestID") || !j["HandleRequestID"].is_string() || !j.contains("Status") || !j["Status"].is_boolean())
	{
	    logger.warn("处理群聊申请缺少参数");
	    return;
	}

	std::string handle_requestID = j.value("HandleRequestID", "");
	bool isAgree = j.value("Status", false);
	handleHandleNewGroupJoinRequest(sess, handle_requestID, isAgree, j.value("AccessToken", ""), requests_id.str());
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
    else if(type == "GetNewGroupJoinRequestsList")
    {
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json No AccessToken");
	    return;
	}
	handleGetNewGroupJoinRequestsList(sess, j.value("AccessToken", ""), requests_id.str());
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
    else if(type == "GetGroupList")
    {
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json No AccessToken");
	    return;
	}
	handleGetGroupList(sess, j.value("AccessToken", ""), requests_id.str());
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
	if(!j.contains("ConversationID") || !j["ConversationID"].is_string() || !j.contains("Content") || !j["Content"].is_string() || !j.contains("TempMsgID") || !j["TempMsgID"].is_string() || !j.contains("ContentType") || !j["ContentType"].is_number())
	{
	    logger.warn("Json No Param");
	    return;
	}
	handleSendMessage(sess, j.value("AccessToken", ""), j.value("ConversationID", ""), j.value<int8_t>("ContentType", 0),j.value("Content", ""), j.value("TempMsgID", ""), requests_id.str());
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
	if(!j.contains("ConversationID") || !j["ConversationID"].is_string() || !j.contains("StartConvSeq") || !j["StartConvSeq"].is_number() || !j.contains("Limit") || !j["Limit"].is_number())
	{
	    logger.warn("Json No Param");
	    return;
	}
	handleSyncMessages(sess, j.value("AccessToken", ""), j.value("ConversationID", ""), j.value<int64_t>("StartConvSeq", 0), j.value<int64_t>("Limit", 0), requests_id.str());
    }
    else if(type == "CreateGroupRequest")
    {
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json No AccessToken");
	    return;
	}
	if(!j.contains("GroupName") || !j["GroupName"].is_string())
	{
	    logger.warn("Json No GroupName");
	    return;
	}
	if(!j.contains("InviteUIDList") || !j["InviteUIDList"].is_array())
	{
	    logger.warn("Json No InviteUIDList");
	    return;
	}

	std::vector<std::string> inviteUIDs;
	inviteUIDs.reserve(j["InviteUIDList"].size());
	for(const auto& item : j["InviteUIDList"])
	{
	    if(item.is_string())
		inviteUIDs.emplace_back(item.get<std::string>());
	}

	if(inviteUIDs.empty())
	{
	    logger.warn("inviteUIDs is empty");
	    return;
	}

	handleCreateGroupRequest(sess, j.value("GroupName", ""), std::move(inviteUIDs), j.value("InviteMsg", ""), j.value("AccessToken", ""), requests_id.str());
    }
    else if(type == "ApplyJoinGroupRequest")
    {
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json No AccessToken");
	    return;
	}
	if(!j.contains("GroupID") || !j["GroupID"].is_string() || !j.contains("VerMsg") || !j["VerMsg"].is_string())
	{
	    logger.warn("Json No Param");
	    return;
	}
	handleApplyJoinGroupRequest(sess, j.value("AccessToken", ""), j.value("GroupID", ""), j.value("VerMsg", ""), requests_id.str());
    }
    else if(type == "GetGroupInfoRequest")
    {
	if(!j.contains("AccessToken") || !j["AccessToken"].is_string())
	{
	    logger.warn("Json No AccessToken");
	    return;
	}
	if(!j.contains("ConversationID") || !j["ConversationID"].is_string())
	{
	    logger.warn("Json No Param");
	    return;
	}
	handleGetGroupInfo(sess, j.value("AccessToken", ""), j.value("ConversationID", ""), requests_id.str());
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
			
				    if(!con1->executeSql(sql1, bind_param))
				    {
					logger.warn("插入长期Token失败");
					info = "登陆错误，请稍候再试";
				    }
				    else
				    {
					my_ulonglong rows = con1->getAffectRows();
					if(rows >= (my_ulonglong)-2)
					{
					    logger.warn("插入长期Token affected error");
					    info = "登陆错误，请稍候再试";
					}
					else
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
						con1->freeResult();
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
	    
		    if(!con->executeSql(sql, bind_param))
			info = "注册失败，请稍候重试";
		    else
		    {
			my_ulonglong rows = con->getAffectRows();
			if(rows == 0 || rows >= (my_ulonglong)-2)
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
		    else
		    {
			my_ulonglong rows = con->getAffectRows();
			if(rows == 0 || rows >= (my_ulonglong)-2)
			    logger.warn("UpdateAvatar affected 0 rows");
			else
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

	if(!j.value("AccessTokenExpired", false))
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

		if(!con->executeSql(sql, bind_param))
		{
		    logger.warn("UpdateUsername Error");
		    result = false;
		}
		else
		{
		    my_ulonglong rows = con->getAffectRows();
		    if(rows >= (my_ulonglong)-2)
		    {
			logger.warn("UpdateUsername affected error");
			result = false;
		    }
		    else
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
		if(!j.value("AccessTokenExpired", false))
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

void ChatServerWorker::handleUpdateGroupNameRequest(session* sess, std::string accessToken, std::string conversationID, std::string groupName, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([accessToken, conversationID, groupName, requests_id, sessPtr](){
	json j_Cache;
	bool isCache = sessPtr->myself->tryGetCache(requests_id, j_Cache);
	bool result = false;
	std::string info;
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	bool uidValid = sessPtr->uid.has_value() && sessPtr->uid.value() == uid_token && uid_token != -1;

	if(groupName.empty())
	    info = "群名不能为空";
	else if(groupName.size() > 64)
	    info = "群名长度不能大于64个字符";
	else if(uidValid)
	{
	    GuardConnection con(SQLPool::getSQLPool());
	    if(!con)
		logger.error("UpdateGroupName get con error");
	    else
	    {
		SQLTransaction sqlTrans(con->getMysql());
		bool ok = [&]() -> bool {
		    int64_t conversationID_int = std::stoll(conversationID);
		    {
			std::string sql = "SELECT 1 FROM chat_groups WHERE snowid = ? AND owner_uid = ? AND status = 0;";
			MYSQL_BIND bind_param[2];
			initLongLongParam(&bind_param[0], &conversationID_int, sizeof(conversationID_int));
			initLongLongParam(&bind_param[1], &uid_token, sizeof(uid_token));

			int8_t status = 0;
			MYSQL_BIND result_param;
			initTinyIntParam(&result_param, &status, sizeof(status));
			if(!con->executeSql(sql, bind_param, &result_param) || con->nextData() <= 0)
			{
			    info = "用户权限不足";
			    return false;
			}
			con->freeResult();
		    }

		    {
			std::string sql = "UPDATE chat_groups SET name = ? WHERE snowid = ? AND status = 0;";
			MYSQL_BIND bind_param[2];
			unsigned long len_groupName = groupName.size();
			initStringParam(&bind_param[0], (char*)groupName.data(), groupName.size(), &len_groupName);
			initLongLongParam(&bind_param[1], &conversationID_int, sizeof(conversationID_int));

			if(!con->executeSql(sql, bind_param))
			{
			    logger.error("UpdateGroupName execute sql failed");
			    return false;
			}

			my_ulonglong rows = con->getAffectRows();
			if(rows <= 0 || rows >= (my_ulonglong)-2)
			{
			    logger.error("UpdateGroupName update affected rows 0");
			    return false;
			}
		    }
		    return true;
		}();

		if(ok)
		{
		    con->freeResult();
		    sqlTrans.commit();
		    result = true;
		}
	    }
	}
	auto* func = new std::function<void()>([sessPtr, result, requests_id, uidValid, isCache, j_Cache, conversationID, groupName](){
	    json j;
	    if(isCache)
		j = j_Cache;
	    else
	    {
		j["Requests_id"] = requests_id;
		j["Type"] = "UpdateGroupNameRequestResp";
		j["Result"] = result;
		j["ConversationID"] = conversationID;
		if(result)
		    j["GroupName"] = groupName;
		if(!uidValid)
		    j["AccessTokenExpired"] = true;
		
		if(!j.value("AccessTokenExpired", false))
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

void ChatServerWorker::handleUpdateGroupAvatarRequest(session* sess, std::string accessToken, std::string conversationID, std::string avatarUrl, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([accessToken, conversationID, avatarUrl, requests_id, sessPtr](){
	json j_Cache;
	bool isCache = sessPtr->myself->tryGetCache(requests_id, j_Cache);
	bool result = false;
	std::string info;
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	bool uidValid = sessPtr->uid.has_value() && sessPtr->uid.value() == uid_token && uid_token != -1;

	if(avatarUrl.empty())
	    info = "上传失败，请稍候重试";
	else if(uidValid)
	{
	    GuardConnection con(SQLPool::getSQLPool());
	    if(!con)
		logger.error("UpdateGroupAvatar get con error");
	    else
	    {
		SQLTransaction sqlTrans(con->getMysql());
		bool ok = [&]() -> bool {
		    int64_t conversationID_int = std::stoll(conversationID);
		    {
			std::string sql = "SELECT 1 FROM chat_groups WHERE snowid = ? AND owner_uid = ? AND status = 0;";
			MYSQL_BIND bind_param[2];
			initLongLongParam(&bind_param[0], &conversationID_int, sizeof(conversationID_int));
			initLongLongParam(&bind_param[1], &uid_token, sizeof(uid_token));

			int8_t status = 0;
			MYSQL_BIND result_param;
			initTinyIntParam(&result_param, &status, sizeof(status));
			if(!con->executeSql(sql, bind_param, &result_param) || con->nextData() <= 0)
			{
			    info = "用户权限不足";
			    return false;
			}
			con->freeResult();
		    }

		    {
			std::string sql = "UPDATE chat_groups SET avatar_url = ? WHERE snowid = ? AND status = 0;";
			MYSQL_BIND bind_param[2];
			unsigned long len_avatarUrl = avatarUrl.size();
			initStringParam(&bind_param[0], (char*)avatarUrl.data(), avatarUrl.size(), &len_avatarUrl);
			initLongLongParam(&bind_param[1], &conversationID_int, sizeof(conversationID_int));

			if(!con->executeSql(sql, bind_param))
			{
			    logger.error("UpdateGroupAvatar execute sql failed");
			    return false;
			}

			my_ulonglong rows = con->getAffectRows();
			if(rows <= 0 || rows >= (my_ulonglong)-2)
			{
			    logger.error("UpdateGroupAvatar update affected rows 0");
			    return false;
			}
		    }
		    return true;
		}();

		if(ok)
		{
		    con->freeResult();
		    sqlTrans.commit();
		    result = true;
		}
	    }
	}
	auto* func = new std::function<void()>([sessPtr, result, requests_id, uidValid, isCache, j_Cache, conversationID, avatarUrl](){
	    json j;
	    if(isCache)
		j = j_Cache;
	    else
	    {
		j["Requests_id"] = requests_id;
		j["Type"] = "UpdateGroupAvatarRequestResp";
		j["Result"] = result;
		j["ConversationID"] = conversationID;
		if(result)
		    j["AvatarUrl"] = avatarUrl;
		if(!uidValid)
		    j["AccessTokenExpired"] = true;
		
		if(!j.value("AccessTokenExpired", false))
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

void ChatServerWorker::handleDeleteFriendRequest(session* sess, std::string accessToken, std::string conversationID, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([accessToken, conversationID, requests_id, sessPtr](){
	json j_Cache;
	bool isCache = sessPtr->myself->tryGetCache(requests_id, j_Cache);
	bool result = false;
	std::string info;
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	bool uidValid = sessPtr->uid.has_value() && sessPtr->uid.value() == uid_token && uid_token != -1;
	int64_t pushUID = 0;

	if(conversationID.empty())
	    info = "删除失败，请稍候重试";
	else if(uidValid)
	{
	    GuardConnection con(SQLPool::getSQLPool());
	    if(!con)
		logger.error("DeleteFriend get con error");
	    else
	    {
		SQLTransaction sqlTrans(con->getMysql());
		bool ok = [&]() -> bool {
		    int64_t conversationID_int = std::stoll(conversationID);
		    int8_t status = 0;
		    int64_t user_a = 0;
		    int64_t user_b = 0;
		    {
			std::string sql = "SELECT user_a, user_b, status FROM friendships WHERE id = ?;";
			MYSQL_BIND bind_param;
			initLongLongParam(&bind_param, &conversationID_int, sizeof(conversationID_int));

			MYSQL_BIND result_param[3];
			initLongLongParam(&result_param[0], &user_a, sizeof(user_a));
			initLongLongParam(&result_param[1], &user_b, sizeof(user_b));
			initTinyIntParam(&result_param[2], &status, sizeof(status));
			if(!con->executeSql(sql, &bind_param, result_param) || con->nextData() <= 0)
			{
			    info = "好友不存在";
			    return false;
			}
			con->freeResult();
		    }

		    if(uid_token != user_a && uid_token != user_b)
		    {
			info = "好友不存在";
			return false;
		    }

		    if(status == 2)
			return true;

		    if(status != 1)
		    {
			info = "好友状态异常";
			return false;
		    }

		    {
			std::string sql = "UPDATE friendships SET verification_msg = NULL, lastaction_uid = ?, status = 2 WHERE id = ? AND status = 1;";
			MYSQL_BIND bind_param[2];
			initLongLongParam(&bind_param[0], &uid_token, sizeof(uid_token));
			initLongLongParam(&bind_param[1], &conversationID_int, sizeof(conversationID_int));

			if(!con->executeSql(sql, bind_param))
			{
			    logger.error("DeleteFriend execute sql failed");
			    return false;
			}

			my_ulonglong rows = con->getAffectRows();
			if(rows <= 0 || rows >= (my_ulonglong)-2)
			{
			    logger.error("DeleteFriend update affected rows 0");
			    return false;
			}
		    }
		    pushUID = uid_token == user_a ? user_b : user_a;
		    return true;
		}();

		if(ok)
		{
		    sqlTrans.commit();
		    result = true;
		}
	    }
	}
	auto* func = new std::function<void()>([sessPtr, result, requests_id, uidValid, isCache, j_Cache, conversationID, info](){
	    json j;
	    if(isCache)
		j = j_Cache;
	    else
	    {
		j["Requests_id"] = requests_id;
		j["Type"] = "DeleteFriendResp";
		j["Result"] = result;
		j["ConversationID"] = conversationID;
		if(!result)
		    j["Info"] = info;

		if(!uidValid)
		    j["AccessTokenExpired"] = true;
		
		if(!j.value("AccessTokenExpired", false))
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

	if(result && pushUID != 0)
	    pushFriendDeleted(pushUID, std::stoll(conversationID));
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

			    if(!con->executeSql(sql1, bind_param1))
				info = "申请添加好友失败，请稍后重试";
			    else
			    {
				my_ulonglong rows = con->getAffectRows();
				if(rows >= (my_ulonglong)-2)
				{
				    logger.warn("AddNewFriendRequest upsert affected error");
				    info = "申请添加好友失败，请稍候重试";
				}
				else if(rows == 1 || rows == 2)
				{
				    result = true;
				    pushNewFriendRequests(sessPtr->uid.value(), userb_uid, verification_msg);
				}
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

		if(!j.value("AccessTokenExpired", false))
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

void ChatServerWorker::handleCreateGroupRequest(session* sess, std::string groupName, std::vector<std::string> inviteUIDs, std::string inviteMsg, std::string accessToken, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, groupName, inviteUIDs = std::move(inviteUIDs), inviteMsg, accessToken, requests_id](){
	json j_Cache;
	bool isCache = sessPtr->myself->tryGetCache(requests_id, j_Cache);
	bool result = false;
	bool uidValid = false;
	std::string info;
	std::string groupID;
	std::string conversationID;

	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	uidValid = sessPtr->uid.has_value() && uid_token != -1 && sessPtr->uid.value() == uid_token;
    
	if(isCache)
	{}
	else if(!uidValid)
	    result = false; 
	else if(groupName.empty())
	    info = "群名称不能为空";
	else if(groupName.size() > 64)
	    info = "群名称不能超过64个字符";
	else
	{
	    int64_t ownerUID = sessPtr->uid.value();
	    std::set<int64_t> inviteeSet;
	    bool hasBadID = false;

	    for(const auto& uid : inviteUIDs)
	    {
		if(!isSnowID(uid))
		{
		    hasBadID = true;
		    break;
		}
		int64_t uid_int = std::stoll(uid);;
		if(uid_int == ownerUID)
		    continue;
		inviteeSet.insert(uid_int);
	    }

	    if(hasBadID)
		info = "邀请成员ID不合法";
	    else if(inviteeSet.empty())
		info = "至少邀请一位好友";
	    else if(inviteeSet.size() > 500)
		info = "邀请人数超出限制 (人数 <= 500)";
	    else
	    {
		std::vector<int64_t> vector_invitee(inviteeSet.begin(), inviteeSet.end());
		std::vector<std::pair<int64_t, int64_t>> inviteeAndRowSnowID;
		int64_t groupSnowID = sfGen.nextid();
		conversationID = std::to_string(groupSnowID);

		GuardConnection con(SQLPool::getSQLPool());
		if(!con)
		    info = "创建群聊失败，请稍后重试";
		else
		{
		    std::string sql_checkInvitee = "SELECT COUNT(*) FROM user WHERE status = 0 AND snowid IN (";
		    for(size_t i = 0; i < vector_invitee.size(); i++)
		    {
			if(i > 0)
			    sql_checkInvitee += ",";
			sql_checkInvitee += std::to_string(vector_invitee[i]);
		    }
		    sql_checkInvitee += ");";

		    int64_t existsCount = 0;
		    MYSQL_BIND result_param;
		    initLongLongParam(&result_param, &existsCount, sizeof(existsCount));
		    
		    con->executeSql(sql_checkInvitee, nullptr, &result_param);
		    if(con->nextData() != 1 || existsCount != static_cast<int64_t>(vector_invitee.size()))
			info = "邀请列表中存在无效用户";
		    else
		    {
			SQL* con_ = con.get();
			SQLTransaction sqlTrans(con_->getMysql());
			int64_t groupID_int = 0;
			bool ok = [&]() -> bool{
			    {
				std::string sql = "INSERT INTO chat_groups(snowid, owner_uid, name, created_at, status) VALUES(?, ?, ?, NOW(), 0);";

				MYSQL_BIND bind_param[3];
				int64_t groupSnowID_copy = groupSnowID;
				initLongLongParam(&bind_param[0], &groupSnowID_copy, sizeof(groupSnowID_copy));
				int64_t ownerUID_copy = ownerUID;
				initLongLongParam(&bind_param[1], &ownerUID_copy, sizeof(ownerUID_copy));
				unsigned long len_groupName = groupName.size();
				initStringParam(&bind_param[2], (char*)groupName.data(), groupName.size(), &len_groupName);
				if(!con_->executeSql(sql, bind_param))
				{
				    logger.error("insert chat_groups failed");
				    return false;
				}
				
				my_ulonglong rows = con_->getAffectRows();
				if(rows == 0 || rows >= (my_ulonglong)-2)
				{
				    logger.error("insert chat_groups affected 0 rows");
				    return false;
				}
				groupID_int = con_->getInsertID();
			    }
			    groupID = std::to_string(groupID_int);
			    
			    {
				std::string sql = "INSERT INTO group_members(snowid, conversation_id, uid, role, join_time, status) VALUES(?, ?, ?, 0, NOW(), 0);";
				MYSQL_BIND bind_param[3];
				int64_t memberSnowid = sfGen.nextid();
				initLongLongParam(&bind_param[0], &memberSnowid, sizeof(memberSnowid));
				int64_t conversationID_int = groupSnowID;
				initLongLongParam(&bind_param[1], &conversationID_int, sizeof(conversationID_int));
				int64_t ownerUID_copy = ownerUID;
				initLongLongParam(&bind_param[2], &ownerUID_copy, sizeof(ownerUID_copy));

				if(!con_->executeSql(sql, bind_param))
				{
				    logger.error("insert owner member failed");
				    return false;
				}

				my_ulonglong rows = con_->getAffectRows();
				if(rows == 0 || rows >= (my_ulonglong)-2)
				{
				    logger.error("insert owner member affected 0 rows");
				    return false;
				}
			    }
			    
			    for(int64_t invitee : vector_invitee)
			    {
				int64_t rowSnowID = sfGen.nextid();
				std::string sql = "INSERT INTO group_join_requests(snowid, conversation_id, initiator_uid, target_uid, source, verification_msg, status) VALUES(?, ?, ?, ?, 0, ?, 0);";
				MYSQL_BIND bind_param[5];
				initLongLongParam(&bind_param[0], &rowSnowID, sizeof(rowSnowID));
				int64_t conversationID_int = groupSnowID;
				initLongLongParam(&bind_param[1], &conversationID_int, sizeof(conversationID_int));
				int64_t inviter_uid = ownerUID;
				initLongLongParam(&bind_param[2], &inviter_uid, sizeof(inviter_uid));
				initLongLongParam(&bind_param[3], &invitee, sizeof(invitee));
				unsigned long len_msg = inviteMsg.size();
				initStringParam(&bind_param[4], (char*)inviteMsg.data(), inviteMsg.size(), &len_msg);

				if(!con_->executeSql(sql, bind_param))
				{
				    logger.error("insert invitation failed");
				    return false;
				}

				my_ulonglong rows = con_->getAffectRows();
				if(rows == 0 || rows >= (my_ulonglong)-2)
				{
				    logger.error("insert invitation affected 0 rows");
				    return false;
				}
				inviteeAndRowSnowID.emplace_back(invitee, rowSnowID);
			    }
			    {
				std::string sql = "INSERT INTO conversation_seq(conversation_id, conversation_type, last_conv_seq) VALUES(?, 1, 0);";
				MYSQL_BIND bind_param;
				unsigned long len_conversationID = conversationID.size();
				initStringParam(&bind_param, (char*)conversationID.data(), conversationID.size(), &len_conversationID);

				if(!con_->executeSql(sql, &bind_param))
				{
				    logger.error("insert conversation_seq failed");
				    return false;
				}

				my_ulonglong rows = con_->getAffectRows();
				if(rows == 0 || rows >= (my_ulonglong)-2)
				{
				    logger.error("insert conversation_seq affected 0 rows");
				    return false;
				}
			    }
			    return true;
			}();

			if(ok)
			{
			    con_->freeResult();
			    sqlTrans.commit();
			    result = true;
			    info = "创建群聊成功";
			    
			    std::string ownerName;
			    {
				std::shared_lock lock(mutex_onlineUser);
				auto it = onlineUser.find(ownerUID);
				if(it != onlineUser.end())
				    ownerName = it->second->username;
			    }

			    for(const auto& ir : inviteeAndRowSnowID)
				pushGroupJoinRequest(ir.first, ir.second, groupName, 0, "", groupName, 0, ownerName, inviteMsg);
			}
			else
			    info = "创建群聊失败, 请稍候重试";
		    }
		}
	    }
	}
	auto* func = new std::function<void()>([result, sessPtr, requests_id, uidValid, info, groupID, conversationID, j_Cache, isCache, groupName](){
	    json j;
	    if(isCache)
	    {
		j = j_Cache;
	    }
	    else
	    {
		j["Requests_id"] = requests_id;
		j["Type"] = "CreateGroupRequestResp";
		j["Result"] = result;
		j["Info"] = info;
		if(result)
		{
		    j["GroupID"] = groupID;
		    j["ConversationID"] = conversationID;
		    j["GroupName"] = groupName;
		}
		else
		{
		    if(!uidValid)
			j["AccessTokenExpired"] = true;
		}

		if(result)
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

void ChatServerWorker::handleApplyJoinGroupRequest(session* sess, std::string accessToken, std::string groupID, std::string verMsg, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, accessToken, groupID, verMsg, requests_id](){
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	bool uidValid = sessPtr->uid.has_value() && uid_token != -1 && sessPtr->uid.value() == uid_token;
	bool result = false;
	std::string info;

	int64_t requestID;
	int64_t conversationID_int = 0;
	int64_t owner_uid = 0;
	std::string groupName;
	std::string applicantName;
	std::string applicantAvatarUrl;
	std::string applicantSID;

	if(uidValid)
	{
	    if(!groupID.empty() && groupID.find_first_not_of("0123456789") == std::string::npos && verMsg.size() <= 30)
	    {
		int64_t applicant_uid = sessPtr->uid.value();
		int64_t groupID_int = std::stoll(groupID);

		GuardConnection con(SQLPool::getSQLPool());
		if(!con)
		    info = "申请失败，请稍候重试";
		else
		{
		    SQLTransaction sqlTrans(con->getMysql());
		    bool ok = [&]() -> bool {
			{
			    std::string sql = "SELECT snowid, owner_uid, name FROM chat_groups WHERE id = ? AND status = 0 LIMIT 1;";
			    MYSQL_BIND bind_param;
			    initLongLongParam(&bind_param, &groupID_int, sizeof(groupID_int));

			    char buf_groupName[64 * 4];
			    unsigned long len_groupName = 0;
			    MYSQL_BIND result_param[3];
			    initLongLongParam(&result_param[0], &conversationID_int, sizeof(conversationID_int));
			    initLongLongParam(&result_param[1], &owner_uid, sizeof(owner_uid));
			    initStringParam(&result_param[2], buf_groupName, sizeof(buf_groupName), &len_groupName);

			    if(!con->executeSql(sql, &bind_param, result_param))
			    {
				logger.error("ApplyJoinGroup SELECT chat_groups Error");
				info = "申请失败，请稍候重试";
				return false;
			    }
			    if(con->nextData() <= 0)
			    {
				info = "群聊不存在或已解散";
				return false;
			    }
			    groupName = std::string(buf_groupName, len_groupName);
			}

			{
			    std::string sql = "SELECT COUNT(*) FROM group_members WHERE conversation_id = ? AND uid = ? AND status = 0;";
			    MYSQL_BIND bind_param[2];
			    initLongLongParam(&bind_param[0], &conversationID_int, sizeof(conversationID_int));
			    initLongLongParam(&bind_param[1], &applicant_uid, sizeof(applicant_uid));

			    int64_t count = 0;
			    MYSQL_BIND result_param;
			    initLongLongParam(&result_param, &count, sizeof(count));

			    if(!con->executeSql(sql,bind_param, &result_param))
			    {
				logger.error("ApplyJoinGroup SELECT group_members Error");
				info = "申请失败，请稍候重试";
				return false;
			    }

			    if(con->nextData() <= 0)
			    {
				info = "申请失败，请稍候重试";
				return false;
			    }

			    if(count > 0)
			    {
				info = "您已经在该群聊中";
				return false;
			    }
			}

			int8_t old_status = -1;
			{
			    std::string sql = "SELECT snowid, status FROM group_join_requests WHERE conversation_id = ? AND target_uid = ? AND source = 1;";
			    MYSQL_BIND bind_param[2];
			    initLongLongParam(&bind_param[0], &conversationID_int, sizeof(conversationID_int));
			    initLongLongParam(&bind_param[1], &applicant_uid, sizeof(applicant_uid));

			    MYSQL_BIND result_param[2];
			    initLongLongParam(&result_param[0], &requestID, sizeof(requestID));
			    initTinyIntParam(&result_param[1], &old_status, sizeof(old_status));

			    if(!con->executeSql(sql, bind_param, result_param))
			    {
				logger.error("ApplyJoinGroup SELECT group_join_requests Error");
				info = "申请失败，请稍候重试";
				return false;
			    }

			    if(con->nextData() > 0)
			    {
				if(old_status == 0)
				{
				    info = "已存在待处理的申请，请等待通过";
				    return false;
				}
				else if(old_status == 1)
				{
				    info = "您已经在该群聊中";
				    return false;
				}
			    }
			    else
				requestID = sfGen.nextid();
			}

			{
			    if(old_status == 2)
			    {
				std::string sql = "UPDATE group_join_requests SET status = 0, verification_msg = ?, created_at = NOW(), handled_at = NULL, handler_uid = NULL WHERE snowid = ?;";
				MYSQL_BIND bind_param[2];
				unsigned long len_verMsg = verMsg.size();
				initStringParam(&bind_param[0], (char*)verMsg.data(), verMsg.size(), &len_verMsg);
				initLongLongParam(&bind_param[1], &requestID, sizeof(requestID));

				if(!con->executeSql(sql, bind_param))
				{
				    logger.error("ApplyJoinGroup UPDATE group_join_requests Error");
				    info = "申请失败，请稍后重试";
				    return false;
				}
			    }
			    else
			    {
				std::string sql = "INSERT INTO group_join_requests(snowid, conversation_id, initiator_uid, target_uid, source, verification_msg, status) VALUES(?, ?, ?, ?, 1, ?, 0);";

				unsigned long len_verMsg = verMsg.size();
				MYSQL_BIND bind_param[5];
				initLongLongParam(&bind_param[0], &requestID, sizeof(requestID));
				initLongLongParam(&bind_param[1], &conversationID_int, sizeof(conversationID_int));
				initLongLongParam(&bind_param[2], &applicant_uid, sizeof(applicant_uid));
				initLongLongParam(&bind_param[3], &applicant_uid, sizeof(applicant_uid));
				initStringParam(&bind_param[4], (char*)verMsg.data(), verMsg.size(), &len_verMsg);

				if(!con->executeSql(sql, bind_param))
				{
				    logger.error("ApplyJoinGroup INSERT group_join_requests Error");
				    info = "申请失败，请稍候重试";
				    return false;
				}

				my_ulonglong rows = con->getAffectRows();
				if(rows == 0 || rows >= (my_ulonglong)-2)
				{
				    logger.error("ApplyJoinGroup INSERT affected rows 0");
				    info = "申请失败，请稍候重试";
				    return false;
				}
			    }
			}

			{
			    std::string sql = "SELECT username, avatar_url, id FROM user WHERE snowid = ? AND status = 0 LIMIT 1;";
			    MYSQL_BIND bind_param;
			    initLongLongParam(&bind_param, &applicant_uid, sizeof(applicant_uid));

			    char buf_username[64 * 4];
			    unsigned long len_username = 0;
			    char buf_avatarUrl[512 * 4];
			    unsigned long len_avatarUrl = 0;
			    int64_t sid = 0;

			    MYSQL_BIND result_param[3];
			    initStringParam(&result_param[0], buf_username, sizeof(buf_username), &len_username);
			    initStringParam(&result_param[1], buf_avatarUrl, sizeof(buf_avatarUrl), &len_avatarUrl);
			    initLongLongParam(&result_param[2], &sid, sizeof(sid));

			    if(!con->executeSql(sql, &bind_param, result_param))
			    {
				logger.error("ApplyJoinGroup SELECT user Error");
				info = "申请失败，请稍候重试";
				return false;
			    }

			    if(con->nextData() <= 0)
			    {
				info = "申请人信息异常";
				return false;
			    }

			    applicantName = std::string(buf_username, len_username);
			    applicantAvatarUrl = std::string(buf_avatarUrl, len_avatarUrl);
			    applicantSID = std::to_string(sid);
			}
			return true;
		    }();

		    if(ok)
		    {
			con->freeResult();
			sqlTrans.commit();
			result = true;
			info = "申请已发送，请等待群主审核";
		    }
		}
	    }
	    else if(verMsg.size() > 30)
		info = "验证消息不能超过30个字符";
	    else
	    {
		info = "群号格式错误";
	    }
	}
	else
	    info = "登陆凭证过期，请重新登陆";

	auto* func = new std::function<void()>([result, sessPtr, requests_id, uidValid, info](){
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "ApplyJoinGroupRequestResp";
	    j["Result"] = result;
	    j["Info"] = info;
	    if(!uidValid)
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

	if(result)
	    pushGroupJoinRequest(owner_uid, requestID, groupName, 1, applicantAvatarUrl, applicantName, std::stoll(applicantSID), "", verMsg);
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

	if(!isCache && isSnowID(handle_uid) && uidValid)
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
		if(!con->executeSql(sql, bind_param))
		{
		    logger.error("HandleNewFriendRequest UPDATE Error");
		}
		else
		{
		    my_ulonglong rows = con->getAffectRows();
		    if(rows >= (my_ulonglong)-2)
			logger.error("HandleNewFriendRequest affected error");
		    else if(rows > 0)
		    {
			result = true;
			if(isAgree)
			    pushNewFriend(sessPtr->uid.value(), handle_uid_);
		    }
		}
	    }
	    else
		logger.error("handleHandleNewFriendRequest 获取数据库连接失败");
	}
	auto* func = new std::function<void()>([result, sessPtr, requests_id, uidValid, isCache, j_Cache, handle_uid](){
	    json j;
	    if(isCache)
		j = j_Cache;
	    else
	    {
		j["Requests_id"] = requests_id;
		j["Type"] = "HandleNewFriendRequestResp";
		j["Result"] = result;
		j["HandleUID"] = handle_uid;
		if(!uidValid)
		    j["AccessTokenExpired"] = true;

		if(!j.value("AccessTokenExpired", false))
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

void ChatServerWorker::handleHandleNewGroupJoinRequest(session* sess, std::string handle_requestID, bool isAgree, std::string accessToken, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, handle_requestID, isAgree, requests_id, accessToken](){
	json j_Cache;
	bool isCache = sessPtr->myself->tryGetCache(requests_id, j_Cache);
	bool result = false;
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	bool uidValid = sessPtr->uid.has_value() && uid_token != -1 && sessPtr->uid.value() == uid_token;
	
	int64_t conversationID = 0;
	std::string groupName;
	std::string groupID;
	std::string avatarUrl;
	int64_t owner_uid = 0;
	bool alreadyHandled = false;

	if(!isCache && isSnowID(handle_requestID) && uidValid)
	{
	    GuardConnection con(SQLPool::getSQLPool());
	    if(!con)
		logger.error("handleHandleNewFriendRequest 获取数据库连接失败");
	    else
	    {
		SQLTransaction sqlTrans(con->getMysql());
		int64_t target_uid = 0;
		int8_t source = 0;

		bool ok = [&]() -> bool {
		    int8_t current_status = -1;
		    {
			std::string sql = "SELECT gjr.conversation_id, gjr.target_uid, gjr.source, cg.owner_uid, cg.name, cg.avatar_url, cg.id, gjr.status FROM group_join_requests gjr JOIN chat_groups cg ON cg.snowid = gjr.conversation_id WHERE gjr.snowid = ?;";

			MYSQL_BIND bind_param;
			int64_t handle_requestID_ = std::stoll(handle_requestID);
			initLongLongParam(&bind_param, &handle_requestID_, sizeof(handle_requestID_));

			char buf_groupName[64 * 4];
			unsigned long len_groupName = 0;
			char buf_avatarUrl[512 * 4];
			unsigned long len_avatarUrl = 0;
			int64_t groupID_int;
			    
			MYSQL_BIND result_param[8];
			initLongLongParam(&result_param[0], &conversationID, sizeof(conversationID));
			initLongLongParam(&result_param[1], &target_uid, sizeof(target_uid));
			initTinyIntParam(&result_param[2], &source, sizeof(source));
			initLongLongParam(&result_param[3], &owner_uid, sizeof(owner_uid));
			initStringParam(&result_param[4], buf_groupName, sizeof(buf_groupName), &len_groupName);
			initStringParam(&result_param[5], buf_avatarUrl, sizeof(buf_avatarUrl), &len_avatarUrl);
			initLongLongParam(&result_param[6], &groupID_int, sizeof(groupID_int));
			initTinyIntParam(&result_param[7], &current_status, sizeof(current_status));

			if(!con->executeSql(sql, &bind_param, result_param))
			{
			    logger.error("HandleNewGroupJoinRequest SELECT Error");
			    return false;
			}

			if(con->nextData() <= 0)
			{
			    logger.error("HandleNewGroupJoinRequest SELECT Not Data");
			    return false;
			}

			groupName = std::string(buf_groupName, len_groupName);
			avatarUrl = std::string(buf_avatarUrl, len_avatarUrl);
			groupID = std::to_string(groupID_int);
			len_groupName = 0;
			len_avatarUrl = 0;
		    }
		   
		    if(current_status != 0)
		    {
			if((current_status == 1 && isAgree) || (current_status == 2 && !isAgree))
			{
			    alreadyHandled = true;
			    return true;
			}
			logger.error("HandleNewGroupJoinRequest 申请状态冲突[current_status=" + std::to_string(current_status) + "]");
			return false;
		    }

		    int64_t operator_uid = sessPtr->uid.value();
		    if(source == 1 && operator_uid != owner_uid)
		    {
			logger.error("HandleNewGroupJoinRequest 非法请求");
			return false;
		    }
		    if(source == 0 && operator_uid != target_uid && operator_uid != owner_uid)
		    {
			logger.error("HandleNewGroupJoinRequest 非法请求");
			return false;
		    }

		    {
			std::string sql = "UPDATE group_join_requests SET status = ?, handler_uid = ?, handled_at = NOW() WHERE snowid = ?;";
			int8_t status = 0;
			if(isAgree)
			    status = 1;
			else
			    status = 2;
			int64_t uid_ = sessPtr->uid.value();
			int64_t handle_requestID_ = std::stoll(handle_requestID);

			MYSQL_BIND bind_param[3];
			initTinyIntParam(&bind_param[0], &status, sizeof(status));
			initLongLongParam(&bind_param[1], &uid_, sizeof(uid_));
			initLongLongParam(&bind_param[2], &handle_requestID_, sizeof(handle_requestID_));

			if(!con->executeSql(sql, bind_param))
			{
			    logger.error("HandleNewGroupJoinRequest UPDATE Error");
			    return false;
			}
		    
			my_ulonglong rows = con->getAffectRows();
			if(rows >= (my_ulonglong)-2 || rows <= 0)
			{
			    logger.error("HandleNewGroupJoinRequest affected error");
			    return false;
			}
		    }
		    
		    if(!isAgree)
			return true;
		    
		    {
			int64_t snowid_insert = sfGen.nextid();
			std::string sql = "INSERT INTO group_members(snowid, conversation_id, uid, role, join_time, status) VALUES(?, ?, ?, 1, NOW(), 0);";
			MYSQL_BIND bind_param[3];
			initLongLongParam(&bind_param[0], &snowid_insert, sizeof(snowid_insert));
			initLongLongParam(&bind_param[1], &conversationID, sizeof(conversationID));
			initLongLongParam(&bind_param[2], &target_uid, sizeof(target_uid));

			if(!con->executeSql(sql, bind_param))
			{
			    logger.error("HandleNewGroupJoinRequest INSERT Error");
			    return false;
			}
			my_ulonglong rows = con->getAffectRows();
			if(rows >= (my_ulonglong)-2 || rows <= 0)
			{
			    logger.error("HandleNewGroupJoinRequest INSERT affected rows 0");
			    return false;
			}
		    }
		    return true;
		}();

		if(ok)
		{
		    con->freeResult();
		    if(!alreadyHandled)
			sqlTrans.commit();
		    result = true;
		}
	    }
	}
	auto* func = new std::function<void()>([result, sessPtr, requests_id, uidValid, isCache, j_Cache, handle_requestID](){
	    json j;
	    if(isCache)
		j = j_Cache;
	    else
	    {
		j["Requests_id"] = requests_id;
		j["Type"] = "HandleNewGroupJoinRequestResp";
		j["Result"] = result;
		j["HandleRequestID"] = handle_requestID;
		if(!uidValid)
		    j["AccessTokenExpired"] = true;

		if(!j.value("AccessTokenExpired", false))
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

	if(result && isAgree && !alreadyHandled)
	{
	    std::vector<int64_t> allMembersUID;
	    json memberInfos_arr = json::array();
	    GuardConnection con(SQLPool::getSQLPool());
	    if(!con)
	    {
		logger.error("HandleNewGroupJoinRequest PushNewGroup Get Connection Error");
		return;
	    }

	    {
		std::string sql = "SELECT u.id, u.snowid, u.username, u.avatar_url FROM user u JOIN group_members gm ON gm.uid = u.snowid WHERE gm.conversation_id = ? AND gm.status = 0 AND u.status = 0;";
		MYSQL_BIND bind_param;
		initLongLongParam(&bind_param, &conversationID, sizeof(conversationID));

		int64_t sid_int = 0;
		int64_t uid_int = 0;
		char buf_username[64 * 4];
		unsigned long len_username = 0;
		char buf_avatarUrl[512 * 4];
		unsigned long len_avatarUrl = 0;

		MYSQL_BIND result_param[4];
		initLongLongParam(&result_param[0], &sid_int, sizeof(sid_int));
		initLongLongParam(&result_param[1], &uid_int, sizeof(uid_int));
		initStringParam(&result_param[2], buf_username, sizeof(buf_username), &len_username);
		initStringParam(&result_param[3], buf_avatarUrl, sizeof(buf_avatarUrl), &len_avatarUrl);

		if(!con->executeSql(sql, &bind_param, result_param))
		{
		    logger.error("HandleNewGroupJoinRequest PushNewGroup execute sql failed");
		    return;
		}

		while(con->nextData() > 0)
		{
		    json item;
		    allMembersUID.emplace_back(uid_int);
		    item["UID"] = std::to_string(uid_int);
		    item["SID"] = std::to_string(sid_int);
		    item["Username"] = std::string(buf_username, len_username);
		    item["AvatarUrl"] = std::string(buf_avatarUrl, len_avatarUrl);
		    memberInfos_arr.push_back(item);

		    len_username = 0;
		    len_avatarUrl = 0;
		}
	    }
	    for(int64_t uid : allMembersUID)
	    {
		pushNewGroup(uid, std::to_string(conversationID), groupID, groupName, avatarUrl, std::to_string(owner_uid), memberInfos_arr);
	    }
	}
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

void ChatServerWorker::handleGetNewGroupJoinRequestsList(session* sess, std::string accessToken, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, accessToken, requests_id](){
	bool result = false;
	json arr = json::array();
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	bool uidValid = sessPtr->uid.has_value() && sessPtr->uid.value() == uid_token && uid_token != -1;

	if(uidValid)
	{
	    GuardConnection con(SQLPool::getSQLPool());
	    if(!con)
		logger.error("GetNewGroupJoinRequestsList create con failed");
	    else
	    {
		SQLTransaction sqlTrans(con->getMysql());
		std::unordered_map<int64_t, json> snowidToJson;
		std::unordered_map<int64_t, int64_t> snowidToInviterUID;
		bool ok = [&]() -> bool {
		    {
			std::string sql = "SELECT gjr.snowid, cg.name, cg.avatar_url, gjr.source, gjr.verification_msg, gjr.initiator_uid FROM group_join_requests gjr JOIN chat_groups cg ON cg.snowid = gjr.conversation_id WHERE gjr.status = 0 AND cg.status = 0 AND (gjr.target_uid = ? OR (gjr.source = 1 AND cg.owner_uid = ?));";

			int64_t uid = sessPtr->uid.value();
			MYSQL_BIND bind_param[2];
			initLongLongParam(&bind_param[0], &uid, sizeof(uid));
			initLongLongParam(&bind_param[1], &uid, sizeof(uid));

			int64_t requestID;
			char buf_groupName[64 * 4];
			unsigned long len_groupName = 0;
			char buf_avatarUrl[512 * 4];
			unsigned long len_avatarUrl = 0;
			int8_t source = 0;
			char buf_verMsg[30 * 4];
			unsigned long len_verMsg = 0;
			int64_t inviterUID = 0;
			
			MYSQL_BIND result_param[6];
			initLongLongParam(&result_param[0], &requestID, sizeof(requestID));
			initStringParam(&result_param[1], buf_groupName, sizeof(buf_groupName), &len_groupName);
			initStringParam(&result_param[2], buf_avatarUrl, sizeof(buf_avatarUrl), &len_avatarUrl);
			initTinyIntParam(&result_param[3], &source, sizeof(source));
			initStringParam(&result_param[4], buf_verMsg, sizeof(buf_verMsg), &len_verMsg);
			initLongLongParam(&result_param[5], &inviterUID, sizeof(inviterUID));

			if(!con->executeSql(sql, bind_param, result_param))
			{
			    logger.error("GetNewGroupJoinRequestsList executeSql failed");
			    return false;
			}

			int ret = 0;
			while((ret = con->nextData()) > 0)
			{
			    json j;
			    j["RequestID"] = std::to_string(requestID);
			    j["Source"] = std::to_string(source);
			    j["GroupName"] = std::string(buf_groupName, len_groupName);
			    j["DisplayAvatarUrl"] = std::string(buf_avatarUrl, len_avatarUrl);
			    j["VerMsg"] = std::string(buf_verMsg, len_verMsg);

			    snowidToJson[requestID] = j;
			    snowidToInviterUID[requestID] = inviterUID;

			    len_groupName = 0;
			    len_avatarUrl = 0;
			    len_verMsg = 0;
			}

			if(ret < 0)
			{
			    logger.error("GetNewGroupJoinRequestsList nextData error");
			    return false;
			}
		    }

		    for(auto& it : snowidToJson)
		    {
			json& j = it.second;
			int source = std::stoi(j.value("Source", "0"));
			if(source == 0)
			{
			    auto it_uid = snowidToInviterUID.find(it.first);
			    if(it_uid == snowidToInviterUID.end())
				continue;

			    std::string inviterName;
			    {
				std::shared_lock lock(mutex_onlineUser);
				auto it = onlineUser.find(it_uid->second);
				if(it != onlineUser.end())
				    inviterName = it->second->username;
			    }
			    if(inviterName.empty())
			    {
				std::string sql = "SELECT username FROM user WHERE snowid = ? AND status = 0;";
				int64_t snowid = it_uid->second;

				MYSQL_BIND bind_param;
				initLongLongParam(&bind_param, &snowid, sizeof(snowid));

				char buf_username[64 * 4];
				unsigned long len_username = 0;
				MYSQL_BIND result_param;
				initStringParam(&result_param, buf_username, sizeof(buf_username), &len_username);
				if(!con->executeSql(sql, &bind_param, &result_param))
				{
				    logger.error("GetNewGroupJoinRequestsList execute sql(select inviterName) failed");
				    return false;
				}

				if(con->nextData() > 0)
				    inviterName = std::string(buf_username, len_username);
			    }
			    
			    if(inviterName.empty())
				continue;

			    j["InviterName"] = inviterName;
			    j["DisplayName"] = j.value("GroupName", "");
			    arr.push_back(j);
			}
			else if(source == 1)
			{
			    //外部加入，需要查询申请人的SID以及名字
			    auto it_uid = snowidToInviterUID.find(it.first);
			    if(it_uid == snowidToInviterUID.end())
				continue;

			    std::string sid;
			    std::string username;
			    std::string avatarUrl;
			    {
				std::shared_lock lock(mutex_onlineUser);
				auto it = onlineUser.find(it_uid->second);
				if(it != onlineUser.end())
				{
				    username = it->second->username;
				    sid = it->second->sid;
				    avatarUrl = it->second->avatar_url;
				}
			    }

			    if(sid.empty() || username.empty())
			    {
				std::string sql = "SELECT username, id, avatar_url FROM user WHERE snowid = ? AND status = 0;";
				int64_t snowid = it_uid->second;
			    
				MYSQL_BIND bind_param;
				initLongLongParam(&bind_param, &snowid, sizeof(snowid));

				char buf_username[64 * 4];
				unsigned long len_username = 0;
				int64_t sid_int = 0;
				char buf_avatarUrl[512 * 4];
				unsigned long len_avatarUrl = 0;

				MYSQL_BIND result_param[3];
				initStringParam(&result_param[0], buf_username, sizeof(buf_username), &len_username);
				initLongLongParam(&result_param[1], &sid_int, sizeof(sid_int));
				initStringParam(&result_param[2], buf_avatarUrl, sizeof(buf_avatarUrl), &len_avatarUrl);

				if(!con->executeSql(sql, &bind_param, result_param))
				{	
				    logger.error("GetNewGroupJoinRequestsList execute sql(select applicantSID) failed");
				    return false;
				}

				if(con->nextData() > 0)
				{
				    username = std::string(buf_username, len_username);
				    sid = std::to_string(sid_int);
				    avatarUrl = std::string(buf_avatarUrl, len_avatarUrl);
				}
				else
				    continue;
			    }
			    j["DisplayName"] = username;
			    j["ApplicantSID"] = sid;
			    j["DisplayAvatarUrl"] = avatarUrl;
			    arr.push_back(j);
			}
		    }
		    con->freeResult();
		    sqlTrans.commit();
		    return true;
		}();

		if(ok)
		    result = true;
	    }
	}
	auto* func = new std::function<void()>([result, sessPtr, requests_id, arr, uidValid](){
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "GetNewGroupJoinRequestsListResp";
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
	    std::string sql = "SELECT friendships.id, user.id, user.snowid, user.username, user.email, user.avatar_url FROM friendships JOIN user ON user.snowid = friendships.user_a WHERE friendships.status = 1 AND friendships.user_b = ? UNION ALL SELECT friendships.id, user.id, user.snowid, user.username, user.email, user.avatar_url FROM friendships JOIN user ON user.snowid = friendships.user_b WHERE friendships.status = 1 AND friendships.user_a = ?;";
	    int64_t uid = sessPtr->uid.value();
	    MYSQL_BIND bind_param[2];
	    initLongLongParam(&bind_param[0], &uid, sizeof(uid));
	    initLongLongParam(&bind_param[1], &uid, sizeof(uid));

	    int64_t conversationID;
	    int64_t user_sid;
	    int64_t user_uid;
	    char buf_username[64*4];
	    char buf_email[128*4];
	    char buf_avatar[512*4];
	    MYSQL_BIND result_param[6];
	    initLongLongParam(&result_param[0], &conversationID, sizeof(conversationID));
	    initLongLongParam(&result_param[1], &user_sid, sizeof(user_sid));
	    initLongLongParam(&result_param[2], &user_uid, sizeof(user_uid));
	    unsigned long len_username = 0;
	    initStringParam(&result_param[3], buf_username, sizeof(buf_username), &len_username);
	    unsigned long len_email = 0;
	    initStringParam(&result_param[4], buf_email, sizeof(buf_email), &len_email);
	    unsigned long len_avatar = 0;
	    initStringParam(&result_param[5], buf_avatar, sizeof(buf_avatar), &len_avatar);

	    GuardConnection con(SQLPool::getSQLPool());
	    if(con)
	    {
		con->executeSql(sql, bind_param, result_param);
		int ret, count = 0;
		while((ret = con->nextData()) == 1)
		{
		    json item;
		    item["ConversationID"] = std::to_string(conversationID);
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

void ChatServerWorker::handleGetGroupList(session* sess, std::string accessToken, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, accessToken, requests_id](){
	bool result = false;
	json arr = json::array();
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	bool uidValid = sessPtr->uid.has_value() && sessPtr->uid.value() == uid_token && uid_token != -1;

	std::unordered_map<std::string, json> map_conversations;

	if(uidValid)
	{
	    GuardConnection con(SQLPool::getSQLPool());
	    SQL* con_ = con.get();
	    {
		SQLTransaction sqlTrans(con_->getMysql());
		bool ok = [&]() -> bool {
		    {
			std::string sql = "SELECT g.id, g.snowid, g.owner_uid, g.name, g.avatar_url FROM chat_groups g JOIN group_members gm ON gm.conversation_id = g.snowid WHERE g.status = 0 AND gm.uid = ? AND gm.status = 0;";
			int64_t uid = sessPtr->uid.value();
			MYSQL_BIND bind_param;
			initLongLongParam(&bind_param, &uid, sizeof(uid));

			MYSQL_BIND result_param[5];
			int64_t groupID_int = 0;
			int64_t conversationID_int = 0;
			int64_t ownerUID_int = 0;
			char buf_groupName[64 * 4];
			unsigned long len_groupName = 0;
			char buf_avatarUrl[512 * 4];
			unsigned long len_avatarUrl = 0;
		    
			initLongLongParam(&result_param[0], &groupID_int, sizeof(groupID_int));
			initLongLongParam(&result_param[1], &conversationID_int, sizeof(conversationID_int));
			initLongLongParam(&result_param[2], &ownerUID_int, sizeof(ownerUID_int));
			initStringParam(&result_param[3], buf_groupName, sizeof(buf_groupName), &len_groupName);
			initStringParam(&result_param[4], buf_avatarUrl, sizeof(buf_avatarUrl), &len_avatarUrl);

			if(!con_->executeSql(sql, &bind_param, result_param))
			{
			    logger.error("GetGroupList SELECT Error");
			    return false;
			}
			int ret = 0;
			while((ret = con_->nextData()) > 0)
			{
			    json j;	
			    j["ConversationID"] = std::to_string(conversationID_int);
			    j["GroupID"] = std::to_string(groupID_int);
			    j["GroupName"] = std::string(buf_groupName, len_groupName);
			    j["OwnerUID"] = std::to_string(ownerUID_int);
			    j["Avatar_Url"] = std::string(buf_avatarUrl, len_avatarUrl);
			    j["MemberInfos"] = json::array();
			    map_conversations[std::to_string(conversationID_int)] = j;
			    len_groupName = 0;
			    len_avatarUrl = 0;
			}
			if(ret < 0)
			{
			    logger.error("GetGroupList SELECT failed");
			    return false;
			}
		    }
		    {
			for(auto& it : map_conversations)
			{
			    std::string sql = "SELECT u.id, u.snowid, u.username, u.avatar_url FROM user u JOIN group_members gm ON gm.uid = u.snowid WHERE gm.conversation_id = ? AND gm.status = 0 AND u.status = 0;";
			    MYSQL_BIND bind_param;
			    int64_t conversationID_int = std::stoll(it.first);
			    initLongLongParam(&bind_param, &conversationID_int, sizeof(conversationID_int));

			    MYSQL_BIND result_param[4];
			    int64_t sid_int = 0;
			    int64_t uid_int = 0;
			    char buf_username[64 * 4];
			    unsigned long len_username = 0;
			    char buf_avatarUrl[512 * 4];
			    unsigned long len_avatarUrl = 0;

			    initLongLongParam(&result_param[0], &sid_int, sizeof(sid_int));
			    initLongLongParam(&result_param[1], &uid_int, sizeof(uid_int));
			    initStringParam(&result_param[2], buf_username, sizeof(buf_username), &len_username);
			    initStringParam(&result_param[3], buf_avatarUrl, sizeof(buf_avatarUrl), &len_avatarUrl);

			    con_->executeSql(sql, &bind_param, result_param);

			    int ret = 0;
			    while ((ret = con_->nextData()) > 0)
			    {
				json item;
				item["Username"] = std::string(buf_username, len_username);
				item["UID"] = std::to_string(uid_int);
				item["SID"] = std::to_string(sid_int);
				item["AvatarUrl"] = std::string(buf_avatarUrl, len_avatarUrl);
				it.second["MemberInfos"].push_back(item);
				len_username = 0;
				len_avatarUrl = 0;
			    }

			    if(ret < 0)
			    {
				logger.error("GetGroupList SELECT Member failed");
				return false;
			    }
			}	
		    }
		    return true;
		}();

		if(ok)
		{
		    con->freeResult();
		    sqlTrans.commit();
		    result = true;
		}
	    }
	}
	if(result)
	{
	    for(auto& it : map_conversations)
		arr.push_back(it.second);
	}

	auto* func = new std::function<void()>([result, sessPtr, requests_id, arr, uidValid](){
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "GetGroupListResp";
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

void ChatServerWorker::handleGetGroupInfo(session* sess, std::string accessToken, std::string conversationID, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, accessToken, conversationID, requests_id](){
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	bool uidValid = sessPtr->uid.has_value() && uid_token != -1 && sessPtr->uid.value() == uid_token;
	bool result = false;
	json arr = json::array();
	json groupInfo;

	if(uidValid && isSnowID(conversationID))
	{
	    int64_t conversationID_int = std::stoll(conversationID);
	    GuardConnection con(SQLPool::getSQLPool());
	    if(con)
	    {
		std::string sql = "SELECT g.name, g.avatar_url, g.owner_uid, g.id FROM chat_groups g JOIN group_members gm ON gm.conversation_id = g.snowid WHERE g.snowid = ? AND gm.uid = ? AND gm.status = 0 AND g.status = 0;";

		MYSQL_BIND bind_param[2];
		initLongLongParam(&bind_param[0], &conversationID_int, sizeof(conversationID_int));
		initLongLongParam(&bind_param[1], &uid_token, sizeof(uid_token));

		char buf_groupName[64 * 4];
		unsigned long len_groupName = 0;
		char buf_avatarUrl[512 * 4];
		unsigned long len_avatarUrl = 0;
		int64_t owner_uid = 0;
		int64_t group_id = 0;

		MYSQL_BIND result_param[4];
		initStringParam(&result_param[0], buf_groupName, sizeof(buf_groupName), &len_groupName);
		initStringParam(&result_param[1], buf_avatarUrl, sizeof(buf_avatarUrl), &len_avatarUrl);
		initLongLongParam(&result_param[2], &owner_uid, sizeof(owner_uid));
		initLongLongParam(&result_param[3], &group_id, sizeof(group_id));

		if(con->executeSql(sql, bind_param, result_param) && con->nextData() > 0)
		{
		    groupInfo["ConversationID"] = conversationID;
		    groupInfo["GroupName"] = std::string(buf_groupName, len_groupName);
		    groupInfo["Avatar_Url"] = std::string(buf_avatarUrl, len_avatarUrl);
		    groupInfo["OwnerUID"] = std::to_string(owner_uid);
		    groupInfo["GroupID"] = std::to_string(group_id);

		    std::string sql_members = "SELECT u.id, u.snowid, u.username, u.avatar_url FROM user u JOIN group_members gm ON gm.uid = u.snowid WHERE gm.conversation_id = ? AND gm.status = 0 AND u.status = 0;";

		    MYSQL_BIND bind_param_m;
		    initLongLongParam(&bind_param_m, &conversationID_int, sizeof(conversationID_int));

		    int64_t sid = 0;
		    int64_t uid = 0;
		    char buf_username[64 * 4];
		    unsigned long len_username = 0;
		    char buf_avatarUrl_member[512 * 4];
		    unsigned long len_avatarUrl_member = 0;

		    MYSQL_BIND result_param_m[4];
		    initLongLongParam(&result_param_m[0], &sid, sizeof(sid));
		    initLongLongParam(&result_param_m[1], &uid, sizeof(uid));
		    initStringParam(&result_param_m[2], buf_username, sizeof(buf_username), &len_username);
		    initStringParam(&result_param_m[3], buf_avatarUrl_member, sizeof(buf_avatarUrl_member), &len_avatarUrl_member);

		    con->freeResult();
		    if(con->executeSql(sql_members, &bind_param_m, result_param_m))
		    {
			while(con->nextData() > 0)
			{
			    json member;
			    member["SID"] = std::to_string(sid);
			    member["UID"] = std::to_string(uid);
			    member["Username"] = std::string(buf_username, len_username);
			    member["AvatarUrl"] = std::string(buf_avatarUrl_member, len_avatarUrl_member);
			    arr.push_back(member);
			    
			    len_username = 0;
			    len_avatarUrl_member = 0;
			}
			groupInfo["MemberInfos"] = arr;
			result = true;
		    }
		}
	    }
	}
	auto* func = new std::function<void()>([result, sessPtr, requests_id, groupInfo, uidValid, conversationID](){
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "GetGroupInfoRequestResp";
	    j["Result"] = result;
	    j["ConversationID"] = conversationID;
	    if(result)
		j["GroupInfo"] = groupInfo;
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

    int64_t friendshipID = 0;
    {
	std::string sql_selectID = "SELECT id FROM friendships WHERE user_a = LEAST(?, ?) AND user_b = GREATEST(?, ?) AND status = 1;";
	MYSQL_BIND bind_param_id[4];
	int64_t a = sendUID;
	int64_t b = pushUID;
	initLongLongParam(&bind_param_id[0], &a, sizeof(a));
	initLongLongParam(&bind_param_id[1], &b, sizeof(b));
	initLongLongParam(&bind_param_id[2], &a, sizeof(a));
	initLongLongParam(&bind_param_id[3], &b, sizeof(b));

	MYSQL_BIND result_param_id;
	initLongLongParam(&result_param_id, &friendshipID, sizeof(friendshipID));
   
	GuardConnection con(SQLPool::getSQLPool());
	if(!con || !con->executeSql(sql_selectID, bind_param_id, &result_param_id) || con->nextData() != 1)
	{
	    logger.error("PushNewFriend GetConversationID Error");
	    return;
	}
    }

    std::string friendshipID_str = std::to_string(friendshipID);
    if(pushSessPtr)
    {
	auto* func = new std::function<void()>([pushSessPtr, sendUID, sendUsername, sendEmail, sendAvatar, sendSID, sendIsOnline, friendshipID_str](){
	    json j;
	    j["Requests_id"] = "125433702";
	    j["Type"] = "PushNewFriend";
	    j["ConversationID"] = friendshipID_str;
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
	auto* func = new std::function<void()>([sendSessPtr, pushUID, pushUsername, pushEmail, pushAvatar, pushSID, pushIsOnline, friendshipID_str](){
	    json j;
	    j["Requests_id"] = "125433702";
	    j["Type"] = "PushNewFriend";
	    j["ConversationID"] = friendshipID_str;
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

void ChatServerWorker::pushNewGroup(int64_t pushUID, const std::string& conversationID, const std::string& groupID, const std::string& groupName, const std::string& avatarUrl, const std::string& ownerUID, json memberInfos_arr)
{
    std::shared_ptr<userInfo> pushInfoPtr;
    std::shared_ptr<session> pushSessPtr;
    {
	std::shared_lock lock(mutex_onlineUser);
	auto it_push = onlineUser.find(pushUID);
	if(it_push == onlineUser.end())
	    return;
	pushInfoPtr = it_push->second;
	pushSessPtr = pushInfoPtr->sessPtr.lock();
    }

    if(!pushInfoPtr || !pushSessPtr)
	return;

    auto* func = new std::function<void()>([pushInfoPtr, groupName, groupID, conversationID, memberInfos_arr, avatarUrl, ownerUID, pushSessPtr](){
	json j;
	j["Requests_id"] = "125433708";
	j["Type"] = "PushNewGroup";
	j["ConversationID"] = conversationID;
	j["OwnerUID"] = ownerUID;
	j["GroupID"] = groupID;
	j["GroupName"] = groupName;
	j["Avatar_Url"] = avatarUrl;
	j["MemberInfos"] = memberInfos_arr;    

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

void ChatServerWorker::pushNewMessage(int64_t conversationID, int64_t sendUID, int64_t pushUID, int64_t messageID, int64_t timestamp, int8_t contentType, const std::string& content, int64_t convSeq)
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
	auto* func = new std::function<void()>([sendUID, messageID, timestamp, content, pushSessPtr, convSeq, contentType, conversationID](){
	    json j;
	    j["Requests_id"] = "125433704";
	    j["Type"] = "PushNewMessage";
	    j["ConversationID"] = std::to_string(conversationID);
	    j["SenderUID"] = std::to_string(sendUID);
	    j["MessageID"] = std::to_string(messageID);
	    j["ContentType"] = std::to_string(contentType);
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

void ChatServerWorker::pushGroupJoinRequest(int64_t pushUID, int64_t requestID, const std::string& groupName, int8_t source, const std::string& displayAvatarUrl, const std::string& displayName, int64_t sid, const std::string& inviterName, const std::string& verMsg)
{
    std::shared_ptr<session> pushSessPtr;
    {
	std::shared_lock lock(mutex_onlineUser);
	auto it = onlineUser.find(pushUID);
	if(it == onlineUser.end())
	    return;
	pushSessPtr = it->second->sessPtr.lock();
    }
    if(!pushSessPtr)
	return;

    auto* func = new std::function<void()>([requestID, pushSessPtr, displayAvatarUrl, displayName, source, groupName, verMsg, sid, inviterName](){
	json j;
	j["Requests_id"] = "125433707";
	j["Type"] = "PushGroupJoinRequest";
	j["RequestID"] = std::to_string(requestID);
	j["GroupName"] = groupName;
	j["Source"] = std::to_string(source);
	j["DisplayName"] = displayName;
	j["DisplayAvatarUrl"] = displayAvatarUrl;
	j["VerMsg"] = verMsg;
	if(source == 0)
	    j["InviterName"] = inviterName;
	else
	    j["ApplicantSID"] = std::to_string(sid);
	     
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

void ChatServerWorker::pushFriendDeleted(int64_t pushUID, int64_t conversationID)
{
    std::shared_ptr<session> pushSessPtr;
    {
	std::shared_lock lock(mutex_onlineUser);
	auto it = onlineUser.find(pushUID);
	if(it == onlineUser.end())
	    return;
	pushSessPtr = it->second->sessPtr.lock();
    }
    if(!pushSessPtr)
	return;

    auto* func = new std::function<void()>([pushSessPtr, conversationID](){
	json j;
	j["Requests_id"] = "125433709";
	j["Type"] = "PushFriendDeleted";
	j["ConversationID"] = std::to_string(conversationID);

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

void ChatServerWorker::handleSendMessage(session* sess, std::string accessToken, std::string conversationID, int8_t contentType, const std::string content, const std::string tempmsgid, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([accessToken, conversationID, content, requests_id, sessPtr, tempmsgid, contentType](){

	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	bool result = false;
	int64_t timestamp = 0;
	std::string info;
	bool uidValid = sessPtr->uid.has_value() && sessPtr->uid.value() == uid_token && uid_token != -1;
	int64_t id = sfGen.nextid();
	int64_t nextSeq = 0;
	int8_t conversationType = -1;

	std::string idempotentKey = conversationID + "_" + tempmsgid;
	json j_Cache;
	bool isCache = sessPtr->myself->tryGetCache(idempotentKey, j_Cache);

	if(!isCache && uidValid && isSnowID(conversationID))
	{
	    GuardConnection con(SQLPool::getSQLPool());
	    if(!con)
		info = "发送消息失败";
	    else
	    {
		int64_t conversationID_int = std::stoll(conversationID);
		int64_t senderUID = sessPtr->uid.value();
		
		{
		    std::string sql = "SELECT conversation_type FROM conversation_seq WHERE conversation_id = ?;";
		    MYSQL_BIND bind_param;
		    initLongLongParam(&bind_param, &conversationID_int, sizeof(conversationID_int));

		    MYSQL_BIND result_param;
		    initTinyIntParam(&result_param, &conversationType, sizeof(conversationType));

		    con->executeSql(sql, &bind_param, &result_param);
		    if(con->nextData() != 1)
			conversationType = -1;
		}

		if(conversationType < 0)
		{
		    std::string sql = "SELECT 1 FROM friendships WHERE id = ?;";
		    MYSQL_BIND bind_param;
		    initLongLongParam(&bind_param, &conversationID_int, sizeof(conversationID_int));

		    MYSQL_BIND result_param;
		    int8_t is_friend = 0;
		    initTinyIntParam(&result_param, &is_friend, sizeof(is_friend));

		    if(con->executeSql(sql, &bind_param, &result_param) && con->nextData() > 0)
			conversationType = 0;
		}

		if(conversationType < 0)
		    info = "会话不存在";
		else
		{
		    std::vector<int64_t> members;
		    std::vector<int64_t> pushMembers;
		    bool hasPermission = false;

		    if(conversationType == 0)
		    {
			std::string sql = "SELECT status, lastaction_uid, user_a, user_b FROM friendships WHERE id = ?;";
			MYSQL_BIND bind_param;
			initLongLongParam(&bind_param, &conversationID_int, sizeof(conversationID_int));

			int8_t status = 0;
			int64_t lastaction_uid = 0;
			int64_t user_a = 0;
			int64_t user_b = 0;

			MYSQL_BIND result_param[4];
			initTinyIntParam(&result_param[0], &status, sizeof(status));
			initLongLongParam(&result_param[1], &lastaction_uid, sizeof(lastaction_uid));
			initLongLongParam(&result_param[2], &user_a, sizeof(user_a));
			initLongLongParam(&result_param[3], &user_b, sizeof(user_b));

			if(!con->executeSql(sql, &bind_param, result_param))
			{
			    logger.warn("SendMessage SELECT friendships Error");
			    info = "发送失败，请稍候重试";
			}
			else if(con->nextData() <= 0 || status != 1 || (senderUID != user_a && senderUID != user_b))
			{
			    if(status == 3)
				info = lastaction_uid == senderUID ? "您已经拉黑对方" : "对方已经把您拉黑";
			    else
				info = "您和对方还不是好友";
			}
			else
			{
			    int64_t receiverUID = senderUID == user_a ? user_b : user_a;
			    members = {senderUID, receiverUID};
			    pushMembers = {receiverUID};
			    hasPermission = true;
			}
		    }
		    else
		    {
			std::string sql = "SELECT uid FROM group_members WHERE conversation_id = ? AND status = 0;";
			MYSQL_BIND bind_param;
			initLongLongParam(&bind_param, &conversationID_int, sizeof(conversationID_int));

			int64_t memberUID = 0;
			MYSQL_BIND result_param;
			initLongLongParam(&result_param, &memberUID, sizeof(memberUID));

			con->executeSql(sql, &bind_param, &result_param);
			while(con->nextData() > 0)
			    members.push_back(memberUID);

			for(int64_t uid : members)
			{
			    if(uid == senderUID)
			    {
				hasPermission = true;
				break;
			    }
			}

			if(!hasPermission)
			    info = "您不在该群内";
			else
			{
			    for(int64_t uid : members)
			    {
				if(uid != senderUID)
				    pushMembers.push_back(uid);
			    }
			}
		    }

		    if(hasPermission)
		    {
			std::string truncContent = content.size() > 10000 ? content.substr(0, 10000) : content;
			
			SQLTransaction sqlTrans(con->getMysql());

			bool ok = [&]() -> bool{
			    {
				std::string sql_insert = "INSERT IGNORE INTO conversation_seq(conversation_id, conversation_type, last_conv_seq) VALUES(?, ?, 0);";
				MYSQL_BIND bind_param[2];
				initLongLongParam(&bind_param[0], &conversationID_int, sizeof(conversationID_int));
				initTinyIntParam(&bind_param[1], &conversationType, sizeof(conversationType));
				if(!con->executeSql(sql_insert, bind_param))
				{
				    logger.warn("Send Message INSERT IGNORE conversation_seq Error");
				    return false;
				}

				my_ulonglong rows = con->getAffectRows();
				if(rows >= (my_ulonglong)-2)
				{
				    logger.warn("Send Message INSERT IGNORE conversation_seq affected error");
				    return false;
				}
			    }

			    int64_t convSeq = 0;
			    {
				std::string sql = "UPDATE conversation_seq SET last_conv_seq = LAST_INSERT_ID(last_conv_seq + 1) WHERE conversation_id = ?;";
				MYSQL_BIND bind_param;
				initLongLongParam(&bind_param, &conversationID_int, sizeof(conversationID_int));
				
				if(!con->executeSql(sql, &bind_param))
				{
				    logger.warn("Send Message Update Error");
				    return false;
				}

				my_ulonglong rows = con->getAffectRows();
				if(rows == 0 || rows >= (my_ulonglong)-2)
				{
				    logger.warn("Send Message UPDATE conversation_seq affected 0 rows");
				    return false;
				}
				convSeq = con->getInsertID();
			    }

			    int64_t newGlobalSeq = 0;
			    {
				std::string sql_updateGlobalSeq = "UPDATE global_seq SET current_value = LAST_INSERT_ID(current_value + 1) WHERE seq_name = 'conv_sync';";
				if(!con->executeSql(sql_updateGlobalSeq, nullptr))
				{
				    logger.warn("Send Message Update global_seq Error");
				    return false;
				}

				my_ulonglong rows = con->getAffectRows();
				if(rows == 0 || rows >= (my_ulonglong)-2)
				{
				    logger.warn("Send Message UPDATE global_seq affected 0 rows");
				    return false;
				}
				newGlobalSeq = con->getInsertID();
			    }

			    {
				std::string sql = "INSERT INTO conversations(uid, conversation_id, last_seq) VALUES(?, ?, ?) ON DUPLICATE KEY UPDATE last_seq = VALUES(last_seq);";
				for(int64_t memberUID : members)
				{
				    MYSQL_BIND bind_param[3];
				    initLongLongParam(&bind_param[0], &memberUID, sizeof(memberUID));
				    initLongLongParam(&bind_param[1], &conversationID_int, sizeof(conversationID_int));
				    initLongLongParam(&bind_param[2], &newGlobalSeq, sizeof(newGlobalSeq));
				    
				    if(!con->executeSql(sql, bind_param))
				    {
					logger.warn("upsert conversations Error");
					return false;
				    }

				    my_ulonglong rows = con->getAffectRows();
				    if(rows >= (my_ulonglong)-2)
				    {
					logger.warn("upsert conversations affected error");
					return false;
				    }
				}
			    }

			    {
				std::string sql = "INSERT INTO messages(snowid, conversation_id, conv_seq, sender_id, type, content, idempotent_key, timestamp) VALUES(?, ?, ?, ?, ?, ?, ?, ?);";
				timestamp = ((id >> 22) + 1704067200000LL) / 1000; 

				MYSQL_BIND bind_param[8];
				initLongLongParam(&bind_param[0], &id, sizeof(id));
				initLongLongParam(&bind_param[1], &conversationID_int, sizeof(conversationID_int));
				initLongLongParam(&bind_param[2], &convSeq, sizeof(convSeq));
				initLongLongParam(&bind_param[3], &senderUID, sizeof(senderUID));
				int8_t contentType_ = contentType;
				initTinyIntParam(&bind_param[4], &contentType_, sizeof(contentType_));
				unsigned long len_message = truncContent.size();
				initStringParam(&bind_param[5], (char*)truncContent.data(), truncContent.size(), &len_message);
				unsigned long len_idempotent = idempotentKey.size();
				initStringParam(&bind_param[6], (char*)idempotentKey.data(), idempotentKey.size(), &len_idempotent);
				initLongLongParam(&bind_param[7], &timestamp, sizeof(timestamp));
				
				if(!con->executeSql(sql, bind_param))
				{
				    logger.warn("SendMessage INSERT messages Error");
				    return false;
				}
				
				my_ulonglong rows = con->getAffectRows();
				if(rows == 0 || rows >= (my_ulonglong)-2)
				{
				    logger.warn("SendMessage INSERT messages affected 0 rows");
				    return false;
				}

				nextSeq = convSeq;
			    }
			    return true;
			}();

			if(ok)
			{
			    con->freeResult();
			    sqlTrans.commit();
			    result = true;

			    for(int64_t receiverUID : pushMembers)
				pushNewMessage(conversationID_int, senderUID, receiverUID, id, timestamp, contentType, truncContent, nextSeq);
			}
			else
			{
			    if(info.empty())
				info = "发送失败，请稍后重试";
			}
		    }
		}
	    }
	}

	auto* func = new std::function<void()>([sessPtr, result, requests_id, id, uidValid, info, timestamp, tempmsgid, isCache, j_Cache, idempotentKey, nextSeq, conversationID](){
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
		j["ConversationID"] = conversationID;
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
		std::string sql_selectFriendUID = "SELECT c.conversation_id FROM conversations c WHERE c.uid = ? AND c.last_seq > ? ORDER BY c.last_seq DESC;";

		MYSQL_BIND bind_selectFriendUID[2];
		initLongLongParam(&bind_selectFriendUID[0], &uid, sizeof(uid));
		int64_t lastConvSyncSeq_copy = lastConvSyncSeq;
		initLongLongParam(&bind_selectFriendUID[1], &lastConvSyncSeq_copy, sizeof(lastConvSyncSeq_copy));

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
		j["ConversationIDArray"] = arr;
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

void ChatServerWorker::handleSyncMessages(session* sess, std::string accessToken, const std::string conversationID, int64_t startConvSeq, int limit, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([accessToken, requests_id, sessPtr, conversationID, startConvSeq, limit](){
	int64_t uid_token = GlobalTools::verifyAccessToken(accessToken);
	bool uidValid = sessPtr->uid.has_value() && sessPtr->uid.value() == uid_token && uid_token != -1;
	bool result = false;
	json arr = json::array();

	if(uidValid)
	{
	    int64_t conversationID_int = std::stoll(conversationID);
	    int64_t uid = sessPtr->uid.value();

	    GuardConnection con(SQLPool::getSQLPool());
	    if(!con)
		logger.warn("同步消息获取数据库连接失败");
	    else
	    {
		int8_t conversationType = -1;
		bool hasPermission = false;

		{
		    std::string sql = "SELECT conversation_type FROM conversation_seq WHERE conversation_id = ?;";
		    MYSQL_BIND bind_param;
		    initLongLongParam(&bind_param, &conversationID_int, sizeof(conversationID_int));

		    MYSQL_BIND result_param;
		    initTinyIntParam(&result_param, &conversationType, sizeof(conversationType));

		    if(con->executeSql(sql, &bind_param, &result_param) && con->nextData() == 1)
		    {
			if(conversationType == 0)
			{
			    std::string sql_f = "SELECT 1 FROM friendships WHERE id = ? AND status = 1 AND (user_a = ? OR user_b = ?);";
			    MYSQL_BIND bind_param_f[3];
			    initLongLongParam(&bind_param_f[0], &conversationID_int, sizeof(conversationID_int));
			    initLongLongParam(&bind_param_f[1], &uid, sizeof(uid));
			    initLongLongParam(&bind_param_f[2], &uid, sizeof(uid));
			    con->executeSql(sql_f, bind_param_f);
			    hasPermission = con->nextData() > 0;
			}
			else if(conversationType == 1)
			{
			    std::string sql_g = "SELECT 1 FROM group_members WHERE conversation_id = ? AND uid = ? AND status = 0;";
			    MYSQL_BIND bind_param_g[2];
			    initLongLongParam(&bind_param_g[0], &conversationID_int, sizeof(conversationID_int));
			    initLongLongParam(&bind_param_g[1], &uid, sizeof(uid));
			    con->executeSql(sql_g, bind_param_g);
			    hasPermission = con->nextData() > 0;
			}
		    }
		    else
			conversationType = -1;
		}

		if(conversationType >= 0 && hasPermission)
		{
		    int64_t currentStartSeq = startConvSeq;

		    while(true)
		    {
			std::string sql = "SELECT snowid, conv_seq, sender_id, type, content, timestamp FROM messages WHERE conversation_id = ? AND conv_seq > ? ORDER BY conv_seq ASC LIMIT ?;";

			MYSQL_BIND bind_selectMessage[3];
			int64_t startConvSeq_copy = currentStartSeq;
			int limit_copy = limit;
			initLongLongParam(&bind_selectMessage[0], &conversationID_int, sizeof(conversationID_int));
			initLongLongParam(&bind_selectMessage[1], &startConvSeq_copy, sizeof(startConvSeq_copy));
			initIntParam(&bind_selectMessage[2], &limit_copy, sizeof(limit_copy));
	    
			MYSQL_BIND result_selectMessage[6];
			int64_t result_messageID = 0;
			int64_t result_convSeq = 0;
			int64_t result_senderID = 0;
			int8_t type = 0;
			char result_buf_content[10000 * 4 + 1];
			unsigned long len_content = 0;
			int64_t result_timestamp = 0;
			initLongLongParam(&result_selectMessage[0], &result_messageID, sizeof(result_messageID));
			initLongLongParam(&result_selectMessage[1], &result_convSeq, sizeof(result_convSeq));
			initLongLongParam(&result_selectMessage[2], &result_senderID, sizeof(result_senderID));
			initTinyIntParam(&result_selectMessage[3], &(type), sizeof(type));
			initStringParam(&result_selectMessage[4], result_buf_content, sizeof(result_buf_content), &len_content);
			initLongLongParam(&result_selectMessage[5], &result_timestamp, sizeof(result_timestamp));

			if(!con->executeSql(sql, bind_selectMessage, result_selectMessage))
			{
			    logger.warn("同步消息执行sql失败");
			    result = false;
			    break;
			}

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
			    item["ContentType"] = std::to_string(type);
			    item["Content"] = content;
			    item["Timestamp"] = std::to_string(result_timestamp);
			    arr.push_back(item);
			    count++;

			    currentStartSeq = result_convSeq;
			    result = true;
			}

			if(ret < 0)
			{
			    result = false;
			    logger.warn("查询消息错误");
			    break;
			}

			result = true;

			if(count >= limit)
			{
			    json j;
			    j["Requests_id"] = requests_id;
			    j["Type"] = "SyncNewMessagesResp";
			    j["Result"] = true;
			    j["ConversationID"] = conversationID;
			    j["Messages"] = arr;
			    {
				std::lock_guard<std::mutex> lock(sessPtr->mutex_sendQueue);
				sessPtr->sendQueue.push(j.dump() + "\n");
			    }
			    sessPtr->myself->handleSendQueueTask(sessPtr);
			    
			    arr.clear();
			    currentStartSeq += 1;
			    continue;
			}
			else
			{
			   if(!arr.empty())
			    {
				json j;
				j["Requests_id"] = requests_id;
				j["Type"] = "SyncNewMessagesResp";
				j["Result"] = true;
				j["ConversationID"] = conversationID;
				j["Messages"] = arr;
				{
				    std::lock_guard<std::mutex> lock(sessPtr->mutex_sendQueue);
				    sessPtr->sendQueue.push(j.dump() + "\n");
				}
				sessPtr->myself->handleSendQueueTask(sessPtr);
				arr.clear();
			    }
			    {
				json j;
				j["Requests_id"] = requests_id;
				j["Type"] = "SyncNewMessagesResp";
				j["Result"] = true;
				j["ConversationID"] = conversationID;
				j["Messages"] = json::array();
				{
				    std::lock_guard<std::mutex> lock(sessPtr->mutex_sendQueue);
				    sessPtr->sendQueue.push(j.dump() + "\n");
				}
				sessPtr->myself->handleSendQueueTask(sessPtr);
			    }
			    break;
			}
		    }
		    if(result)
			return;
	    	}
	    }
	}
	else
	    logger.warn("同步消息SELECT 好友会话ID失败");

	auto* func = new std::function<void()>([sessPtr, result, requests_id, uidValid, conversationID](){
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "SyncNewMessagesResp";
	    j["Result"] = result;
	    j["ConversationID"] = conversationID;
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

bool ChatServerWorker::isSnowID(const std::string& id)
{
    std::regex pattern(R"(^\d{18}$)");
    return std::regex_match(id, pattern);
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
