/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringView.h>

// Every environment variable LibWM reads goes through here, so the full set (and
// each one's precedence and fallback) is documented in one place. See README.md
// "Environment variables".
namespace LibWM::Environment {

// The value of `name` when it is set and non-empty.
Optional<ByteString> get(char const* name);

// True when `name` is set, non-empty and not "0".
bool is_enabled(char const* name);

// The bundled SerenityOS resource tree (fonts, themes, icons, cursors):
// $LIBWM_RES, else $SERENITY_RES_ROOT, else the build tree's Base/res.
ByteString resource_root();

// Maps a Serenity absolute "/res/..." path into resource_root(); any other path
// is returned unchanged.
ByteString host_resource_path(StringView path);

// Where the in-process ConfigServer persists `<domain>.ini` files:
// $LIBWM_CONFIG_DIR, else $XDG_CONFIG_HOME/libgui-wayland, else
// ~/.config/libgui-wayland.
ByteString config_directory();

// `$XDG_RUNTIME_DIR/<name>`, or `/tmp/<name>` without a runtime directory.
ByteString runtime_path(StringView name);

// The directory holding the session's application and service binaries:
// $LIBWM_APP_BIN_DIR (or its historical name $SDE_APP_BIN_DIR). Empty if unset.
ByteString app_bin_dir();

// $SDE_CLIENT_SCALE as a v120 scale (0 or invalid selects 1x). Empty if unset,
// meaning "follow the compositor".
Optional<int> client_scale_override_v120();

// $LIBWM_FORCE_CSD: draw client-side titlebars even when the compositor offers
// server-side decorations.
bool force_csd();

}

// The SerenityOS portal socket paths. Session-scoped paths contain `%sid`, which
// Core::SessionManagement::parse_path_with_sid() expands.
namespace LibWM::PortalPaths {

constexpr StringView window = "/tmp/portal/window"sv;
constexpr StringView clipboard = "/tmp/session/%sid/portal/clipboard"sv;
constexpr StringView launch = "/tmp/session/%sid/portal/launch"sv;
constexpr StringView config = "/tmp/session/%sid/portal/config"sv;
constexpr StringView file_system_access = "/tmp/session/%sid/portal/filesystemaccess"sv;
constexpr StringView image_decoder = "/tmp/session/%sid/portal/image"sv;
constexpr StringView web_content = "/tmp/session/%sid/portal/webcontent"sv;
constexpr StringView request = "/tmp/session/%sid/portal/request"sv;
constexpr StringView sql = "/tmp/session/%sid/portal/sql"sv;
constexpr StringView notify = "/tmp/session/%sid/portal/notify"sv;
constexpr StringView audio = "/tmp/session/%sid/portal/audio"sv;
constexpr StringView audio_manager = "/tmp/session/%sid/portal/audiomanager"sv;

// The template with `%sid` expanded for the current session.
ByteString expand(StringView template_path);

}
