#include <gtest/gtest.h>

#include <string>

// Built twice: the consumer probe's library is linked AFTER the other shared
// protocol library, which therefore comes first in the ELF lookup scope.
extern "C" const char* PROBE_FUNCTION();

// Detector: both shared protocol libraries export the lp_* C ABI. Without
// version nodes the consumer bound to whichever library was loaded first.
TEST(ProtocolSymbolIsolationTest, TheConsumerBindsToTheLibraryItLinked)
{
    const std::string where = PROBE_FUNCTION();
    EXPECT_NE(where.find(EXPECTED_LIBRARY), std::string::npos) << where;
}
