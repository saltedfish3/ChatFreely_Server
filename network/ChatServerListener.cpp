#include "ChatServerListener.h"

Logger ChatServerListener::logger("listener");

ChatServerListener::ChatServerListener(unsigned short port,int workerNum, int uploadWorkerNum)
    :count(0)
{
    this->base = event_base_new();
    if(!this->base)
    {
	logger.error("Create Event_Base Error");
	exit(1);
    }

    sockaddr_in saddr;
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(port);
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);

    this->listener = evconnlistener_new_bind(this->base, cb_listener, this, LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr));
    if(!this->listener)
    {
	if(this->base)
	    event_base_free(this->base);	
	this->base = nullptr;
	logger.error(std::string("Create Listener Error") + evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
	exit(1);
    }

    this->ev_sigint = evsignal_new(this->base, SIGINT, cb_sigint, this);
    if(!this->ev_sigint)
    {
	if(this->base)
	    event_base_free(this->base);
	this->base = nullptr;
	if(this->listener)
	    evconnlistener_free(this->listener);
	this->listener = nullptr;
	logger.error("Create Signal-INT Error");
    }
    event_add(this->ev_sigint, nullptr);

    this->list_worker.reserve(workerNum>0?workerNum:1);
    try
    {
	for(int i = 0; i < (workerNum>0?workerNum:1); i++)
	{
	    auto worker = std::make_unique<ChatServerWorker>();
	    worker->run();
	    this->list_worker.emplace_back(std::move(worker));
	}
	SQLPool::getSQLPool();
	ThreadPool::getThreadPool();
    }catch(const std::runtime_error& e)
    {
	logger.error(e.what());
	exit(1);
    }

    this->list_UploadWorker.reserve(uploadWorkerNum>0?uploadWorkerNum:1);
    for(int i = 0; i < (uploadWorkerNum>0?uploadWorkerNum:0); i++)
    {
	auto worker = std::make_unique<ChatServerUploadWorker>();
	worker->run();
	this->list_UploadWorker.emplace_back(std::move(worker));
    }
}

ChatServerListener::~ChatServerListener()
{
    if(this->listener)
	evconnlistener_free(this->listener);
    if(this->ev_sigint)
	event_free(ev_sigint);
    if(this->base)
	event_base_free(this->base);
}

void ChatServerListener::start()
{
    logger.info("Start Server");
    if(base)
	event_base_dispatch(this->base);
}

void ChatServerListener::cb_listener(evconnlistener* listener,evutil_socket_t fd, sockaddr* addr, int socklen, void* arg)
{
    ChatServerListener* csl = static_cast<ChatServerListener*>(arg);
    if(!csl)
    {
	logger.error("cb_listener pointer arg error:nullptr");
	exit(1);
    }
    csl->count = (csl->count + 1)%csl->list_worker.size();
    csl->list_worker[csl->count]->addConnect(fd);
    logger.info("Client Connect");
}

void ChatServerListener::cb_sigint(evutil_socket_t sig,short events, void* arg)
{
    flag_shutdown = true;
    ChatServerListener* csl = static_cast<ChatServerListener*>(arg);
    if(csl->base)
	event_base_loopbreak(csl->base);
    logger.info("Shutdown now...");
}
