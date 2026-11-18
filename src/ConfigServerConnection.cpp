/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ConfigServerConnection.h"
#include "Environment.h"
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>

namespace LibWM {

ConfigServerConnection::ConfigServerConnection(NonnullOwnPtr<Core::LocalSocket> socket)
    : IPC::Connection<ConfigServerEndpoint, ConfigClientEndpoint>(*this, move(socket))
    , ConfigClientEndpoint::Proxy<ConfigServerEndpoint>(*this, {})
{
}

ByteString ConfigServerConnection::config_directory()
{
    return Environment::config_directory();
}

ByteString ConfigServerConnection::domain_file_path(ByteString const& domain_name)
{
    LexicalPath const lexical { domain_name };
    auto safe_name = lexical.basename();
    if (safe_name.is_empty())
        safe_name = "default"sv;
    return ByteString::formatted("{}/{}.ini", config_directory(), safe_name);
}

ConfigServerConnection::GroupMap& ConfigServerConnection::domain(ByteString const& name)
{
    auto& groups = m_domains.ensure(name);
    if (!m_loaded_domains.contains(name)) {
        m_loaded_domains.set(name);
        load_domain_from_disk(name, groups);
    }
    return groups;
}

void ConfigServerConnection::load_domain_from_disk(ByteString const& domain_name, GroupMap& groups)
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

void ConfigServerConnection::sync_domain_to_disk(ByteString const& domain_name)
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

ConfigServerConnection::Value* ConfigServerConnection::find_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key)
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

void ConfigServerConnection::store(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, Value value)
{
    domain(domain_name).ensure(group_name).set(key, move(value));
    sync_domain_to_disk(domain_name);
}

void ConfigServerConnection::monitor_domain(ByteString const& domain_name)
{
    m_monitored_domains.set(domain_name);
}

void ConfigServerConnection::pledge_domains(Vector<ByteString> const& domains)
{
    for (auto const& domain_name : domains)
        m_pledged_domains.set(domain_name);
}

Messages::ConfigServer::ListConfigGroupsResponse ConfigServerConnection::list_config_groups(ByteString const& domain_name)
{
    Vector<ByteString> groups;
    for (auto const& entry : domain(domain_name))
        groups.append(entry.key);
    return Messages::ConfigServer::ListConfigGroupsResponse(move(groups));
}

Messages::ConfigServer::ListConfigKeysResponse ConfigServerConnection::list_config_keys(ByteString const& domain_name, ByteString const& group_name)
{
    Vector<ByteString> keys;
    auto& groups = domain(domain_name);
    if (auto group_it = groups.find(group_name); group_it != groups.end()) {
        for (auto const& entry : group_it->value)
            keys.append(entry.key);
    }
    return Messages::ConfigServer::ListConfigKeysResponse(move(keys));
}

Messages::ConfigServer::ReadStringValueResponse ConfigServerConnection::read_string_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key)
{
    if (auto* value = find_value(domain_name, group_name, key); value && (value->type == Value::Type::String || value->from_disk))
        return Messages::ConfigServer::ReadStringValueResponse(value->string);
    return Messages::ConfigServer::ReadStringValueResponse(Optional<ByteString> {});
}

Messages::ConfigServer::ReadI32ValueResponse ConfigServerConnection::read_i32_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key)
{
    if (auto* value = find_value(domain_name, group_name, key)) {
        if (value->type == Value::Type::I32)
            return Messages::ConfigServer::ReadI32ValueResponse(Optional<i32> { static_cast<i32>(value->integer) });
        if (value->from_disk)
            return Messages::ConfigServer::ReadI32ValueResponse(value->string.to_number<i32>());
    }
    return Messages::ConfigServer::ReadI32ValueResponse(Optional<i32> {});
}

Messages::ConfigServer::ReadU32ValueResponse ConfigServerConnection::read_u32_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key)
{
    if (auto* value = find_value(domain_name, group_name, key)) {
        if (value->type == Value::Type::U32)
            return Messages::ConfigServer::ReadU32ValueResponse(Optional<u32> { static_cast<u32>(value->integer) });
        if (value->from_disk)
            return Messages::ConfigServer::ReadU32ValueResponse(value->string.to_number<u32>());
    }
    return Messages::ConfigServer::ReadU32ValueResponse(Optional<u32> {});
}

Messages::ConfigServer::ReadBoolValueResponse ConfigServerConnection::read_bool_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key)
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

void ConfigServerConnection::write_string_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, ByteString const& value)
{
    store(domain_name, group_name, key, Value { Value::Type::String, value, 0, false, false });
}

void ConfigServerConnection::write_i32_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, i32 value)
{
    store(domain_name, group_name, key, Value { Value::Type::I32, {}, value, false, false });
}

void ConfigServerConnection::write_u32_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, u32 value)
{
    store(domain_name, group_name, key, Value { Value::Type::U32, {}, value, false, false });
}

void ConfigServerConnection::write_bool_value(ByteString const& domain_name, ByteString const& group_name, ByteString const& key, bool value)
{
    store(domain_name, group_name, key, Value { Value::Type::Bool, {}, 0, value, false });
}

void ConfigServerConnection::remove_key_entry(ByteString const& domain_name, ByteString const& group_name, ByteString const& key)
{
    auto& groups = domain(domain_name);
    if (auto group_it = groups.find(group_name); group_it != groups.end()) {
        group_it->value.remove(key);
        sync_domain_to_disk(domain_name);
    }
}

void ConfigServerConnection::remove_group_entry(ByteString const& domain_name, ByteString const& group_name)
{
    domain(domain_name).remove(group_name);
    sync_domain_to_disk(domain_name);
}

void ConfigServerConnection::add_group_entry(ByteString const& domain_name, ByteString const& group_name)
{
    domain(domain_name).ensure(group_name);
    sync_domain_to_disk(domain_name);
}

}
