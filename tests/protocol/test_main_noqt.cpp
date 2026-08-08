// Entry point for the Qt-LOOP-FREE half of the suite.
//
// The whole point of this binary is the ONE line that is missing from
// test_main.cpp: it never constructs a QCoreApplication. Everything it links
// is the same code the main suite exercises; the only difference is that
// QCoreApplication::instance() is null for the whole run, which is the state a
// Qt-free host process is permanently in and which the main suite — where the
// very first thing main() does is construct the application — can never reach.
//
// It is a SEPARATE EXECUTABLE rather than a test case inside protocol_tests
// because "no Qt application exists" is a property of the process, not of a
// test: QCoreApplication is a process-wide singleton, and destroying the one
// the other 30 test files rely on partway through a run would be a far worse
// trade than one more binary.
#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
