#include "ChatServerUploadWorker.h"
#include "miniocpp/args.h"
#include "miniocpp/client.h"
#include "miniocpp/http.h"
#include "miniocpp/providers.h"
#include "miniocpp/request.h"
#include <charconv>
#include <cstring>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

Logger ChatServerUploadWorker::logger("UploadWorker");

ChatServerUploadWorker::ChatServerUploadWorker()
    :is_run(false),
    base(nullptr),
    http(nullptr)
{
    this->base = event_base_new();
    if(!this->base)
	throw std::runtime_error("UploadWorker Create Base Error");

    this->http = evhttp_new(this->base);
    if(!this->http)
    {
	event_base_free(this->base);
	throw std::runtime_error("UploadWorker Create Http Error");
    }

    if(evhttp_bind_socket(this->http, "0.0.0.0", UploadPort) != 0)
    {
	evhttp_free(this->http);
	event_base_free(this->base);
	throw std::runtime_error("UploadWorker Bind Port Error");
    }
    
    evhttp_set_gencb(this->http, cb_distribute, nullptr);
}

ChatServerUploadWorker::~ChatServerUploadWorker()
{
    stop();
    if(this->http)
	evhttp_free(this->http);
    if(this->base)
	event_base_free(this->base);
}

void ChatServerUploadWorker::run()
{
    if(this->is_run)
	return;
    this->is_run = true;
    this->myThread = std::thread(&ChatServerUploadWorker::waiting, this);
}

void ChatServerUploadWorker::stop()
{
    if(this->base)
	event_base_loopbreak(this->base);
    if(this->is_run && this->myThread.joinable())
	this->myThread.join();
    this->is_run = false;
}

minio::s3::Client& ChatServerUploadWorker::getMinioClient()
{
    static minio::s3::BaseUrl baseUrl("http://192.168.153.128:9000");
    static minio::creds::StaticProvider providers("saltedfish_test", "saltedfish_test");
    static minio::s3::Client client(baseUrl, &providers);
    return client;
}

void ChatServerUploadWorker::waiting()
{
    if(this->base)
    {
	timeval tv = {1,0};
	while(!flag_shutdown)
	{
	    event_base_loopexit(this->base, &tv);
	    event_base_dispatch(this->base);
	}
	event_base_loop(this->base, EVLOOP_NONBLOCK);
    }
}

void ChatServerUploadWorker::cb_distribute(evhttp_request* req, void* arg)
{
    const char* uri = evhttp_request_get_uri(req);
    if(!uri)
    {
	evhttp_send_reply(req, 404, nullptr, nullptr);
	return;
    }

    std::string url(uri);
    while(!url.empty() && std::isspace(url.front()))
	url.erase(0,1);
    while(!url.empty() && std::isspace(url.back()))
	url.pop_back();
    
    evhttp_cmd_type command = evhttp_request_get_command(req);
    std::string path = url;
    size_t ppos = path.find('?');
    if(ppos != std::string::npos)
	path = path.substr(0, ppos);

    if(command == EVHTTP_REQ_POST && path == "/upload/init")
    {
	cb_uploadInit(req, nullptr);
    }
    else if(command == EVHTTP_REQ_POST && path == "/upload/part")
    {
	cb_uploadPart(req, nullptr);
    }
    else if(command == EVHTTP_REQ_POST && path == "/upload/complete")
    {
	cb_uploadComplete(req, nullptr);
    }
    else
    {
	evhttp_send_reply(req, 404, nullptr, nullptr);
	return;
    }
}

bool ChatServerUploadWorker::isExists(const std::string& bucket, const std::string& key)
{
    minio::s3::StatObjectArgs args;
    args.bucket = bucket;
    args.object = key;
    
    auto resp = getMinioClient().StatObject(args);
    return static_cast<bool>(resp);
}

std::string ChatServerUploadWorker::createUploadUrl(const std::string& bucket, const std::string& key)
{
    minio::s3::GetPresignedObjectUrlArgs args;
    args.bucket = bucket;
    args.object = key;
    args.method = minio::http::Method::kPut;
    args.expiry_seconds = 3600;

    auto resp = getMinioClient().GetPresignedObjectUrl(args);
    if(!resp)
	return "";
    return resp.url;
}

std::string ChatServerUploadWorker::createMultipartUpload(const std::string& bucket, const std::string& key, const std::string& mimeType)
{
    minio::s3::CreateMultipartUploadArgs args;
    args.bucket = bucket;
    args.object = key;
    args.headers.Add("Content-Type", mimeType);

    auto resp = getMinioClient().CreateMultipartUpload(args);
    if(!resp)
    {
	logger.error("创建分页上传失败:" + resp.Error().String());
	return "";
    }
    return resp.upload_id;
}

void ChatServerUploadWorker::abortMultipartUpload(const std::string& bucket, const std::string& key, const std::string& uploadId)
{
    minio::s3::AbortMultipartUploadArgs args;
    args.bucket = bucket;
    args.object = key;
    args.upload_id = uploadId;

    auto resp = getMinioClient().AbortMultipartUpload(args);
    if(!resp)
	logger.warn("终止分片上传失败:" + resp.Error().String());
}

bool ChatServerUploadWorker::uploadPart(const std::string& bucket, const std::string& key, const std::string& uploadId, int partNumber, const char* data, size_t size, std::string& outEtag)
{
    minio::s3::UploadPartArgs args;
    args.bucket = bucket;
    args.object = key;
    args.upload_id = uploadId;
    args.part_number = static_cast<unsigned int>(partNumber);
    args.data = std::string_view(data, size);
    args.buf = const_cast<char*>(data);
    args.part_size = size;

    auto resp = getMinioClient().UploadPart(args);
    if(!resp)
    {
	logger.error("上传分片失败:" + resp.Error().String());
	return false;
    }
    outEtag = resp.etag;
    return true;
}

bool ChatServerUploadWorker::completeMultipartUpload(const std::string& bucket, const std::string& key, const std::string& uploadId, const std::vector<std::pair<int, std::string>>& parts)
{
    minio::s3::CompleteMultipartUploadArgs args;
    args.bucket = bucket;
    args.object = key;
    args.upload_id = uploadId;

    for(auto& [num, etag] : parts)
	args.parts.emplace_back(static_cast<unsigned int>(num), etag);

    auto resp = getMinioClient().CompleteMultipartUpload(args);
    if(!resp)
    {
	logger.error("合并分页失败:" + resp.Error().String());
	return false;
    }
    return true;
}

void ChatServerUploadWorker::cb_uploadInit(evhttp_request* req, void* arg)
{
    auto input_header = evhttp_request_get_input_headers(req);
    if(!verifyAccessToken(input_header))
    {
        evhttp_send_reply(req, 401, nullptr, nullptr);
	return;
    }
	
    evbuffer* buf_input = evhttp_request_get_input_buffer(req);
    size_t len_body = evbuffer_get_length(buf_input);
    if(len_body == 0 || len_body > 4096)
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }

    std::string body(len_body, '\0');
    evbuffer_copyout(buf_input, &body[0], len_body);

    json j = json::parse(body, nullptr, false);
    if(j.is_discarded())
    {
	logger.warn("parse Json Error");
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }
    
    if(!j.contains("Md5") || !j["Md5"].is_string() || !j.contains("Suffix") || !j["Suffix"].is_string() || !j.contains("Size") || !j["Size"].is_string())
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }

    std::string md5 = j.value("Md5", "");
    std::string suffix = j.value("Suffix", "");
    std::string sizeStr = j.value("Size", "");
    int64_t size = 0; 
    auto [ptr, ec] = std::from_chars(sizeStr.data(), sizeStr.data() + sizeStr.size(), size);
    if(ec != std::errc() || ptr != sizeStr.data() + sizeStr.size())
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }

    for(char& c : suffix)
	c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if(md5.size() != 32 || !std::all_of(md5.begin(), md5.end(), [](char c){
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }))
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }

    std::string head;
    std::string mimeType;
    int64_t maxSize = 0;
    if(suffix == "jpg" || suffix == "jpeg" || suffix == "png")
    {
	head = "images/";
	maxSize = 10LL * 1024 * 1024;
	mimeType = (suffix == "png") ? "image/png" : "image/jpeg";
    }
    else if(suffix == "mp4" || suffix == "mov" || suffix == "webm")
    {
	head = "videos/";
	maxSize = 1024LL * 1024 * 1024;
	if(suffix == "mp4")
	    mimeType = "video/mp4";
	else if(suffix == "mov")
	    mimeType = "video/quicktime";
	else
	    mimeType = "video/webm";
    }
    else
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }
    
    if(size <= 0 || size > maxSize)
    {
	evhttp_send_reply(req, 413, nullptr, nullptr);
	return;
    }

    std::string key = head + md5 + "." + suffix;
    std::string publicUrl = "http://192.168.153.128/chat/" + key;

    json resp;
    resp["Url"] = publicUrl;

    if(isExists("chat", key))
    {
	resp["Exists"] = true;
    }
    else
    {
	const int64_t part_size = 5LL * 1024 * 1024;
	const int64_t max_part_count = 10000;
	const int64_t multipart_size = 100LL * 1024 * 1024;
	
	bool needMultipart = size > multipart_size;

	if(!needMultipart)
	{
	    std::string uploadUrl = createUploadUrl("chat", key);
	    if(uploadUrl.empty())
	    {
		evhttp_send_reply(req, 500, nullptr, nullptr);
		return;
	   }
	    resp["Exists"] = false;
	    resp["UploadUrl"] = uploadUrl;
	}
	else 
	{
	    std::string uploadId = createMultipartUpload("chat", key, mimeType);
	    if(uploadId.empty())
	    {
		evhttp_send_reply(req, 500, nullptr, nullptr);
		return;
	    }

	    int64_t partCount = (size + part_size - 1) / part_size;
	    if(partCount > max_part_count)
	    {
		abortMultipartUpload("chat", key, uploadId);
		evhttp_send_reply(req, 413, nullptr, nullptr);
		return;
	    }

	    resp["Exists"] = false;
	    resp["ObjectKey"] = key;
	    resp["UploadId"] = uploadId;
	    resp["PartSize"] = std::to_string(part_size);
	}
    }

    std::string respStr = resp.dump();
    evbuffer* out = evbuffer_new();
    evbuffer_add(out, respStr.c_str(), respStr.size());
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
    evhttp_send_reply(req, 200, nullptr, out);
}

void ChatServerUploadWorker::cb_uploadPart(evhttp_request* req, void* arg)
{
    auto input_header = evhttp_request_get_input_headers(req);
    if(!verifyAccessToken(input_header))
    {
        evhttp_send_reply(req, 401, nullptr, nullptr);
	return;
    }

    const char* uri = evhttp_request_get_uri(req);
    if(!uri)
    {
        evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }

    std::string url(uri);
    std::string key = getUrlQueryParam(url, "ObjectKey");
    std::string uploadId = getUrlQueryParam(url, "UploadId");
    std::string partNumStr = getUrlQueryParam(url, "partNumber");

    if(key.empty() || uploadId.empty() || partNumStr.empty())
    {
        evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }

    int64_t partNumber = 0;
    auto [ptr, ec] = std::from_chars(partNumStr.data(), partNumStr.data() + partNumStr.size(), partNumber);
    if(ec != std::errc() || ptr != partNumStr.data() + partNumStr.size())
    {
        evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }

    if(key.find("..") != std::string::npos || (!startsWith(key, "images/") && !startsWith(key, "videos/")))
    {
        evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }
    
    evbuffer* buf_input = evhttp_request_get_input_buffer(req);
    size_t len_body = evbuffer_get_length(buf_input);
    if(len_body == 0 || len_body > 6 * 1024 * 1024)
    {
        evhttp_send_reply(req, 413, nullptr, nullptr);
	return;
    }

    std::string body(len_body, '\0');
    evbuffer_copyout(buf_input, &body[0], len_body);

    std::string etag;
    if(!uploadPart("chat", key, uploadId, static_cast<int>(partNumber), body.data(), body.size(), etag))
    {
        evhttp_send_reply(req, 500, nullptr, nullptr);
	return;
    }

    json resp;
    resp["ETag"] = etag;
    std::string respStr = resp.dump();

    evbuffer* out = evbuffer_new();
    evbuffer_add(out, respStr.c_str(), respStr.size());
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
    evhttp_send_reply(req, 200, nullptr, out);
}

void ChatServerUploadWorker::cb_uploadComplete(evhttp_request* req, void* arg)
{
    auto input_header = evhttp_request_get_input_headers(req);
    if(!verifyAccessToken(input_header))
    {
        evhttp_send_reply(req, 401, nullptr, nullptr);
	return;
    }

    evbuffer* buf_input = evhttp_request_get_input_buffer(req);
    size_t len_body = evbuffer_get_length(buf_input);
    if(len_body == 0 || len_body > 1024 * 1024)
    {
        evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }

    std::string body(len_body, '\0');
    evbuffer_copyout(buf_input, &body[0], len_body);

    json j = json::parse(body, nullptr, false);
    if(j.is_discarded())
    {
        evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }

    if(!j.contains("ObjectKey") || !j["ObjectKey"].is_string() || 
	!j.contains("UploadId") || !j["UploadId"].is_string() || 
	!j.contains("Parts") || !j["Parts"].is_array())
    {
        evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }

    std::string key = j.value("ObjectKey", "");
    std::string uploadId = j.value("UploadId", "");

    if(key.find("..") != std::string::npos || (!startsWith(key, "images/") && !startsWith(key, "videos/")))
    {
        evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }

    std::vector<std::pair<int, std::string>> parts;
    for(auto& p : j["Parts"])
    {
	if(!p.is_object())
	{
	    evhttp_send_reply(req, 400, nullptr, nullptr);
	    return;
	}
	if(!p.contains("PartNumber") || !p["PartNumber"].is_string() || 
	    !p.contains("ETag") || !p["ETag"].is_string())
	{
	    evhttp_send_reply(req, 400, nullptr, nullptr);
	    return;
	}

	std::string partNumStr = p.value("PartNumber", "");
	std::string etag = p.value("ETag", "");

	int64_t partNumber = 0;
	auto [ptr, ec] = std::from_chars(partNumStr.data(), partNumStr.data() + partNumStr.size(), partNumber);
	if(ec != std::errc() || ptr != partNumStr.data() + partNumStr.size())
	{
	    evhttp_send_reply(req, 400, nullptr, nullptr);
	    return;
	}
	
	if(partNumber <= 0 || partNumber > 10000 || etag.empty())
	{
	    evhttp_send_reply(req, 400, nullptr, nullptr);
	    return;
	}
	parts.emplace_back(static_cast<int>(partNumber), etag);
    }

    if(parts.empty() || parts.size() > 10000)
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }

    if(!completeMultipartUpload("chat", key, uploadId, parts))
    {
	evhttp_send_reply(req, 500, nullptr, nullptr);
	return;
    }

    int64_t maxSize = 0;
    if(startsWith(key, "images/"))
	maxSize = 10LL * 1024 * 1024;
    else if(startsWith(key, "videos/"))
	maxSize = 1024LL * 1024 * 1024;
    else
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }

    int64_t actualSize = 0;
    if(!getObjectSize("chat", key, actualSize) || actualSize > maxSize)
    {
	removeObject("chat", key);
	logger.warn("合并的文件大小超过最大值，执行删除:" + key);
	evhttp_send_reply(req, 413, nullptr, nullptr);
	return;
    }

    json resp;
    resp["Url"] = "http://192.168.153.128/chat/" + key;
    std::string respStr = resp.dump();

    evbuffer* out = evbuffer_new();
    evbuffer_add(out, respStr.data(), respStr.size());
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
    evhttp_send_reply(req, 200, nullptr, out);
}

bool ChatServerUploadWorker::verifyAccessToken(const evkeyvalq* evq)
{
    const char* auth = evhttp_find_header(evq, "Authorization");
    if(!auth)
        return false;

    std::string authStr(auth);
    std::string lowerAuth = authStr;

    for(char& c : lowerAuth)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t pos_tokenLine = lowerAuth.find("bearer ");
    if(pos_tokenLine == std::string::npos)
        return false;

    std::string accessToken = authStr.substr(pos_tokenLine + 7);
    accessToken.erase(accessToken.find_last_not_of(" \t\r\n") + 1);
    int64_t uid = GlobalTools::verifyAccessToken(accessToken);
    if(uid == -1)
        return false;
    return true;
}

std::string ChatServerUploadWorker::getUrlQueryParam(const std::string& url, const std::string& name)
{
    std::string key = "?" + name + "=";
    size_t pos = url.find(key);
    if(pos == std::string::npos)
    {
	key = "&" + name + "=";
	pos = url.find(key);
	if(pos == std::string::npos)
	    return "";
	pos += key.size();
    }
    else
	pos += key.size();
    size_t end = url.find('&', pos);
    if(end == std::string::npos)
	end = url.size();
    return url.substr(pos, end - pos);
}

bool ChatServerUploadWorker::getObjectSize(const std::string& bucket, const std::string& key, int64_t& outSize)
{
    minio::s3::StatObjectArgs args;
    args.bucket = bucket;
    args.object = key;

    auto resp = getMinioClient().StatObject(args);
    if(!resp)
    {
	logger.error("获取Object大小失败:" + resp.Error().String());
	return false;
    }
    outSize = static_cast<int64_t>(resp.size);
    return true;
}

bool ChatServerUploadWorker::removeObject(const std::string& bucket, const std::string& key)
{
    minio::s3::RemoveObjectArgs args;
    args.bucket = bucket;
    args.object = key;

    auto resp = getMinioClient().RemoveObject(args);
    return static_cast<bool>(resp);
}

bool ChatServerUploadWorker::startsWith(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}
