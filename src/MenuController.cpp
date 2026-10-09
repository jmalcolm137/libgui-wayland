/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "MenuController.h"
#include <AK/CharacterTypes.h>
#include <AK/NonnullOwnPtr.h>
#include <Kernel/API/KeyCode.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Palette.h>
#include <LibGfx/SystemTheme.h>

namespace LibWM {

static int item_height()
{
    auto& font = Gfx::FontDatabase::default_font();
    return max(static_cast<int>(font.preferred_line_height()), 16 + 2) + 4;
}

static Gfx::Palette system_palette()
{
    return Gfx::Palette { Gfx::PaletteImpl::create_with_anonymous_buffer(Gfx::current_system_theme_buffer()) };
}

MenuController::Menu* MenuController::find_menu(i32 menu_id)
{
    auto it = m_menus.find(menu_id);
    return it == m_menus.end() ? nullptr : it->value.ptr();
}

MenuController::Menu const* MenuController::find_menu(i32 menu_id) const
{
    auto it = m_menus.find(menu_id);
    return it == m_menus.end() ? nullptr : it->value.ptr();
}

void MenuController::create_menu(i32 menu_id, ByteString const& name, i32 minimum_width)
{
    auto menu = make<Menu>();
    menu->id = menu_id;
    menu->name = name;
    menu->minimum_width = minimum_width;
    m_menus.set(menu_id, move(menu));
}

void MenuController::set_menu_name(i32 menu_id, ByteString const& name)
{
    if (auto* menu = find_menu(menu_id)) {
        menu->name = name;
        if (menu->window_id != -1 && menubar_changed)
            menubar_changed(menu->window_id);
    }
}

void MenuController::set_menu_minimum_width(i32 menu_id, i32 minimum_width)
{
    if (auto* menu = find_menu(menu_id))
        menu->minimum_width = minimum_width;
}

void MenuController::destroy_menu(i32 menu_id)
{
    if (m_open_menu_id == menu_id)
        close_open_menu();
    m_menus.remove(menu_id);
    m_menubar_order.remove_first_matching([&](auto id) { return id == menu_id; });
}

void MenuController::add_menu(i32 window_id, i32 menu_id)
{
    if (auto* menu = find_menu(menu_id)) {
        menu->window_id = window_id;
        m_menubar_order.append(menu_id);
        if (menubar_changed)
            menubar_changed(window_id);
    }
}

void MenuController::add_menu_item(i32 menu_id, i32 identifier, i32 submenu_id, ByteString const& text, bool enabled, bool visible, bool checkable, bool checked, bool is_default, ByteString const& shortcut, bool exclusive)
{
    auto* menu = find_menu(menu_id);
    if (!menu)
        return;
    Item item;
    item.identifier = identifier;
    item.submenu_id = submenu_id;
    item.text = text;
    item.enabled = enabled;
    item.visible = visible;
    item.checkable = checkable;
    item.checked = checked;
    item.is_default = is_default;
    item.shortcut = shortcut;
    item.exclusive = exclusive;
    menu->items.append(move(item));
    if (menu->is_open)
        layout_popup(*menu);
    if (menu->window_id != -1 && menubar_changed)
        menubar_changed(menu->window_id);
}

void MenuController::add_menu_separator(i32 menu_id)
{
    auto* menu = find_menu(menu_id);
    if (!menu)
        return;
    Item item;
    item.is_separator = true;
    menu->items.append(move(item));
    if (menu->is_open)
        layout_popup(*menu);
    if (menu->window_id != -1 && menubar_changed)
        menubar_changed(menu->window_id);
}

void MenuController::update_menu_item(i32 menu_id, i32 identifier, i32 submenu_id, ByteString const& text, bool enabled, bool visible, bool checkable, bool checked, bool is_default, ByteString const& shortcut)
{
    auto* menu = find_menu(menu_id);
    if (!menu)
        return;
    for (auto& item : menu->items) {
        if (item.identifier != identifier)
            continue;
        item.submenu_id = submenu_id;
        item.text = text;
        item.enabled = enabled;
        item.visible = visible;
        item.checkable = checkable;
        item.checked = checked;
        item.is_default = is_default;
        item.shortcut = shortcut;
        break;
    }
}

void MenuController::remove_menu_item(i32 menu_id, i32 identifier)
{
    auto* menu = find_menu(menu_id);
    if (!menu)
        return;
    menu->items.remove_first_matching([&](auto const& item) { return item.identifier == identifier; });
    if (menu->window_id != -1 && menubar_changed)
        menubar_changed(menu->window_id);
}

bool MenuController::window_has_menubar(i32 window_id) const
{
    for (auto menu_id : m_menubar_order) {
        auto* menu = find_menu(menu_id);
        if (menu && menu->window_id == window_id && !menu->items.is_empty())
            return true;
    }
    return false;
}

int MenuController::menubar_height() const
{
    return max(20, Gfx::FontDatabase::default_font().pixel_size_rounded_up() + 6);
}

void MenuController::layout_menubar(i32 window_id)
{
    int x = 0;
    auto& font = Gfx::FontDatabase::default_font();
    for (auto menu_id : m_menubar_order) {
        auto* menu = find_menu(menu_id);
        if (!menu || menu->window_id != window_id)
            continue;
        int text_width = font.width(Gfx::parse_ampersand_string(menu->name));
        int width = text_width + 14;
        menu->menubar_rect = { x, 0, width, menubar_height() };
        x += width;
    }
}

void MenuController::render_menubar(i32 window_id, Gfx::Painter& painter, Gfx::IntRect const& rect)
{
    layout_menubar(window_id);
    auto palette = system_palette();
    auto& font = Gfx::FontDatabase::default_font();

    painter.fill_rect(rect, palette.menu_base());
    for (auto menu_id : m_menubar_order) {
        auto* menu = find_menu(menu_id);
        if (!menu || menu->window_id != window_id)
            continue;
        auto item_rect = menu->menubar_rect;
        bool active = (m_hovered_menubar_menu == menu_id) || (m_open_menu_id != -1 && menu->is_open);
        auto text_color = palette.menu_base_text();
        if (active) {
            painter.fill_rect(item_rect, palette.menu_selection());
            text_color = palette.menu_selection_text();
        }
        painter.draw_ui_text(item_rect, menu->name, font, Gfx::TextAlignment::Center, text_color);
    }
}

void MenuController::layout_popup(Menu& menu)
{
    auto& font = Gfx::FontDatabase::default_font();

    int const stripe_width = 24;
    int const frame = 2;
    int widest_text = 0;
    int widest_shortcut = 0;
    for (auto& item : menu.items) {
        if (item.is_separator)
            continue;
        widest_text = max(widest_text, font.width(Gfx::parse_ampersand_string(item.text)));
        if (!item.shortcut.is_empty())
            widest_shortcut = max(widest_shortcut, font.width(item.shortcut));
    }
    int widest_item = widest_text + stripe_width;
    if (widest_shortcut)
        widest_item += 50 + widest_shortcut;
    int width = max(menu.minimum_width, widest_item + 28 + frame * 2);

    int y = frame;
    for (auto& item : menu.items) {
        int height = item.is_separator ? 8 : item_height();
        item.rect = { frame, y, width - frame * 2, height };
        y += height;
    }
    menu.popup_size = { width, y + frame };
}

Gfx::IntSize MenuController::popup_size(i32 menu_id) const
{
    auto* menu = find_menu(menu_id);
    return menu ? menu->popup_size : Gfx::IntSize {};
}

i32 MenuController::menu_window(i32 menu_id) const
{
    auto* menu = find_menu(menu_id);
    return menu ? menu->window_id : -1;
}

void MenuController::open_popup(i32 menu_id, i32 window_id, Gfx::IntRect anchor)
{
    auto* menu = find_menu(menu_id);
    if (!menu)
        return;
    close_open_menu();
    menu->window_id = window_id;
    layout_popup(*menu);
    menu->is_open = true;
    menu->hovered_valid = false;
    m_open_menu_id = menu_id;
    if (show_popup)
        show_popup(menu_id, window_id, anchor, menu->popup_size);
    if (visibility_changed)
        visibility_changed(menu_id, true);
}

void MenuController::close_menu(i32 menu_id)
{
    if (m_open_menu_id != menu_id)
        return;
    if (auto* menu = find_menu(menu_id)) {
        menu->is_open = false;
        menu->hovered_valid = false;
    }
    m_open_menu_id = -1;
    if (hide_popup)
        hide_popup(menu_id);
    if (visibility_changed)
        visibility_changed(menu_id, false);
}

void MenuController::render_popup(i32 menu_id, Gfx::Painter& painter) const
{
    auto* menu = find_menu(menu_id);
    if (!menu)
        return;
    auto palette = system_palette();
    auto& font = Gfx::FontDatabase::default_font();

    Gfx::IntRect rect { {}, menu->popup_size };
    painter.draw_rect(rect, Gfx::Color::Black);
    painter.fill_rect(rect.shrunken(2, 2), palette.menu_base());
    Gfx::IntRect stripe { 2, 2, 24, rect.height() - 4 };
    painter.fill_rect(stripe, palette.menu_stripe());

    for (size_t i = 0; i < menu->items.size(); ++i) {
        auto const& item = menu->items[i];
        if (item.is_separator) {
            int y = item.rect.y() + item.rect.height() / 2;
            painter.draw_line({ item.rect.x() + 4, y }, { item.rect.right() - 4, y }, palette.threed_shadow1());
            continue;
        }
        bool hovered = menu->hovered_valid && menu->hovered == static_cast<int>(i);
        auto text_color = item.enabled ? palette.menu_base_text() : palette.color(Gfx::ColorRole::DisabledText);
        if (hovered && item.enabled) {
            painter.fill_rect(item.rect, palette.menu_selection());
            text_color = palette.menu_selection_text();
        }
        Gfx::IntRect text_rect = item.rect.translated(24 + 6, 0);

        if (item.checkable) {
            Gfx::IntRect check_rect { item.rect.x() + 6, 0, 12, 12 };
            check_rect.center_vertically_within(text_rect);
            if (item.checked)
                painter.draw_text(check_rect, item.exclusive ? "\xE2\x97\x8F"sv : "\xE2\x9C\x93"sv, Gfx::TextAlignment::Center, text_color);
        }

        painter.draw_ui_text(text_rect, item.text, font, Gfx::TextAlignment::CenterLeft, text_color);
        if (!item.shortcut.is_empty())
            painter.draw_text(item.rect.translated(-14, 0), item.shortcut, Gfx::TextAlignment::CenterRight, text_color);
        if (item.submenu_id != -1)
            painter.draw_text(item.rect.translated(-6, 0), "\xE2\x96\xB6"sv, Gfx::TextAlignment::CenterRight, text_color);
    }
}

int MenuController::item_index_at(Menu const& menu, Gfx::IntPoint position) const
{
    for (size_t i = 0; i < menu.items.size(); ++i) {
        if (menu.items[i].rect.contains(position))
            return static_cast<int>(i);
    }
    return -1;
}

void MenuController::close_open_menu()
{
    if (m_open_menu_id == -1)
        return;
    auto menu_id = m_open_menu_id;
    i32 window_id = -1;
    if (auto* menu = find_menu(menu_id)) {
        menu->is_open = false;
        menu->hovered_valid = false;
        window_id = menu->window_id;
    }
    m_open_menu_id = -1;
    m_hovered_menubar_menu = -1;
    if (hide_popup)
        hide_popup(menu_id);
    if (visibility_changed)
        visibility_changed(menu_id, false);
    if (window_id != -1 && menubar_changed)
        menubar_changed(window_id);
}

void MenuController::on_menubar_motion(i32 window_id, Gfx::IntPoint position)
{
    i32 hovered = -1;
    for (auto menu_id : m_menubar_order) {
        auto* menu = find_menu(menu_id);
        if (menu && menu->window_id == window_id && menu->menubar_rect.contains(position))
            hovered = menu_id;
    }
    if (hovered == m_hovered_menubar_menu)
        return;
    m_hovered_menubar_menu = hovered;
    if (menubar_changed)
        menubar_changed(window_id);

    // With a menu already open, sliding across the menubar switches menus.
    if (m_open_menu_id != -1 && hovered != -1 && hovered != m_open_menu_id)
        on_menubar_press(window_id, position);
}

void MenuController::on_menubar_left(i32 window_id)
{
    if (m_hovered_menubar_menu == -1)
        return;
    m_hovered_menubar_menu = -1;
    if (menubar_changed)
        menubar_changed(window_id);
}

void MenuController::on_menubar_press(i32 window_id, Gfx::IntPoint position)
{
    for (auto menu_id : m_menubar_order) {
        auto* menu = find_menu(menu_id);
        if (!menu || menu->window_id != window_id || !menu->menubar_rect.contains(position))
            continue;

        if (m_open_menu_id == menu_id) {
            close_open_menu();
            return;
        }
        open_popup(menu_id, window_id, menu->menubar_rect);
        return;
    }
    close_open_menu();
}

void MenuController::on_popup_motion(i32 menu_id, Gfx::IntPoint position)
{
    auto* menu = find_menu(menu_id);
    if (!menu)
        return;
    int index = item_index_at(*menu, position);
    int previous = menu->hovered_valid ? menu->hovered : -1;
    if (index == previous)
        return;
    menu->hovered = index;
    menu->hovered_valid = true;
    if (previous >= 0 && previous < static_cast<int>(menu->items.size()) && item_left)
        item_left(menu_id, menu->items[previous].identifier);
    if (index >= 0 && index < static_cast<int>(menu->items.size()) && menu->items[index].enabled && item_entered)
        item_entered(menu_id, menu->items[index].identifier);
    if (redraw_popup)
        redraw_popup(menu_id);
}

void MenuController::on_popup_button(i32 menu_id, Gfx::IntPoint position, bool pressed)
{
    if (pressed)
        return;
    auto* menu = find_menu(menu_id);
    if (!menu)
        return;
    int index = item_index_at(*menu, position);
    if (index >= 0 && index < static_cast<int>(menu->items.size())) {
        auto& item = menu->items[index];
        if (item.enabled && !item.is_separator && item_activated)
            item_activated(menu_id, item.identifier);
    }
    close_open_menu();
}

void MenuController::on_popup_closed(i32 menu_id)
{
    if (m_open_menu_id != menu_id)
        return;
    i32 window_id = -1;
    if (auto* menu = find_menu(menu_id)) {
        menu->is_open = false;
        window_id = menu->window_id;
    }
    m_open_menu_id = -1;
    m_hovered_menubar_menu = -1;
    if (visibility_changed)
        visibility_changed(menu_id, false);
    if (window_id != -1 && menubar_changed)
        menubar_changed(window_id);
}

void MenuController::move_selection(Menu& menu, int delta)
{
    int const count = menu.items.size();
    if (count == 0)
        return;
    int index = menu.hovered_valid ? menu.hovered : (delta > 0 ? -1 : count);
    for (int step = 0; step < count; ++step) {
        index = (index + delta + count) % count;
        auto& item = menu.items[index];
        if (item.is_separator || !item.enabled)
            continue;
        int previous = menu.hovered_valid ? menu.hovered : -1;
        if (previous >= 0 && previous < count && item_left)
            item_left(menu.id, menu.items[previous].identifier);
        menu.hovered = index;
        menu.hovered_valid = true;
        if (item_entered)
            item_entered(menu.id, item.identifier);
        if (redraw_popup)
            redraw_popup(menu.id);
        return;
    }
}

void MenuController::activate_selected(Menu& menu)
{
    if (!menu.hovered_valid || menu.hovered < 0 || menu.hovered >= static_cast<int>(menu.items.size()))
        return;
    auto& item = menu.items[menu.hovered];
    if (item.enabled && !item.is_separator && item_activated)
        item_activated(menu.id, item.identifier);
    close_open_menu();
}

void MenuController::switch_menubar(i32 window_id, int direction)
{
    Vector<i32> menus;
    for (auto menu_id : m_menubar_order) {
        auto* menu = find_menu(menu_id);
        if (menu && menu->window_id == window_id && !menu->items.is_empty())
            menus.append(menu_id);
    }
    if (menus.is_empty())
        return;
    auto current = menus.find_first_index(m_open_menu_id);
    int index = current.has_value() ? static_cast<int>(current.value()) : 0;
    int next = (index + direction + static_cast<int>(menus.size())) % static_cast<int>(menus.size());
    if (auto* menu = find_menu(menus[next]))
        open_popup(menus[next], window_id, menu->menubar_rect);
}

static u32 menu_accelerator(ByteString const& name)
{
    for (size_t i = 0; i < name.length(); ++i) {
        if (name[i] != '&')
            continue;
        if (i + 1 < name.length() && name[i + 1] == '&') {
            ++i;
            continue;
        }
        if (i + 1 < name.length())
            return static_cast<u32>(to_ascii_lowercase(name[i + 1]));
    }
    return 0;
}

bool MenuController::handle_key(i32 window_id, u32 key_code, u32 code_point, u32 modifiers, bool is_press)
{
    // Alt+letter opens (or switches to) the matching menubar menu.
    if (modifiers & Mod_Alt) {
        if (!is_press)
            return m_open_menu_id != -1;
        if (code_point != 0 && code_point < 128) {
            u32 wanted = static_cast<u32>(to_ascii_lowercase(static_cast<char>(code_point)));
            for (auto menu_id : m_menubar_order) {
                auto* menu = find_menu(menu_id);
                if (!menu || menu->window_id != window_id || menu->items.is_empty())
                    continue;
                if (menu_accelerator(menu->name) == wanted) {
                    open_popup(menu_id, window_id, menu->menubar_rect);
                    return true;
                }
            }
        }
        return m_open_menu_id != -1;
    }

    if (m_open_menu_id == -1) {
        // F10 opens the first menubar menu of the focused window.
        if (is_press && key_code == Key_F10 && window_id != -1) {
            for (auto menu_id : m_menubar_order) {
                auto* menu = find_menu(menu_id);
                if (menu && menu->window_id == window_id && !menu->items.is_empty()) {
                    open_popup(menu_id, window_id, menu->menubar_rect);
                    return true;
                }
            }
        }
        return false;
    }

    // A menu is open: consume every key so the client doesn't react, and
    // navigate on press.
    if (!is_press)
        return true;
    auto* menu = find_menu(m_open_menu_id);
    if (!menu)
        return false;
    switch (key_code) {
    case Key_Up:
        move_selection(*menu, -1);
        return true;
    case Key_Down:
        move_selection(*menu, 1);
        return true;
    case Key_Return:
        activate_selected(*menu);
        return true;
    case Key_Escape:
        close_open_menu();
        return true;
    case Key_Left:
        switch_menubar(menu->window_id, -1);
        return true;
    case Key_Right:
        switch_menubar(menu->window_id, 1);
        return true;
    default:
        return true;
    }
}

}
