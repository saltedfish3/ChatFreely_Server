#pragma once
#include <atomic>
#include <cstdint>
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <random>
#include <iomanip>
#include <ctime>
#include <chrono>
#include <regex>
#include <iostream>
#include <stdexcept>
#include <optional>
#include <sodium.h>

#include "../global/GlobalVar.h"
#include "../third_party/json.hpp"
#include "../third_party/snowflake.hpp"
#include "../thread/ThreadPool.h"
#include "../sql/SQLPool.h"
#include "../logger/Logger.h"

//Message Type(client)
//Heartbeat Login Register Send Hello
//Heartbeat 
//Login	    "Email":String  "Password":String
//Register  "Username":String   "Email":String  "Password":String
//Send	    "From":StringID	To":StringID  "Content":String
//Update_Avatar  "Url":String
//UnLogin
//UpdateUsername  "Username":String
//AddNewFriendRequest "UID":String "Receiver_Email/Receiver_SID":String
//
//Message Type(server)
//HeartbeatResp LoginResp RegisterResp Message HelloResp
//HeartbeatResp
//LoginResp "Result":bool  "Info":String  if true --->  "UID":int64_t "SID":int64_t	if false --->  "From":"Email/Password"
//RegisterResp "Result":bool  "Info":String  if false --->  "From":"Email/Password/username"
//Message "IsGroup":bool  "From":StringID  "Content":String
//HelloResp "HeartbeatNum":int
//UpdateAvatarResp   "Result":bool
//UnLoginResp
//UpdateUsernameResp  "Result":bool
//AddNewFriendRequestResp "Result":bool "Info":String

class ChatServerWorker
{
    using json = nlohmann::json;
    struct session : public std::enable_shared_from_this<session>
    {
	ChatServerWorker* myself;
	bufferevent* bev;
	std::optional<int64_t> uid;
    };
    static Logger logger;
public:
    ChatServerWorker();
    ~ChatServerWorker();
    void run();
    void stop();
    bool isRun();    
    void addConnect(int fd);

private:
    event_base* base;
    std::thread myThread;
    //session
    std::unordered_map<int, std::shared_ptr<session>> list_session;
    std::atomic<bool> is_run;
    void listen();
        
    static void cb_read(bufferevent* bev, void* arg);
    static void cb_write(bufferevent* bev, void* arg);
    static void cb_event(bufferevent* bev, short events, void* arg);

    static void unPack(const char* line_json,session* sess);

    static void handleHello(bufferevent* bev, const std::string requests_id);
    static void handleHeartbeat(bufferevent* bev, const std::string requests_id);
    static void handleLogin(session* sess, std::string email, std::string password, const std::string requests_id);
    static void handleRegister(session* sess, std::string email, std::string password, std::string username, const std::string requests_id);
    static void handleSend(bufferevent* bev, const std::string requests_id);
    static void handleUploadAvatar(session* sess, std::string url, const std::string requests_id);
    static void handleUnLogin(session* sess, const std::string requests_id);
    static void handleUpdateUsername(session* sess, std::string username, const std::string requests_id);

    static void handleAddNewFriendRequest(session* sess, std::string uid, std::string receiver_info, const std::string requests_id);

    static bool isEmail(const std::string& email);
    static bool isPassword(const std::string& password);
    static bool isSID(const std::string& sid);
    static std::string hash_password(const std::string& password);
    static bool verify_password(const std::string& password, const std::string& stored_hash);

    static void initStringParam(MYSQL_BIND* pargma, char* buf_string, size_t buffer_length, unsigned long* length);
    static void initLongLongParam(MYSQL_BIND* pargma, int64_t* buf_longlong, size_t buffer_length);
    static void initTinyIntParam(MYSQL_BIND* pargma, int8_t* buf_tinyint, size_t buffer_length);
};
