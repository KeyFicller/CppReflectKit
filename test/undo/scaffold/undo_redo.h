#pragma once

#include "undo/reflection_undo.h"

#include <optional>
#include <string>
#include <vector>

enum class Unit { Celsius, Fahrenheit };

struct Nested {
    int m_a = 1;
    std::string m_b = "n";
};

struct DbObject : undo::DbObject<DbObject> {
    explicit DbObject(undo::Session &_session)
        : undo::DbObject<DbObject>(_session) {}

    MEMBER(int, m_int, 3);
    MEMBER(int, m_int2, 4);
    MEMBER(double, m_ratio, 1.5);
    MEMBER(std::string, m_name, "hello");
    MEMBER(std::optional<int>, m_opt);
    MEMBER(std::vector<int>, m_vec);
    MEMBER(Unit, m_unit, Unit::Celsius);
    MEMBER(Nested, m_nested);
};
