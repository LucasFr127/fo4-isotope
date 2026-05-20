// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/output/Hdr10Color.hpp"
#include "TestHarness.hpp"

#include <array>
#include <cstdlib>
#include <limits>

namespace {

void TestSt2084ReferenceVectors()
{
    struct Reference
    {
        double nits;
        double encoded;
    };
    constexpr std::array<Reference, 12> references{{
        {0.0, 7.309559025783966e-7},
        {0.001, 0.006302377054571391},
        {0.01, 0.02148621379868528},
        {0.1, 0.06233686566269587},
        {1.0, 0.14994573210018022},
        {80.0, 0.4858567653886785},
        {100.0, 0.508078421517399},
        {203.0, 0.5806888810416109},
        {400.0, 0.6525785975630672},
        {1000.0, 0.751827096247041},
        {2000.0, 0.8274246448588193},
        {10000.0, 1.0}
    }};

    double previous = -1.0;
    for (const auto& reference : references)
    {
        const auto encoded = fo4hdr::EncodeSt2084Pq(reference.nits);
        FO4HDR_CHECK(fo4hdr::test::Approx(encoded, reference.encoded, 1.0e-12));
        FO4HDR_CHECK(encoded >= previous);
        FO4HDR_CHECK(encoded >= 0.0 && encoded <= 1.0);
        previous = encoded;
    }
}

void TestSt2084InputBounds()
{
    using fo4hdr::EncodeSt2084Pq;
    FO4HDR_CHECK(EncodeSt2084Pq(-1.0) == EncodeSt2084Pq(0.0));
    FO4HDR_CHECK(EncodeSt2084Pq(20000.0) == 1.0);
    FO4HDR_CHECK(EncodeSt2084Pq(std::numeric_limits<double>::infinity()) == 1.0);
    FO4HDR_CHECK(EncodeSt2084Pq(-std::numeric_limits<double>::infinity()) ==
        EncodeSt2084Pq(0.0));
    FO4HDR_CHECK(EncodeSt2084Pq(std::numeric_limits<double>::quiet_NaN()) ==
        EncodeSt2084Pq(0.0));
}

}  // namespace

int main()
{
    TestSt2084ReferenceVectors();
    TestSt2084InputBounds();
    return fo4hdr::test::failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
