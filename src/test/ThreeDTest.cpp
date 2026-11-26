/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The 3D rendering target for the GL suite. 3DFileViewer is built by the desktop
// now; this app exercises the same client-side path: LibGL (EGLGPU or LibSoftGPU)
// renders an animated, lit triangle into an offscreen bitmap, LibGUI paints the
// bitmap, and LibWM presents it as a wl_shm buffer. The suite checks that frames
// keep arriving and that the window dump is non-trivial.

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

class ThreeDTestWidget final : public GUI::Widget {
    C_OBJECT(ThreeDTestWidget)

public:
    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Widget::paint_event(event);
        GUI::Painter painter(*this);
        painter.add_clip_rect(event.rect());
        painter.draw_scaled_bitmap(rect(), *m_bitmap, m_bitmap->rect());
    }

private:
    ThreeDTestWidget()
        : m_bitmap(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, { 200, 150 }).release_value_but_fixme_should_propagate_errors())
    {
        m_context = MUST(GL::create_context(*m_bitmap));
        start_timer(16);
        draw_frame();
    }

    virtual void timer_event(Core::TimerEvent&) override
    {
        draw_frame();
    }

    void draw_frame()
    {
        GL::make_context_current(m_context);
        glViewport(0, 0, m_bitmap->width(), m_bitmap->height());
        glClearColor(0.1f, 0.12f, 0.16f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glRotatef(static_cast<float>(m_frame) * 3.0f, 0.0f, 0.0f, 1.0f);

        glBegin(GL_TRIANGLES);
        glColor3f(0.9f, 0.2f, 0.2f);
        glVertex2f(0.0f, 0.8f);
        glColor3f(0.2f, 0.9f, 0.3f);
        glVertex2f(-0.7f, -0.6f);
        glColor3f(0.2f, 0.4f, 0.9f);
        glVertex2f(0.7f, -0.6f);
        glEnd();

        m_context->present();
        ++m_frame;
        if (m_frame == 1 || m_frame % 10 == 0)
            dbgln("3D-TEST: frame {}", m_frame);
        update();
    }

    RefPtr<Gfx::Bitmap> m_bitmap;
    OwnPtr<GL::GLContext> m_context;
    int m_frame { 0 };
};

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("LibWM 3D Test");
    window->set_rect(100, 100, 240, 180);
    (void)window->set_main_widget<ThreeDTestWidget>();
    window->show();

    auto quit_timer = Core::Timer::create_single_shot(2500, [&] { app->quit(0); });
    quit_timer->start();
    return app->exec();
}
