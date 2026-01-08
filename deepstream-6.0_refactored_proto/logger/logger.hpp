#include <unistd.h>
#include "../spdlog/spdlog.h"
#include "../spdlog/sinks/basic_file_sink.h"
#include "../spdlog/sinks/daily_file_sink.h"

#include "../spdlog/fmt/fmt.h"
#include "../spdlog/sinks/sink.h"

using namespace std;

std::shared_ptr<spdlog::logger> getLogger(const char* logger_name);