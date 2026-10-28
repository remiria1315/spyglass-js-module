#pragma once

#include <string_view>

#include "bedrock/version.h"

// Cut against Minecraft.Windows.exe 1.26.40.5, 1.26.44.3, 1.26.50.27,
// 1.26.51.1, 1.26.60.5, 1.26.60.21, 1.26.60.30 and 1.26.70.21, and against the x86_64
// libminecraftpe.so of the 1.26.44.3 Android release. None of them name these
// functions at runtime. The Windows client carries RTTI for webrtc only, and
// the Android .dynsym drops every game symbol, so both are located the same
// way: from the strings that survive the strip, and from the exception table
// function index that turns an address back into the function holding it. That
// index is .pdata on Windows and .eh_frame_hdr on Android. Follow the recipes
// again after a client update. Do not widen a pattern to make it match.
//
// The release and preview clients are separate builds, and a pattern cut
// against one must never be scanned against the other, so a build targets one
// of them and compiles in that set alone. MINECRAFT_VERSION and
// MINECRAFT_PREVIEW pick it, and verify_client() checks the running client
// against what was picked before any pattern is scanned.
//
// MinecraftPackets::createPacket hangs off the string a failed read reports
// with.
//
// Both directions are hooked on the packet itself rather than by pattern: every
// packet class carries its own read and write, so the walk creates one packet
// per id with createPacket and swaps the two vtable slots it finds. 1.26.60.21
// inlined readNoHeader into _sortAndPacketizeEvents and left no out-of-line
// body to hook, which is what that walk replaces. createPacket is therefore the
// single thing capture depends on, in both directions.
//
// 1. find the string "readNoHeader failed! packetId: {}". it is unique on both
// platforms, in
//    .rdata on Windows and .rodata on Android
// 2. it has one lea xref, inside the outlined addFrameToError that a failed
// read builds its error
//    frame with. that helper is about 0x1d0 bytes on both platforms and does
//    nothing else
// 3. the helper has exactly one caller, the NetworkSystem::runEvents packet
// lambda. it is 0x251f
//    bytes on 1.26.40.5, 0x2080 on 1.26.44.3 and 0x1ef3 on 1.26.50.27, and it
//    takes each packet off the peer, creates it and reads it. it is 0x2931
//    bytes on 1.26.60.5, 0x28bc on 1.26.60.21, and 0x280f on 1.26.60.30 and
//    1.26.70.21, which inline the read
// 4. createPacket is the callee whose head bounds-checks the packet id and
// dispatches through a
//    rip-relative jump table: cmp <id32>, <max> / ja / lea <r>, [rip+table] /
//    movsxd rax,
//    [<r>+rax*4] / add rax, <r> / jmp rax. <max> is the highest
//    MinecraftPacketIds value and moves with the protocol: 0x15f on 1.26.40.5
//    and 1.26.44.3, 0x161 on 1.26.50.27, 0x165 on 1.26.60.21 and 0x169 on
//    1.26.60.30. nothing reads it any more, so every pattern wildcards that
//    byte. 1.26.50.27 and 1.26.60.21 open with a 0x30 frame and a near ja,
//    1.26.60.30 keeps the frame but takes a short ja, and the release shape is
//    a 0x20 frame and a short ja
// 5. confirm createPacket on its cases. each one is a three-instruction tail
// that calls a single
//    make_packet helper and rejoins the epilogue
//
// NetworkSystem::_sendInternal is where a serialized packet is handed to the
// connection's peer. It is found from NetworkSystem::send, which serializes the
// packet through slot 5 of its vtable and then calls it with the identifier, the
// packet and the send buffer. Cut against the Windows 1.26.50 release client,
// where send is 0x3f0 bytes and its last call is _sendInternal.
//
// MouseDevice::feed is where every mouse channel meets, so the overlay stops
// the game's mouse there rather than at one transport. Windows only: the
// launcher owns input on Android.
//
// 1. the window procedure's own mouse handlers are dead whenever the GameInput
// runtime is up. each
//    opens by testing the handler pointer at Platform_GameCore+0xa0 and does
//    nothing when it is set, so the live mouse arrives from a per-frame poll
//    rather than from a message
// 2. that poll holds seven of feed's fourteen call sites. it takes an absolute
// screen position,
//    puts it through AppPlatform::screenToClient and feeds it as a motion
//    action
// 3. feed is the seven-argument overload. the four-argument one takes no dx, dy
// or
//    forceMotionlessPointer, so only this one reads four arguments back off the
//    stack
// 4. the prologue pushes eight registers, spills the four register arguments,
// then reads those
//    stack arguments at rsp+0xb0, 0xb8, 0xc0 and 0xc8. that run is unique with
//    no wildcard on 1.26.40.5, 1.26.44.3, 1.26.50.27, 1.26.60.21 and
//    1.26.60.30

namespace spyglass {

struct Signatures {
  std::string_view name;
  std::string_view create_packet;
  std::string_view send;
  std::string_view send_internal_call;
};

const Signatures &signatures();

void verify_client();

#ifdef _WIN32

#if MINECRAFT_PREVIEW

#if MINECRAFT_VERSION_HEX >= MINECRAFT_VERSION(1, 26, 60, 30)

// 1.26.60.30
constexpr Signatures kClient{
    .name = MINECRAFT_CLIENT,
    .create_packet = "56 48 83 EC 30 48 89 CE 81 FA ? 01 00 00 77 ? 89 D0 48 "
                     "8D 0D ? ? ? ? 48 63 04 81 48 01 C8 FF E0 "
                     "0F 57 C0 0F 11 06 48 89 F0 48 83 C4 30 5E",
};

#else

// 1.26.50.27, 1.26.60.21
constexpr Signatures kClient{
    .name = MINECRAFT_CLIENT,
    .create_packet = "56 48 83 EC 30 48 89 CE 81 FA ? 01 00 00 0F 87 ? ? ? ? "
                     "89 D0 48 8D 0D ? ? ? ? 48 63 04 81 48 01 "
                     "C8 FF E0 0F 57 C0 0F 11 06 48 89 F0 48 83 C4 30 5E",
};

#endif

#else

#if MINECRAFT_VERSION_HEX < MINECRAFT_VERSION(1, 26, 60, 0)

// 1.26.40.5, 1.26.44.3, 1.26.51.1
constexpr Signatures kClient{
    .name = MINECRAFT_CLIENT,
    .create_packet = "56 48 83 EC 20 48 89 CE 81 FA ? 01 00 00 77 ? 89 D0 48 "
    .send = "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 08 01 00 00 48 8D AC "
            "24 80 00 00 00 48 C7 85 80 00 00 00 FE FF FF FF 45 89 CD 4C 89 C6 "
            "48 89 D7 48 89 CB 48 8D 4D B0 41 B8 B0 00 00 00 E8 ? ? ? ? 48 8B 0D "
            "? ? ? ? 48 8B 01 48 8B 40 08 BA B8 00 00 00 FF 15 ? ? ? ?",
    .send_internal_call = "48 89 D9 48 89 FA 49 89 F0 E8",
};

#else
#error                                                                         \
    "no Windows release pattern set for this client; the 1.26.60 line is preview only so far"
#endif

#endif

constexpr std::string_view kMouseFeed =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 48 44 89 CF 44 89 C3 89 D5 "
    "48 89 CE 44 0F B7 A4 24 C0 "
    "00 00 00 44 0F B7 AC 24 B8 00 00 00 44 0F B7 BC 24 B0 00 00 00 0F B6 84 "
    "24 C8 00 00 00";

#else

#if MINECRAFT_PREVIEW ||                                                       \
    MINECRAFT_VERSION_HEX >= MINECRAFT_VERSION(1, 26, 50, 0)
#error "the Android set is cut against the 1.26.4x release client only"
#endif

// 1.26.44.3, and the rest of 1.26.4x
constexpr Signatures kClient{
    .name = MINECRAFT_CLIENT,
    .create_packet = "55 48 89 E5 53 50 48 89 FB 81 FE 5F 01 00 00 77 12 89 F0 "
                     "48 8D 0D ? ? ? ? 48 63 04 81 48 01 C8 "
                     "FF E0 0F 57 C0 0F 11 03",
};

#endif

} // namespace spyglass
