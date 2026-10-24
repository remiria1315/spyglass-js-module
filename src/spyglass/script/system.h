#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spyglass::script {

struct HttpRequest {
    std::string method;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct HttpResponse {
    int status{0};
    std::string headers;
    std::string body;
};

struct WebSocketMessage {
    bool closed{false};
    bool binary{false};
    int code{0};
    std::string data;
};

struct ProcessResult {
    int code{0};
    std::string output;
};

HttpResponse http_request(const HttpRequest &request, int timeout_ms);

int websocket_open(const std::string &url, const std::vector<std::pair<std::string, std::string>> &headers,
                   int timeout_ms);

void websocket_send(int handle, std::string_view data, bool binary);

WebSocketMessage websocket_receive(int handle);

void websocket_close(int handle, int code, std::string_view reason);

ProcessResult run_process(const std::string &file, const std::vector<std::string> &args,
                          const std::filesystem::path &cwd, int timeout_ms);

}  // namespace spyglass::script
