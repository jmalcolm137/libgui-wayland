/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/LexicalPath.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/Socket.h>
#include <LibIPC/Connection.h>
#include <ConfigServer/ConfigClientEndpoint.h>
#include <ConfigServer/ConfigServerEndpoint.h>
#include <stdlib.h>

#include "ConfigServerDefaultStub.h"

namespace LibWM {

// An in-process ConfigServer with disk persistence.
//
// SerenityOS applications read and write their settings (window geometry,
// recent files, render preferences, ...) through this portal. Values are
// grouped by domain -> group -> key, typed in memory, and persisted to
// `<config_dir>/<domain>.ini` (`$LIBWM_CONFIG_DIR` or
// `$XDG_CONFIG_HOME/libgui-wayland` or `~/.config/libgui-wayland`).
class ConfigServerConnection final
    : public IPC::Connection<ConfigServerEndpoint, ConfigClientEndpoint>
    , public ConfigServerDefaultStub
    , public ConfigClientEndpoint::Proxy<ConfigServerEndpoint> {
public:
    static NonnullRefPtr<ConfigServerConnection> create(NonnullOwnPtr<Core::LocalSocket> socket)
    {
        return adopt_ref(*new ConfigServerConnection(move(socket)));
    }

    static ByteString config_directory()
    {
        if (auto const* dir = getenv("LIBWM_CONFIG_DIR"); dir && *dir)
            return ByteString(dir);
        if (auto const* xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg)
            return ByteString::formatted("{}/libgui-wayland", xdg);
        if (auto const* home = getenv("HOME"); home && *home)
            return ByteString::formatted("{}/.config/libgui-wayland", home);
        return ByteString { "/tmp/libgui-wayland-config" };
    }

    static ByteString domain_file_path(ByteString const& domain_name)
    {
        LexicalPath const lexical { domain_name };
        auto safe_name = lexical.basename();
        if (safe_name.is_empty())
            safe_name = "default"sv;
        return ByteString::formatted("{}/{}.ini", config_directory(), safe_name);
    }

private:
    explicit ConfigServerConnection(NonnullOwnPtr<Core::LocalSocket> socket)
        : IPC::Connection<ConfigServerEndpoint, ConfigClientEndpoint>(*this, move(socket))
        , ConfigClientEndpoint::Proxy<ConfigServerEndpoint>(*this, {})
    {
    }

    struct Value {
        enum class Type {
            String,
            I32,
            U32,
            Bool,
        };
        Type type { Type::String };
        ByteString string;
        i64 integer { 0 };
        bool boolean { false };
        // Entries loaded from an existing INI file on disk are untyped text until
        // overwritten in memory; allow typed readers to parse them on demand.
        bool from_disk { false };
    };

    using KeyMap = HashMap<ByteString, Value>;
    using GroupMap = HashMap<ByteString, KeyMap>;

    GroupMap& domain(ByteString const& name)
    {
        auto& groups = m_domains.ensure(name);
        if (!m_loaded_domains.contains(name)) {
            m_loaded_domains.set(name);
            load_domain_from_disk(name, groups);
        }
        return groups;
    }

    static void load_domain_from_disk(ByteString const& domain_name, GroupMap& groups)
    {
        auto path = domain_file_path(domain_name);
        auto config_or_error = Core::ConfigFile::open(path, Core::ConfigFile::AllowWriting::No);
        if (config_or_error.is_error())
            return;
        auto& config = *config_or_error.value();
        for (auto const& group_name : config.groups()) {
            auto& group = groups.ensure(group_name);
            for (auto const& key : config.keys(group_name)) {
                if (!group.contains(key))
                    group.set(key, Value { Value::Type::String, config.read_entry(group_name, key), 0, false, true });
            }
        }
    }

    void sync_domain_to_disk(ByteString const& domain_name)
    {
        auto dir = config_directory();
        if (Core::Directory::create(dir, Core::Directory::CreateDirectories::Yes).is_error())
            return;
        auto path = domain_file_path(domain_name);
        auto file_or_error = Core::File::open(path, Core::File::OpenMode::Write);
        if (file_or_error.is_error())
            return;

        StringBuilder builder;
        if (auto domain_it = m_domains.find(domain_name); domain_it != m_domains.end()) {
            for (auto const& group_entry : domain_it->value) {
                builder.appendff("[{}]\n", group_entry.key);
                for (auto const& key_entry : group_entry.value) {
                    auto const& val = key_entry.value;
                    switch (val.type) {
                    case Value::Type::String:
                        builder.appendff("{}={}\n", key_entry.key, val.string);
                        break;
                    case Value::Type::I32:
                        builder.appendff("{}={}\n", key_entry.key, static_cast<i32>(val.integer));
                        break;
                    case Value::Type::U32:
                        builder.appendff("{}={}\n", key_entry.key, static_cast<u32>(val.integer));
                        break;
                    case Value::Type::Bool:
                        builder.appendff("{}={}\n", key_entry.key, val.boolean ? "true"sv : "false"sv);
                        break;
                    }
                }
                builder.append('\n');
            }
        }
        (void)file_or_error.value()->write_until_depleted(builder.string_view());
    }

    Value* find_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key)
    {
        auto& groups = domain(domain_name);
        auto group_it = groups.find(group_name);
        if (group_it == groups.end())
            return nullptr;
        auto key_it = group_it->value.find(key);
        if (key_it == group_it->value.end())
            return nullptr;
        return &key_it->value;
    }

    void store(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, Value value)
    {
        domain(domain_name).ensure(group_name).set(key, move(value));
        sync_domain_to_disk(domain_name);
    }

    void enable_permissive_mode() override { }
    void pledge_domains(Vector<ByteString> const& domains) override
    {
        for (auto const& domain_name : domains)
            m_pledged_domains.set(domain_name);
    }
    void monitor_domain(ByteString const& domain_name) override { m_monitored_domains.set(domain_name); }

    Messages::ConfigServer::ListConfigGroupsResponse list_config_groups(ByteString const& domain_name) override
    {
        Vector<ByteString> groups;
        for (auto const& entry : domain(domain_name))
            groups.append(entry.key);
        return Messages::ConfigServer::ListConfigGroupsResponse(move(groups));
    }

    Messages::ConfigServer::ListConfigKeysResponse list_config_keys(ByteString const& domain_name, ByteString const& group_name) override
    {
        Vector<ByteString> keys;
        auto& groups = domain(domain_name);
        if (auto group_it = groups.find(group_name); group_it != groups.end()) {
            for (auto const& entry : group_it->value)
                keys.append(entry.key);
        }
        return Messages::ConfigServer::ListConfigKeysResponse(move(keys));
    }

    Messages::ConfigServer::ReadStringValueResponse read_string_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key) override
    {
        if (auto* value = find_value(domain_name, group_name, key); value && (value->type == Value::Type::String || value->from_disk))
            return Messages::ConfigServer::ReadStringValueResponse(value->string);
        return Messages::ConfigServer::ReadStringValueResponse(Optional<ByteString> {});
    }

    Messages::ConfigServer::ReadI32ValueResponse read_i32_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key) override
    {
        if (auto* value = find_value(domain_name, group_name, key)) {
            if (value->type == Value::Type::I32)
                return Messages::ConfigServer::ReadI32ValueResponse(Optional<i32> { static_cast<i32>(value->integer) });
            if (value->from_disk)
                return Messages::ConfigServer::ReadI32ValueResponse(value->string.to_number<i32>());
        }
        return Messages::ConfigServer::ReadI32ValueResponse(Optional<i32> {});
    }

    Messages::ConfigServer::ReadU32ValueResponse read_u32_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key) override
    {
        if (auto* value = find_value(domain_name, group_name, key)) {
            if (value->type == Value::Type::U32)
                return Messages::ConfigServer::ReadU32ValueResponse(Optional<u32> { static_cast<u32>(value->integer) });
            if (value->from_disk)
                return Messages::ConfigServer::ReadU32ValueResponse(value->string.to_number<u32>());
        }
        return Messages::ConfigServer::ReadU32ValueResponse(Optional<u32> {});
    }

    Messages::ConfigServer::ReadBoolValueResponse read_bool_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key) override
    {
        if (auto* value = find_value(domain_name, group_name, key)) {
            if (value->type == Value::Type::Bool)
                return Messages::ConfigServer::ReadBoolValueResponse(Optional<bool> { value->boolean });
            if (value->from_disk) {
                if (value->string.equals_ignoring_ascii_case("true"sv) || value->string == "1"sv)
                    return Messages::ConfigServer::ReadBoolValueResponse(Optional<bool> { true });
                if (value->string.equals_ignoring_ascii_case("false"sv) || value->string == "0"sv)
                    return Messages::ConfigServer::ReadBoolValueResponse(Optional<bool> { false });
            }
        }
        return Messages::ConfigServer::ReadBoolValueResponse(Optional<bool> {});
    }

    void write_string_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, ByteString const& value) override
    {
        store(domain_name, group_name, key, Value { Value::Type::String, value, 0, false, false });
    }

    void write_i32_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, i32 value) override
    {
        store(domain_name, group_name, key, Value { Value::Type::I32, {}, value, false, false });
    }

    void write_u32_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, u32 value) override
    {
        store(domain_name, group_name, key, Value { Value::Type::U32, {}, value, false, false });
    }

    void write_bool_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, bool value) override
    {
        store(domain_name, group_name, key, Value { Value::Type::Bool, {}, 0, value, false });
    }

    void remove_key_entry(ByteString const& domain_name, ByteString const& group_name, ByteString const& key) override
    {
        auto& groups = domain(domain_name);
        if (auto group_it = groups.find(group_name); group_it != groups.end()) {
            group_it->value.remove(key);
            sync_domain_to_disk(domain_name);
        }
    }

    void remove_group_entry(ByteString const& domain_name, ByteString const& group_name) override
    {
        domain(domain_name).remove(group_name);
        sync_domain_to_disk(domain_name);
    }

    void add_group_entry(ByteString const& domain_name, ByteString const& group_name) override
    {
        domain(domain_name).ensure(group_name);
        sync_domain_to_disk(domain_name);
    }

    HashMap<ByteString, GroupMap> m_domains;
    HashTable<ByteString> m_loaded_domains;
    HashTable<ByteString> m_pledged_domains;
    HashTable<ByteString> m_monitored_domains;
};

}
