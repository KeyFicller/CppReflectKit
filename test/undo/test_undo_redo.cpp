#include <gtest/gtest.h>

#include "scaffold/undo_redo.h"

// The whole point of the chapter: reflection finds a member's index and saves or
// restores each field, so rollback needs no per-type code.
TEST(UndoRedo, rollback_restores_field)
{
    DbObject obj;
    obj.m_object_id = 1;
    gl_object_map[1] = &obj;

    const int data_before = obj.get_m_field_int();
    EXPECT_EQ(data_before, 3) << "the MEMBER macro initialises m_field_int to 3";

    command_start();
    obj.open();
    obj.touch_m_field_int() = 100;
    obj.close();
    command_end();
    EXPECT_EQ(obj.get_m_field_int(), 100) << "touch_m_field_int() writes through";

    command_rollback();
    EXPECT_EQ(obj.get_m_field_int(), data_before)
        << "command_rollback restores the field from its reflected backup";
}
