#include "RakNetTypes.h"

#include <netdb.h>

#include <cstdio>
#include <cstring>

#include <arpa/inet.h>

namespace RakNet {

SystemAddress::SystemAddress()
{
    std::memset(&address, 0, sizeof(address));
    address.addr4.sin_family = AF_INET;
    systemIndex = static_cast<SystemIndex>(-1);
    debugPort = 0;
}

SystemAddress::SystemAddress(const char *str)
{
    std::memset(&address, 0, sizeof(address));
    address.addr4.sin_family = AF_INET;
    systemIndex = static_cast<SystemIndex>(-1);
    FromString(str);
}

SystemAddress::SystemAddress(const char *str, unsigned short port)
{
    std::memset(&address, 0, sizeof(address));
    address.addr4.sin_family = AF_INET;
    systemIndex = static_cast<SystemIndex>(-1);
    FromStringExplicitPort(str, port);
}

unsigned char SystemAddress::GetIPVersion() const
{
    return address.addr4.sin_family == AF_INET ? 4 : 6;
}

unsigned short SystemAddress::GetPort() const
{
    return ntohs(address.addr4.sin_port);
}

unsigned short SystemAddress::GetPortNetworkOrder() const
{
    return address.addr4.sin_port;
}

void SystemAddress::SetPortHostOrder(unsigned short s)
{
    address.addr4.sin_port = htons(s);
    debugPort = s;
}

void SystemAddress::SetPortNetworkOrder(unsigned short s)
{
    address.addr4.sin_port = s;
    debugPort = ntohs(s);
}

void SystemAddress::CopyPort(const SystemAddress &right)
{
    SetPortNetworkOrder(right.address.addr4.sin_port);
}

bool SystemAddress::EqualsExcludingPort(const SystemAddress &right) const
{
    return (address.addr4.sin_family == AF_INET &&
            address.addr4.sin_addr.s_addr == right.address.addr4.sin_addr.s_addr) ||
           (address.addr4.sin_family == AF_INET6 &&
            std::memcmp(address.addr6.sin6_addr.s6_addr, right.address.addr6.sin6_addr.s6_addr,
                        sizeof(address.addr6.sin6_addr.s6_addr)) == 0);
}

SystemAddress &SystemAddress::operator=(const SystemAddress &input)
{
    std::memcpy(&address, &input.address, sizeof(address));
    systemIndex = input.systemIndex;
    debugPort = input.debugPort;
    return *this;
}

bool SystemAddress::operator==(const SystemAddress &right) const
{
    return address.addr4.sin_port == right.address.addr4.sin_port && EqualsExcludingPort(right);
}

bool SystemAddress::operator!=(const SystemAddress &right) const
{
    return !(*this == right);
}

/// Only ever used as a hash bucket index for the remote system lookup; it does not go on the wire.
/// Upstream reached for SuperFastHash here, which this port does not carry.
unsigned long SystemAddress::ToInteger(const SystemAddress &sa)
{
    unsigned long hash = 2166136261UL;
    const auto mix = [&hash](const void *data, std::size_t size) {
        const auto *bytes = static_cast<const unsigned char *>(data);
        for (std::size_t i = 0; i < size; i++) {
            hash = (hash ^ bytes[i]) * 16777619UL;
        }
    };
    mix(&sa.address.addr4.sin_port, sizeof(sa.address.addr4.sin_port));
    if (sa.address.addr4.sin_family == AF_INET) {
        mix(&sa.address.addr4.sin_addr.s_addr, sizeof(sa.address.addr4.sin_addr.s_addr));
    }
    else {
        mix(sa.address.addr6.sin6_addr.s6_addr, sizeof(sa.address.addr6.sin6_addr.s6_addr));
    }
    return hash;
}

void SystemAddress::ToString(bool writePort, char *dest, char portDelineator) const
{
    if (*this == UNASSIGNED_SYSTEM_ADDRESS) {
        std::strcpy(dest, "UNASSIGNED_SYSTEM_ADDRESS");
        return;
    }

    char portSeparator[2] = {portDelineator, 0};
    if (address.addr4.sin_family == AF_INET) {
        inet_ntop(AF_INET, &address.addr4.sin_addr, dest, INET6_ADDRSTRLEN);
    }
    else {
        inet_ntop(AF_INET6, &address.addr6.sin6_addr, dest, INET6_ADDRSTRLEN);
    }
    if (writePort) {
        std::strcat(dest, portSeparator);
        std::sprintf(dest + std::strlen(dest), "%u", GetPort());
    }
}

const char *SystemAddress::ToString(bool writePort, char portDelineator) const
{
    static thread_local char buffer[INET6_ADDRSTRLEN + 16];
    ToString(writePort, buffer, portDelineator);
    return buffer;
}

bool SystemAddress::FromString(const char *str, char portDelineator, int ipVersion)
{
    return FromStringExplicitPort(str, 0, ipVersion);
}

bool SystemAddress::FromStringExplicitPort(const char *str, unsigned short port, int ipVersion)
{
    if (str == nullptr) {
        *this = UNASSIGNED_SYSTEM_ADDRESS;
        return true;
    }

    addrinfo hints{};
    hints.ai_family = ipVersion == 6 ? AF_INET6 : (ipVersion == 4 ? AF_INET : AF_UNSPEC);
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo *result = nullptr;
    if (getaddrinfo(str, nullptr, &hints, &result) != 0 || result == nullptr) {
        return false;
    }

    std::memset(&address, 0, sizeof(address));
    std::memcpy(&address.sa_stor, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);

    SetPortHostOrder(port);
    return true;
}

void SystemAddress::FixForIPVersion(const SystemAddress &boundAddressToSocket)
{
    (void)boundAddressToSocket;
}

bool SystemAddress::IsLoopback() const
{
    if (address.addr4.sin_family == AF_INET) {
        return ntohl(address.addr4.sin_addr.s_addr) == INADDR_LOOPBACK;
    }
    return IN6_IS_ADDR_LOOPBACK(&address.addr6.sin6_addr);
}

void SystemAddress::SetToLoopback()
{
    SetToLoopback(GetIPVersion());
}

void SystemAddress::SetToLoopback(unsigned char ipVersion)
{
    const auto port = GetPort();
    std::memset(&address, 0, sizeof(address));
    if (ipVersion == 6) {
        address.addr6.sin6_family = AF_INET6;
        address.addr6.sin6_addr = in6addr_loopback;
    }
    else {
        address.addr4.sin_family = AF_INET;
        address.addr4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    SetPortHostOrder(port);
}

RakNetGUID::RakNetGUID()
{
    g = (uint64_t)-1;
    systemIndex = (SystemIndex)-1;
}

bool RakNetGUID::operator==(const RakNetGUID &right) const
{
    return g == right.g;
}

bool RakNetGUID::operator!=(const RakNetGUID &right) const
{
    return g != right.g;
}

void RakNetGUID::ToString(char *dest) const
{
    if (*this == UNASSIGNED_RAKNET_GUID) {
        std::strcpy(dest, "UNASSIGNED_RAKNET_GUID");
        return;
    }
    std::sprintf(dest, "%llu", (unsigned long long)g);
}

const char *RakNetGUID::ToString() const
{
    static thread_local char buffer[32];
    ToString(buffer);
    return buffer;
}

}  // namespace RakNet
