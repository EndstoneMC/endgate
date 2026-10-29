#include "endgate/thread.h"

#include <utility>

#ifndef _WIN32
#include <pthread.h>
#endif

namespace endgate {

namespace {
thread_local std::string CurrentName;
}  // namespace

const std::string &Thread::getName()
{
    return CurrentName;
}

void Thread::setName(std::string name)
{
#ifndef _WIN32
    constexpr std::size_t os_name_limit = 15;
    pthread_setname_np(pthread_self(), name.substr(0, os_name_limit).c_str());
#endif
    CurrentName = std::move(name);
}

}  // namespace endgate
