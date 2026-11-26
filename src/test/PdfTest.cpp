/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// PDF rendering through LibWM, without the PDFViewer application (which the
// desktop builds and tests now). The app opens a PDF through the in-process
// FileSystemAccess portal, renders a page with LibPDF, and reacts to Ctrl+=,
// the wheel and a compositor resize. The suite reads the traces and the frame.

#include "LibWM.h"
#include <LibCore/ArgsParser.h>
#include <LibCore/Timer.h>
#include <LibFileSystemAccessClient/Client.h>
#include <LibGUI/Application.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Bitmap.h>
#include <LibMain/Main.h>
#include <LibPDF/Document.h>
#include <LibPDF/Renderer.h>

namespace {

class PdfTestWidget final : public GUI::Widget {
    C_OBJECT(PdfTestWidget)

public:
    ErrorOr<void> open(StringView path)
    {
        auto response = FileSystemAccessClient::Client::the().request_file_read_only_approved(window(), ByteString { path });
        if (response.is_error())
            return response.release_error();
        auto file = response.release_value();
        m_buffer = TRY(file.release_stream()->read_until_eof());

        auto maybe_document = PDF::Document::create(m_buffer);
        if (maybe_document.is_error()) {
            dbgln("PDF-TEST: open error: {}", maybe_document.error().message());
            return Error::from_string_literal("could not open the PDF");
        }
        m_document = maybe_document.release_value();
        auto maybe_initialized = m_document->initialize();
        if (maybe_initialized.is_error()) {
            dbgln("PDF-TEST: initialize error: {}", maybe_initialized.error().message());
            return Error::from_string_literal("could not initialize the PDF");
        }
        dbgln("PDF-TEST: opened '{}' ({} page(s))", file.filename(), m_document->get_page_count());

        render();
        return {};
    }

private:
    void render()
    {
        if (!m_document)
            return;
        auto maybe_page = m_document->get_page(0);
        if (maybe_page.is_error()) {
            dbgln("PDF-TEST: page error: {}", maybe_page.error().message());
            return;
        }
        auto page = maybe_page.release_value();

        int const width = max(1, static_cast<int>(page.media_box.width() * m_zoom));
        int const height = max(1, static_cast<int>(page.media_box.height() * m_zoom));
        auto maybe_bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { width, height });
        if (maybe_bitmap.is_error()) {
            dbgln("PDF-TEST: bitmap error: {}", maybe_bitmap.error());
            return;
        }
        m_bitmap = maybe_bitmap.release_value();

        PDF::RenderingPreferences preferences;
        auto result = PDF::Renderer::render(*m_document, page, m_bitmap, Gfx::Color::White, preferences);
        if (result.is_error()) {
            dbgln("PDF-TEST: render failed ({} error(s))", result.error().errors().size());
            return;
        }
        dbgln("PDF-TEST: rendered {}x{} at {:.2f}x", m_bitmap->width(), m_bitmap->height(), m_zoom);
        update();
    }

    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Widget::paint_event(event);
        GUI::Painter painter(*this);
        painter.fill_rect(event.rect(), Gfx::Color::from_rgb(0x808080));
        if (m_bitmap) {
            Gfx::IntPoint origin { max(0, (width() - m_bitmap->width()) / 2), -m_scroll_y };
            painter.blit(origin, *m_bitmap, m_bitmap->rect());
        }
    }

    virtual void resize_event(GUI::ResizeEvent& event) override
    {
        dbgln("PDF-TEST: resized {}x{}", event.size().width(), event.size().height());
        update();
    }

    virtual void keydown_event(GUI::KeyEvent& event) override
    {
        if (event.ctrl() && (event.key() == Key_Equal || event.key() == Key_Plus)) {
            m_zoom = min(3.0f, m_zoom * 1.2f);
            dbgln("PDF-TEST: zoom {:.2f}", m_zoom);
            render();
            event.accept();
        }
    }

    virtual void mousewheel_event(GUI::MouseEvent& event) override
    {
        m_scroll_y = max(0, m_scroll_y + event.wheel_delta_y() * 20);
        dbgln("PDF-TEST: scrolled to {} ({})", m_scroll_y, event.wheel_delta_y());
        update();
        event.accept();
    }

    ByteBuffer m_buffer;
    RefPtr<PDF::Document> m_document;
    RefPtr<Gfx::Bitmap> m_bitmap;
    float m_zoom { 1.0f };
    int m_scroll_y { 0 };
};

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();

    StringView path;
    Core::ArgsParser args_parser;
    args_parser.add_positional_argument(path, "PDF file to open", "path");
    args_parser.parse(arguments);

    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("LibWM PDF Test");
    window->set_rect(100, 100, 640, 400);
    auto widget = window->set_main_widget<PdfTestWidget>();
    widget->set_fill_with_background_color(true);
    window->show();

    TRY(widget->open(path));

    auto quit_timer = Core::Timer::create_single_shot(5500, [&] { app->quit(0); });
    quit_timer->start();
    return app->exec();
}
