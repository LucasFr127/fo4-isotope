// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <cstdlib>

int main()
{
    fo4hdr::tests::RunHdrCoreTests();
    fo4hdr::tests::RunRuntimeValidationTests();
    return fo4hdr::test::failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
