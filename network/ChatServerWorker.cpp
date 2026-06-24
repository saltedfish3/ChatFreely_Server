#include "ChatServerWorker.h"
#include <algorithm>
#include <bits/types/struct_timeval.h>
#include <cstring>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/util.h>
#include <functional>
#include <memory>
#include <sodium/core.h>
#include <sodium/crypto_pwhash.h>
#include <stdexcept>

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
	handleUploadAvatar(sess, j.value("Url", ""), requests_id.str());
    }
    else if(type == "UnLogin")
	handleUnLogin(sess, requests_id.str());
    else if(type == "UpdateUsername")
    {
	if(!j.contains("Username") || !j["Username"].is_string())
	{
	    logger.warn("Json no Username");
	    return;
	}
	handleUpdateUsername(sess, j.value("Username", ""), requests_id.str());
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
			info = "登陆成功";
			is_login = true;
			auto user_info = std::make_shared<userInfo>();
			user_info->username = std::string(buf_username,len_username);
			user_info->email = email;
			onlineUser[uid] = user_info;
			sessPtr->uid = uid;
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
	auto* func = new std::function<void()>([is_login, info = std::move(info), from = std::move(from), g_uid , g_sid, username = std::move(g_username), avatar_url = std::move(g_avatar_url), sess, requests_id](){
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

void ChatServerWorker::handleUploadAvatar(session* sess, std::string url, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, url = std::move(url), requests_id](){
	bool result = false;
	if(sessPtr->uid.has_value())
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
	auto* func = new std::function<void()>([result, requests_id, sessPtr](){
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "UpdateAvatarResp";
	    j["Result"] = result;
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

void ChatServerWorker::handleUnLogin(session* sess, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    json j;
    j["Requests_id"] = requests_id;
    j["Type"] = "UnLoginResp";
    std::string data = j.dump() + "\n";
    bufferevent_write(sessPtr->bev, data.c_str(), data.size());
    if(sessPtr->uid.has_value())
	onlineUser.erase(sessPtr->uid.value());
    sessPtr->uid.reset();
}

void ChatServerWorker::handleUpdateUsername(session* sess, std::string username, const std::string requests_id)
{
    auto sessPtr = sess->shared_from_this();
    ThreadPool::getThreadPool()->addTask([sessPtr, username, requests_id](){
	bool result = false;
	if(sessPtr->uid.has_value() && !username.empty())
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
	auto* func = new std::function<void()>([sessPtr, result, requests_id](){
	    json j;
	    j["Requests_id"] = requests_id;
	    j["Type"] = "UpdateUsernameResp";
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
    });
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
