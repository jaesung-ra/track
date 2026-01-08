#include "logger.hpp"

std::shared_ptr<spdlog::logger> getLogger(const char* logger_name){
    if(spdlog::get(logger_name) != nullptr){
        return spdlog::get(logger_name);
    }
    std::string log_file_path = "/home/nvidia/Desktop/deepstream_gb/logs/";
    log_file_path += logger_name;
    log_file_path += ".txt";

    std::shared_ptr<spdlog::logger> file_logger = spdlog::daily_logger_mt(logger_name, log_file_path, 23, 59);
    file_logger->set_level(spdlog::level::debug);
    file_logger->flush_on(spdlog::level::debug);
    return spdlog::get(logger_name);
}