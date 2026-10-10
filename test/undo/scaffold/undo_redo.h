#pragma once

// Scaffolding for the undo/redo chapter: the reflected object model (DbObject,
// MEMBER) and the stream/rollback machinery. The test itself lives next door in
// test_undo_redo.cpp. Everything is deliberately at file scope with
// internal linkage, because this header is included into exactly one test
// translation unit.

#include <cstring>
#include <meta>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <stack>

struct DbObject;

template <typename T>
consteval std::size_t get_member_index(std::meta::info target)
{
    constexpr auto fields =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));
    std::size_t i = 0;
    template for (constexpr std::meta::info field : fields) {
        if (field == target)
            return i;
        ++i;
    }
#if defined(__clang__)
    __builtin_unreachable();
#else
    return i;
#endif
}

namespace {
    std::string gl_stream_buffer;

    int read_int(const char* _data, std::size_t _offset)
    {
        int value = 0;
        std::memcpy(&value, _data + _offset, sizeof(value));
        return value;
    }

    struct CommandAnchor {
        int m_begin = 0;
        int m_end = 0;
    };

    std::stack<CommandAnchor> gl_command_anchors;

    std::string gl_command_buffer;

    std::unordered_map<int, DbObject*> gl_object_map;
}

template <typename T>
struct Member {
    static_assert(std::is_trivially_copyable_v<T>,
                  "MEMBER requires a trivially copyable type: the undo/redo path copies raw bytes");

    template <typename ...Args>
    Member(DbObject* _owner, Args&& ..._args)
        : m_value(std::forward<Args>(_args)...), m_owner(_owner)
        {}

    const T& get() const {
        return m_value;
    }

    T& touch() {
        return m_value;
    }

protected:
    T m_value;
    DbObject* m_owner = nullptr;
};

namespace {

// Clang-p2996 上仅用 is_template/template_of 与 ^^Member 比较可能不成立；
consteval bool undo_is_member_holder_type(std::meta::info dealt_ty)
{
    if (!std::meta::has_template_arguments(dealt_ty))
        return false;
    auto args = std::define_static_array(std::meta::template_arguments_of(dealt_ty));
    if (args.size() != 1uz)
        return false;
    const std::meta::info rebuilt =
        std::meta::substitute(^^Member, args);
    return std::meta::is_same_type(rebuilt, dealt_ty);
}

} // namespace

#define MEMBER(type, name, ...)                                                                              \
public:                                                                                                       \
type& touch_##name()                                                                                         \
    {                                                                                                        \
        if (m_open_status == 0)                                                                              \
            throw std::runtime_error("DbObject is not open");                                                   \
        using owner_type = std::remove_cvref_t<decltype(*this)>;                                                \
        int index = static_cast<int>(get_member_index<owner_type>(^^owner_type::name));                              \
        m_character_stream.append(reinterpret_cast<const char*>(&index), sizeof(index));                         \
        m_character_stream.append(reinterpret_cast<const char*>(&name.get()), sizeof(type));                       \
        const int framed_bytes = sizeof(int) + sizeof(type);                                                          \
        m_character_stream.append(reinterpret_cast<const char*>(&framed_bytes), sizeof(framed_bytes));              \
        return name.touch();                                                                                 \
    }                                                                                                        \
    const type& get_##name() const                                                                           \
    {                                                                                                        \
        return name.get();                                                                                   \
    }                                                                                                        \
private:                                                                                                     \
    Member<type> name { this, __VA_ARGS__ }


constexpr int FULL_BACKUP = -999;

struct DbObject {
    MEMBER(int, m_field_int, 3);
    MEMBER(int, m_field_int2, 4);
public:
    void open() {
        if (m_open_status != 0)
            throw std::runtime_error("DbObject is already open");
        m_open_status = 1;

        m_character_stream.clear();

        full_backup();
    }

    void close() {
        if (m_open_status == 0)
            throw std::runtime_error("DbObject is not open");

        m_open_status = 0;
        commit_undo_data();
        m_character_stream.clear();

    }

    void restore_from_backup(const std::string& _backup_data) {
        std::ptrdiff_t l = 0;
        std::ptrdiff_t r = static_cast<std::ptrdiff_t>(_backup_data.size());

        while (l < r) {
            if (r - l < static_cast<std::ptrdiff_t>(sizeof(int)))
                throw std::runtime_error("restore_from_backup: truncated footer");

            r -= static_cast<std::ptrdiff_t>(sizeof(int));
            const int body_len = read_int(_backup_data.data(), static_cast<std::size_t>(r));
            if (body_len < 0 || r - body_len < l)
                throw std::runtime_error("restore_from_backup: invalid body_len");

            r -= body_len;
            const int session_start = static_cast<int>(r);

            const int tag = read_int(_backup_data.data(), static_cast<std::size_t>(session_start));
            const int after_tag_off = session_start + static_cast<int>(sizeof(int));

            const int inner_bytes = session_start + body_len - after_tag_off;
                if (inner_bytes <= 0)
                throw std::runtime_error("restore_from_backup: truncated full snapshot");
            const char* blob_start = _backup_data.data() + static_cast<std::size_t>(after_tag_off);
            if (tag == FULL_BACKUP) {
                restore_from_fullback(std::string_view(blob_start, static_cast<std::size_t>(inner_bytes)));
                continue;
            }
            if (tag >= 0) {
                restore_from_partialback(tag, std::string_view(blob_start, static_cast<std::size_t>(inner_bytes)));
                continue;
            } else {
                throw std::runtime_error("restore_from_backup: invalid tag");
            }
        }
    }

    void full_backup() {

        m_character_stream.append((const char*)&FULL_BACKUP, sizeof(FULL_BACKUP));

        int len_stream = m_character_stream.size();

        constexpr auto fields = std::define_static_array(std::meta::nonstatic_data_members_of(^^DbObject, std::meta::access_context::unchecked()));
        template for (constexpr std::meta::info field : fields) {
            constexpr std::meta::info fty = std::meta::dealias(std::meta::type_of(field));
            if constexpr (undo_is_member_holder_type(fty)) {
                constexpr auto targs =
                    std::define_static_array(std::meta::template_arguments_of(fty));
                using field_type = typename[:targs[0]:];
                if constexpr (std::is_same_v<field_type, std::string>) {
                    const int len = static_cast<int>(this->[:field:].get().size());
                    m_character_stream.append((const char*)&len, sizeof(len));
                    m_character_stream.append(this->[:field:].get().c_str(),
                                              static_cast<std::string::size_type>(len));
                } else {
                    m_character_stream.append((const char*)&(this->[:field:].get()), sizeof(field_type));
                }
            }
        }

        int full_backup_len = m_character_stream.size() - len_stream;

        // Not efficient, but it's ok for now.
        m_character_stream.append((const char*)&full_backup_len, sizeof(full_backup_len));

        m_full_backup_len = m_character_stream.size();
    }

    void restore_from_fullback(const std::string_view& backup_data){

        int offset = 0;

        constexpr auto fields = std::define_static_array(std::meta::nonstatic_data_members_of(^^DbObject, std::meta::access_context::unchecked()));
        template for (constexpr std::meta::info field : fields) {
            constexpr std::meta::info fty = std::meta::dealias(std::meta::type_of(field));
            if constexpr (undo_is_member_holder_type(fty)) {
                constexpr auto targs =
                    std::define_static_array(std::meta::template_arguments_of(fty));
                using field_type = typename[:targs[0]:];
                if constexpr (std::is_same_v<field_type, std::string>) {
                    const int elem_len = read_int(backup_data.data(), static_cast<std::size_t>(offset));
                    offset += 4;
                    this->[:field:].touch().clear();
                    this->[:field:].touch().append(backup_data.data() + offset,
                                                   backup_data.data() + offset +
                                                       static_cast<std::size_t>(elem_len));
                    offset += elem_len;
                } else {

                    std::memcpy(&this->[:field:].touch(), backup_data.data() + offset, sizeof(field_type));
                    offset += sizeof(field_type);
                }
            }
        }

    }

    void restore_from_partialback(int _back_mem_index, const std::string_view& _backup_data) {

        constexpr auto fields = std::define_static_array(std::meta::nonstatic_data_members_of(^^DbObject, std::meta::access_context::unchecked()));
        template for (constexpr std::meta::info field : fields) {
            constexpr std::meta::info fty = std::meta::dealias(std::meta::type_of(field));
            if constexpr (undo_is_member_holder_type(fty)) {
                const int mem_index = static_cast<int>(get_member_index<DbObject>(field));
                if (mem_index == _back_mem_index) {
                    constexpr auto targs =
                    std::define_static_array(std::meta::template_arguments_of(fty));
                    using field_type = typename[:targs[0]:];
                    if (static_cast<std::size_t>(_backup_data.size()) != sizeof(field_type))
                        throw std::runtime_error("partial backup: payload size mismatch");
                    std::memcpy(&this->[:field:].touch(), _backup_data.data(), sizeof(field_type));
                }
            }
        }
    }

    void commit_undo_data() {

        int m_start = 0;
        int m_size = 0;
        if (m_character_stream.size() == static_cast<std::size_t>(m_full_backup_len)) {
            m_start = 0;
            m_size = m_full_backup_len;
        } else if (m_character_stream.size() > static_cast<std::size_t>(m_full_backup_len)) {
            m_start = m_full_backup_len;
            m_size = static_cast<int>(m_character_stream.size()) - m_full_backup_len;
        } else {
            m_start = 0;
            m_size = static_cast<int>(m_character_stream.size());
        }

        std::string backup_data = m_character_stream.substr(static_cast<std::size_t>(m_start), static_cast<std::size_t>(m_size));

        backup_data.insert(0, reinterpret_cast<const char*>(&m_object_id), sizeof(m_object_id));
        const int nbytes = static_cast<int>(backup_data.size());
        backup_data.append(reinterpret_cast<const char*>(&nbytes), sizeof(nbytes));

        gl_command_buffer.append(backup_data);

    }

public:
    int m_object_id = 0;
protected:
    int m_open_status = 0;

    /**
     * --------   [Operation type(int)   Backup length(int)   Backup data(variable length)]  Rollback length(int)  -------------
     *            |                                                                       |               |>
     *            >----------------------------------------------------------------------<               |
     *                                               |                                                  |
     *                                               >------------------------------------------------->
     * Step1. Read the rollabck length from the stream end.
     * Step2. Anchor rollback to before the operation type.
     * Step3. Read the operation type.
     * Step4. Read the backup length and backup data.
     * Step5. Object restore from the backup data.
     */
    std::string m_character_stream;
    int m_full_backup_len = 0;
};

namespace {
    
    void command_start(){
        gl_command_buffer.clear();
    }

    void command_end() {
        gl_command_anchors.push({static_cast<int>(gl_stream_buffer.size()), static_cast<int>(gl_stream_buffer.size() + gl_command_buffer.size())});
        gl_stream_buffer.append(gl_command_buffer);
        gl_command_buffer.clear();
    }

    void command_rollback() {
        if (gl_command_anchors.empty()) {
            throw std::runtime_error("No command anchor found");
        }
        CommandAnchor anchor = gl_command_anchors.top();
        gl_command_anchors.pop();

        std::string backup_data = gl_stream_buffer.substr(anchor.m_begin, anchor.m_end - anchor.m_begin);

        int r = static_cast<int>(backup_data.size());
        while (r > 0) {
            if (r < static_cast<int>(sizeof(int)) * 2)
                throw std::runtime_error("rollback: malformed frame (footer)");

            const int nbytes =
                read_int(backup_data.data(), static_cast<std::size_t>(r - static_cast<int>(sizeof(int))));
            r -= static_cast<int>(sizeof(int));

            if (nbytes < static_cast<int>(sizeof(int)) || nbytes > r)
                throw std::runtime_error("rollback: nbytes out of range");

            const int session_start = r - nbytes;
            const char* segment = backup_data.data() + static_cast<std::size_t>(session_start);

            const int object_id = read_int(segment, 0);
            const int payload_len =
                nbytes - static_cast<int>(sizeof(int));
            if (payload_len < 0)
                throw std::runtime_error("rollback: negative payload_len");

            DbObject* object = gl_object_map.at(object_id);
            object->restore_from_backup(std::string(segment + sizeof(int), static_cast<std::size_t>(payload_len)));

            r = session_start;
        }
    }
}
