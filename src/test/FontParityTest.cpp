/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <AK/String.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibMain/Main.h>
#include <stdlib.h>

// Confirms the WS7 font-parity decision (serenity-desktop-environment
// docs/10-font-parity-decision.md): the default UI family (Katica) ships only as
// bitmap faces at 10pt/12pt, so Widget::set_font_size() - which looks the size up
// with Font::AllowInexactSizeMatch::No - resolves to nothing, and size-based zoom
// is a no-op. That matches upstream SerenityOS, which also defaults to Katica.
//
// A scalable face (SerenitySans-Regular.ttf) resolves at arbitrary sizes; that is
// the R4/G5 path to working zoom.
ErrorOr<int> serenity_main(Main::Arguments)
{
    auto const* root = getenv("SERENITY_RES_ROOT");
    if (!root || !*root)
        root = getenv("LIBWM_RES");
    if (!root || !*root) {
        warnln("FONT-PARITY: set SERENITY_RES_ROOT to the SerenityOS resource root");
        return 1;
    }
    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_byte_string(root))));

    auto get = [](StringView family, float size) {
        return Gfx::FontDatabase::the().get(MUST(FlyString::from_utf8(family)), size, 400, Gfx::FontWidth::Normal, 0, Gfx::Font::AllowInexactSizeMatch::No);
    };

    bool const katica_10 = get("Katica"sv, 10) != nullptr;
    bool const katica_14 = get("Katica"sv, 14) != nullptr;
    bool const sans_14 = get("SerenitySans"sv, 14) != nullptr;

    outln("FONT-PARITY: Katica 10 resolved={}", katica_10);
    outln("FONT-PARITY: Katica 14 resolved={} (expected false: no such bitmap face)", katica_14);
    outln("FONT-PARITY: SerenitySans 14 resolved={} (expected true: scalable)", sans_14);

    bool const confirmed = katica_10 && !katica_14 && sans_14;
    outln("FONT-PARITY: zoom-no-op confirmed={}", confirmed);
    return confirmed ? 0 : 1;
}
