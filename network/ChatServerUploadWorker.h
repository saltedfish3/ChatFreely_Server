#pragma once
#include <atomic>
#include <thread>
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/http.h>
#include <stdexcept>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <miniocpp/client.h>
#include <utility>
#include <vector>
#include "../global/GlobalVar.h"
#include "../logger/Logger.h"

class ChatServerUploadWorker
{
    static Logger logger;
    using json = nlohmann::json;
public:
    ChatServerUploadWorker();
    ~ChatServerUploadWorker();
    void run();
    void stop();
private:
    std::atomic<bool> is_run;
    std::thread myThread;
    event_base* base;
    evhttp* http;

    static minio::s3::Client& getMinioClient(); 

    void waiting();
    static void cb_distribute(evhttp_request* req, void* arg);
    static void cb_uploadInit(evhttp_request* req, void* arg);
    static void cb_uploadPart(evhttp_request* req, void* arg);
    static void cb_uploadComplete(evhttp_request* req, void* arg);
    static void cb_uploadSingle(evhttp_request* req, void* arg);
    static bool isExists(const std::string& bucket, const std::string& key);
    static std::string createUploadUrl(const std::string& bucket, const std::string& key);
    static std::string createMultipartUpload(const std::string& bucket, const std::string& key, const std::string& mimeType);
    static void abortMultipartUpload(const std::string& bucket, const std::string& key, const std::string& uploadId);
    static bool uploadPart(const std::string& bucket, const std::string& key, const std::string& uploadId, int partNumber, const char* data, size_t size, std::string& outEtag);
    static bool completeMultipartUpload(const std::string& bucket, const std::string& key, const std::string& uploadId, const std::vector<std::pair<int, std::string>>& parts);

    static bool verifyAccessToken(const evkeyvalq* evq);
    static std::string getUrlQueryParam(const std::string& url, const std::string& name);
    static bool getObjectSize(const std::string& bucket, const std::string& key, int64_t& outSize);
    static bool removeObject(const std::string& bucket, const std::string& key);
    static bool getObject(const std::string& bucket, const std::string& key, std::string& outData);
    static bool putObject(const std::string& bucket, const std::string& key, const std::string& data, const std::string& mimeType);
    static bool startsWith(const std::string& s, const std::string& prefix);
    static bool generateThumbnailFromData(const std::string& bucket, const std::string& objectKey, const std::string& originalData, std::string& outThumbUrl);
};
