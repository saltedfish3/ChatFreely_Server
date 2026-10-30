#pragma once
#include <atomic>
#include <cstdint>
#include <event2/event.h>
#include <event2/thread.h>
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
#include <jwt-cpp/jwt.h>
#include <vector>

#include "../global/GlobalVar.h"
#include "../third_party/json.hpp"
#include "../third_party/snowflake.hpp"
#include "../thread/ThreadPool.h"
#include "../sql/SQLPool.h"
#include "../sql/SQLTransaction.h"
#include "../sql/GuardConnect.h"
#include "../logger/Logger.h"
#include "../redis/RedisClient.h"

class ChatServerWorker
{
    using json = nlohmann::json;
    static Logger logger;
public:
    ChatServerWorker();
    ~ChatServerWorker();
    void run();
    void stop();
    bool isRun();    
    void addConnect(int fd, const std::string& client_ip);

private:
    struct UserInfo
    {
	std::string username;
	std::string email;
	std::string avatar_url;
	std::string sid;
    };

    event_base* base;
    std::thread myThread;
    
    std::unordered_map<int, std::shared_ptr<session>> list_session;
    std::atomic<bool> is_run;
    std::shared_mutex mutex_idempotentCache;
    std::unordered_map<std::string, idempotentRecord> idempotentCache;

    void listen();
        
    static void cb_read(bufferevent* bev, void* arg);
    static void cb_write(bufferevent* bev, void* arg);
    static void cb_event(bufferevent* bev, short events, void* arg);

    static void unPack(const char* line_json,session* sess);

    static void handleHello(session* sess, const std::string clientSessionID, const std::string requests_id);
    static void handleHeartbeat(session* sess, const std::string requests_id);
    static void handleLogin(session* sess, std::string email, std::string password, const std::string requests_id);
    static void handleAccessTokenLogin(session* sess, std::string accessToken, const std::string requests_id);
    static void handleRegister(session* sess, std::string email, std::string password, std::string username, const std::string requests_id);
    static void handleUploadAvatar(session* sess, std::string url, std::string accessToken, const std::string requests_id);
    static void handleUnLogin(session* sess, std::string accessToken, const std::string requests_id);
    static void handleUpdateUsername(session* sess, std::string username, std::string accessToken, const std::string requests_id);

    static void handleUpdateGroupNameRequest(session* sess, std::string accessToken, std::string conversationID, std::string groupName, const std::string requests_id);

    static void handleUpdateGroupAvatarRequest(session* sess, std::string accessToken, std::string conversationID, std::string avatarUrl, const std::string requests_id);

    static void handleDeleteFriendRequest(session* sess, std::string accessToken, std::string conversationID, const std::string requests_id);

    static void handleExitGroupRequest(session* sess, std::string accessToken, std::string conversationID, const std::string requests_id);

    static void handleDismissGroupRequest(session* sess, std::string accessToken, std::string conversationID, const std::string requests_id);

    static void handleAddNewFriendRequest(session* sess, std::string receiver_info, std::string verification_msg, std::string accessToken, const std::string requests_id);
    static void handleCreateGroupRequest(session* sess, std::string groupName, std::vector<std::string> inviteUIDs, std::string inviteMsg, std::string accessToken, const std::string requests_id);
    static void handleApplyJoinGroupRequest(session* sess, std::string accessToken, std::string groupID, std::string verMsg, const std::string requests_id);
    static void handleHandleNewFriendRequest(session* sess, std::string handle_uid, bool isAgree, std::string accessToken, const std::string requests_id);
    static void handleHandleNewGroupJoinRequest(session* sess, std::string handle_requestID, bool isAgree, std::string accessToken, const std::string requests_id);
    static void handleRemoveGroupMemberRequest(session* sess, std::string accessToken, std::string conversationID, std::string removeUID, const std::string requests_id);
    static void handleGetNewFriendRequestsList(session* sess, std::string accessToken, const std::string requests_id);

    static void handleGetNewGroupJoinRequestsList(session* sess, std::string accessToken, const std::string requests_id);

    static void handleGetFriendList(session* sess, std::string accessToken, const std::string requests_id);
    
    static void handleGetGroupList(session* sess, std::string accessToken, const std::string requests_id);

    static void handleGetGroupInfo(session* sess, std::string accessToken, std::string conversationID, const std::string requests_id);

    static void handleGetConversationsFriendUIDListAndSeq(session* sess, std::string accessToken, int64_t lastConvSyncSeq, const std::string requests_id);

    static void handleGetDeletedConversationInfo(session* sess, std::string accessToken, std::vector<std::string> friendConversationIDs, std::vector<std::string> groupConversationIDs, const std::string requests_id);

    static void handleSyncMessages(session* sess, std::string accessToken, const std::string conversationID, int64_t startConvSeq, int limit, const std::string requests_id);

    static void handleSendMessage(session* sess, std::string accessToken, std::string conversationID, int8_t contentType, const std::string content, const std::string tempmsgid, const std::string requests_id);

    static void handleRefreshToken(session* sess, std::string refreshToken, const std::string requests_id);

    static void pushNewFriendRequests(int64_t sendUID, int64_t pushUID, const std::string& verMsg);

    static void pushNewFriend(int64_t sendUID, int64_t pushUID);

    static void pushNewGroup(int64_t pushUID, const std::string& conversationID, const std::string& groupID, const std::string& groupName, const std::string& avatarUrl, const std::string& ownerUID, json memberInfos_arr);

    static void pushFriendStatus(int64_t changeUID, bool isOnline);

    static void pushFriendUsername(int64_t changeUID, const std::string& username);

    static void pushFriendAvatar(int64_t changeUID, const std::string& avatar);

    static void pushNewMessage(int64_t conversationID, int64_t sendUID, int64_t pushUID, int64_t messageID, int64_t timestamp, int8_t contentType, const std::string& content, int64_t convSeq);

    static void pushGroupJoinRequest(int64_t pushUID, int64_t requestID, const std::string& groupName, int8_t source, const std::string& displayAvatarUrl, const std::string& displayName, int64_t sid, const std::string& inviterName, const std::string& verMsg);

    static void pushFriendDeleted(int64_t pushUID, int64_t conversationID);

    static void pushGroupDismiss(int64_t pushUID, int64_t conversationID);

    static void pushRemoveGroupMember(int64_t pushUID, int64_t conversationID);

    static bool isEmail(const std::string& email);
    static bool isPassword(const std::string& password);
    static bool isSID(const std::string& sid);
    static bool isSnowID(const std::string& id);
    static std::string hash_password(const std::string& password);
    static bool verify_password(const std::string& password, const std::string& stored_hash);

    static void initStringParam(MYSQL_BIND* pargma, char* buf_string, size_t buffer_length, unsigned long* length);
    static void initLongLongParam(MYSQL_BIND* pargma, int64_t* buf_longlong, size_t buffer_length);
    static void initTinyIntParam(MYSQL_BIND* pargma, int8_t* buf_tinyint, size_t buffer_length);
    static void initIntParam(MYSQL_BIND* pargma, int* buf_int, size_t buffer_length);

    static std::string createAccessToken(int64_t uid, const std::chrono::seconds& expire_duration = std::chrono::minutes(45));
    static std::pair<std::string, std::string> createRefreshToken();

    static bool getUserInfo(int64_t uid, UserInfo& outInfo);

    void setCache(const std::string& requests_id, const json& responseJson, int seconds, const std::string& scope = "");
    bool tryGetCache(const std::string& requests_id, json& outJson, const std::string& scope = "");
    void cleanExpiredCache();

    void handleSendQueueTask(std::shared_ptr<session> sessPtr);

};
