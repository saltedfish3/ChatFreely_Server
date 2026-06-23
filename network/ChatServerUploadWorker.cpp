#include "ChatServerUploadWorker.h"
#include <cctype>
#include <cstring>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <filesystem>
#include <string>

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
    if(command == EVHTTP_REQ_POST && url == "/upload")
    {
	cb_upload(req, nullptr);
    }
    else if(command == EVHTTP_REQ_GET && url.find("/upload/") != std::string::npos)
    {
	if(url.size() > 8)
	    cb_download(req, nullptr);
	else
	{
	    evhttp_send_reply(req, 404, nullptr, nullptr);
	    return;
	}
    }
    else
    {
	evhttp_send_reply(req, 404, nullptr, nullptr);
	return;
    }
}

void ChatServerUploadWorker::cb_upload(evhttp_request* req, void* arg)
{
    //using multipart/form-data
    const char* ct = evhttp_find_header(evhttp_request_get_input_headers(req), "Content-Type");
    if(!ct)
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }

    std::string content_type(ct);
    const std::string preKeyWord= "boundary=";
    size_t pos = content_type.find(preKeyWord);
    if(pos == std::string::npos)
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }
    
    std::string raw_boundary = content_type.substr(pos + preKeyWord.size());
    auto start = raw_boundary.find_first_not_of(" \t\"'");
    auto end = raw_boundary.find_last_not_of(" \t\"'");
    if(start == std::string::npos)
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }
    std::string boundary = "--" + raw_boundary.substr(start, end - start + 1);

    evbuffer* buf_input = evhttp_request_get_input_buffer(req);
    size_t len_body = evbuffer_get_length(buf_input);
    if(len_body == 0)
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }
    if(len_body > 2*1024*1024)
    {
	evhttp_send_reply(req, 413, nullptr, nullptr);
	return;
    }
    
    //get body
    std::string body(len_body, '\0');
    evbuffer_copyout(buf_input, &body[0], len_body);

    size_t pos_header = body.find(boundary);
    if(pos_header == std::string::npos)
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }

    //get data type
    size_t pos_end_header = body.find("\r\n\r\n", pos_header + boundary.size());
    if(pos_end_header == std::string::npos)
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }

    std::string original_header = body.substr(pos_header + boundary.size(), pos_end_header - (pos_header + boundary.size()));
    std::string header = original_header;
    for(char& c : header)
	c = std::tolower(static_cast<unsigned char>(c));
    size_t pos_cd = header.find("content-disposition:");
    if(pos_cd == std::string::npos)
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }
    size_t pos_end_cd = header.find("\r\n", pos_cd);
    if(pos_end_cd == std::string::npos)
	pos_end_cd = header.size();

    pos_cd += 20;
    while(pos_cd < header.size() && header[pos_cd] == ' ')
	pos_cd++;
    std::string cd = header.substr(pos_cd, pos_end_cd - pos_cd);

    size_t pos_formData = cd.find("form-data");
    if(pos_formData == std::string::npos)
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }
    pos_formData += 9; 

    std::string requestName;
    std::string filename;
    std::string file_type;
    size_t pos_requestName = cd.find("name=\"", pos_formData);
    if(pos_requestName == std::string::npos)
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }
    pos_requestName += 6;
    size_t pos_end_requestName = cd.find("\"", pos_requestName);
    if(pos_end_requestName == std::string::npos)
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }
    requestName = cd.substr(pos_requestName,pos_end_requestName - pos_requestName);
    if(requestName != "file")
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }

    size_t pos_fileName = cd.find("filename=\"", pos_formData);
    if(pos_fileName == std::string::npos)
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }
    pos_fileName += 10;
    size_t pos_end_fileName = cd.find("\"",pos_fileName);
    if(pos_end_fileName == std::string::npos)
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }
    filename = cd.substr(pos_fileName,pos_end_fileName - pos_fileName);
    if(filename.empty())
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }


    size_t pos_fileType = header.find("content-type:");
    if(pos_fileType == std::string::npos)
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }
    pos_fileType += 13;
    size_t pos_end_fileType = header.find("\r\n",pos_fileType);
    if(pos_end_fileType == std::string::npos)
	pos_end_fileType = header.size();
    while(pos_fileType < header.size() && header[pos_fileType] == ' ')
	pos_fileType++;

    file_type = header.substr(pos_fileType,pos_end_fileType - pos_fileType);
    if(file_type != "image/jpeg" && file_type != "image/png")
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }
    
    //get file data
    pos_end_header += 4;

    size_t pos_end = body.find(boundary, pos_end_header);
    if(pos_end == std::string::npos)
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }
    
    if(pos_end < 2 || body[pos_end - 2] != '\r' || body[pos_end - 1] != '\n')
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }
    pos_end -= 2;
    size_t len_data = pos_end - pos_end_header;
    const char* binary_data = &body[pos_end_header];
    bool is_jpeg = false;
    bool is_png = false;
    std::string suffix;
    if(len_data >= 3)
	is_jpeg = isJpeg(binary_data);
    if(len_data >= 8)
	is_png = isPng(binary_data);
    if(!is_jpeg && !is_png)
    {
	evhttp_send_reply(req, 400, nullptr, nullptr);
	return;
    }
    if(is_jpeg)
	suffix = ".jpg";
    else if(is_png)
	suffix = ".png";
    std::string filepath = "upload/" + std::to_string(sfGen.nextid()) + suffix;
    try
    {
	std::filesystem::create_directories(std::filesystem::path(filepath).parent_path());
    }catch(const std::filesystem::filesystem_error& e)
    {
	logger.warn(std::string("Create Upload DIR Error:") + e.what());
	evhttp_send_reply(req, 500, nullptr, nullptr);
	return;
    }
    std::ofstream ofs(filepath, std::ios::binary);
    if(!ofs.is_open())
    {
	logger.warn("Open Avatar File Error");
	evhttp_send_reply(req, 500, nullptr, nullptr);
	return;
    }
    ofs.write(binary_data, len_data);
    ofs.close();

    //give back url
    evbuffer* out = evbuffer_new();
    evbuffer_add_printf(out, "{\"Url\":\"http://192.168.153.128:9001/%s\"}",filepath.c_str());
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
    evhttp_send_reply(req, 200, nullptr, out);
}

bool ChatServerUploadWorker::isJpeg(const char* binary_data)
{
    return static_cast<unsigned char>(binary_data[0]) == 0xFF &&
	static_cast<unsigned char>(binary_data[1]) == 0xD8 &&
	static_cast<unsigned char>(binary_data[2]) == 0xFF;
}

bool ChatServerUploadWorker::isPng(const char* binary_data)
{
    return static_cast<unsigned char>(binary_data[0]) == 0x89 &&
	static_cast<unsigned char>(binary_data[1]) == 0x50 &&
	static_cast<unsigned char>(binary_data[2]) == 0x4E &&
	static_cast<unsigned char>(binary_data[3]) == 0x47 &&
	static_cast<unsigned char>(binary_data[4]) == 0x0D &&
	static_cast<unsigned char>(binary_data[5]) == 0x0A &&
	static_cast<unsigned char>(binary_data[6]) == 0x1A &&
	static_cast<unsigned char>(binary_data[7]) == 0x0A;
}

void ChatServerUploadWorker::cb_download(evhttp_request* req, void* arg)
{
    const char* uri = evhttp_request_get_uri(req);

    std::string filepath = "." + std::string(uri);
    if(filepath.find("..") != std::string::npos)
    {
	evhttp_send_reply(req, 403, nullptr, nullptr);
	return;
    }
    std::ifstream ifs(filepath, std::ios::binary);
    if(!ifs.is_open())
    {
	evhttp_send_reply(req, 404, nullptr, nullptr);
	return;
    }

    evbuffer* data = evbuffer_new();
    char buf[4096];
    while(ifs.read(buf, sizeof(buf)).gcount() > 0)
	evbuffer_add(data, buf, ifs.gcount());
    ifs.close();
    
    std::string ct = "application/octet-stream";

    size_t pos_point = filepath.rfind(".");
    if(pos_point != std::string::npos)
    {
	if(filepath.substr(pos_point) == ".png")
	    ct = "image/png";
	else if(filepath.substr(pos_point) == ".jpeg" ||
		filepath.substr(pos_point) == ".jpg")
	    ct = "image/jpeg";
    }
    evkeyvalq* header = evhttp_request_get_output_headers(req);
    evhttp_add_header(header, "Content-Type", ct.c_str());
    evhttp_add_header(header, "Cache-Control", "public, max-age=86400");
    evhttp_add_header(header, "Content-Length", std::to_string(evbuffer_get_length(data)).c_str());

    evhttp_send_reply(req, 200, nullptr, data);
}
