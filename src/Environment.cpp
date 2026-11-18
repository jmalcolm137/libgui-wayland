/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Environment.h"
#include <LibCore/SessionManagement.h>
#include <stdlib.h>

namespace LibWM::Environment {

Optional<ByteString> get(char const* name)
{
    if (auto const* value = getenv(name); value && *value)
        return ByteString { value };
    return {};
}

bool is_enabled(char const* name)
{
    auto value = get(name);
    return value.has_value() && *value != "0"sv;
}

ByteString resource_root()
{
    if (auto root = get("LIBWM_RES"); root.has_value())
        return root.release_value();
    if (auto root = get("SERENITY_RES_ROOT"); root.has_value())
        return root.release_value();
    return ByteString { LIBWM_DEFAULT_RES };
}

ByteString host_resource_path(StringView path)
{
    if (!path.starts_with("/res/"sv))
        return path;
    return ByteString::formatted("{}/{}", resource_root(), path.substring_view(5));
}

ByteString config_directory()
{
    if (auto dir = get("LIBWM_CONFIG_DIR"); dir.has_value())
        return dir.release_value();
    if (auto xdg = get("XDG_CONFIG_HOME"); xdg.has_value())
        return ByteString::formatted("{}/libgui-wayland", *xdg);
    if (auto home = get("HOME"); home.has_value())
        return ByteString::formatted("{}/.config/libgui-wayland", *home);
    return ByteString { "/tmp/libgui-wayland-config" };
}

ByteString runtime_path(StringView name)
{
    if (auto runtime = get("XDG_RUNTIME_DIR"); runtime.has_value())
        return ByteString::formatted("{}/{}", *runtime, name);
    return ByteString::formatted("/tmp/{}", name);
}

ByteString app_bin_dir()
{
    if (auto dir = get("LIBWM_APP_BIN_DIR"); dir.has_value())
        return dir.release_value();
    if (auto dir = get("SDE_APP_BIN_DIR"); dir.has_value())
        return dir.release_value();
    return {};
}

Optional<int> client_scale_override_v120()
{
    auto value = get("SDE_CLIENT_SCALE");
    if (!value.has_value())
        return {};
    double const requested = strtod(value->characters(), nullptr);
    return requested > 0.0 ? static_cast<int>(__builtin_lround(requested * 120.0)) : 120;
}

bool force_csd()
{
    return is_enabled("LIBWM_FORCE_CSD");
}

}

namespace LibWM::PortalPaths {

ByteString expand(StringView template_path)
{
    return MUST(Core::SessionManagement::parse_path_with_sid(template_path));
}

}
