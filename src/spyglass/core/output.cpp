#include "spyglass/core/output.h"

#include <string>
#include <system_error>

#include <Windows.h>

namespace spyglass {
namespace {

std::filesystem::path create()
{
    std::filesystem::path directory;
    std::error_code ec;

    if (const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0); length != 0) {
        std::wstring local(length, L'\0');
        local.resize(GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), length));
        directory = std::filesystem::path{local} / L"spyglass";
    }

    if (directory.empty()) {
        directory = std::filesystem::temp_directory_path(ec) / "spyglass";
    }

    std::filesystem::create_directories(directory, ec);
    return directory;
}

}  // namespace

const std::filesystem::path &output_directory()
{
    static const std::filesystem::path directory = create();
    return directory;
}

std::string path_text(const std::filesystem::path &path)
{
    const auto text = path.u8string();
    return std::string{text.begin(), text.end()};
}

std::filesystem::path path_of(const std::string_view text)
{
    return std::filesystem::path{std::u8string{text.begin(), text.end()}};
}

}  // namespace spyglass
