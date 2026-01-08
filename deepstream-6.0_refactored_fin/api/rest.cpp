#include <iostream>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include "rest.h"

using namespace std;

CurlGlobalInit::CurlGlobalInit() {
    curl_global_init(CURL_GLOBAL_ALL);
}

CurlGlobalInit::~CurlGlobalInit() {
    curl_global_cleanup();
}

size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp){
    size_t real_size = size * nmemb;
    MemoryStruct* mem = static_cast<MemoryStruct*>(userp);
    mem->memory.append(static_cast<char*>(contents), real_size);
    return real_size;
}

std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> connectCurl() {
    static CurlGlobalInit curl_init;

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if(!curl){
        return std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>(nullptr, &curl_easy_cleanup);
    }
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, "data");
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl.get(), CURLOPT_FORBID_REUSE, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);

    return curl;
}

std::string replaceAll(std::string str, const std::string& from, const std::string& to) {
    size_t startPos = 0;
    while ((startPos = str.find(from, startPos)) != std::string::npos) {
        str.replace(startPos, from.length(), to);
        startPos += to.length();
    }
    return str;
}

std::string executeQueryTimeOut(const std::string& host, int port, const std::string& query, long timeout) {
    auto curl_ptr = connectCurl();
    if (!curl_ptr) {
        return R"({"status":-2})";  
    }

    CURL* curl = curl_ptr.get();  

    std::string encoded_query = replaceAll(query, " ", "%20");

    std::string url = "http://" + host + ":" + std::to_string(port) + "/api/1.0/?Procedure=@AdHoc&Parameters=[\"" + encoded_query + "\"]";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);

    MemoryStruct response;
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        return R"({"status":-2})";
    }

    return response.memory.empty() ? R"({"status":-2})" : response.memory;
}