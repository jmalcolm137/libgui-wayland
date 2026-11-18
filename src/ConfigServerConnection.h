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
#include <LibCore/Socket.h>
#include <LibIPC/Connection.h>
#include <ConfigServer/ConfigClientEndpoint.h>
#include <ConfigServer/ConfigServerEndpoint.h>

#include "ConfigServerDefaultStub.h"

namespace LibWM {

// An in-process ConfigServer with disk persistence.
//
// SerenityOS applications read and write their settings (window geometry,
// recent files, render preferences, ...) through this portal. Values are
// grouped by domain -> group -> key, typed in memory, and persisted to
// `<config_dir>/<domain>.ini` (see Environment::config_directory()).
class ConfigServerConnection final
    : public IPC::Connection<ConfigServerEndpoint, ConfigClientEndpoint>
    , public ConfigServerDefaultStub
    , public ConfigClientEndpoint::Proxy<ConfigServerEndpoint> {
public:
    static NonnullRefPtr<ConfigServerConnection> create(NonnullOwnPtr<Core::LocalSocket> socket)
    {
        return adopt_ref(*new ConfigServerConnection(move(socket)));
    }

    static ByteString config_directory();
    static ByteString domain_file_path(ByteString const& domain_name);

private:
    explicit ConfigServerConnection(NonnullOwnPtr<Core::LocalSocket> socket);

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

    GroupMap& domain(ByteString const& name);
    static void load_domain_from_disk(ByteString const& domain_name, GroupMap& groups);
    void sync_domain_to_disk(ByteString const& domain_name);
    Value* find_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key);
    void store(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, Value value);

    void enable_permissive_mode() override { }
    void pledge_domains(Vector<ByteString> const& domains) override;
    void monitor_domain(ByteString const& domain_name) override;
    Messages::ConfigServer::ListConfigGroupsResponse list_config_groups(ByteString const& domain_name) override;
    Messages::ConfigServer::ListConfigKeysResponse list_config_keys(ByteString const& domain_name, ByteString const& group_name) override;
    Messages::ConfigServer::ReadStringValueResponse read_string_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key) override;
    Messages::ConfigServer::ReadI32ValueResponse read_i32_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key) override;
    Messages::ConfigServer::ReadU32ValueResponse read_u32_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key) override;
    Messages::ConfigServer::ReadBoolValueResponse read_bool_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key) override;
    void write_string_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, ByteString const& value) override;
    void write_i32_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, i32 value) override;
    void write_u32_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, u32 value) override;
    void write_bool_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, bool value) override;
    void remove_key_entry(ByteString const& domain_name, ByteString const& group_name, ByteString const& key) override;
    void remove_group_entry(ByteString const& domain_name, ByteString const& group_name) override;
    void add_group_entry(ByteString const& domain_name, ByteString const& group_name) override;

    HashMap<ByteString, GroupMap> m_domains;
    HashTable<ByteString> m_loaded_domains;
    HashTable<ByteString> m_pledged_domains;
    HashTable<ByteString> m_monitored_domains;
};

}
