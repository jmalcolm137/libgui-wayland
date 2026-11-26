/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Client-side GL through LibWM, in CI: renders a solid colour with LibGL (the
// EGLGPU backend when the suite forces LIBGL_GPU_DRIVER=eglgpu, LibSoftGPU
// otherwise), paints the resulting bitmap in a LibGUI window, and presents it as
// a wl_shm buffer. The suite reads the window dump: a correct colour means the
// whole path (GPU device, LibGL, LibGUI, LibWM) worked, not merely that the
// process stayed alive.

#include "LibWM.h"
#include <LibCore/Timer.h>
#include <LibGL/GL/gl.h>
#include <LibGL/GLContext.h>
#include <LibGUI/Application.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Bitmap.h>
#include <LibMain/Main.h>

namespace {

class GLTestWidget final : public GUI::Widget {
    C_OBJECT(GLTestWidget)

public:
    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Widget::paint_event(event);
        GUI::Painter painter(*this);
        painter.add_clip_rect(event.rect());
        painter.draw_scaled_bitmap(rect(), *m_bitmap, m_bitmap->rect());
    }

private:
    GLTestWidget()
        : m_bitmap(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, { 64, 64 }).release_value_but_fixme_should_propagate_errors())
    {
        m_context = MUST(GL::create_context(*m_bitmap));
        GL::make_context_current(m_context);

        // A colour with three distinct channels, so a channel swap is visible.
        glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        m_context->present();

        auto pixel = m_bitmap->get_pixel(32, 32);
        dbgln("GL-TEST: rendered {}x{} centre={},{},{}", m_bitmap->width(), m_bitmap->height(),
            pixel.red(), pixel.green(), pixel.blue());
    }

    RefPtr<Gfx::Bitmap> m_bitmap;
    OwnPtr<GL::GLContext> m_context;
};

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();

    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("LibWM GL Test");
    window->set_rect(100, 100, 240, 180);
    auto widget = window->set_main_widget<GLTestWidget>();
    widget->update();
    window->show();

    auto quit_timer = Core::Timer::create_single_shot(1500, [&] { app->quit(0); });
    quit_timer->start();
    return app->exec();
}
