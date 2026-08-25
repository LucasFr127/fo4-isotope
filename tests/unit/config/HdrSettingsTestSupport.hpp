// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

namespace fo4hdr::tests {

inline constexpr std::string_view kOriginal{
    "\xEF\xBB\xBF"
    "; retained heading\r\n[Output]\r\nDisplayPeakNits=1000 ; peak comment\r\n"
    "UiPaperWhiteNits=203\r\nVideoWhiteNits=203\r\nUnknownOutput=kept\r\n"
    "[ToneMapper]\r\nAlgorithm=AgX # operator comment\r\n"
    "[Other]\r\nUntouched = yes\r\n"};

}  // namespace fo4hdr::tests
