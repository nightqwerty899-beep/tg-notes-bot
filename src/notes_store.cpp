#include "notes_store.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#include <sqlite3.h>

namespace {

void check(int rc, sqlite3* db, const char* what) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        throw std::runtime_error(std::string(what) + ": " + sqlite3_errmsg(db));
    }
}

// sqlite3_column_text возвращает nullptr на SQLITE_NULL — в std::string это UB.
std::string columnText(sqlite3_stmt* stmt, int col) {
    const auto* p = sqlite3_column_text(stmt, col);
    return p ? std::string(reinterpret_cast<const char*>(p)) : std::string();
}

// Цикл выборки обязан закончиться именно SQLITE_DONE, а не ошибкой.
void checkDone(int rc, sqlite3* db, const char* what) {
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string(what) + ": " + sqlite3_errmsg(db));
    }
}

Note readNote(sqlite3_stmt* stmt) {
    Note n;
    n.id = sqlite3_column_int64(stmt, 0);
    n.chat_id = sqlite3_column_int64(stmt, 1);
    n.text = columnText(stmt, 2);
    if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
        n.remind_at = sqlite3_column_int64(stmt, 3);
    }
    n.reminded = sqlite3_column_int(stmt, 4) != 0;
    n.created_at = sqlite3_column_int64(stmt, 5);
    return n;
}

}  // namespace

NotesStore::NotesStore(const std::string& db_path) {
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(db_path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        const std::string msg = db_ ? sqlite3_errmsg(db_) : "unknown";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error("Cannot open database: " + db_path + ": " + msg);
    }

    sqlite3_busy_timeout(db_, 5000);

    const char* sql = R"SQL(
        PRAGMA journal_mode = WAL;
        CREATE TABLE IF NOT EXISTS notes (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            chat_id INTEGER NOT NULL,
            text TEXT NOT NULL,
            remind_at INTEGER,
            reminded INTEGER NOT NULL DEFAULT 0,
            created_at INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_notes_chat ON notes(chat_id);
        CREATE INDEX IF NOT EXISTS idx_notes_remind ON notes(remind_at, reminded);
    )SQL";

    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("Schema error: " + msg);
    }
}

NotesStore::~NotesStore() {
    std::lock_guard<std::mutex> lock(mu_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

int64_t NotesStore::add(int64_t chat_id, const std::string& text,
                        std::optional<int64_t> remind_at) {
    const char* sql =
        "INSERT INTO notes(chat_id, text, remind_at, reminded, created_at) "
        "VALUES(?, ?, ?, 0, strftime('%s','now'))";

    std::lock_guard<std::mutex> lock(mu_);

    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "prepare add");

    sqlite3_bind_int64(stmt, 1, chat_id);
    sqlite3_bind_text(stmt, 2, text.c_str(), -1, SQLITE_TRANSIENT);
    if (remind_at) {
        sqlite3_bind_int64(stmt, 3, *remind_at);
    } else {
        sqlite3_bind_null(stmt, 3);
    }

    checkDone(sqlite3_step(stmt), db_, "insert note");
    const int64_t id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(stmt);
    return id;
}

bool NotesStore::remove(int64_t chat_id, int64_t note_id) {
    const char* sql = "DELETE FROM notes WHERE id = ? AND chat_id = ?";

    std::lock_guard<std::mutex> lock(mu_);

    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "prepare del");
    sqlite3_bind_int64(stmt, 1, note_id);
    sqlite3_bind_int64(stmt, 2, chat_id);
    checkDone(sqlite3_step(stmt), db_, "delete note");
    const bool ok = sqlite3_changes(db_) > 0;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<Note> NotesStore::list(int64_t chat_id) const {
    const char* sql =
        "SELECT id, chat_id, text, remind_at, reminded, created_at "
        "FROM notes WHERE chat_id = ? ORDER BY id DESC LIMIT 50";

    std::lock_guard<std::mutex> lock(mu_);

    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "prepare list");
    sqlite3_bind_int64(stmt, 1, chat_id);

    std::vector<Note> notes;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        notes.push_back(readNote(stmt));
    }
    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error(std::string("list: ") + sqlite3_errmsg(db_));
    }
    sqlite3_finalize(stmt);
    return notes;
}

std::vector<Note> NotesStore::dueReminders(int64_t now_unix) const {
    const char* sql =
        "SELECT id, chat_id, text, remind_at, reminded, created_at "
        "FROM notes "
        "WHERE reminded = 0 AND remind_at IS NOT NULL AND remind_at <= ? "
        "ORDER BY remind_at ASC LIMIT 100";

    std::lock_guard<std::mutex> lock(mu_);

    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "prepare due");
    sqlite3_bind_int64(stmt, 1, now_unix);

    std::vector<Note> notes;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        notes.push_back(readNote(stmt));
    }
    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error(std::string("due reminders: ") + sqlite3_errmsg(db_));
    }
    sqlite3_finalize(stmt);
    return notes;
}

void NotesStore::markReminded(int64_t note_id) {
    const char* sql = "UPDATE notes SET reminded = 1 WHERE id = ?";

    std::lock_guard<std::mutex> lock(mu_);

    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "prepare mark");
    sqlite3_bind_int64(stmt, 1, note_id);
    checkDone(sqlite3_step(stmt), db_, "mark reminded");
    sqlite3_finalize(stmt);
}
