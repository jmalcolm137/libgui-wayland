/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <LibCore/EventLoop.h>
#include <LibDesktop/Launcher.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibImageDecoderClient/Client.h>
#include <LibMain/Main.h>
#include <LibURL/URL.h>

// Exercises the two session-service portals, launch and image, so the *same*
// binary can be run with and without an external session service:
//
//   * with no service, libgui-wayland's in-process fallback answers;
//   * with a service listening on the portal path, LibWM defers to it.
//
// It prints one marker per probe. The driver (scripts/run-services-test.sh)
// asserts on them, and on whether LibWM deferred. See docs/06-service-layering.md.

static void probe_launch_handlers(StringView label, URL::URL const& url)
{
    auto handlers = Desktop::Launcher::get_handlers_with_details_for_url(url);
    dbgln("LAUNCH-PROBE: {} handlers={}", label, handlers.size());
    for (auto const& handler : handlers)
        dbgln("LAUNCH-PROBE:   {} => '{}'", label, handler->executable);
}

ErrorOr<int> serenity_main(Main::Arguments)
{
    LibWM::initialize();
    Core::EventLoop event_loop;

    // The launch portal: handler resolution goes through the LaunchServer
    // (libgui-wayland's in-process fallback, or the SDE session service).
    probe_launch_handlers("png"sv, URL::create_with_file_scheme("/tmp/sde-probe.png"sv));
    probe_launch_handlers("txt"sv, URL::create_with_file_scheme("/tmp/sde-probe.txt"sv));

    // The image portal: decode a small image we generate here.
    auto bitmap = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 4, 3 }));
    bitmap->fill(Gfx::Color::from_rgb(0x3366cc));
    auto encoded = TRY(Gfx::PNGWriter::encode(*bitmap));

    auto client = TRY(ImageDecoderClient::Client::try_create());
    auto decoded = TRY(client->decode_image(encoded.bytes(), {}, {}, {}, {})->await());
    dbgln("IMAGE-PROBE: decoded {}x{} frames={}",
        decoded.frames[0].bitmap->width(),
        decoded.frames[0].bitmap->height(),
        decoded.frames.size());

    dbgln("SERVICE-LAYERING-TEST: done");
    return 0;
}
