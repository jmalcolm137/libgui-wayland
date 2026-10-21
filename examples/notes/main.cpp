/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGUI/AboutDialog.h>
#include <LibGUI/Action.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/Icon.h>
#include <LibGUI/ItemListModel.h>
#include <LibGUI/Label.h>
#include <LibGUI/ListView.h>
#include <LibGUI/Menu.h>
#include <LibGUI/Model.h>
#include <LibGUI/Statusbar.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/Toolbar.h>
#include <LibGUI/ToolbarContainer.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Bitmap.h>
#include <LibMain/Main.h>

namespace {

// A model is the data behind a view. A view never owns the data; it holds a
// RefPtr to the model and calls into it. Changes are announced with did_update().
class NotesModel final : public GUI::Model {
public:
    static NonnullRefPtr<NotesModel> create(Vector<ByteString> notes)
    {
        return adopt_ref(*new NotesModel(move(notes)));
    }

    virtual int row_count(GUI::ModelIndex const& = {}) const override { return m_notes.size(); }
    virtual int column_count(GUI::ModelIndex const&) const override { return 1; }

    virtual GUI::Variant data(GUI::ModelIndex const& index, GUI::ModelRole role) const override
    {
        if (role == GUI::ModelRole::Display)
            return m_notes[index.row()];
        return {};
    }

    void add_note(ByteString text)
    {
        m_notes.append(move(text));
        did_update();
    }

    bool remove_note(int row)
    {
        if (row < 0 || row >= static_cast<int>(m_notes.size()))
            return false;
        m_notes.remove(row);
        did_update();
        return true;
    }

    ByteString const& note_at(int row) const { return m_notes[row]; }

private:
    explicit NotesModel(Vector<ByteString> notes)
        : m_notes(move(notes))
    {
    }

    Vector<ByteString> m_notes;
};

// Runtime-loaded GML. In a larger application this string normally lives in a
// .gml file and is compiled into C++ by GMLCompiler at build time.
static constexpr char notes_gml[] = R"(
@GUI::Widget {
    fill_with_background_color: true
    layout: @GUI::VerticalBoxLayout {
        margins: [8]
        spacing: 6
    }

    @GUI::ToolbarContainer {
        fixed_height: 32
        layout: @GUI::HorizontalBoxLayout {
            margins: [0]
            spacing: 0
        }

        @GUI::Toolbar {
            name: "toolbar"
        }
    }

    @GUI::Widget {
        fixed_height: 28
        layout: @GUI::HorizontalBoxLayout {
            spacing: 6
        }

        @GUI::Label {
            text: "Note:"
            fixed_width: 42
        }

        @GUI::TextBox {
            name: "note_input"
            placeholder: "Write a note and press Add..."
        }

        @GUI::Button {
            name: "add_button"
            text: "Add"
            fixed_width: 64
        }
    }

    @GUI::ListView {
        name: "notes_list"
        hover_highlighting: true
    }

    @GUI::Statusbar {
        name: "statusbar"
        fixed_height: 24
    }
}
)";

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    auto app_icon = TRY(GUI::Icon::try_create_default_icon("app-text-editor"sv));

    auto window = GUI::Window::construct();
    window->set_title(ByteString { "Notes" });
    window->set_icon(app_icon.bitmap_for_size(16));
    window->resize(560, 440);

    auto main_widget = window->set_main_widget<GUI::Widget>();
    main_widget->set_min_size(420, 320);
    TRY(main_widget->load_from_gml(StringView { notes_gml, sizeof(notes_gml) - 1 }));

    auto& toolbar = *main_widget->find_descendant_of_type_named<GUI::Toolbar>("toolbar");
    auto& input = *main_widget->find_descendant_of_type_named<GUI::TextBox>("note_input");
    auto& add_button = *main_widget->find_descendant_of_type_named<GUI::Button>("add_button");
    auto& list = *main_widget->find_descendant_of_type_named<GUI::ListView>("notes_list");
    auto& statusbar = *main_widget->find_descendant_of_type_named<GUI::Statusbar>("statusbar");

    Vector<ByteString> initial_notes;
    initial_notes.append(ByteString { "Read the LibGUI guide" });
    initial_notes.append(ByteString { "Build a small application" });
    initial_notes.append(ByteString { "Run it on every compositor you can find" });
    auto model = NotesModel::create(move(initial_notes));
    list.set_model(model);

    auto update_status = [&] {
        auto count = model->row_count();
        statusbar.set_text(MUST(String::formatted("{} note{}", count, count == 1 ? "" : "s")));
    };
    update_status();

    auto add_note = [&] {
        auto text = input.text().trim_whitespace();
        if (text.is_empty())
            return;
        model->add_note(move(text));
        input.clear();
        update_status();
        auto index = model->index(model->row_count() - 1);
        list.selection().set(index);
        list.scroll_into_view(index, false, true);
    };

    add_button.on_click = [&](unsigned) {
        add_note();
    };
    input.on_return_pressed = [&] {
        add_note();
    };

    auto new_action = GUI::Action::create(
        ByteString { "&New Note" },
        { Mod_Ctrl, Key_N },
        TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/new.png"sv)),
        [&](auto&) {
            input.clear();
            input.set_focus(true);
        });
    new_action->set_status_tip("Prepare a new note"_string);

    auto delete_action = GUI::Action::create(
        ByteString { "&Delete Note" },
        { Mod_None, Key_Delete },
        TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/delete.png"sv)),
        [&](auto&) {
            auto selected = list.selection().first();
            if (selected.is_valid())
                model->remove_note(selected.row());
            update_status();
        });
    delete_action->set_status_tip("Delete the selected note"_string);

    toolbar.add_action(*new_action);
    toolbar.add_action(*delete_action);

    auto file_menu = window->add_menu("&File"_string);
    file_menu->add_action(*new_action);
    file_menu->add_action(*delete_action);
    file_menu->add_separator();
    file_menu->add_action(GUI::CommonActions::make_quit_action([&](auto&) {
        app->quit(0);
    }));

    auto help_menu = window->add_menu("&Help"_string);
    help_menu->add_action(GUI::CommonActions::make_about_action("Notes"_string, app_icon, window));

    window->show();
    input.set_focus(true);
    return app->exec();
}
