#include "spyglass/install.h"

#include <exception>
#include <format>

#ifdef _WIN32
#include <Windows.h>
#else
#include <android/log.h>
#endif

#include "spyglass/error.h"
#include "spyglass/network.h"
#include "spyglass/overlay/install.h"
#include "spyglass/overlay/store.h"
#include "spyglass/script/engine.h"

namespace spyglass {
namespace {

template <typename Step>
void attempt(const char *what, Step &&step)
{
    try {
        step();
    }
    catch (const std::exception &e) {
        report_error(std::format("{}: {}", what, e.what()));
    }
    catch (...) {
        report_error(std::format("{}: unknown error", what));
    }
}

void alert(const char *reason)
{
    const auto message = std::format("The overlay did not install, so nothing else did either.\n\n{}", reason);
#ifdef _WIN32
    MessageBoxA(nullptr, message.c_str(), "Spyglass", MB_OK | MB_ICONERROR | MB_TOPMOST);
#else
    __android_log_print(ANDROID_LOG_ERROR, "spyglass", "%s", message.c_str());
#endif
}

}  // namespace

void install()
{
    attempt("stale captures", sweep_captures);

    try {
        install_overlay();
    }
    catch (const std::exception &e) {
        alert(e.what());
        return;
    }
    catch (...) {
        alert("unknown error");
        return;
    }

    attempt("packet hook", install_network_hook);
    attempt("script modules", [] {
        script::set_sender([](const script::Side side, const Direction direction, const int id,
                              const std::string_view body, const int player, const int sub_client) {
            if (side == script::Side::Server) {
                return direction == Direction::Outbound && send_to_player(id, body, player, sub_client);
            }
            if (direction == Direction::Outbound) {
                return send_to_server(id, body, sub_client);
            }
            return sub_client == 0 && send_to_client(id, body);
        });
        script::start(packet_names());
    });
}

}  // namespace spyglass
