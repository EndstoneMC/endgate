#include "GetTime.h"

#include <chrono>

namespace RakNet {

TimeMS GetTimeMS()
{
    using namespace std::chrono;
    return static_cast<TimeMS>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

Time GetTime()
{
    return static_cast<Time>(GetTimeMS());
}

TimeUS GetTimeUS()
{
    using namespace std::chrono;
    return static_cast<TimeUS>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace RakNet
