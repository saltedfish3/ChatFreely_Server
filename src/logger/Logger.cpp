#include "Logger.h"
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

Logger::Logger(const std::string& model)
    :model(model)
{
    file_path = "logs/" + model;
    updateMidnight(std::chrono::system_clock::now());
    openFile();
}

Logger::~Logger()
{
    ofs.flush();
}

void Logger::info(const std::string& msg)
{
    write("INFO", msg);
}

void Logger::error(const std::string& msg)
{
    write("ERROR", msg);
}

void Logger::warn(const std::string& msg)
{
    write("WARN", msg);
}

void Logger::updateMidnight(const std::chrono::system_clock::time_point& now)
{
    std::time_t time_t_midnight = std::chrono::system_clock::to_time_t(now);
    std::tm tm_midnight;
    localtime_r(&time_t_midnight,&tm_midnight);
    tm_midnight.tm_hour = 0;
    tm_midnight.tm_min = 0;
    tm_midnight.tm_sec = 0;
    this->midnight_ = std::chrono::system_clock::from_time_t(std::mktime(&tm_midnight));
}

void Logger::openFile()
{
    this->ofs.close();
    if(!std::filesystem::create_directories(this->file_path) && !std::filesystem::exists(this->file_path))
	throw std::runtime_error("Error to create log directory" + this->file_path);

    std::time_t midnight = std::chrono::system_clock::to_time_t(this->midnight_);
    std::tm tm_midnight;
    localtime_r(&midnight, &tm_midnight);

    std::ostringstream filename; 
    filename << this->file_path << "/server_" << std::put_time(&tm_midnight, "%Y-%m-%d") << ".log";
    this->ofs = std::ofstream(filename.str(), std::ios::app);
    if(!this->ofs.is_open())
    {
	std::cerr << "open log file error\n";
	throw std::runtime_error("open log file error");
    }
}

std::string Logger::getCurrentTime(const std::chrono::system_clock::time_point& now)
{
    std::time_t t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now;
    localtime_r(&t_now, &tm_now);
    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S"); 
    return oss.str();
}

void Logger::write(const std::string& level, const std::string& msg)
{
    std::lock_guard<std::mutex> lock(this->mutex_ofs);
    auto now = std::chrono::system_clock::now();
    if(now >= this->midnight_ + std::chrono::hours(24))
    {
	ofs.flush();
	updateMidnight(now);
	openFile();
    }
    std::ostringstream oss;
    oss << " ["  << getCurrentTime(now) << "] [" << level << "] " << msg << "\n";
    this->ofs << oss.str();
    if(level == "WARN" || level == "ERROR")
	this->ofs.flush();
    if(DEBUG)
	std::cout << oss.str();
}
