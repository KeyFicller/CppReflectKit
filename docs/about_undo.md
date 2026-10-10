# Undo/redo from reflection

Derive a database object from `undo::DbObject<Derived>` and declare each field with
`MEMBER()`; undo and redo then need no per-type code, and a field written twice still
undoes to its original. The one idea: **turn a field into a compile-time index plus a
codec-able byte payload, so capture and restore are two reflection walks over one codec.**
Build prerequisites: [`../ReadMe.md`](../ReadMe.md).

## The deal

```cpp
// What you already have:
struct Document {
    int m_page = 0;
    std::string m_title;
    std::optional<Unit> m_unit;
    std::vector<int> m_marks;
};
```

```cpp
// What you add — a base and a marker per field:
struct Document : undo::DbObject<Document> {
    explicit Document(undo::Session& session) : undo::DbObject<Document>(session) {}

    MEMBER(int, m_page, 0);
    MEMBER(std::string, m_title);
    MEMBER(std::optional<Unit>, m_unit);
    MEMBER(std::vector<int>, m_marks);
};
```

Writes go through the generated `touch_m_page()`, reads through `get_m_page()`. Bracket
one user operation with `begin()` / `end()` and open/close each object inside it:

```cpp
undo::Session session;
Document doc{session};
session.begin("Move page");
doc.open();
doc.touch_m_page() = 7;
doc.touch_m_title() = std::string("Chapter one");
doc.close();
session.end();
session.undo();   // m_page is 0, m_title is "" again
session.redo();   // both are back
```

## How the derivation works

**1. `MEMBER()` is the registration.** The base is CRTP; it scans the derived class's
members by reflection, so there is no registry, no static initializer, no per-type
code. `MEMBER(type, name, …)` generates the accessors and, on the first write in a
command, appends the field's before value to the object's stream:

```cpp
#define MEMBER(type, name, ...)                                                          \
public:                                                                                  \
    type& touch_##name()                                                                 \
    {                                                                                    \
        using owner_type = std::remove_cvref_t<decltype(*this)>;                         \
        constexpr std::uint32_t kIndex =                                                 \
            static_cast<std::uint32_t>(::undo::member_index<owner_type>(^^owner_type::name)); \
        if (this->begin_record(kIndex)) {                                                \
            m_character_stream.append(reinterpret_cast<const char*>(&kIndex), sizeof(kIndex)); \
            ::undo::detail::encode(m_character_stream, name.get());                      \
        }                                                                                \
        return name.touch();                                                             \
    }                                                                                    \
    const type& get_##name() const { return name.get(); }                                \
private:                                                                                 \
    ::undo::Member<type> name{__VA_ARGS__}
```

**2. One member index, `consteval`.** A payload names fields by position, computed once
at compile time — the same key in the before image, the after image and `apply_payload`:

```cpp
template <typename T> consteval std::size_t member_index(std::meta::info target);
// MEMBER(int, m_page, 0) => ^^Document::m_page has index 0
```

**3. One codec, type-dispatched.** A field is encoded as `<index:uint32><payload>`, and
`encode` / `decode` recurse on the type, so the same pair covers every field:

```cpp
template <typename T> void encode(std::string& _out, const T& _value);
template <typename T> void decode(std::string_view _in, std::size_t& _offset, T& _out);
```

**4. Restoring is one reflection walk.** The base never names a field; it walks the
derived type's members and matches by index:

```cpp
void apply_payload(std::string_view _payload) override
{
    auto& self = static_cast<Derived&>(*this);
    constexpr auto fields = std::define_static_array(
        std::meta::nonstatic_data_members_of(^^Derived, std::meta::access_context::unchecked()));
    // read <index, value>, find the field whose member_index matches, decode into it
    self.[:field:].touch() = std::move(value);
}
```

One reflection pass feeds the capture; the same reflections drive the restore, so a
declared field cannot be forgotten and a payload cannot name a field that is not
declared.

## What is derived, per declaration

| From one `MEMBER(type, name, …)` | Projection |
|---|---|
| the macro body | `touch_name()` (record once per command) and `get_name()` |
| `member_index<Derived>` | the field's **key** in the before and after images |
| `detail::encode` / `detail::decode` | the field's **bytes**, recursing into the type |

The codec is the only place that knows the type:

| Field type | Encoded as |
|---|---|
| `bool` | one byte |
| integral / floating-point | raw `sizeof(T)` bytes |
| `enum` | its underlying type |
| `std::string` | `uint32` length + bytes |
| `std::optional<T>` | presence byte + `T` when present |
| `std::vector<T>` | `uint32` count + each element |
| `std::array<T, N>` | `N` elements |
| nested reflected struct | each of its members, in order |

Anything else is a `static_assert` naming the supported set. Payloads are host-endian
and replayed in the same process — a capture format, not a persistence format.

## Consuming it

A **command** is the unit of undo; a **transaction** is a *savepoint* on the history,
independent of commands.

| | Command | Transaction |
|---|---|---|
| Brackets | `begin()` / `end()` | `begin_transaction()` / `commit()` / `rollback_to()` |
| Unit of | the undo/redo stacks | a savepoint (`Mark`) |
| Relation | — | 1 transaction : N commands, or part of one |
| On commit | pushes an undo node | releases the mark, adds no node |
| On rollback | drops the command in progress | reverts the command tail, then undoes and drops later commands |
| May start | at a command boundary | mid-command |

Both rollbacks share one primitive: abort any open object, then walk the entries and
commands back from the mark. `undo::Transaction` is the RAII handle — construct to take
the mark, `commit()` to keep, destroy without either to roll back. A command that wrote
nothing is dropped; committing with an object still open, or calling `undo()` / `redo()`
mid-command, is rejected. A mark taken mid-command reverts to that command's start when
it covers a field already written in the command — there is no second snapshot.

Pass a label to `begin()` (or `set_label()` later); `undo_stack()` / `redo_stack()`
return labels, most recent first. `undo_depth()` / `redo_depth()` count applied and
undone commands, and `undo_to()` / `redo_to()` undo or redo whole commands until that
depth is reached (both return whether anything changed).

The example object lives in `test/undo/scaffold/undo_redo.h`; the library is
`lib/inc/undo/reflection_undo.h`, header-only, tested in `test/undo/test_undo_redo.cpp`.
