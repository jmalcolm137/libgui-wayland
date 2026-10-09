/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>

namespace Gfx {
class Painter;
}

namespace LibWM {

// Owns the WindowServer's server-side menu model and draws menus.
//
// In SerenityOS the menubar and dropdown menus (including submenus) are drawn
// by the WindowServer, not the client: LibGUI only sends the menu model and, for
// context menus, a popup request. LibWM renders the menubar into the top of each
// window's surface and each open menu into its own xdg_popup; a submenu is an
// xdg_popup parented to the popup of its parent menu.
class MenuController {
public:
    struct Item {
        i32 identifier { -1 };
        i32 submenu_id { -1 };
        ByteString text;
        bool enabled { true };
        bool visible { true };
        bool checkable { false };
        bool checked { false };
        bool is_default { false };
        ByteString shortcut;
        bool exclusive { false };
        bool is_separator { false };
        Gfx::IntRect rect; // popup-local
    };

    struct Menu {
        i32 id { -1 };
        ByteString name;
        i32 minimum_width { 0 };
        Vector<Item> items;
        i32 window_id { -1 };
        Gfx::IntRect menubar_rect; // window-content-local
        bool is_open { false };
        Gfx::IntSize popup_size;
        int hovered { -1 };
        bool hovered_valid { false };
    };

    // --- model updates ---
    void create_menu(i32 menu_id, ByteString const& name, i32 minimum_width);
    void set_menu_name(i32 menu_id, ByteString const& name);
    void set_menu_minimum_width(i32 menu_id, i32 minimum_width);
    void destroy_menu(i32 menu_id);
    void add_menu(i32 window_id, i32 menu_id);
    void add_menu_item(i32 menu_id, i32 identifier, i32 submenu_id, ByteString const& text, bool enabled, bool visible, bool checkable, bool checked, bool is_default, ByteString const& shortcut, bool exclusive);
    void add_menu_separator(i32 menu_id);
    void update_menu_item(i32 menu_id, i32 identifier, i32 submenu_id, ByteString const& text, bool enabled, bool visible, bool checkable, bool checked, bool is_default, ByteString const& shortcut);
    void remove_menu_item(i32 menu_id, i32 identifier);

    // --- menubar ---
    bool window_has_menubar(i32 window_id) const;
    int menubar_height() const;
    void render_menubar(i32 window_id, Gfx::Painter&, Gfx::IntRect const&);

    // --- popup ---
    Gfx::IntSize popup_size(i32 menu_id) const;
    void render_popup(i32 menu_id, Gfx::Painter&) const;
    i32 menu_window(i32 menu_id) const;
    void open_root(i32 menu_id, i32 window_id, Gfx::IntRect anchor);
    void close_menu(i32 menu_id);

    // --- interaction (coordinates are surface-local) ---
    void on_menubar_motion(i32 window_id, Gfx::IntPoint position);
    void on_menubar_left(i32 window_id);
    void on_menubar_press(i32 window_id, Gfx::IntPoint position);
    void on_popup_motion(i32 menu_id, Gfx::IntPoint position);
    void on_popup_button(i32 menu_id, Gfx::IntPoint position, bool pressed);
    void on_popup_closed(i32 menu_id);

    // Keyboard navigation. Returns true if the key was consumed by a menu.
    bool handle_key(i32 window_id, u32 key_code, u32 code_point, u32 modifiers, bool is_press);

    // --- callbacks (wired by WindowServerConnection) ---
    Function<void(i32 menu_id, i32 window_id, i32 parent_popup_id, Gfx::IntRect anchor, Gfx::IntSize size, bool is_submenu)> show_popup;
    Function<void(i32 menu_id)> hide_popup;
    Function<void(i32 menu_id, u32 identifier)> item_activated;
    Function<void(i32 menu_id, u32 identifier)> item_entered;
    Function<void(i32 menu_id, u32 identifier)> item_left;
    Function<void(i32 menu_id, bool visible)> visibility_changed;
    Function<void(i32 window_id)> menubar_changed;
    Function<void(i32 menu_id)> redraw_popup;

private:
    Menu* find_menu(i32 menu_id);
    Menu const* find_menu(i32 menu_id) const;
    void layout_menubar(i32 window_id);
    void layout_popup(Menu&);
    int item_index_at(Menu const&, Gfx::IntPoint) const;
    i32 open_menu_id() const { return m_open_menus.is_empty() ? -1 : m_open_menus.last(); }
    void close_all_menus();
    void close_deeper_than(i32 menu_id);
    void open_submenu_for(Menu& parent, int index);
    void move_selection(Menu&, int delta);
    void select_index(Menu&, int index);
    void activate_selected(Menu&);
    void switch_menubar(i32 window_id, int direction);

    HashMap<i32, NonnullOwnPtr<Menu>> m_menus;
    Vector<i32> m_menubar_order;
    Vector<i32> m_open_menus; // root .. deepest
    i32 m_hovered_menubar_menu { -1 };
};

}
