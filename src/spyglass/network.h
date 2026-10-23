#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bedrock/common_types.h"
#include "bedrock/platform/result.h"

class Packet;
class ReadOnlyBinaryStream;

namespace cereal {
struct ReflectionCtx;
}

namespace spyglass {

inline constexpr std::size_t kPacketIdLimit = 512;

struct Hooks {
    void *create_packet{nullptr};
    void *send_internal{nullptr};
    std::size_t packet_handlers{0};
    std::size_t packet_classes{0};
};

void install_network_hook();

const Hooks &hooks();

const std::vector<std::string> &packet_names();

[[nodiscard]] const cereal::ReflectionCtx *reflection_ctx();
[[nodiscard]] std::shared_ptr<Packet> create_packet(int id);
[[nodiscard]] bool send_to_server(int id, std::string_view body);
[[nodiscard]] bool send_to_client(int id, std::string_view body);
[[nodiscard]] bool send_to_player(int id, std::string_view body, int player);
Bedrock::Result<void> read_no_header(Packet &packet, ReadOnlyBinaryStream &stream, const cereal::ReflectionCtx &ctx,
                                     SubClientId sub_id);

}  // namespace spyglass
