#pragma once

// The reflection-driven undo/redo library.
//
// A database object derives from DbObject<Derived> and declares its fields with
// MEMBER(). The base walks those fields by reflection, so capturing and
// restoring them needs no per-type code: each field's bytes are produced by a
// small type-dispatched codec (see detail::encode / detail::decode) that
// mirrors the JSON layer's coverage -- scalars, strings, enums, optional,
// vector, array and nested reflected structs.
//
// A Session owns the undo and redo stacks. A command is one user operation:
// open()/close() each participating object inside begin()/end(), then undo()
// and redo() move between the two stacks. Only fields that were actually
// written are recorded, so a command stores the before and after image of
// exactly the fields it touched.
//
// A Transaction is a savepoint independent of commands: begin_transaction()
// marks the history, rollback_to() reverts every write recorded after the mark,
// and commit() releases the mark without creating an undo node. Because the
// mark is just a position, a transaction may start mid-command or span several,
// and undo stays per-command. Its rollback reverts the command in progress down
// to the mark, then undoes and drops the commands pushed since; objects still
// open are aborted first.
//
// The example object under test lives in test/undo/scaffold/undo_redo.h.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <meta>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace undo {

class DbObjectBase;

// --- value codec ------------------------------------------------------------
// A field is encoded as: <index:uint32><payload>. The payload is host-endian
// and meant to be replayed in the same process, not persisted across machines.

namespace detail {

template <typename T> struct is_optional : std::false_type {};
template <typename T> struct is_optional<std::optional<T>> : std::true_type {};

template <typename T> struct is_vector : std::false_type {};
template <typename T, typename A>
struct is_vector<std::vector<T, A>> : std::true_type {};

template <typename T> struct is_array : std::false_type {};
template <typename T, std::size_t N>
struct is_array<std::array<T, N>> : std::true_type {};

template <typename T>
inline constexpr bool is_string_v =
    std::is_same_v<std::remove_cv_t<T>, std::string>;

/// Appends one encoded value to `_out`, dispatching on the reflected type.
template <typename T> void encode(std::string &_out, const T &_value) {
  if constexpr (std::is_same_v<std::remove_cv_t<T>, bool>) {
    _out.push_back(static_cast<char>(_value ? 1 : 0));
  } else if constexpr (std::is_enum_v<T>) {
    encode(_out, static_cast<std::underlying_type_t<T>>(_value));
  } else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>) {
    _out.append(reinterpret_cast<const char *>(&_value), sizeof(T));
  } else if constexpr (is_string_v<T>) {
    const std::uint32_t count = static_cast<std::uint32_t>(_value.size());
    _out.append(reinterpret_cast<const char *>(&count), sizeof(count));
    _out.append(_value.data(), count);
  } else if constexpr (is_optional<T>::value) {
    _out.push_back(static_cast<char>(_value.has_value() ? 1 : 0));
    if (_value) {
      encode(_out, *_value);
    }
  } else if constexpr (is_vector<T>::value) {
    const std::uint32_t count = static_cast<std::uint32_t>(_value.size());
    _out.append(reinterpret_cast<const char *>(&count), sizeof(count));
    for (const auto &element : _value) {
      encode(_out, element);
    }
  } else if constexpr (is_array<T>::value) {
    for (const auto &element : _value) {
      encode(_out, element);
    }
  } else if constexpr (std::is_class_v<T>) {
    constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(
            ^^T, std::meta::access_context::unchecked()));
    template for (constexpr std::meta::info member : members) {
      encode(_out, _value.[:member:]);
    }
  } else {
    static_assert(
        !sizeof(T),
        "undo: unsupported field type (expected a scalar, string, enum, "
        "optional, vector, array or reflected struct)");
  }
}

/// Reads one encoded value from `_in` at `_offset`, advancing it past the
/// value. The mirror of encode().
template <typename T>
void decode(std::string_view _in, std::size_t &_offset, T &_out) {
  if constexpr (std::is_same_v<std::remove_cv_t<T>, bool>) {
    _out = _in[_offset++] != 0;
  } else if constexpr (std::is_enum_v<T>) {
    std::underlying_type_t<T> raw{};
    decode(_in, _offset, raw);
    _out = static_cast<T>(raw);
  } else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>) {
    std::memcpy(&_out, _in.data() + _offset, sizeof(T));
    _offset += sizeof(T);
  } else if constexpr (is_string_v<T>) {
    std::uint32_t count{};
    std::memcpy(&count, _in.data() + _offset, sizeof(count));
    _offset += sizeof(count);
    _out.assign(_in.data() + _offset, count);
    _offset += count;
  } else if constexpr (is_optional<T>::value) {
    const bool present = _in[_offset++] != 0;
    if (present) {
      typename T::value_type value{};
      decode(_in, _offset, value);
      _out = std::move(value);
    } else {
      _out.reset();
    }
  } else if constexpr (is_vector<T>::value) {
    std::uint32_t count{};
    std::memcpy(&count, _in.data() + _offset, sizeof(count));
    _offset += sizeof(count);
    _out.clear();
    _out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      typename T::value_type element{};
      decode(_in, _offset, element);
      _out.push_back(std::move(element));
    }
  } else if constexpr (is_array<T>::value) {
    for (std::size_t i = 0; i < _out.size(); ++i) {
      decode(_in, _offset, _out[i]);
    }
  } else if constexpr (std::is_class_v<T>) {
    constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(
            ^^T, std::meta::access_context::unchecked()));
    template for (constexpr std::meta::info member : members) {
      decode(_in, _offset, _out.[:member:]);
    }
  } else {
    static_assert(!sizeof(T), "undo: unsupported field type");
  }
}

} // namespace detail

// --- the member template class ---------------------------------------------

/// The database object member template class: one reflected field. touch()
/// hands out the mutable value and MEMBER() records it in the object's before
/// image the first time it is written in a command.
template <typename T> struct Member {
  template <typename... Args>
  Member(Args &&..._args) : m_value(std::forward<Args>(_args)...) {}

  const T &get() const { return m_value; }
  T &touch() { return m_value; }

protected:
  T m_value;
};

/// Index of `target` among `T`'s nonstatic data members, computed at compile
/// time.
template <typename T>
consteval std::size_t member_index(std::meta::info target) {
  constexpr auto fields =
      std::define_static_array(std::meta::nonstatic_data_members_of(
          ^^T, std::meta::access_context::unchecked()));
  std::size_t index = 0;
  template for (constexpr std::meta::info field : fields) {
    if (field == target)
      return index;
    ++index;
  }
#if defined(__clang__)
  __builtin_unreachable();
#else
  return index;
#endif
}

/// True when `dealt_ty` is an instantiation of Member<...>. Clang-p2996 cannot
/// compare `^^Member` through is_template/template_of reliably, so it is
/// rebuilt from the same arguments and compared as a type.
consteval bool is_member_holder_type(std::meta::info dealt_ty) {
  if (!std::meta::has_template_arguments(dealt_ty))
    return false;
  auto args =
      std::define_static_array(std::meta::template_arguments_of(dealt_ty));
  if (args.size() != 1uz)
    return false;
  const std::meta::info rebuilt = std::meta::substitute(^^Member, args);
  return std::meta::is_same_type(rebuilt, dealt_ty);
}

// --- the database object class ---------------------------------------------

/// The type-erased handle a Session stores, so it can restore through a pointer
/// without knowing Derived.
class DbObjectBase {
public:
  virtual ~DbObjectBase() = default;

  /// Writes the fields named by `_payload` back into the object. Called by
  /// Session::undo / Session::redo.
  virtual void apply_payload(std::string_view _payload) = 0;

  /// Discards an in-progress open(): puts the touched fields back and closes
  /// the object. Called by Session::rollback for objects a command left open.
  virtual void abort() = 0;
};

class Session;

/// The database object class. Derive from it (CRTP), pass the owning Session to
/// the base constructor, and declare fields with MEMBER().
template <typename Derived> class DbObject : public DbObjectBase {
public:
  explicit DbObject(Session &_session) : m_session(&_session) {}

  /// True while the object is between open() and close().
  bool is_open() const { return m_open_status != 0; }

  /// Starts recording this object in the current command and clears the before
  /// image. Every write between open() and close() is captured. Defined after
  /// Session because it registers with it.
  void open();

  /// Commits the recorded before/after image to the session's current command.
  /// A command that wrote nothing records nothing. Declared here, defined after
  /// Session because it calls into it.
  void close();

  void apply_payload(std::string_view _payload) override {
    auto &self = static_cast<Derived &>(*this);
    constexpr auto fields =
        std::define_static_array(std::meta::nonstatic_data_members_of(
            ^^Derived, std::meta::access_context::unchecked()));

    std::size_t offset = 0;
    while (offset < _payload.size()) {
      if (_payload.size() - offset < sizeof(std::uint32_t)) {
        throw std::runtime_error("undo: truncated payload");
      }
      std::uint32_t index{};
      std::memcpy(&index, _payload.data() + offset, sizeof(index));
      offset += sizeof(index);

      bool applied = false;
      template for (constexpr std::meta::info field : fields) {
        constexpr std::meta::info field_type =
            std::meta::dealias(std::meta::type_of(field));
        if constexpr (is_member_holder_type(field_type)) {
          constexpr std::uint32_t kIndex =
              static_cast<std::uint32_t>(member_index<Derived>(field));
          if (!applied && index == kIndex) {
            constexpr auto args = std::define_static_array(
                std::meta::template_arguments_of(field_type));
            using value_type = typename[:args[0]:];
            value_type value{};
            detail::decode(_payload, offset, value);
            self.[:field:].touch() = std::move(value);
            applied = true;
          }
        }
      }
      if (!applied) {
        throw std::runtime_error("undo: payload references an unknown member");
      }
    }
  }

  /// m_character_stream is exactly the before image, so aborting an open object
  /// is replaying it and clearing the open state.
  void abort() override {
    if (m_open_status != 0) {
      apply_payload(m_character_stream);
      m_open_status = 0;
    }
    m_character_stream.clear();
    m_touched.clear();
  }

protected:
  /// Records `_index` in the before image the first time the field is written
  /// in this command, so a field written twice still undoes to its original.
  bool begin_record(std::uint32_t _index) {
    for (const std::uint32_t seen : m_touched) {
      if (seen == _index) {
        return false;
      }
    }
    m_touched.push_back(_index);
    return true;
  }

  /// The after image: the same fields as the before image, holding their
  /// current values. Built at close() from the fields recorded at touch time.
  std::string build_after_payload() {
    auto &self = static_cast<Derived &>(*this);
    constexpr auto fields =
        std::define_static_array(std::meta::nonstatic_data_members_of(
            ^^Derived, std::meta::access_context::unchecked()));

    std::string out;
    for (const std::uint32_t index : m_touched) {
      bool written = false;
      template for (constexpr std::meta::info field : fields) {
        constexpr std::meta::info field_type =
            std::meta::dealias(std::meta::type_of(field));
        if constexpr (is_member_holder_type(field_type)) {
          constexpr std::uint32_t kIndex =
              static_cast<std::uint32_t>(member_index<Derived>(field));
          if (!written && index == kIndex) {
            out.append(reinterpret_cast<const char *>(&kIndex), sizeof(kIndex));
            detail::encode(out, self.[:field:].get());
            written = true;
          }
        }
      }
      if (!written) {
        throw std::runtime_error("undo: untracked member index");
      }
    }
    return out;
  }

  Session *m_session = nullptr;
  int m_open_status = 0;
  std::string m_character_stream;       // before image of the current command
  std::vector<std::uint32_t> m_touched; // member indices already recorded
};

// --- the session ------------------------------------------------------------

/// Owns the undo and redo stacks and the command in progress.
///
/// A command is the unit of undo. Prefer the RAII savepoint for anything that
/// can throw, so a failed block rolls back and leaves no object open:
///   undo::Session session;
///   Obj obj{session};
///   {
///       undo::Transaction tx{session};   // marks the history
///       session.begin();
///       obj.open();
///       obj.touch_field() = 42;
///       obj.close();
///       session.end();
///       tx.commit();                     // keeps the command; now undoable
///   }                                    // no commit() -> reverted
///
/// The bare form is the same without the savepoint's automatic rollback:
///   session.begin(); ...; session.end();
class Session {
public:
  /// Starts a new command, optionally labelled so a UI can name the step in the
  /// history. Writing outside begin()/end() is rejected at close().
  /// Starting a command while one is already in progress is rejected: the two
  /// would share the session's stacks and the first command's writes would be
  /// silently dropped.
  void begin(std::string _label = {}) {
    if (m_active) {
      throw std::runtime_error(
          "undo::Session: a command is already in progress");
    }
    m_current = Command{};
    m_current.label = std::move(_label);
    m_open.clear();
    m_active = true;
  }

  /// Sets or replaces the label of the command in progress. Use it when the
  /// name is only known after the work has started.
  void set_label(std::string _label) {
    if (!m_active) {
      throw std::runtime_error("undo::Session: no command in progress");
    }
    m_current.label = std::move(_label);
  }

  /// Ends the command. A command that recorded nothing is dropped; a command
  /// that recorded something clears the redo stack, as a fresh edit must.
  ///
  /// Committing while any object is still open is rejected: a half-open object
  /// has no before/after image to record, so the only alternatives are silently
  /// dropping the writes or committing a half-applied edit. Call close() on
  /// every object first (rollback() closes the stragglers).
  void end() {
    if (!m_open.empty()) {
      throw std::runtime_error(
          "undo::Session: cannot commit while objects are still open");
    }
    if (!m_current.entries.empty()) {
      m_undo.push_back(std::move(m_current));
      m_redo.clear();
    }
    m_current = Command{};
    m_open.clear();
    m_active = false;
  }

  /// A recorded position in the session's history. Everything recorded after a
  /// mark can be reverted with rollback_to(); that is what Transaction wraps.
  struct Mark {
    std::size_t undo_depth;      ///< committed commands on the undo stack
    std::size_t current_entries; ///< entries in the command in progress
  };

  /// Opens a savepoint transaction and returns its mark. Only one may be open
  /// at a time: two would overlap with no defined revert order.
  ///
  /// A transaction is independent of commands. It may start mid-command or span
  /// several, and commit() creates no undo node -- undo nodes stay per-command.
  Mark begin_transaction() {
    if (m_transaction_active) {
      throw std::runtime_error(
          "undo::Session: a transaction is already in progress");
    }
    m_transaction_active = true;
    return Mark{m_undo.size(), m_current.entries.size()};
  }

  /// Closes the savepoint opened by begin_transaction(). Keeps every command
  /// recorded since; the caller keeps the marks it wants to retain.
  void end_transaction() { m_transaction_active = false; }

  /// Reverts every recorded write after `_mark`: aborts any object still open,
  /// restores the entries the command in progress appended after the mark, and
  /// undoes and drops the commands pushed after it. Command lifecycle is left
  /// alone, so a mark taken mid-command lets that command continue.
  void rollback_to(const Mark &_mark) {
    revert_since(_mark.undo_depth, _mark.current_entries);
  }

  /// Reverts the command in progress and discards it; nothing reaches the undo
  /// stack. Objects still open are aborted first -- that undoes their
  /// uncommitted writes and closes them -- and then the committed entries are
  /// replayed from their before image, so every field ends up as it was before
  /// begin(). Aborting before the entries matters when one object was closed
  /// and reopened in the same command: the entry restore must win.
  void rollback() {
    revert_since(m_undo.size(), 0);
    m_current = Command{};
    m_active = false;
  }

  /// True while a command is open (between begin() and end()).
  bool active() const { return m_active; }

  bool can_undo() const { return !m_undo.empty(); }
  bool can_redo() const { return !m_redo.empty(); }

  /// Applied commands, most recent first: front() is what undo() undoes next.
  std::vector<std::string> undo_stack() const {
    std::vector<std::string> out;
    out.reserve(m_undo.size());
    for (auto command = m_undo.rbegin(); command != m_undo.rend(); ++command) {
      out.push_back(command->label);
    }
    return out;
  }

  /// Undone commands, most recent first: front() is what redo() redoes next.
  std::vector<std::string> redo_stack() const {
    std::vector<std::string> out;
    out.reserve(m_redo.size());
    for (auto command = m_redo.rbegin(); command != m_redo.rend(); ++command) {
      out.push_back(command->label);
    }
    return out;
  }

  /// Number of applied commands. This is the target undo_to() / redo_to() take.
  std::size_t undo_depth() const { return m_undo.size(); }
  std::size_t redo_depth() const { return m_redo.size(); }

  /// Restores the fields of the most recent command. Returns false when there
  /// is nothing to undo. Rejected while a command is in progress: the undo
  /// would move fields under the command being recorded and corrupt its before
  /// image.
  bool undo() {
    if (m_active) {
      throw std::runtime_error(
          "undo::Session: cannot undo while a command is in progress");
    }
    if (m_undo.empty()) {
      return false;
    }
    Command command = std::move(m_undo.back());
    m_undo.pop_back();
    for (auto entry = command.entries.rbegin(); entry != command.entries.rend();
         ++entry) {
      entry->object->apply_payload(entry->before);
    }
    m_redo.push_back(std::move(command));
    return true;
  }

  /// Re-applies the fields of the most recently undone command. Returns false
  /// when there is nothing to redo. Rejected while a command is in progress,
  /// for the same reason as undo().
  bool redo() {
    if (m_active) {
      throw std::runtime_error(
          "undo::Session: cannot redo while a command is in progress");
    }
    if (m_redo.empty()) {
      return false;
    }
    Command command = std::move(m_redo.back());
    m_redo.pop_back();
    for (Entry &entry : command.entries) {
      entry.object->apply_payload(entry.after);
    }
    m_undo.push_back(std::move(command));
    return true;
  }

  /// Undoes whole commands until `_depth` commands remain applied, then returns
  /// whether anything changed. undo_to(0) undoes the entire history.
  bool undo_to(std::size_t _depth) {
    bool changed = false;
    while (m_undo.size() > _depth && undo()) {
      changed = true;
    }
    return changed;
  }

  /// Redoes whole commands until `_depth` commands are applied, then returns
  /// whether anything changed. redo_to(undo_depth() + redo_depth()) redoes all.
  bool redo_to(std::size_t _depth) {
    bool changed = false;
    while (m_undo.size() < _depth && redo()) {
      changed = true;
    }
    return changed;
  }

  /// Called by DbObject::close(); appends the object's before/after image to
  /// the command in progress.
  void commit(DbObjectBase &_object, std::string _before, std::string _after) {
    if (!m_active) {
      throw std::runtime_error("undo::Session: close() outside begin()/end()");
    }
    m_current.entries.push_back(
        Entry{&_object, std::move(_before), std::move(_after)});
  }

  /// Called by DbObject::open() / close(): tracks which objects are mid-open so
  /// rollback() can abort the ones an exception left behind.
  void attach_open(DbObjectBase *_object) { m_open.push_back(_object); }

  void detach_open(DbObjectBase *_object) {
    for (auto it = m_open.begin(); it != m_open.end(); ++it) {
      if (*it == _object) {
        m_open.erase(it);
        return;
      }
    }
  }

private:
  /// The one undo primitive: abort open objects, then walk back the entries and
  /// commands recorded after the given position. Aborting first matters when an
  /// object was closed and reopened: the entry restore must win.
  void revert_since(std::size_t _undo_depth, std::size_t _current_entries) {
    for (DbObjectBase *object : m_open) {
      object->abort();
    }
    m_open.clear();
    while (m_current.entries.size() > _current_entries) {
      Entry &entry = m_current.entries.back();
      entry.object->apply_payload(entry.before);
      m_current.entries.pop_back();
    }
    while (m_undo.size() > _undo_depth) {
      Command command = std::move(m_undo.back());
      m_undo.pop_back();
      for (auto entry = command.entries.rbegin();
           entry != command.entries.rend(); ++entry) {
        entry->object->apply_payload(entry->before);
      }
    }
    m_redo.clear();
  }

  struct Entry {
    DbObjectBase *object;
    std::string before;
    std::string after;
  };
  struct Command {
    std::string label;
    std::vector<Entry> entries;
  };

  Command m_current;
  std::vector<Command> m_undo;
  std::vector<Command> m_redo;
  std::vector<DbObjectBase *> m_open;
  bool m_active = false;
  bool m_transaction_active = false;
};

// Out-of-line because they need Session complete, while Session itself only
// needs DbObjectBase.
template <typename Derived> void DbObject<Derived>::open() {
  if (m_open_status != 0) {
    throw std::runtime_error("undo::DbObject is already open");
  }
  m_open_status = 1;
  m_character_stream.clear();
  m_touched.clear();
  m_session->attach_open(this);
}

template <typename Derived> void DbObject<Derived>::close() {
  if (m_open_status == 0) {
    throw std::runtime_error("undo::DbObject is not open");
  }
  m_open_status = 0;
  m_session->detach_open(this);
  if (!m_touched.empty()) {
    m_session->commit(*this, m_character_stream, build_after_payload());
  }
  m_character_stream.clear();
  m_touched.clear();
}

/// A RAII savepoint handle, independent of commands.
///
/// Constructing it marks the session's history. commit() releases the mark,
/// keeping every command recorded since as its own undo node; rollback()
/// reverts everything recorded after the mark. Destroying it without either --
/// which is what an exception causes -- rolls back, so a failed block leaves no
/// partial edit and no object stuck open.
///
/// It does not call begin()/end(); a transaction may start mid-command or span
/// several, and a command is still the unit of an undo node.
class Transaction {
public:
  explicit Transaction(Session &_session)
      : m_session(&_session), m_mark(_session.begin_transaction()) {}

  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;
  Transaction(Transaction &&) = delete;
  Transaction &operator=(Transaction &&) = delete;

  /// Releases the savepoint, keeping the commands recorded since it was taken.
  void commit() {
    if (!m_finished) {
      m_session->end_transaction();
      m_finished = true;
    }
  }

  /// Reverts every write recorded since the savepoint was taken.
  void rollback() {
    if (!m_finished) {
      m_session->rollback_to(m_mark);
      m_session->end_transaction();
      m_finished = true;
    }
  }

  ~Transaction() {
    if (!m_finished) {
      m_session->rollback_to(m_mark);
      m_session->end_transaction();
    }
  }

private:
  Session *m_session;
  Session::Mark m_mark;
  bool m_finished = false;
};

} // namespace undo

/// Declares one backed-up field on an undo::DbObject<Derived> subclass. `type`
/// is the field type, `name` the member name, and the optional third argument
/// its initial value: MEMBER(int, m_field_int, 3) is an int field starting
/// at 3.
///
/// Writes go through the generated touch_##name(); get_##name() reads. Each
/// write records the field's before value once per command, and close() records
/// the after value, so the pair is what undo() and redo() restore.
#define MEMBER(type, name, ...)                                                \
public:                                                                        \
  type &touch_##name() {                                                       \
    if (m_open_status == 0) {                                                  \
      throw std::runtime_error("undo::DbObject is not open");                  \
    }                                                                          \
    using owner_type = std::remove_cvref_t<decltype(*this)>;                   \
    constexpr std::uint32_t kIndex = static_cast<std::uint32_t>(               \
        ::undo::member_index<owner_type>(^^owner_type::name));                 \
    if (this->begin_record(kIndex)) {                                          \
      m_character_stream.append(reinterpret_cast<const char *>(&kIndex),       \
                                sizeof(kIndex));                               \
      ::undo::detail::encode(m_character_stream, name.get());                  \
    }                                                                          \
    return name.touch();                                                       \
  }                                                                            \
  const type &get_##name() const { return name.get(); }                        \
                                                                               \
private:                                                                       \
  ::undo::Member<type> name { __VA_ARGS__ }
