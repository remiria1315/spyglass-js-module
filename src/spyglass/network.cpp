#include "spyglass/network.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <map>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "bedrock/core/utility/binary_stream.h"
#include "bedrock/network/minecraft_packets.h"
#include <nlohmann/json.hpp>

#include "bedrock/network/network_system.h"
#include "bedrock/network/packet.h"
#include "bedrock/network/packet_handler_dispatcher.h"
#include "spyglass/core/output.h"
#include "spyglass/detail.h"
#include "spyglass/error.h"
#include "spyglass/filename.h"
#include "spyglass/hook.h"
#include "spyglass/pattern.h"
#include "spyglass/overlay/view.h"
#include "spyglass/reflect.h"
#include "spyglass/script/chat.h"
#include "spyglass/script/engine.h"
#include "spyglass/signature.h"

namespace {

void *g_create_packet = nullptr;
void *g_send_internal = nullptr;
std::atomic<const cereal::ReflectionCtx *> g_reflection{nullptr};
spyglass::Hooks g_hooks;

constexpr bool kBreakFirstSubChunk = false;
constexpr int kSubChunkPacket = 174;
std::atomic_bool g_broke_one{false};

// MSVC reverses the read overload pair and Itanium spends a second slot on the destructor, which
// cancel; the write is a lone virtual, so it lands one slot later on Itanium
#ifdef _WIN32
constexpr std::size_t kPacketWriteSlot = 5;
#else
constexpr std::size_t kPacketWriteSlot = 6;
#endif
constexpr std::size_t kPacketReadSlot = 9;
#ifdef _WIN32
constexpr std::size_t kHandlerHandleSlot = 1;
#else
constexpr std::size_t kHandlerHandleSlot = 2;
#endif

std::mutex g_inject_mutex;
std::atomic_size_t g_pending{0};
std::mutex g_side_mutex;
std::map<const void *, spyglass::script::Side> g_sides;
std::mutex g_players_mutex;
std::vector<NetworkIdentifier> g_players;
thread_local const Packet *g_read_packet = nullptr;
thread_local std::string g_read_body;

struct ServerFrame {
    int player;
    std::string frame;
};
std::vector<ServerFrame> g_injected_server;

constexpr std::array kServerbound{1, 4, 8, 69, 77, 129, 144, 152, 193};
constexpr std::array kClientbound{2, 3, 5, 6, 7, 143};

spyglass::script::Side side_of(const void *object, const int id, const bool sender)
{
    using spyglass::script::Side;
    const std::lock_guard lock{g_side_mutex};
    if (const auto found = g_sides.find(object); found != g_sides.end()) {
        return found->second;
    }
    const auto serverbound = std::ranges::find(kServerbound, id) != kServerbound.end();
    const auto clientbound = std::ranges::find(kClientbound, id) != kClientbound.end();
    if (!serverbound && !clientbound) {
        return Side::Client;
    }
    return g_sides.emplace(object, serverbound == sender ? Side::Client : Side::Server).first->second;
}

int player_index(const NetworkIdentifier &id)
{
    constexpr auto kComparable = sizeof(NetworkIdentifier) - sizeof(std::uint32_t);
    const std::lock_guard lock{g_players_mutex};
    for (std::size_t i = 0; i < g_players.size(); ++i) {
        if (std::memcmp(&g_players[i], &id, kComparable) == 0) {
            return static_cast<int>(i);
        }
    }
    g_players.push_back(id);
    return static_cast<int>(g_players.size() - 1);
}

std::size_t header_size(const std::string_view frame)
{
    std::size_t header = 1;
    while (header < 5 && header < frame.size() && (static_cast<std::uint8_t>(frame[header - 1]) & 0x80U) != 0) {
        ++header;
    }
    return header;
}

int packet_id_of(const std::string_view frame)
{
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < header_size(frame) && i < frame.size(); ++i) {
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(frame[i]) & 0x7FU) << (7 * i);
    }
    return static_cast<int>(value & 0x3FFU);
}

std::string frame_of(const int id, const std::string_view body, const std::uint32_t header_bits = 0)
{
    std::string frame;
    for (auto rest = static_cast<std::uint32_t>(id) | header_bits;; rest >>= 7U) {
        const auto low = static_cast<char>(rest & 0x7FU);
        frame.push_back(rest >= 0x80U ? static_cast<char>(low | 0x80) : low);
        if (rest < 0x80U) {
            break;
        }
    }
    frame.append(body);
    return frame;
}

std::vector<std::string> g_injected;
std::vector<std::pair<int, std::string>> g_injected_inbound;
thread_local bool g_delivering = false;

constexpr auto kPacketRead = &Packet::readDetour;
constexpr auto kPacketWrite = &Packet::writeDetour;

void *original_of(const Packet &packet, const std::size_t ordinal)
{
    auto **vtable = *reinterpret_cast<void ***>(const_cast<Packet *>(&packet));
    return spyglass::vfunc_original(vtable, ordinal);
}

constexpr int kMaxErrorDepth = 16;

nlohmann::ordered_json error_json(const Bedrock::ErrorInfo<std::error_code> &info, const int depth)
{
    nlohmann::ordered_json error{
        {"reason", std::format("{} ({} {})", info.error.message(), info.error.category().name(), info.error.value())},
    };

    if (!info.call_stack.frames.empty()) {
        auto &frames = error["frames"] = nlohmann::ordered_json::array();
        for (const auto &entry : info.call_stack.frames) {
            auto filename = entry.frame.filename;
            if (filename.empty() || filename == "-") {
                filename = spyglass::filename_of(entry.frame.filename_hash);
            }
            auto label = filename.empty() ? std::format("<{:016x}>:{}", entry.frame.filename_hash, entry.frame.line)
                                          : std::format("{}:{}", filename, entry.frame.line);
            if (entry.context.has_value()) {
                label += std::format(" - {}", entry.context->value);
            }
            frames.push_back(std::move(label));
        }
    }

    if (depth < kMaxErrorDepth && !info.branches.empty()) {
        auto &causes = error["causes"] = nlohmann::ordered_json::array();
        for (const auto &branch : info.branches) {
            causes.push_back(error_json(branch, depth + 1));
        }
    }
    return error;
}

nlohmann::ordered_json error_of(const Bedrock::Result<void> &result)
{
    if (result.asExpected().has_value()) {
        return {};
    }
    return error_json(result.asExpected().error(), 0);
}

}  // namespace

Bedrock::Result<void> read_packet_header(ReadOnlyBinaryStream &stream, int &id)
{
    const auto header = stream.getUnsignedVarInt();
    id = header.asExpected().has_value() ? static_cast<int>(header.asExpected().value() & 0x3FF) : -1;
    return {};
}

void record_sent(const Packet &packet, const std::string_view body)
{
    const auto id = static_cast<int>(packet.getId());
    auto &overlay = spyglass::View::getInstance();
    overlay.onPacketSend({
        .id = id,
        .name = packet.getName(),
        .decoded = true,
        .unread = 0,
        .sub_id = static_cast<std::uint8_t>(packet.getSenderSubId()),
        .fields = overlay.wants_fields(id, spyglass::Direction::Outbound)
                      ? spyglass::decode_fields(const_cast<Packet &>(packet), id)
                      : nlohmann::ordered_json{},
        .body = body,
    });
}

void Packet::writeDetour(BinaryStream &stream, const cereal::ReflectionCtx &reflection_ctx,
                         const std::optional<SerializationMode> override_mode) const
{
    const auto begin = stream.written().size();
    SPYGLASS_CALL_ORIGINAL(kPacketWrite, original_of(*this, kPacketWriteSlot), this, stream, reflection_ctx,
                           override_mode);
    if (spyglass::hooks().send_internal == nullptr) {
        const auto written = stream.written();
        record_sent(*this, written.substr(std::min(begin, written.size())));
    }
}

void NetworkSystem::sendInternalDetour(const NetworkIdentifier &id, const Packet &packet, const std::string &data)
{
    using spyglass::script::Side;
    const auto packet_id = static_cast<int>(packet.getId());
    const auto side = side_of(this, packet_id, true);
    const auto header = header_size(data);
    const std::string_view body = std::string_view{data}.substr(std::min(header, data.size()));

    auto cancelled = false;
    std::optional<std::string> rewritten;
    if (side == Side::Client && packet_id == static_cast<int>(MinecraftPacketIds::Text)) {
        const auto command = spyglass::script::chat_command(body);
        cancelled = command && spyglass::script::handle_chat(*command);
    }
    if (!cancelled && spyglass::script::wants(side, spyglass::Direction::Outbound, packet_id)) {
        auto verdict = spyglass::script::dispatch(
            {.direction = spyglass::Direction::Outbound,
             .id = packet_id,
             .name = packet.getName(),
             .sub_id = static_cast<std::uint8_t>(packet.getSenderSubId()),
             .body = body,
             .side = side,
             .player = side == Side::Server ? player_index(id) : -1},
            [&] { return spyglass::decode_fields(const_cast<Packet &>(packet), packet_id); });
        cancelled = verdict.cancelled;
        if (verdict.body) {
            rewritten = data.substr(0, header) + *verdict.body;
        }
    }
    if (!cancelled) {
        const auto &frame = rewritten ? *rewritten : data;
        SPYGLASS_CALL_ORIGINAL(&NetworkSystem::sendInternalDetour, g_send_internal, this, id, packet, frame);
        if (side == Side::Client) {
            record_sent(packet, std::string_view{frame}.substr(std::min(header, frame.size())));
        }
    }

    if (g_pending.load(std::memory_order_relaxed) == 0) {
        return;
    }
    std::vector<std::string> to_server;
    std::vector<ServerFrame> to_players;
    {
        const std::lock_guard lock{g_inject_mutex};
        if (side == Side::Server) {
            to_players.swap(g_injected_server);
        }
        else {
            to_server.swap(g_injected);
        }
        g_pending.store(g_injected.size() + g_injected_inbound.size() + g_injected_server.size(),
                        std::memory_order_relaxed);
    }
    for (const auto &frame : to_server) {
        const auto injected = spyglass::create_packet(packet_id_of(frame));
        if (injected) {
            SPYGLASS_CALL_ORIGINAL(&NetworkSystem::sendInternalDetour, g_send_internal, this, id, *injected, frame);
            record_sent(*injected, std::string_view{frame}.substr(header_size(frame)));
        }
    }
    for (const auto &[player, frame] : to_players) {
        const auto injected = spyglass::create_packet(packet_id_of(frame));
        if (!injected) {
            continue;
        }
        std::vector<NetworkIdentifier> targets;
        {
            const std::lock_guard lock{g_players_mutex};
            if (player < 0) {
                targets = g_players;
            }
            else if (static_cast<std::size_t>(player) < g_players.size()) {
                targets.push_back(g_players[static_cast<std::size_t>(player)]);
            }
        }
        for (const auto &target : targets) {
            SPYGLASS_CALL_ORIGINAL(&NetworkSystem::sendInternalDetour, g_send_internal, this, target, *injected, frame);
        }
    }
}

Bedrock::Result<void> Packet::readDetour(ReadOnlyBinaryStream &stream, const cereal::ReflectionCtx &reflection_ctx)
{
    g_reflection.store(&reflection_ctx, std::memory_order_relaxed);
    auto *const original = original_of(*this, kPacketReadSlot);
    const auto begin = stream.getReadPointer();
    const auto view = stream.getView();

    const auto id = static_cast<int>(getId());

    if (kBreakFirstSubChunk && id == kSubChunkPacket && !g_broke_one.exchange(true)) {
        const auto body = view.substr(std::min(begin, view.size()));
        ReadOnlyBinaryStream truncated{body.substr(0, body.size() / 2), true};
        auto broken = SPYGLASS_CALL_ORIGINAL(kPacketRead, original, this, truncated, reflection_ctx);
        stream.setReadPointer(view.size());
        auto &overlay = spyglass::View::getInstance();
        overlay.onPacketReceive({
            .id = id,
            .name = getName(),
            .decoded = broken.asExpected().has_value(),
            .unread = static_cast<std::uint32_t>(body.size() - truncated.getReadPointer()),
            .error = error_of(broken),
            .fields = overlay.wants_fields(id, spyglass::Direction::Inbound)
                          ? spyglass::decode_fields(*this, id)
                          : nlohmann::ordered_json{},
            .body = body,
        });
        return broken;
    }

    auto result = SPYGLASS_CALL_ORIGINAL(kPacketRead, original, this, stream, reflection_ctx);

    auto body = view.substr(std::min(begin, view.size()));
    auto unread = static_cast<std::uint32_t>(stream.getUnreadLength());
    g_read_packet = nullptr;
    if (result.asExpected().has_value() &&
        (spyglass::script::wants(spyglass::script::Side::Client, spyglass::Direction::Inbound, id) ||
         spyglass::script::wants(spyglass::script::Side::Server, spyglass::Direction::Inbound, id))) {
        g_read_packet = this;
        g_read_body.assign(body);
    }

    auto &overlay = spyglass::View::getInstance();
    overlay.onPacketReceive({
        .id = id,
        .name = getName(),
        .decoded = result.asExpected().has_value(),
        .unread = unread,
        .sub_id = static_cast<std::uint8_t>(getSenderSubId()),
        .error = error_of(result),
        .fields = overlay.wants_fields(id, spyglass::Direction::Inbound)
                      ? spyglass::decode_fields(*this, id)
                      : nlohmann::ordered_json{},
        .body = body,
    });
    return result;
}

void IPacketHandlerDispatcher::handleDetour(const NetworkIdentifier &id, NetEventCallback &callback,
                                            std::shared_ptr<Packet> &packet) const
{
    using spyglass::script::Side;
    auto **const vtable = *reinterpret_cast<void ***>(const_cast<IPacketHandlerDispatcher *>(this));
    const auto packet_id = static_cast<int>(packet->getId());
    const auto side = side_of(&callback, packet_id, false);

    auto cancelled = false;
    if (g_read_packet == packet.get() && spyglass::script::wants(side, spyglass::Direction::Inbound, packet_id)) {
        g_read_packet = nullptr;
        auto verdict = spyglass::script::dispatch(
            {.direction = spyglass::Direction::Inbound,
             .id = packet_id,
             .name = packet->getName(),
             .sub_id = static_cast<std::uint8_t>(packet->getSenderSubId()),
             .body = g_read_body,
             .side = side,
             .player = side == Side::Server ? player_index(id) : -1},
            [&] { return spyglass::decode_fields(*packet, packet_id); });
        cancelled = verdict.cancelled;
        if (verdict.body && !cancelled && spyglass::reflection_ctx() != nullptr) {
            ReadOnlyBinaryStream replacement{*verdict.body, true};
            spyglass::read_no_header(*packet, replacement, *spyglass::reflection_ctx(), packet->getSenderSubId());
        }
    }
    if (!cancelled) {
        SPYGLASS_CALL_ORIGINAL(&IPacketHandlerDispatcher::handleDetour,
                               spyglass::vfunc_original(vtable, kHandlerHandleSlot), this, id, callback, packet);
    }
    if (g_delivering || side != Side::Client || g_pending.load(std::memory_order_relaxed) == 0) {
        return;
    }

    std::vector<std::pair<int, std::string>> injected;
    {
        const std::lock_guard lock{g_inject_mutex};
        injected.swap(g_injected_inbound);
        g_pending.store(g_injected.size() + g_injected_inbound.size() + g_injected_server.size(),
                        std::memory_order_relaxed);
    }
    g_delivering = true;
    for (const auto &[injected_id, body] : injected) {
        auto created = spyglass::create_packet(injected_id);
        const auto *const ctx = spyglass::reflection_ctx();
        if (!created || ctx == nullptr || created->handler() == nullptr) {
            continue;
        }
        ReadOnlyBinaryStream stream{body, true};
        auto result = spyglass::read_no_header(*created, stream, *ctx, static_cast<SubClientId>(0));
        if (result.asExpected().has_value()) {
            static_cast<const IPacketHandlerDispatcher *>(created->handler())->handle(id, callback, created);
        }
    }
    g_delivering = false;
}

namespace spyglass {
namespace {

void hook_packet_classes()
{
    std::unordered_set<void **> seen;
    std::size_t classes = 0;
    for (std::size_t id = 1; id < kPacketIdLimit; ++id) {
        const auto packet = create_packet(static_cast<int>(id));
        if (!packet) {
            continue;
        }
        auto **vtable = *reinterpret_cast<void ***>(packet.get());
        if (!seen.insert(vtable).second) {
            continue;
        }
        if (static_cast<std::size_t>(packet->getId()) != id) {
            report_error(std::format("Packet::read: id {} reports itself as {}, refusing to hook a vtable that does "
                                     "not match the mirror",
                                     id, static_cast<int>(packet->getId())));
            return;
        }
        if (swap_vfunc("Packet::writeWithSerializationMode", vtable, kPacketWriteSlot,
                       detail::fp_cast(kPacketWrite)) &&
            swap_vfunc("Packet::read", vtable, kPacketReadSlot, detail::fp_cast(kPacketRead))) {
            ++classes;
        }
    }
    g_hooks.packet_classes = classes;

    for (std::size_t id = 1; id < kPacketIdLimit; ++id) {
        const auto packet = create_packet(static_cast<int>(id));
        if (packet && packet->handler() != nullptr &&
            swap_vfunc("PacketHandlerDispatcher::handle", *reinterpret_cast<void ***>(const_cast<void *>(packet->handler())),
                       kHandlerHandleSlot, detail::fp_cast(&IPacketHandlerDispatcher::handleDetour))) {
            ++g_hooks.packet_handlers;
        }
    }
    if (g_hooks.packet_handlers == 0) {
        report_error("PacketHandlerDispatcher::handle is not hooked, so a packet cannot be sent to the client");
    }
}

}  // namespace

void install_network_hook()
{
    verify_client();
    const auto &signature = signatures();

    g_create_packet = locate("MinecraftPackets::createPacket", signature.create_packet);
    g_hooks.create_packet = g_create_packet;
    if (g_create_packet != nullptr) {
        hook_packet_classes();
    }

    if (!signature.send.empty()) {
        auto *const send = locate("NetworkSystem::send", signature.send);
        if (send != nullptr) {
            try {
                const auto call_size = static_cast<std::size_t>(std::ranges::count(signature.send_internal_call, ' ') + 1);
                const auto *const call = static_cast<const std::uint8_t *>(
                    find_within(send, 0x400, signature.send_internal_call));
                std::int32_t relative = 0;
                std::memcpy(&relative, call + call_size, sizeof(relative));
                auto *const target = const_cast<std::uint8_t *>(call + call_size + sizeof(relative)) + relative;
                g_hooks.send_internal = install_hook_at("NetworkSystem::_sendInternal", target,
                                                        detail::fp_cast(&NetworkSystem::sendInternalDetour),
                                                        &g_send_internal);
            }
            catch (const std::exception &e) {
                report_error(std::format("NetworkSystem::_sendInternal: {}", e.what()));
            }
        }
    }
    if (g_hooks.send_internal == nullptr) {
        report_error("NetworkSystem::_sendInternal is not hooked, so modules cannot see, cancel or change packets that are "
                     "sent, and cannot send packets");
    }
}

const Hooks &hooks()
{
    return g_hooks;
}

const std::vector<std::string> &packet_names()
{
    static std::vector<std::string> names;
    static std::once_flag once;
    if (g_create_packet != nullptr) {
        std::call_once(once, [] {
            names.resize(kPacketIdLimit);
            const auto create = reinterpret_cast<decltype(&MinecraftPackets::createPacket)>(g_create_packet);
            std::size_t highest = 0;
            for (std::size_t id = 1; id < names.size(); ++id) {
                if (const auto packet = create(static_cast<MinecraftPacketIds>(id))) {
                    names[id] = packet->getName();
                    highest = id;
                }
            }
            names.resize(highest + 1);
        });
    }
    return names;
}

const cereal::ReflectionCtx *reflection_ctx()
{
    return g_reflection.load(std::memory_order_relaxed);
}

bool send_to_server(const int id, const std::string_view body, const int sub_client)
{
    if (g_hooks.send_internal == nullptr || id < 1 || id >= static_cast<int>(kPacketIdLimit) ||
        body.size() > (1U << 20U) || !create_packet(id)) {
        return false;
    }
    const std::lock_guard lock{g_inject_mutex};
    g_injected.push_back(frame_of(id, body, static_cast<std::uint32_t>(sub_client) << 10U));
    g_pending.store(g_injected.size() + g_injected_inbound.size() + g_injected_server.size(), std::memory_order_relaxed);
    return true;
}

bool send_to_client(const int id, const std::string_view body)
{
    if (g_hooks.packet_handlers == 0 || id < 1 || id >= static_cast<int>(kPacketIdLimit) ||
        body.size() > (1U << 20U) || !create_packet(id)) {
        return false;
    }
    const std::lock_guard lock{g_inject_mutex};
    g_injected_inbound.emplace_back(id, std::string{body});
    g_pending.store(g_injected.size() + g_injected_inbound.size() + g_injected_server.size(), std::memory_order_relaxed);
    return true;
}

bool send_to_player(const int id, const std::string_view body, const int player, const int sub_client)
{
    {
        const std::lock_guard lock{g_players_mutex};
        if (player >= static_cast<int>(g_players.size()) || (player < 0 && g_players.empty())) {
            return false;
        }
    }
    if (g_hooks.send_internal == nullptr || id < 1 || id >= static_cast<int>(kPacketIdLimit) ||
        body.size() > (1U << 20U) || !create_packet(id)) {
        return false;
    }
    const std::lock_guard lock{g_inject_mutex};
    g_injected_server.push_back({player, frame_of(id, body, static_cast<std::uint32_t>(sub_client) << 12U)});
    g_pending.store(g_injected.size() + g_injected_inbound.size() + g_injected_server.size(), std::memory_order_relaxed);
    return true;
}

std::shared_ptr<Packet> create_packet(const int id)
{
    if (g_create_packet == nullptr || id < 0) {
        return nullptr;
    }
    const auto create = reinterpret_cast<decltype(&MinecraftPackets::createPacket)>(g_create_packet);
    return create(static_cast<MinecraftPacketIds>(id));
}

Bedrock::Result<void> read_no_header(Packet &packet, ReadOnlyBinaryStream &stream, const cereal::ReflectionCtx &ctx,
                                     const SubClientId sub_id)
{
    auto *const original = original_of(packet, kPacketReadSlot);
    if (original == nullptr) {
        return {};
    }
    packet.setSenderSubId(sub_id);
    return SPYGLASS_CALL_ORIGINAL(kPacketRead, original, &packet, stream, ctx);
}

}  // namespace spyglass
