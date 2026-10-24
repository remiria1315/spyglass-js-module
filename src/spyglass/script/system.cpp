#include "spyglass/script/system.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#include <winhttp.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#endif

#include "spyglass/core/output.h"

namespace spyglass::script {

#ifdef _WIN32

namespace {

std::wstring wide(const std::string &text)
{
    if (text.empty()) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()));
    return out;
}

std::string narrow(const std::wstring &text)
{
    if (text.empty()) {
        return {};
    }
    std::string out(static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()), nullptr, nullptr);
    return out;
}

struct WinHttpCloser {
    void operator()(void *handle) const { WinHttpCloseHandle(handle); }
};

using WinHttpHandle = std::unique_ptr<void, WinHttpCloser>;

struct HandleCloser {
    void operator()(void *handle) const
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

using Handle = std::unique_ptr<void, HandleCloser>;

}  // namespace

HttpResponse http_request(const HttpRequest &request, const int timeout_ms)
{
    const auto url = wide(request.url);
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)) {
        throw std::runtime_error{std::format("'{}' is not an http or https url", request.url)};
    }

    const WinHttpHandle session{WinHttpOpen(L"spyglass", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                            WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) {
        throw std::runtime_error{"WinHttpOpen failed"};
    }
    WinHttpSetTimeouts(session.get(), timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    const WinHttpHandle connection{WinHttpConnect(
        session.get(), std::wstring{parts.lpszHostName, parts.dwHostNameLength}.c_str(), parts.nPort, 0)};
    if (!connection) {
        throw std::runtime_error{"WinHttpConnect failed"};
    }

    const auto target = std::wstring{parts.lpszUrlPath, parts.dwUrlPathLength} +
                        std::wstring{parts.lpszExtraInfo, parts.dwExtraInfoLength};
    const WinHttpHandle handle{WinHttpOpenRequest(connection.get(), wide(request.method).c_str(), target.c_str(),
                                                  nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                  parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)};
    if (!handle) {
        throw std::runtime_error{"WinHttpOpenRequest failed"};
    }

    std::string headers;
    for (const auto &[name, value] : request.headers) {
        headers += std::format("{}: {}\r\n", name, value);
    }
    const auto wide_headers = wide(headers);
    auto *const body = request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char *>(request.body.data());
    const auto size = static_cast<DWORD>(request.body.size());
    if (!WinHttpSendRequest(handle.get(), wide_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wide_headers.c_str(),
                            static_cast<DWORD>(-1), body, size, size, 0) ||
        !WinHttpReceiveResponse(handle.get(), nullptr)) {
        throw std::runtime_error{std::format("the request failed, error {}", GetLastError())};
    }

    HttpResponse response;
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(handle.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<int>(status);

    DWORD raw_size = 0;
    WinHttpQueryHeaders(handle.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                        &raw_size, WINHTTP_NO_HEADER_INDEX);
    std::wstring raw(raw_size / sizeof(wchar_t), L'\0');
    if (WinHttpQueryHeaders(handle.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                            &raw_size, WINHTTP_NO_HEADER_INDEX)) {
        response.headers = narrow(raw);
    }

    for (DWORD available = 0; WinHttpQueryDataAvailable(handle.get(), &available) && available != 0;) {
        if (response.body.size() + available > 8U * 1024U * 1024U) {
            throw std::runtime_error{"the response is larger than 8 MB"};
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(handle.get(), chunk.data(), available, &read)) {
            throw std::runtime_error{"reading the response failed"};
        }
        response.body.append(chunk, 0, read);
    }
    return response;
}

namespace {

struct WebSocketSession {
    WinHttpHandle session;
    WinHttpHandle connection;
    WinHttpHandle socket;
    std::mutex send_mutex;
};

std::mutex g_sockets_mutex;
std::map<int, std::shared_ptr<WebSocketSession>> g_sockets;
int g_next_socket = 1;

std::shared_ptr<WebSocketSession> socket_of(const int handle)
{
    const std::lock_guard lock{g_sockets_mutex};
    const auto found = g_sockets.find(handle);
    if (found == g_sockets.end()) {
        throw std::runtime_error{"the WebSocket is closed"};
    }
    return found->second;
}

}  // namespace

int websocket_open(const std::string &url, const std::vector<std::pair<std::string, std::string>> &headers,
                   const int timeout_ms)
{
    auto http_url = url;
    if (http_url.starts_with("wss://")) {
        http_url.replace(0, 6, "https://");
    }
    else if (http_url.starts_with("ws://")) {
        http_url.replace(0, 5, "http://");
    }
    else {
        throw std::runtime_error{std::format("'{}' is not a ws or wss url", url)};
    }
    const auto wide_url = wide(http_url);
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) {
        throw std::runtime_error{std::format("'{}' is not a valid url", url)};
    }

    auto socket = std::make_shared<WebSocketSession>();
    socket->session.reset(WinHttpOpen(L"spyglass", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (!socket->session) {
        throw std::runtime_error{"WinHttpOpen failed"};
    }
    WinHttpSetTimeouts(socket->session.get(), timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    socket->connection.reset(WinHttpConnect(socket->session.get(),
                                            std::wstring{parts.lpszHostName, parts.dwHostNameLength}.c_str(),
                                            parts.nPort, 0));
    if (!socket->connection) {
        throw std::runtime_error{"WinHttpConnect failed"};
    }

    const auto target = std::wstring{parts.lpszUrlPath, parts.dwUrlPathLength} +
                        std::wstring{parts.lpszExtraInfo, parts.dwExtraInfoLength};
    const WinHttpHandle request{WinHttpOpenRequest(socket->connection.get(), L"GET", target.c_str(), nullptr,
                                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)};
    if (!request || !WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        throw std::runtime_error{"WinHttpOpenRequest failed"};
    }

    std::string header_text;
    for (const auto &[name, value] : headers) {
        header_text += std::format("{}: {}\r\n", name, value);
    }
    const auto wide_headers = wide(header_text);
    if (!WinHttpSendRequest(request.get(), wide_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wide_headers.c_str(),
                            static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        throw std::runtime_error{std::format("the WebSocket connection failed, error {}", GetLastError())};
    }
    socket->socket.reset(WinHttpWebSocketCompleteUpgrade(request.get(), 0));
    if (!socket->socket) {
        throw std::runtime_error{"the server did not upgrade the connection to a WebSocket"};
    }
    DWORD no_timeout = 0;
    WinHttpSetOption(socket->socket.get(), WINHTTP_OPTION_RECEIVE_TIMEOUT, &no_timeout, sizeof(no_timeout));

    const std::lock_guard lock{g_sockets_mutex};
    const auto handle = g_next_socket++;
    g_sockets.emplace(handle, std::move(socket));
    return handle;
}

void websocket_send(const int handle, const std::string_view data, const bool binary)
{
    const auto socket = socket_of(handle);
    const std::lock_guard lock{socket->send_mutex};
    const auto error = WinHttpWebSocketSend(
        socket->socket.get(),
        binary ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE : WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char *>(data.data()), static_cast<DWORD>(data.size()));
    if (error != ERROR_SUCCESS) {
        throw std::runtime_error{std::format("the WebSocket send failed, error {}", error)};
    }
}

WebSocketMessage websocket_receive(const int handle)
{
    const auto socket = socket_of(handle);
    WebSocketMessage message;
    std::array<char, 8192> chunk{};
    for (;;) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const auto error = WinHttpWebSocketReceive(socket->socket.get(), chunk.data(),
                                                   static_cast<DWORD>(chunk.size()), &read, &type);
        if (error != ERROR_SUCCESS || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            message = {};
            message.closed = true;
            message.code = 1006;
            if (error == ERROR_SUCCESS) {
                USHORT status = 0;
                DWORD length = 0;
                std::array<char, 123> reason{};
                WinHttpWebSocketQueryCloseStatus(socket->socket.get(), &status, reason.data(),
                                                 static_cast<DWORD>(reason.size()), &length);
                message.code = status;
                message.data.assign(reason.data(), length);
                WinHttpWebSocketClose(socket->socket.get(), status, nullptr, 0);
            }
            const std::lock_guard lock{g_sockets_mutex};
            g_sockets.erase(handle);
            return message;
        }
        message.data.append(chunk.data(), read);
        message.binary = type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
                         type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE;
        if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            return message;
        }
    }
}

void websocket_close(const int handle, const int code, const std::string_view reason)
{
    const auto socket = socket_of(handle);
    const std::lock_guard lock{socket->send_mutex};
    WinHttpWebSocketShutdown(socket->socket.get(), static_cast<USHORT>(code), const_cast<char *>(reason.data()),
                             static_cast<DWORD>(reason.size()));
}

ProcessResult run_process(const std::string &file, const std::vector<std::string> &args,
                          const std::filesystem::path &cwd, const int timeout_ms)
{
    auto quote = [](const std::string &word) {
        if (!word.empty() && word.find_first_of(" \t\"") == std::string::npos) {
            return wide(word);
        }
        std::wstring out = L"\"";
        for (const auto c : wide(word)) {
            if (c == L'"') {
                out += L'\\';
            }
            out += c;
        }
        return out + L"\"";
    };
    auto command = quote(file);
    for (const auto &arg : args) {
        command += L' ' + quote(arg);
    }

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) {
        throw std::runtime_error{"CreatePipe failed"};
    }
    const Handle reader{read_end};
    Handle writer{write_end};
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    const Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr)};

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.get();
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    PROCESS_INFORMATION info{};
    const auto directory = cwd.wstring();
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        directory.empty() ? nullptr : directory.c_str(), &startup, &info)) {
        throw std::runtime_error{std::format("cannot start '{}', error {}", file, GetLastError())};
    }
    const Handle process{info.hProcess};
    const Handle thread{info.hThread};
    writer.reset();

    ProcessResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeout_ms};
    for (;;) {
        DWORD available = 0;
        const auto open = PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr) != 0;
        if (open && available != 0) {
            std::string chunk(std::min<DWORD>(available, 65536), '\0');
            DWORD read = 0;
            ReadFile(read_end, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr);
            result.output.append(chunk, 0, read);
            if (result.output.size() > 8U * 1024U * 1024U) {
                TerminateProcess(process.get(), 1);
                throw std::runtime_error{"the output is larger than 8 MB"};
            }
            continue;
        }
        if (!open || WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            TerminateProcess(process.get(), 1);
            throw std::runtime_error{std::format("'{}' ran longer than {} ms", file, timeout_ms)};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    WaitForSingleObject(process.get(), 1000);
    DWORD code = 0;
    GetExitCodeProcess(process.get(), &code);
    result.code = static_cast<int>(code);
    return result;
}

#else

HttpResponse http_request(const HttpRequest &, int)
{
    throw std::runtime_error{"fetch is not available on this platform"};
}

int websocket_open(const std::string &, const std::vector<std::pair<std::string, std::string>> &, int)
{
    throw std::runtime_error{"WebSocket is not available on this platform"};
}

void websocket_send(int, std::string_view, bool)
{
    throw std::runtime_error{"WebSocket is not available on this platform"};
}

WebSocketMessage websocket_receive(int)
{
    throw std::runtime_error{"WebSocket is not available on this platform"};
}

void websocket_close(int, int, std::string_view)
{
    throw std::runtime_error{"WebSocket is not available on this platform"};
}

ProcessResult run_process(const std::string &file, const std::vector<std::string> &args,
                          const std::filesystem::path &cwd, const int timeout_ms)
{
    int output[2];
    if (pipe(output) != 0) {
        throw std::runtime_error{"pipe failed"};
    }

    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(file.c_str()));
    for (const auto &arg : args) {
        argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);
    const auto directory = path_text(cwd);

    const auto child = fork();
    if (child < 0) {
        close(output[0]);
        close(output[1]);
        throw std::runtime_error{"fork failed"};
    }
    if (child == 0) {
        dup2(output[1], 1);
        dup2(output[1], 2);
        close(output[0]);
        close(output[1]);
        const auto null = open("/dev/null", 0);
        dup2(null, 0);
        if (!directory.empty() && chdir(directory.c_str()) != 0) {
            _exit(126);
        }
        execvp(file.c_str(), argv.data());
        _exit(127);
    }
    close(output[1]);

    ProcessResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeout_ms};
    auto failure = std::string{};
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            kill(child, SIGKILL);
            failure = std::format("'{}' ran longer than {} ms", file, timeout_ms);
            break;
        }
        pollfd entry{output[0], POLLIN, 0};
        const auto ready = poll(&entry, 1, static_cast<int>(std::min<long long>(left.count(), 50)));
        if (ready > 0) {
            char buffer[4096];
            const auto read = ::read(output[0], buffer, sizeof(buffer));
            if (read <= 0) {
                break;
            }
            result.output.append(buffer, static_cast<std::size_t>(read));
            if (result.output.size() > 8U * 1024U * 1024U) {
                kill(child, SIGKILL);
                failure = "the output is larger than 8 MB";
                break;
            }
        }
    }
    close(output[0]);
    int status = 0;
    waitpid(child, &status, 0);
    if (!failure.empty()) {
        throw std::runtime_error{failure};
    }
    result.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (result.code == 127 && result.output.empty()) {
        throw std::runtime_error{std::format("cannot start '{}'", file)};
    }
    return result;
}

#endif

}  // namespace spyglass::script
