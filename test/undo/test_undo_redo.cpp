#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "scaffold/undo_redo.h"

// The whole point of the chapter: reflection finds a member's index and saves or
// restores each field, so undo and redo need no per-type code. One test per type
// the codec dispatches on, plus the session's command semantics.

namespace {

/// Brackets one command around a body that writes through touch_ accessors.
template <typename Fn>
void command(undo::Session& _session, DbObject& _object, Fn&& _body)
{
    _session.begin();
    _object.open();
    _body();
    _object.close();
    _session.end();
}

} // namespace

TEST(UndoRedo, int_field_undo_and_redo)
{
    undo::Session session;
    DbObject obj{session};
    EXPECT_FALSE(session.can_undo());
    EXPECT_FALSE(session.can_redo());

    session.begin();
    obj.open();
    obj.touch_m_int() = 100;
    obj.close();
    session.end();
    EXPECT_EQ(obj.get_m_int(), 100) << "the write lands";

    EXPECT_TRUE(session.undo());
    EXPECT_EQ(obj.get_m_int(), 3) << "undo restores the declared default";

    EXPECT_TRUE(session.redo());
    EXPECT_EQ(obj.get_m_int(), 100) << "redo re-applies the write";
}

TEST(UndoRedo, double_field_round_trips)
{
    undo::Session session;
    DbObject obj{session};

    command(session, obj, [&] { obj.touch_m_ratio() = 9.25; });
    EXPECT_DOUBLE_EQ(obj.get_m_ratio(), 9.25);

    session.undo();
    EXPECT_DOUBLE_EQ(obj.get_m_ratio(), 1.5);
    session.redo();
    EXPECT_DOUBLE_EQ(obj.get_m_ratio(), 9.25);
}

TEST(UndoRedo, string_field_round_trips)
{
    undo::Session session;
    DbObject obj{session};

    command(session, obj, [&] { obj.touch_m_name() = std::string("world"); });
    EXPECT_EQ(obj.get_m_name(), "world");

    session.undo();
    EXPECT_EQ(obj.get_m_name(), "hello");
    session.redo();
    EXPECT_EQ(obj.get_m_name(), "world");
}

TEST(UndoRedo, optional_field_round_trips)
{
    undo::Session session;
    DbObject obj{session};

    command(session, obj, [&] { obj.touch_m_opt() = 7; });
    ASSERT_TRUE(obj.get_m_opt().has_value());
    EXPECT_EQ(*obj.get_m_opt(), 7);

    session.undo();
    EXPECT_FALSE(obj.get_m_opt().has_value());
    session.redo();
    ASSERT_TRUE(obj.get_m_opt().has_value());
    EXPECT_EQ(*obj.get_m_opt(), 7);
}

TEST(UndoRedo, vector_field_round_trips)
{
    undo::Session session;
    DbObject obj{session};

    command(session, obj, [&] { obj.touch_m_vec() = std::vector<int>{1, 2, 3}; });
    EXPECT_EQ(obj.get_m_vec(), (std::vector<int>{1, 2, 3}));

    session.undo();
    EXPECT_TRUE(obj.get_m_vec().empty());
    session.redo();
    EXPECT_EQ(obj.get_m_vec(), (std::vector<int>{1, 2, 3}));
}

TEST(UndoRedo, enum_field_round_trips)
{
    undo::Session session;
    DbObject obj{session};

    command(session, obj, [&] { obj.touch_m_unit() = Unit::Fahrenheit; });
    EXPECT_EQ(obj.get_m_unit(), Unit::Fahrenheit);

    session.undo();
    EXPECT_EQ(obj.get_m_unit(), Unit::Celsius);
    session.redo();
    EXPECT_EQ(obj.get_m_unit(), Unit::Fahrenheit);
}

TEST(UndoRedo, nested_struct_round_trips)
{
    undo::Session session;
    DbObject obj{session};

    command(session, obj, [&] { obj.touch_m_nested().m_a = 42; });
    EXPECT_EQ(obj.get_m_nested().m_a, 42);

    session.undo();
    EXPECT_EQ(obj.get_m_nested().m_a, 1);
    session.redo();
    EXPECT_EQ(obj.get_m_nested().m_a, 42);
}

// A field written twice in one command must undo to its value before the
// command, not to the first value touched.
TEST(UndoRedo, repeated_write_undoes_to_original)
{
    undo::Session session;
    DbObject obj{session};

    command(session, obj, [&] {
        obj.touch_m_int() = 10;
        obj.touch_m_int() = 20;
    });
    EXPECT_EQ(obj.get_m_int(), 20);

    session.undo();
    EXPECT_EQ(obj.get_m_int(), 3);
    session.redo();
    EXPECT_EQ(obj.get_m_int(), 20);
}

TEST(UndoRedo, several_fields_in_one_command)
{
    undo::Session session;
    DbObject obj{session};

    command(session, obj, [&] {
        obj.touch_m_int() = 9;
        obj.touch_m_name() = std::string("world");
    });

    session.undo();
    EXPECT_EQ(obj.get_m_int(), 3);
    EXPECT_EQ(obj.get_m_name(), "hello");
    session.redo();
    EXPECT_EQ(obj.get_m_int(), 9);
    EXPECT_EQ(obj.get_m_name(), "world");
}

TEST(UndoRedo, empty_command_records_nothing)
{
    undo::Session session;
    DbObject obj{session};

    command(session, obj, [] {});
    EXPECT_FALSE(session.can_undo()) << "a command that wrote nothing is dropped";
}

TEST(UndoRedo, empty_history_returns_false)
{
    undo::Session session;
    EXPECT_FALSE(session.undo());
    EXPECT_FALSE(session.redo());
}

TEST(UndoRedo, new_command_clears_redo)
{
    undo::Session session;
    DbObject obj{session};

    command(session, obj, [&] { obj.touch_m_int() = 10; });
    session.undo();
    ASSERT_TRUE(session.can_redo());

    command(session, obj, [&] { obj.touch_m_int() = 30; });
    EXPECT_FALSE(session.can_redo()) << "a fresh edit invalidates the redo stack";
    EXPECT_EQ(obj.get_m_int(), 30);
}

// --- labels and stack introspection ------------------------------------------

TEST(UndoRedo, command_label_is_visible_in_the_stack)
{
    undo::Session session;
    DbObject obj{session};

    command(session, obj, [&] { obj.touch_m_int() = 10; });
    {
        session.begin("Move page");
        obj.open();
        obj.touch_m_int() = 20;
        obj.close();
        session.end();
    }

    auto stack = session.undo_stack();
    ASSERT_EQ(stack.size(), 2u);
    EXPECT_EQ(stack[0], "Move page") << "most recent first";
    EXPECT_EQ(stack[1], "") << "unlabelled commands are empty, not absent";

    ASSERT_TRUE(session.undo());
    auto after = session.undo_stack();
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0], "");
    auto redo = session.redo_stack();
    ASSERT_EQ(redo.size(), 1u);
    EXPECT_EQ(redo[0], "Move page");
}

TEST(UndoRedo, label_can_be_set_mid_command)
{
    undo::Session session;
    DbObject obj{session};

    session.begin();
    session.set_label("Rename");
    obj.open();
    obj.touch_m_int() = 10;
    obj.close();
    session.end();

    ASSERT_EQ(session.undo_stack().size(), 1u);
    EXPECT_EQ(session.undo_stack()[0], "Rename");
}

TEST(UndoRedo, set_label_outside_a_command_is_rejected)
{
    undo::Session session;
    EXPECT_THROW(session.set_label("x"), std::runtime_error);
}

TEST(UndoRedo, undo_to_depth)
{
    undo::Session session;
    DbObject obj{session};

    for (int i = 1; i <= 4; ++i) {
        command(session, obj, [&] { obj.touch_m_int() = i * 10; });
    }
    EXPECT_EQ(session.undo_depth(), 4u);

    EXPECT_TRUE(session.undo_to(2));
    EXPECT_EQ(session.undo_depth(), 2u);
    EXPECT_EQ(obj.get_m_int(), 20);

    EXPECT_FALSE(session.undo_to(2)) << "already at depth 2";
    EXPECT_TRUE(session.undo_to(0));
    EXPECT_EQ(obj.get_m_int(), 3) << "back to the default";
    EXPECT_EQ(session.undo_depth(), 0u);
}

TEST(UndoRedo, redo_to_depth)
{
    undo::Session session;
    DbObject obj{session};

    for (int i = 1; i <= 3; ++i) {
        command(session, obj, [&] { obj.touch_m_int() = i * 10; });
    }
    session.undo_to(0);
    EXPECT_EQ(session.redo_depth(), 3u);

    EXPECT_TRUE(session.redo_to(2));
    EXPECT_EQ(obj.get_m_int(), 20);
    EXPECT_EQ(session.redo_depth(), 1u);

    EXPECT_TRUE(session.redo_to(3));
    EXPECT_EQ(obj.get_m_int(), 30);
    EXPECT_FALSE(session.redo_to(3)) << "nothing left to redo";
}

TEST(UndoRedo, empty_command_is_not_listed)
{
    undo::Session session;
    DbObject obj{session};

    command(session, obj, [&] { obj.touch_m_int() = 5; });
    command(session, obj, [] {});
    EXPECT_EQ(session.undo_stack().size(), 1u) << "a command that wrote nothing is dropped";
}

// --- transactions: savepoints independent of commands ------------------------
// A Transaction marks the history; a command is still the unit of an undo node.

TEST(UndoRedo, transaction_commit_keeps_the_command)
{
    undo::Session session;
    DbObject obj{session};

    {
        undo::Transaction tx{session};
        session.begin();
        obj.open();
        obj.touch_m_int() = 55;
        obj.close();
        session.end();
        tx.commit();
    }

    EXPECT_EQ(obj.get_m_int(), 55);
    ASSERT_TRUE(session.can_undo());
    session.undo();
    EXPECT_EQ(obj.get_m_int(), 3);
}

TEST(UndoRedo, transaction_rollback_reverts_the_command)
{
    undo::Session session;
    DbObject obj{session};

    {
        undo::Transaction tx{session};
        session.begin();
        obj.open();
        obj.touch_m_int() = 55;
        obj.close();
        session.end();
        tx.rollback();
    }

    EXPECT_EQ(obj.get_m_int(), 3) << "rollback restores the before image";
    EXPECT_FALSE(session.can_undo()) << "a rolled-back command never reaches the undo stack";
}

TEST(UndoRedo, transaction_destructor_without_commit_rolls_back)
{
    undo::Session session;
    DbObject obj{session};

    {
        undo::Transaction tx{session};
        session.begin();
        obj.open();
        obj.touch_m_int() = 55;
        obj.close();
        session.end();
        // No commit(): the destructor must roll back.
    }

    EXPECT_EQ(obj.get_m_int(), 3);
    EXPECT_FALSE(session.can_undo());
}

TEST(UndoRedo, transaction_rolls_back_on_exception)
{
    undo::Session session;
    DbObject obj{session};

    EXPECT_THROW(
        {
            undo::Transaction tx{session};
            session.begin();
            obj.open();
            obj.touch_m_int() = 55;
            obj.close();
            session.end();
            throw std::runtime_error("boom");
        },
        std::runtime_error);

    EXPECT_EQ(obj.get_m_int(), 3) << "the throwing transaction left no change behind";
    EXPECT_FALSE(session.can_undo());
}

// A transaction is independent of commands: it can span several, and rollback
// reverts all of them.
TEST(UndoRedo, transaction_spans_multiple_commands)
{
    undo::Session session;
    DbObject obj{session};

    {
        undo::Transaction tx{session};
        session.begin();
        obj.open();
        obj.touch_m_int() = 10;
        obj.close();
        session.end();

        session.begin();
        obj.open();
        obj.touch_m_name() = std::string("world");
        obj.close();
        session.end();

        tx.rollback();
    }

    EXPECT_EQ(obj.get_m_int(), 3) << "the first command was reverted";
    EXPECT_EQ(obj.get_m_name(), "hello") << "the second command was reverted";
    EXPECT_FALSE(session.can_undo());
}

TEST(UndoRedo, transaction_commit_keeps_each_command_as_a_node)
{
    undo::Session session;
    DbObject obj{session};

    {
        undo::Transaction tx{session};
        session.begin();
        obj.open();
        obj.touch_m_int() = 10;
        obj.close();
        session.end();

        session.begin();
        obj.open();
        obj.touch_m_int() = 20;
        obj.close();
        session.end();
        tx.commit();
    }

    EXPECT_EQ(obj.get_m_int(), 20);
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(obj.get_m_int(), 10) << "undo is per command, not per transaction";
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(obj.get_m_int(), 3);
}

// A transaction may start mid-command: only writes after the mark revert, and the
// command carries on.
TEST(UndoRedo, transaction_starts_mid_command)
{
    undo::Session session;
    DbObject obj{session};

    session.begin();
    obj.open();
    obj.touch_m_int() = 10;
    obj.close();
    {
        undo::Transaction tx{session};
        obj.open();
        obj.touch_m_int2() = 99;
        obj.close();
        tx.rollback();
    }
    EXPECT_EQ(obj.get_m_int(), 10) << "the pre-mark write is kept";
    EXPECT_EQ(obj.get_m_int2(), 4) << "the post-mark write is reverted";

    session.end();
    ASSERT_TRUE(session.can_undo());
    session.undo();
    EXPECT_EQ(obj.get_m_int(), 3);
}

// An exception before close() leaves the object open; a rollback must abort it,
// not just drop the (empty) command.
TEST(UndoRedo, transaction_aborts_object_left_open)
{
    undo::Session session;
    DbObject obj{session};

    session.begin();
    EXPECT_THROW(
        {
            undo::Transaction tx{session};
            obj.open();
            obj.touch_m_int() = 55;
            throw std::runtime_error("boom");
        },
        std::runtime_error);

    EXPECT_EQ(obj.get_m_int(), 3) << "the open object is aborted, not left half-written";
    EXPECT_FALSE(obj.is_open());
    session.end();

    // The object is usable again, which it would not be had it stayed open.
    {
        undo::Transaction tx{session};
        session.begin();
        obj.open();
        obj.touch_m_int() = 7;
        obj.close();
        session.end();
        tx.commit();
    }
    EXPECT_EQ(obj.get_m_int(), 7);
}

TEST(UndoRedo, end_rejects_open_object_then_rollback_closes_it)
{
    undo::Session session;
    DbObject obj{session};

    session.begin();
    obj.open();
    obj.touch_m_int() = 100;

    EXPECT_THROW(session.end(), std::runtime_error);
    EXPECT_TRUE(obj.is_open()) << "the rejected commit must not close objects";

    session.rollback();
    EXPECT_EQ(obj.get_m_int(), 3);
    EXPECT_FALSE(obj.is_open());
    EXPECT_FALSE(session.can_undo());
}

// One object in a command can be closed and reopened: the aborted reopen must not
// overwrite the entry restore, so rollback lands on the command's start, not on
// the middle value.
TEST(UndoRedo, rollback_lands_on_command_start_when_object_reopened)
{
    undo::Session session;
    DbObject obj{session};

    session.begin();
    obj.open();
    obj.touch_m_int() = 100;
    obj.close();          // records before = 3
    obj.open();           // reopens; its before image is now 100
    obj.touch_m_int() = 200;
    session.rollback();

    EXPECT_EQ(obj.get_m_int(), 3)
        << "rollback must reach the value before the command, not the value at reopen";
    EXPECT_FALSE(session.can_undo());
}

// While a command is in progress, undo/redo would move fields under the recording
// command and corrupt its before image, so both are rejected.
TEST(UndoRedo, undo_and_redo_are_rejected_mid_command)
{
    undo::Session session;
    DbObject obj{session};
    command(session, obj, [&] { obj.touch_m_int() = 10; });

    session.begin();
    obj.open();
    obj.touch_m_int() = 20;
    EXPECT_THROW(session.undo(), std::runtime_error);
    EXPECT_THROW(session.redo(), std::runtime_error);
    EXPECT_TRUE(session.active()) << "the command is still in progress";
    obj.close();
    session.end();

    // The command recorded cleanly and undoes as usual.
    EXPECT_EQ(obj.get_m_int(), 20);
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(obj.get_m_int(), 10);
}

// Only one command may be in progress at a time: a nested begin() would share the
// session's stacks and silently drop the outer command.
TEST(UndoRedo, nested_command_is_rejected)
{
    undo::Session session;
    DbObject obj{session};

    session.begin();
    EXPECT_THROW(session.begin(), std::runtime_error);

    // The outer command is intact and still usable.
    obj.open();
    obj.touch_m_int() = 42;
    obj.close();
    session.end();
    EXPECT_EQ(obj.get_m_int(), 42);
    session.undo();
    EXPECT_EQ(obj.get_m_int(), 3);
}

// Only one transaction may be open at a time.
TEST(UndoRedo, nested_transaction_is_rejected)
{
    undo::Session session;
    EXPECT_THROW(
        {
            undo::Transaction outer{session};
            undo::Transaction inner{session};
        },
        std::runtime_error);
}

TEST(UndoRedo, nested_transaction_inner_destructor_does_not_end_outer)
{
    undo::Session session;
    DbObject obj{session};

    {
        undo::Transaction outer{session};
        EXPECT_THROW({ undo::Transaction inner{session}; }, std::runtime_error);
        // inner never began, so its destructor must not release outer's savepoint.

        session.begin();
        obj.open();
        obj.touch_m_int() = 42;
        obj.close();
        session.end();
        outer.commit();
    }

    EXPECT_EQ(obj.get_m_int(), 42);
    EXPECT_TRUE(session.can_undo());
}
