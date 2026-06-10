#pragma once
#include <fstream>
#include <iostream>
#include <string>
#include <chrono>
#include <filesystem>
#include <mutex>

#define DEBUG true

class Logger
{
public:
    Logger(const std::string& model);
    Logger(const Logger&) = delete;
    ~Logger();

    void info(const std::string& msg);
    void error(const std::string& msg);
    void warn(const std::string& msg);

private:
    std::string model;
    std::string file_path;

    std::ofstream ofs;
    std::mutex mutex_ofs;

    std::chrono::system_clock::time_point midnight_;

    void updateMidnight(const std::chrono::system_clock::time_point& now);
    void openFile();
    void write(const std::string& level, const std::string& msg);
    std::string getCurrentTime(const std::chrono::system_clock::time_point& now);
};
