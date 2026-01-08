#ifndef REST_H
#define REST_H

#include <memory>
#include <string>
#include <curl/curl.h>
#include "../../json/json.h"

class CurlGlobalInit {
public:
    CurlGlobalInit();
    ~CurlGlobalInit();
};

struct MemoryStruct {
    std::string memory;
};

std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> connectCurl();

std::string replaceAll(std::string str, const std::string& from, const std::string& to);

std::string executeQueryTimeOut(const std::string& host, int port, const std::string& query, long timeout);

#endif  