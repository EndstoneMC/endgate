#pragma once

#include <gtest/gtest.h>

#include "endgate/brigadier/exceptions.h"

namespace endgate::brigadier::support {

template <typename F>
void expectSyntaxError(F &&function, const CommandExceptionType &type, int cursor)
{
    try {
        function();
    }
    catch (const CommandSyntaxException &ex) {
        EXPECT_EQ(&ex.getType(), &type);
        EXPECT_EQ(ex.getCursor(), cursor);
        return;
    }
    ADD_FAILURE() << "expected a CommandSyntaxException";
}

}  // namespace endgate::brigadier::support
