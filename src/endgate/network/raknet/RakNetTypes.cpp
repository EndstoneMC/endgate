#include "RakNetTypes.h"

#include <cstring>

namespace RakNet {

SystemAddress::SystemAddress()
{
    std::memset(&address, 0, sizeof(address));
    address.addr4.sin_family = AF_INET;
    systemIndex = static_cast<SystemIndex>(-1);
    debugPort = 0;
}

}  // namespace RakNet
