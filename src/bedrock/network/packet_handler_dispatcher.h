#pragma once

#include <memory>

#include "bedrock/network/network_identifier.h"

class NetEventCallback;
class Packet;

class IPacketHandlerDispatcher {
public:
    virtual ~IPacketHandlerDispatcher() = default;
    virtual void handle(const NetworkIdentifier &id, NetEventCallback &callback,
                        std::shared_ptr<Packet> &packet) const = 0;

    void handleDetour(const NetworkIdentifier &id, NetEventCallback &callback, std::shared_ptr<Packet> &packet) const;
};
