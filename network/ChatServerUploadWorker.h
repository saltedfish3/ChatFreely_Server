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
#include "../global/GlobalVar.h"
#include "../logger/Logger.h"

class ChatServerUploadWorker
{
    static Logger logger;
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

    void waiting();
    static void cb_distribute(evhttp_request* req, void* arg);
    static void cb_upload(evhttp_request* req, void* arg);
    static void cb_download(evhttp_request* req, void* arg);
    static bool isJpeg(const char* binary_data);
    static bool isPng(const char* binary_data);
};
