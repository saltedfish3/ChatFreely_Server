#pragma once
#include <algorithm>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <event2/bufferevent.h>
#include <memory>
#include <stdexcept>
#include <iostream>
#include <vector>
#include <string>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fstream>
#include <filesystem>
#include "ChatServerWorker.h"
#include "ChatServerUploadWorker.h"
#include "../global/GlobalVar.h"
#include "../logger/Logger.h"

class ChatServerListener
{

    static Logger logger;
public:
    ChatServerListener(const ChatServerListener& csl) = delete;
    ChatServerListener& operator=(const ChatServerListener& csl) = delete;
    ChatServerListener(unsigned short port,int workerNum, int uploadWorkerNum);
    ~ChatServerListener();

    void start();
private:
    event_base* base;
    evconnlistener* listener;
    event* ev_sigint;
 
    std::vector<std::unique_ptr<ChatServerWorker>> list_worker;
    std::vector<std::unique_ptr<ChatServerUploadWorker>> list_UploadWorker;
    int count;

    static void cb_listener(evconnlistener* listener,evutil_socket_t fd,sockaddr* addr, int socklen, void* arg);
    static void cb_sigint(evutil_socket_t sig, short events, void* arg);
    static void loadJwtSecret(const std::string& filepath);
};
