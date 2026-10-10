/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <AK/String.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGUI/Notification.h>
#include <LibMain/Main.h>

// Shows a notification through LibGUI::Notification, so the whole path -
// NotificationServer spawn, the notify portal, and the banner window type - can
// be exercised. See docs/09-r3-implementation-plan.md §3.
ErrorOr<int> serenity_main(Main::Arguments)
{
    LibWM::initialize();

    // An iconless notification currently trips the ShareableBitmap encoder, so
    // give it a (trivial) icon.
    auto icon = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 16, 16 }));
    icon->fill(Gfx::Color::from_rgb(0x3366cc));

    auto notification = GUI::Notification::construct();
    notification->set_title("SDE"_string);
    notification->set_text("Spike 2: NotificationServer reuse"_string);
    notification->set_icon(icon.ptr());
    notification->show();
    dbgln("NOTIFY-TEST: notification shown");

    Core::EventLoop loop;
    auto quit = Core::Timer::create_single_shot(500, [&] { loop.quit(0); });
    quit->start();
    return loop.exec();
}
