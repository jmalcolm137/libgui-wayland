/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/Notification.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibMain/Main.h>

// Shows notifications through LibGUI::Notification, so the whole path -
// NotificationServer spawn, the notify portal, and the banner window type - can
// be exercised. See docs/09-r3-implementation-plan.md §3 (the reuse spike).
//
// This mirrors the unmodified SerenityOS `notify` utility: a GUI::Application is
// created *first*, because GUI::Notification::show() makes a synchronous IPC call
// whose incoming messages are dispatched through the current Core::EventLoop. A
// bare Core::EventLoop or an event loop created after show() is not enough.
//
// The driver (serenity-desktop-environment/scripts/run-notification-test.sh)
// asserts on the markers below and on the compositor log.
ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();
    auto app = TRY(GUI::Application::create(arguments));

    // 1) A notification with an icon and a launch URL.
    auto icon = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 16, 16 }));
    icon->fill(Gfx::Color::from_rgb(0x3366cc));

    auto with_icon = GUI::Notification::construct();
    with_icon->set_title("SDE Notify"_string);
    with_icon->set_text("Notification with icon and launch URL"_string);
    with_icon->set_icon(icon.ptr());
    with_icon->set_launch_url(URL::URL { "https://example.com/"sv });
    with_icon->show();
    dbgln("NOTIFY-TEST: shown 1 icon=yes launch=yes is_showing={}", with_icon->is_showing());

    // 2) An iconless notification (the default Gfx::ShareableBitmap path).
    auto iconless = GUI::Notification::construct();
    iconless->set_title("SDE Notify 2"_string);
    iconless->set_text("Iconless notification"_string);
    iconless->show();
    dbgln("NOTIFY-TEST: shown 2 icon=no is_showing={}", iconless->is_showing());

    // Update the first one in place (update_notification_text/launch_url).
    with_icon->set_text("Updated text"_string);
    with_icon->update();
    dbgln("NOTIFY-TEST: updated 1 is_showing={}", with_icon->is_showing());

    auto quit = Core::Timer::create_single_shot(1000, [&] { app->quit(0); });
    quit->start();
    return app->exec();
}
