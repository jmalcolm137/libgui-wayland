/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/String.h>
#include <LibCore/Socket.h>
#include <LibIPC/Connection.h>
#include <ConfigServer/ConfigClientEndpoint.h>
#include <ConfigServer/ConfigServerEndpoint.h>

#include "ConfigServerDefaultStub.h"

namespace LibWM {

// A minimal in-process ConfigServer.
//
// SerenityOS applications read and write their settings (window geometry,
// recent files, render preferences, ...) through this portal. The real service
// is SERENITYOS-gated, so this keeps the same protocol backed by an in-memory
// store. Values are grouped by domain -> group -> key and are typed.
class ConfigServerConnection final
    : public IPC::Connection<ConfigServerEndpoint, ConfigClientEndpoint>
    , public ConfigServerDefaultStub
    , public ConfigClientEndpoint::Proxy<ConfigServerEndpoint> {
public:
    static NonnullRefPtr<ConfigServerConnection> create(NonnullOwnPtr<Core::LocalSocket> socket)
    {
        return adopt_ref(*new ConfigServerConnection(move(socket)));
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
    };

    using KeyMap = HashMap<ByteString, Value>;
    using GroupMap = HashMap<ByteString, KeyMap>;

    GroupMap& domain(ByteString const& name) { return m_domains.ensure(name); }

    Value* find_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key)
    {
        auto domain_it = m_domains.find(domain_name);
        if (domain_it == m_domains.end())
            return nullptr;
        auto group_it = domain_it->value.find(group_name);
        if (group_it == domain_it->value.end())
            return nullptr;
        auto key_it = group_it->value.find(key);
        if (key_it == group_it->value.end())
            return nullptr;
        return &key_it->value;
    }

    void store(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, Value value)
    {
        domain(domain_name).ensure(group_name).set(key, move(value));
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
        if (auto it = m_domains.find(domain_name); it != m_domains.end())
            for (auto const& entry : it->value)
                groups.append(entry.key);
        return Messages::ConfigServer::ListConfigGroupsResponse(move(groups));
    }

    Messages::ConfigServer::ListConfigKeysResponse list_config_keys(ByteString const& domain_name, ByteString const& group_name) override
    {
        Vector<ByteString> keys;
        if (auto domain_it = m_domains.find(domain_name); domain_it != m_domains.end()) {
            if (auto group_it = domain_it->value.find(group_name); group_it != domain_it->value.end())
                for (auto const& entry : group_it->value)
                    keys.append(entry.key);
        }
        return Messages::ConfigServer::ListConfigKeysResponse(move(keys));
    }

    Messages::ConfigServer::ReadStringValueResponse read_string_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key) override
    {
        if (auto* value = find_value(domain_name, group_name, key); value && value->type == Value::Type::String)
            return Messages::ConfigServer::ReadStringValueResponse(value->string);
        return Messages::ConfigServer::ReadStringValueResponse(Optional<ByteString> {});
    }

    Messages::ConfigServer::ReadI32ValueResponse read_i32_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key) override
    {
        if (auto* value = find_value(domain_name, group_name, key); value && value->type == Value::Type::I32)
            return Messages::ConfigServer::ReadI32ValueResponse(Optional<i32> { static_cast<i32>(value->integer) });
        return Messages::ConfigServer::ReadI32ValueResponse(Optional<i32> {});
    }

    Messages::ConfigServer::ReadU32ValueResponse read_u32_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key) override
    {
        if (auto* value = find_value(domain_name, group_name, key); value && value->type == Value::Type::U32)
            return Messages::ConfigServer::ReadU32ValueResponse(Optional<u32> { static_cast<u32>(value->integer) });
        return Messages::ConfigServer::ReadU32ValueResponse(Optional<u32> {});
    }

    Messages::ConfigServer::ReadBoolValueResponse read_bool_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key) override
    {
        if (auto* value = find_value(domain_name, group_name, key); value && value->type == Value::Type::Bool)
            return Messages::ConfigServer::ReadBoolValueResponse(Optional<bool> { value->boolean });
        return Messages::ConfigServer::ReadBoolValueResponse(Optional<bool> {});
    }

    void write_string_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, ByteString const& value) override
    {
        store(domain_name, group_name, key, Value { Value::Type::String, value, 0, false });
    }

    void write_i32_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, i32 value) override
    {
        store(domain_name, group_name, key, Value { Value::Type::I32, {}, value, false });
    }

    void write_u32_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, u32 value) override
    {
        store(domain_name, group_name, key, Value { Value::Type::U32, {}, value, false });
    }

    void write_bool_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, bool value) override
    {
        store(domain_name, group_name, key, Value { Value::Type::Bool, {}, 0, value });
    }

    void remove_key_entry(ByteString const& domain_name, ByteString const& group_name, ByteString const& key) override
    {
        if (auto domain_it = m_domains.find(domain_name); domain_it != m_domains.end()) {
            if (auto group_it = domain_it->value.find(group_name); group_it != domain_it->value.end())
                group_it->value.remove(key);
        }
    }

    void remove_group_entry(ByteString const& domain_name, ByteString const& group_name) override
    {
        if (auto domain_it = m_domains.find(domain_name); domain_it != m_domains.end())
            domain_it->value.remove(group_name);
    }

    void add_group_entry(ByteString const& domain_name, ByteString const& group_name) override
    {
        domain(domain_name).ensure(group_name);
    }

    HashMap<ByteString, GroupMap> m_domains;
    HashTable<ByteString> m_pledged_domains;
    HashTable<ByteString> m_monitored_domains;
};

}
