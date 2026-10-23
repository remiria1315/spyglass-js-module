#pragma once

#include <filesystem>
#include <string>
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

struct ProcessResult {
    int code{0};
    std::string output;
};

HttpResponse http_request(const HttpRequest &request, int timeout_ms);

ProcessResult run_process(const std::string &file, const std::vector<std::string> &args,
                          const std::filesystem::path &cwd, int timeout_ms);

}  // namespace spyglass::script
