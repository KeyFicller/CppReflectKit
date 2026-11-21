#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace undo {
namespace detail {

class DbObjectBase {
public:
    virtual ~DbObjectBase() = default;

    virtual void apply_payload(std::string_view _payload) = 0;
    virtual void abort() = 0;
};

}

/**
 * @brief Records commands and replays their changes through undo and redo.
 * @note Objects referenced by history must outlive any replay of that history.
 */
class Session {
public:
    /**
     * @brief Starts an undoable command.
     * @param _label The label shown in history.
     * @throws std::runtime_error If a command is already active.
     */
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

    /**
     * @brief Updates the label of the active command.
     * @param _label The new label shown in history.
     * @throws std::runtime_error If no command is active.
     */
    void set_label(std::string _label) {
        if (!m_active) {
            throw std::runtime_error("undo::Session: no command in progress");
        }
        m_current.label = std::move(_label);
    }

    /**
     * @brief Commits the active command.
     * @details An empty command leaves history intact.
     * @throws std::runtime_error If an object is still open.
     */
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

    struct Mark {
        /**
         * @brief Number of committed commands on the undo stack.
         */
        std::size_t undo_depth;
        /**
         * @brief Number of entries in the command in progress.
         */
        std::size_t current_entries;
    };

    /**
     * @brief Opens a savepoint that may start mid-command or span several
     * commands.
     * @return The marker to pass to rollback_to().
     * @throws std::runtime_error If a savepoint is already active.
     */
    Mark begin_transaction() {
        if (m_transaction_active) {
            throw std::runtime_error(
                "undo::Session: a transaction is already in progress");
        }
        m_transaction_active = true;
        return Mark{m_undo.size(), m_current.entries.size()};
    }

    /**
     * @brief Closes the savepoint without changing command history.
     */
    void end_transaction() { m_transaction_active = false; }

    /**
     * @brief Reverts writes after the mark while preserving command state.
     * @param _mark Savepoint returned by begin_transaction().
     */
    void rollback_to(const Mark &_mark) {
        revert_since(_mark.undo_depth, _mark.current_entries);
    }

    /**
     * @brief Reverts and discards the command in progress.
     */
    void rollback() {
        revert_since(m_undo.size(), 0);
        m_current = Command{};
        m_active = false;
    }

    /**
     * @brief Reports whether a command is in progress.
     */
    bool active() const { return m_active; }

    /**
     * @brief Reports whether the undo stack contains a command.
     */
    bool can_undo() const { return !m_undo.empty(); }
    /**
     * @brief Reports whether the redo stack contains a command.
     */
    bool can_redo() const { return !m_redo.empty(); }

    /**
     * @brief Returns applied command labels, most recent first.
     */
    std::vector<std::string> undo_stack() const {
        std::vector<std::string> out;
        out.reserve(m_undo.size());
        for (auto command = m_undo.rbegin(); command != m_undo.rend();
             ++command) {
            out.push_back(command->label);
        }
        return out;
    }

    /**
     * @brief Returns undone command labels, most recent first.
     */
    std::vector<std::string> redo_stack() const {
        std::vector<std::string> out;
        out.reserve(m_redo.size());
        for (auto command = m_redo.rbegin(); command != m_redo.rend();
             ++command) {
            out.push_back(command->label);
        }
        return out;
    }

    /**
     * @brief Counts the commands on the undo stack.
     */
    std::size_t undo_depth() const { return m_undo.size(); }
    /**
     * @brief Counts the commands on the redo stack.
     */
    std::size_t redo_depth() const { return m_redo.size(); }

    /**
     * @brief Undoes the most recent applied command.
     * @return Whether a command was undone.
     * @throws std::runtime_error If a command is active.
     */
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
        for (auto entry = command.entries.rbegin();
             entry != command.entries.rend(); ++entry) {
            entry->object->apply_payload(entry->before);
        }
        m_redo.push_back(std::move(command));
        return true;
    }

    /**
     * @brief Reapplies the most recently undone command.
     * @return Whether a command was reapplied.
     * @throws std::runtime_error If a command is active.
     */
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

    /**
     * @brief Undoes commands down to the requested depth.
     * @param _depth Number of commands to leave applied. Pass 0 to undo the
     * entire history.
     * @return Whether any commands were undone.
     */
    bool undo_to(std::size_t _depth) {
        bool changed = false;
        while (m_undo.size() > _depth && undo()) {
            changed = true;
        }
        return changed;
    }

    /**
     * @brief Redoes commands up to the requested depth or until redo is
     * exhausted.
     * @param _depth Target number of applied commands.
     * @return Whether any commands were reapplied.
     */
    bool redo_to(std::size_t _depth) {
        bool changed = false;
        while (m_undo.size() < _depth && redo()) {
            changed = true;
        }
        return changed;
    }

private:
    template <typename Derived> friend class DbObject;

    void record_change(detail::DbObjectBase &_object, std::string _before,
                       std::string _after) {
        if (!m_active) {
            throw std::runtime_error(
                "undo::Session: close() outside begin()/end()");
        }
        m_current.entries.push_back(
            Entry{&_object, std::move(_before), std::move(_after)});
    }

    void attach_open(detail::DbObjectBase *_object) {
        m_open.push_back(_object);
    }

    void detach_open(detail::DbObjectBase *_object) {
        for (auto it = m_open.begin(); it != m_open.end(); ++it) {
            if (*it == _object) {
                m_open.erase(it);
                return;
            }
        }
    }

    void revert_since(std::size_t _undo_depth, std::size_t _current_entries) {
        for (detail::DbObjectBase *object : m_open) {
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
        detail::DbObjectBase *object;
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
    std::vector<detail::DbObjectBase *> m_open;
    bool m_active = false;
    bool m_transaction_active = false;
};

/**
 * @brief A savepoint that rolls back on destruction unless committed.
 */
class Transaction {
public:
    /**
     * @brief Opens a savepoint on a session.
     * @param _session A session that outlives this transaction.
     * @throws std::runtime_error If the session already has a savepoint.
     */
    explicit Transaction(Session &_session)
        : m_session(&_session), m_mark(_session.begin_transaction()) {}

    /**
     * @brief Copy construction is disabled for savepoint handles.
     */
    Transaction(const Transaction &) = delete;
    /**
     * @brief Copy assignment is disabled for savepoint handles.
     */
    Transaction &operator=(const Transaction &) = delete;
    /**
     * @brief Move construction is disabled for savepoint handles.
     */
    Transaction(Transaction &&) = delete;
    /**
     * @brief Move assignment is disabled for savepoint handles.
     */
    Transaction &operator=(Transaction &&) = delete;

    /**
     * @brief Releases the savepoint and keeps the changes.
     */
    void commit() {
        if (!m_finished) {
            m_session->end_transaction();
            m_finished = true;
        }
    }

    /**
     * @brief Reverts the changes since the savepoint and releases it.
     */
    void rollback() {
        if (!m_finished) {
            m_session->rollback_to(m_mark);
            m_session->end_transaction();
            m_finished = true;
        }
    }

    /**
     * @brief Rolls back the savepoint if it is still active.
     */
    ~Transaction() { rollback(); }

private:
    Session *m_session;
    Session::Mark m_mark;
    bool m_finished = false;
};

}
