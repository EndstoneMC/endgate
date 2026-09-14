#include "GetTime.h"

#include <chrono>

namespace RakNet {

TimeUS GetTimeUS()
{
    using namespace std::chrono;
    static const auto startup = steady_clock::now();
    return static_cast<TimeUS>(duration_cast<microseconds>(steady_clock::now() - startup).count());
}

TimeMS GetTimeMS()
{
    return static_cast<TimeMS>(GetTimeUS() / 1000);
}

Time GetTime()
{
    return static_cast<Time>(GetTimeUS() / 1000);
}

}  // namespace RakNet
