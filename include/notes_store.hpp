#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct Note {
    int64_t id{};
    int64_t chat_id{};
    std::string text;
    std::optional<int64_t> remind_at;  // unix timestamp, UTC
    bool reminded{};
    int64_t created_at{};
};

// Потокобезопасен: каждый метод берёт внутренний mutex, зовёшь из любого потока.
class NotesStore {
public:
    explicit NotesStore(const std::string& db_path);
    ~NotesStore();

    NotesStore(const NotesStore&) = delete;
    NotesStore& operator=(const NotesStore&) = delete;
    NotesStore(NotesStore&&) = delete;
    NotesStore& operator=(NotesStore&&) = delete;

    int64_t add(int64_t chat_id, const std::string& text,
                std::optional<int64_t> remind_at);
    bool remove(int64_t chat_id, int64_t note_id);
    std::vector<Note> list(int64_t chat_id) const;
    std::vector<Note> dueReminders(int64_t now_unix) const;
    void markReminded(int64_t note_id);

private:
    struct sqlite3* db_{nullptr};
    mutable std::mutex mu_;
};
