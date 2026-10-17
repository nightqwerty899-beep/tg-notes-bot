#include "notes_store.hpp"
#include "telegram_client.hpp"

#include <curl/curl.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <algorithm>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* BTN_NEW = "📝 Новая заметка";
constexpr const char* BTN_LIST = "📋 Мои заметки";
constexpr const char* BTN_HELP = "❓ Помощь";
constexpr const char* BTN_CANCEL = "❌ Отмена";

constexpr int kPollTimeoutSec = 25;
constexpr int64_t kMaxRemindMinutes = 60 * 24 * 30;  // 30 суток
constexpr size_t kMaxSessions = 5000;
constexpr auto kSessionTtl = std::chrono::hours(1);

volatile std::sig_atomic_t g_stop = 0;

extern "C" void onSignal(int) { g_stop = 1; }

bool stopping() { return g_stop != 0; }

// Спит кусками, чтобы Ctrl+C не ждал полный интервал.
void interruptibleSleep(int ms) {
    for (int slept = 0; slept < ms && !stopping(); slept += 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

enum class Phase {
    Idle,
    WaitingNote,
    WaitingRemindChoice,
    WaitingMinutes,
};

struct Session {
    Phase phase{Phase::Idle};
    std::string pending_text;
    std::chrono::steady_clock::time_point touched{std::chrono::steady_clock::now()};
};

std::mutex sessions_mu;
std::unordered_map<int64_t, Session> sessions;

// Ниже все обращения к sessions — только под sessions_mu и без сетевых вызовов.

void setSession(int64_t chat_id, Phase phase, std::string text) {
    std::lock_guard<std::mutex> lock(sessions_mu);
    auto& s = sessions[chat_id];
    s.phase = phase;
    s.pending_text = std::move(text);
    s.touched = std::chrono::steady_clock::now();
}

void setPhase(int64_t chat_id, Phase phase) {
    std::lock_guard<std::mutex> lock(sessions_mu);
    auto& s = sessions[chat_id];
    s.phase = phase;
    s.touched = std::chrono::steady_clock::now();
}

void resetSession(int64_t chat_id) { setSession(chat_id, Phase::Idle, {}); }

Session snapshot(int64_t chat_id) {
    std::lock_guard<std::mutex> lock(sessions_mu);
    auto it = sessions.find(chat_id);
    if (it == sessions.end()) {
        return Session{};
    }
    it->second.touched = std::chrono::steady_clock::now();
    return it->second;
}

// Подчищаем чаты, которые молчали дольше часа. Без этого карта растёт
// бесконечно.
void pruneSessions() {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(sessions_mu);
    if (sessions.size() < kMaxSessions) {
        return;
    }
    for (auto it = sessions.begin(); it != sessions.end();) {
        if (now - it->second.touched > kSessionTtl) {
            it = sessions.erase(it);
        } else {
            ++it;
        }
    }
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

// Всё, что вставляется в parse_mode=HTML, должно пройти через это.
std::string escapeHtml(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
        }
    }
    return out;
}

std::optional<std::string> loadEnvFile(const std::string& path, const std::string& key) {
    std::ifstream in(path);
    if (!in) {
        return std::nullopt;
    }
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        auto k = trim(line.substr(0, eq));
        auto v = trim(line.substr(eq + 1));
        if (!v.empty() && ((v.front() == '"' && v.back() == '"') ||
                           (v.front() == '\'' && v.back() == '\''))) {
            v = v.substr(1, v.size() - 2);
        }
        if (k == key) {
            return v;
        }
    }
    return std::nullopt;
}

std::string requireToken() {
    if (const char* env = std::getenv("BOT_TOKEN"); env && *env) {
        return env;
    }
    if (auto from_file = loadEnvFile(".env", "BOT_TOKEN")) {
        return *from_file;
    }
    if (auto from_file = loadEnvFile("config.env", "BOT_TOKEN")) {
        return *from_file;
    }
    throw std::runtime_error("Не найден BOT_TOKEN. Создай файл .env с BOT_TOKEN=...");
}

std::string mainKeyboard() {
    return R"({
      "keyboard": [
        [{"text": "📝 Новая заметка"}],
        [{"text": "📋 Мои заметки"}, {"text": "❓ Помощь"}]
      ],
      "resize_keyboard": true,
      "is_persistent": true
    })";
}

std::string yesNoKeyboard() {
    return R"({
      "inline_keyboard": [
        [
          {"text": "✅ Да", "callback_data": "remind_yes"},
          {"text": "❌ Нет", "callback_data": "remind_no"}
        ]
      ]
    })";
}

std::string minutesKeyboard() {
    return R"({
      "inline_keyboard": [
        [
          {"text": "5 мин", "callback_data": "min_5"},
          {"text": "15 мин", "callback_data": "min_15"},
          {"text": "30 мин", "callback_data": "min_30"}
        ],
        [
          {"text": "1 час", "callback_data": "min_60"},
          {"text": "2 часа", "callback_data": "min_120"},
          {"text": "❌ Отмена", "callback_data": "remind_cancel"}
        ]
      ]
    })";
}

std::string helpText() {
    return
        "<b>Заметки с напоминаниями</b>\n\n"
        "Нажми <b>📝 Новая заметка</b> — бот спросит текст, а потом время "
        "напоминания (кнопками или своим вводом).\n\n"
        "<b>Команды</b>\n"
        "/add текст — заметка без напоминания\n"
        "/remind 30 купить хлеб — через 30 минут\n"
        "/remind 12:35 позвонить маме — сегодня в 12:35\n"
        "/remind 2ч30м выйти из дома — через 2 часа 30 минут\n"
        "/list — список заметок\n"
        "/del ID — удалить заметку #ID\n\n"
        "<b>Время можно вводить по-разному</b>\n"
        "<code>30</code> — через 30 минут\n"
        "<code>90</code> или <code>1ч30м</code> — через полтора часа\n"
        "<code>12:35</code> или <code>9:05</code> — сегодня в это время "
        "(если время уже прошло — завтра)\n"
        "<code>23:59</code> — поздно вечером сегодня\n\n"
        "Команды работают в любой момент, даже посреди диалога.";
}

// 12:35 -> "12:35", с учётом местной таймзоны.
std::string formatClock(int64_t ts) {
    const std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
    if (!localtime_r(&t, &tm)) {
        return "?";
    }
    std::ostringstream ss;
    ss << std::put_time(&tm, "%H:%M");
    return ss.str();
}

std::string formatStamp(int64_t ts) {
    const std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
    if (!localtime_r(&t, &tm)) {
        return "?";
    }
    std::ostringstream ss;
    ss << std::put_time(&tm, "%d.%m %H:%M");
    return ss.str();
}

std::string humanMinutes(int64_t minutes) {
    const int64_t days = minutes / (60 * 24);
    const int64_t hours = (minutes % (60 * 24)) / 60;
    const int64_t mins = minutes % 60;

    std::string out;
    if (days) {
        out += std::to_string(days) + (days == 1 ? " день" : " дн");
    }
    if (hours) {
        if (!out.empty()) out += " ";
        out += std::to_string(hours) + (hours == 1 ? " час" : " ч");
    }
    if (mins || out.empty()) {
        if (!out.empty()) out += " ";
        out += std::to_string(mins) + " мин";
    }
    return out;
}

std::string formatNote(const Note& n) {
    std::ostringstream ss;
    ss << "<b>#" << n.id << "</b> " << escapeHtml(n.text);
    if (n.remind_at) {
        ss << "\n⏰ " << (n.reminded ? "напомнил " : "ожидает ") << formatStamp(*n.remind_at);
    }
    return ss.str();
}

struct Command {
    std::string name;
    std::string args;
};

Command parseCommand(const std::string& text) {
    Command cmd;
    if (text.empty() || text[0] != '/') {
        return cmd;
    }
    std::string body = text.substr(1);
    const auto at = body.find('@');
    const auto sp = body.find(' ');
    if (at != std::string::npos && (sp == std::string::npos || at < sp)) {
        body.erase(at, (sp == std::string::npos ? body.size() : sp) - at);
    }
    const auto space = body.find(' ');
    if (space == std::string::npos) {
        cmd.name = body;
    } else {
        cmd.name = body.substr(0, space);
        cmd.args = trim(body.substr(space + 1));
    }
    for (char& c : cmd.name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return cmd;
}

// --- Разбор времени ---------------------------------------------------------

struct RemindSpec {
    bool is_clock{false};
    int64_t minutes{0};  // когда is_clock == false: сколько минут от сейчас
    int hour{0};         // когда is_clock == true
    int minute{0};
    int second{0};
};

struct ResolvedRemind {
    int64_t at{};
    bool tomorrow{false};
};

bool parseNumber(const std::string& s, long& out) {
    if (s.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (errno == ERANGE || end == s.c_str() || *end != '\0') {
        return false;
    }
    out = v;
    return true;
}

// Приводит кириллические М/Ч/Д и латиницу к нижнему регистру: в локали "C"
// isalpha() на UTF-8 байтах не срабатывает.
std::string lowerUnit(std::string s) {
    static const std::pair<const char*, const char*> kCyrLower[] = {
        {"\xD0\x9C", "\xD0\xBC"},  // М -> м
        {"\xD0\xA7", "\xD1\x87"},  // Ч -> ч
        {"\xD0\x94", "\xD0\xB4"},  // Д -> д
    };
    for (const auto& [from, to] : kCyrLower) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, 2, to);
            pos += 2;
        }
    }
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

// 12:35 / 12.35 / 12:35:00 — время суток в местной таймзоне.
std::optional<RemindSpec> parseClockTime(const std::string& raw) {
    std::string norm;
    for (const char c : raw) {
        norm += (c == '.' || c == ':') ? ':' : c;
    }

    std::vector<std::string> parts;
    std::string cur;
    for (const char c : norm) {
        if (c == ':') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    if (parts.size() < 2 || parts.size() > 3) {
        return std::nullopt;
    }

    long h = 0;
    long m = 0;
    long s = 0;
    if (!parseNumber(parts[0], h) || !parseNumber(parts[1], m)) {
        return std::nullopt;
    }
    if (parts.size() == 3 && !parseNumber(parts[2], s)) {
        return std::nullopt;
    }
    if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) {
        return std::nullopt;
    }

    RemindSpec spec;
    spec.is_clock = true;
    spec.hour = static_cast<int>(h);
    spec.minute = static_cast<int>(m);
    spec.second = static_cast<int>(s);
    return spec;
}

// 30 / 30мин / 2ч / 1ч30м / 1h30m / 1д — длительность.
std::optional<RemindSpec> parseDuration(const std::string& raw) {
    int64_t total_minutes = 0;
    bool any = false;
    size_t i = 0;

    while (i < raw.size()) {
        if (raw[i] == ' ') {
            ++i;
            continue;
        }

        const size_t num_start = i;
        while (i < raw.size() && std::isdigit(static_cast<unsigned char>(raw[i]))) {
            ++i;
        }
        if (i == num_start) {
            return std::nullopt;  // не число там, где ждали число
        }
        long value = 0;
        if (!parseNumber(raw.substr(num_start, i - num_start), value)) {
            return std::nullopt;
        }

        while (i < raw.size() && raw[i] == ' ') {
            ++i;
        }
        const size_t unit_start = i;
        while (i < raw.size() && raw[i] != ' ' && raw[i] != ':' && raw[i] != '.' &&
               std::isdigit(static_cast<unsigned char>(raw[i])) == 0) {
            ++i;
        }
        const std::string unit = lowerUnit(raw.substr(unit_start, i - unit_start));

        int64_t multiplier = 0;
        if (unit.empty() || unit == "м" || unit == "мин" || unit == "минут" ||
            unit == "мин." || unit == "м." || unit == "m" || unit == "min") {
            multiplier = 1;
        } else if (unit == "ч" || unit == "час" || unit == "часа" || unit == "час." ||
                   unit == "h" || unit == "hr") {
            multiplier = 60;
        } else if (unit == "д" || unit == "дн" || unit == "день" || unit == "дня" ||
                   unit == "d" || unit == "day") {
            multiplier = 60 * 24;
        } else {
            return std::nullopt;
        }

        if (value < 0) {
            return std::nullopt;
        }
        total_minutes += static_cast<int64_t>(value) * multiplier;
        any = true;
    }

    if (!any || total_minutes <= 0) {
        return std::nullopt;
    }
    RemindSpec spec;
    spec.minutes = total_minutes;
    return spec;
}

// Точка входа для всех вариантов ввода: и «12:35», и «2ч30м».
std::optional<RemindSpec> parseRemindSpec(const std::string& raw) {
    const std::string s = trim(raw);
    if (s.empty()) {
        return std::nullopt;
    }
    if (s.find(':') != std::string::npos || s.find('.') != std::string::npos) {
        return parseClockTime(s);
    }
    return parseDuration(s);
}

// Для времени суток: сегодня, а если уже позади — завтра.
std::optional<ResolvedRemind> resolveRemind(const RemindSpec& spec, int64_t now) {
    ResolvedRemind r;
    if (!spec.is_clock) {
        if (spec.minutes > kMaxRemindMinutes) {
            return std::nullopt;
        }
        r.at = now + spec.minutes * 60;
        return r;
    }

    std::time_t base = static_cast<std::time_t>(now);
    std::tm tm{};
    if (!localtime_r(&base, &tm)) {
        return std::nullopt;
    }
    tm.tm_hour = spec.hour;
    tm.tm_min = spec.minute;
    tm.tm_sec = spec.second;
    tm.tm_isdst = -1;

    std::time_t target = std::mktime(&tm);
    if (target == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }
    if (target <= now) {
        std::tm next{};
        if (!localtime_r(&target, &next)) {
            return std::nullopt;
        }
        next.tm_mday += 1;
        next.tm_isdst = -1;
        target = std::mktime(&next);
        if (target == static_cast<std::time_t>(-1)) {
            return std::nullopt;
        }
        r.tomorrow = true;
    }
    r.at = static_cast<int64_t>(target);
    return r;
}

int64_t nowUnix() {
    return static_cast<int64_t>(std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now()));
}

void remindUsage(TelegramClient& tg, int64_t chat_id) {
    tg.sendMessage(chat_id,
                   "Не понял время. Подойдёт <code>30</code> (минут), "
                   "<code>2ч30м</code> или время суток <code>12:35</code>.",
                   mainKeyboard());
}

// --- Сохранение заметок ------------------------------------------------------

void saveNoteWithoutRemind(TelegramClient& tg, NotesStore& store, int64_t chat_id,
                           const std::string& text) {
    const int64_t id = store.add(chat_id, text, std::nullopt);
    resetSession(chat_id);
    tg.sendMessage(chat_id,
                   "Заметка <b>#" + std::to_string(id) + "</b> записана без напоминания.",
                   mainKeyboard());
}

bool saveNoteWithRemind(TelegramClient& tg, NotesStore& store, int64_t chat_id,
                        const std::string& text, const RemindSpec& spec) {
    const auto resolved = resolveRemind(spec, nowUnix());
    if (!resolved) {
        tg.sendMessage(chat_id,
                       "Слишком большой срок. Максимум — 30 дней, "
                       "напоминание по часам: <code>12:35</code>.",
                       mainKeyboard());
        return false;
    }

    const int64_t id = store.add(chat_id, text, resolved->at);
    resetSession(chat_id);

    const std::string when =
        spec.is_clock
            ? std::string(resolved->tomorrow ? "завтра" : "сегодня") + " в " +
                  formatClock(resolved->at)
            : "через " + humanMinutes(spec.minutes);

    tg.sendMessage(chat_id,
                   "Готово! Заметка <b>#" + std::to_string(id) + "</b> сохранена.\n"
                   "⏰ Напомню " + when + ".",
                   mainKeyboard());
    return true;
}

// --- Диалог -----------------------------------------------------------------

void sendWelcome(TelegramClient& tg, int64_t chat_id) {
    resetSession(chat_id);
    tg.sendMessage(chat_id,
                   "Привет! Я бот для заметок.\nВыбери действие кнопкой ниже 👇",
                   mainKeyboard());
}

void startNewNote(TelegramClient& tg, int64_t chat_id) {
    setSession(chat_id, Phase::WaitingNote, {});
    tg.sendMessage(chat_id,
                   "<b>Сделайте заметку:</b>\nНапишите текст следующим сообщением.",
                   mainKeyboard());
}

void showList(TelegramClient& tg, NotesStore& store, int64_t chat_id) {
    const auto notes = store.list(chat_id);
    if (notes.empty()) {
        tg.sendMessage(chat_id, "Пока пусто. Нажми «📝 Новая заметка».", mainKeyboard());
        return;
    }
    std::ostringstream ss;
    ss << "<b>Твои заметки</b>\n\n";
    for (const auto& n : notes) {
        ss << formatNote(n) << "\n\n";
    }
    tg.sendMessage(chat_id, ss.str(), mainKeyboard());
}

void handleCallback(TelegramClient& tg, NotesStore& store, const TgUpdate& upd) {
    tg.answerCallback(upd.callback_id);

    const Session snap = snapshot(upd.chat_id);
    const bool has_text = !snap.pending_text.empty();

    if (upd.text == "remind_cancel") {
        if (has_text) {
            saveNoteWithoutRemind(tg, store, upd.chat_id, snap.pending_text);
        } else {
            resetSession(upd.chat_id);
            tg.sendMessage(upd.chat_id, "Отменено.", mainKeyboard());
        }
        return;
    }

    if (upd.text == "remind_no") {
        if (snap.phase != Phase::WaitingRemindChoice || !has_text) {
            resetSession(upd.chat_id);
            tg.sendMessage(upd.chat_id, "Сессия устарела. Начни заново: «📝 Новая заметка».",
                           mainKeyboard());
            return;
        }
        saveNoteWithoutRemind(tg, store, upd.chat_id, snap.pending_text);
        return;
    }

    if (upd.text == "remind_yes") {
        if (snap.phase != Phase::WaitingRemindChoice || !has_text) {
            resetSession(upd.chat_id);
            tg.sendMessage(upd.chat_id, "Сессия устарела. Начни заново: «📝 Новая заметка».",
                           mainKeyboard());
            return;
        }
        setPhase(upd.chat_id, Phase::WaitingMinutes);
        tg.sendMessage(upd.chat_id,
                       "Через сколько напомнить?\n"
                       "Выбери кнопку или напиши своё: <code>30</code> (минут), "
                       "<code>2ч30м</code> либо время суток <code>12:35</code>.",
                       minutesKeyboard());
        return;
    }

    if (upd.text.rfind("min_", 0) == 0) {
        if (snap.phase != Phase::WaitingMinutes || !has_text) {
            resetSession(upd.chat_id);
            tg.sendMessage(upd.chat_id, "Сессия устарела. Начни заново: «📝 Новая заметка».",
                           mainKeyboard());
            return;
        }
        long minutes = 0;
        if (!parseNumber(upd.text.substr(4), minutes)) {
            remindUsage(tg, upd.chat_id);
            return;
        }
        RemindSpec spec;
        spec.minutes = minutes;
        saveNoteWithRemind(tg, store, upd.chat_id, snap.pending_text, spec);
        return;
    }
}

void handleCommand(TelegramClient& tg, NotesStore& store, int64_t chat_id,
                   const Command& cmd) {
    if (cmd.name == "start") {
        sendWelcome(tg, chat_id);
        return;
    }
    if (cmd.name == "help") {
        tg.sendMessage(chat_id, helpText(), mainKeyboard());
        return;
    }
    if (cmd.name == "new") {
        startNewNote(tg, chat_id);
        return;
    }
    if (cmd.name == "add") {
        if (cmd.args.empty()) {
            startNewNote(tg, chat_id);
            return;
        }
        setSession(chat_id, Phase::WaitingRemindChoice, cmd.args);
        tg.sendMessage(chat_id,
                       "Заметка записана ✅\nХватит добавить напоминание?",
                       yesNoKeyboard());
        return;
    }
    if (cmd.name == "remind") {
        std::istringstream iss(cmd.args);
        std::string when;
        iss >> when;
        std::string note;
        std::getline(iss, note);
        note = trim(note);
        if (when.empty() || note.empty()) {
            tg.sendMessage(chat_id,
                           "Примеры:\n"
                           "<code>/remind 30 купить молоко</code>\n"
                           "<code>/remind 12:35 позвонить маме</code>\n"
                           "<code>/remind 2ч30м выйти из дома</code>",
                           mainKeyboard());
            return;
        }
        const auto spec = parseRemindSpec(when);
        if (!spec) {
            remindUsage(tg, chat_id);
            return;
        }
        saveNoteWithRemind(tg, store, chat_id, note, *spec);
        return;
    }
    if (cmd.name == "list") {
        showList(tg, store, chat_id);
        return;
    }
    if (cmd.name == "del") {
        long id = 0;
        if (!parseNumber(cmd.args, id) || id <= 0) {
            tg.sendMessage(chat_id, "Использование: <code>/del ID</code>", mainKeyboard());
            return;
        }
        if (store.remove(chat_id, id)) {
            tg.sendMessage(chat_id, "Удалено <b>#" + std::to_string(id) + "</b>",
                           mainKeyboard());
        } else {
            tg.sendMessage(chat_id, "Не нашёл заметку #" + std::to_string(id),
                           mainKeyboard());
        }
        return;
    }

    tg.sendMessage(chat_id, "Не знаю команду <code>/" + escapeHtml(cmd.name) +
                                "</code>\nНажми /help",
                   mainKeyboard());
}

void handleMessage(TelegramClient& tg, NotesStore& store, const TgUpdate& msg) {
    const std::string& text = msg.text;

    if (text == BTN_NEW) {
        startNewNote(tg, msg.chat_id);
        return;
    }
    if (text == BTN_LIST) {
        showList(tg, store, msg.chat_id);
        return;
    }
    if (text == BTN_HELP) {
        tg.sendMessage(msg.chat_id, helpText(), mainKeyboard());
        return;
    }
    if (text == BTN_CANCEL) {
        resetSession(msg.chat_id);
        tg.sendMessage(msg.chat_id, "Отменено.", mainKeyboard());
        return;
    }

    // Команды работают всегда, даже посреди диалога.
    const Command cmd = parseCommand(text);
    if (!cmd.name.empty()) {
        handleCommand(tg, store, msg.chat_id, cmd);
        return;
    }

    const Session snap = snapshot(msg.chat_id);

    if (snap.phase == Phase::WaitingNote) {
        if (trim(text).empty()) {
            tg.sendMessage(msg.chat_id, "Пришли обычный текст заметки (или «❌ Отмена»).");
            return;
        }
        setSession(msg.chat_id, Phase::WaitingRemindChoice, text);
        tg.sendMessage(msg.chat_id,
                       "Заметка записана ✅\nХватит добавить напоминание?",
                       yesNoKeyboard());
        return;
    }

    if (snap.phase == Phase::WaitingMinutes) {
        const auto spec = parseRemindSpec(text);
        if (!spec) {
            remindUsage(tg, msg.chat_id);
            return;
        }
        if (snap.pending_text.empty()) {
            resetSession(msg.chat_id);
            tg.sendMessage(msg.chat_id, "Сессия устарела. Начни заново: «📝 Новая заметка».",
                           mainKeyboard());
            return;
        }
        saveNoteWithRemind(tg, store, msg.chat_id, snap.pending_text, *spec);
        return;
    }

    if (snap.phase == Phase::WaitingRemindChoice) {
        // Если сразу отправили время — считаем, что это ответ «Да, напомни в это время».
        const auto spec = parseRemindSpec(text);
        if (spec) {
            if (snap.pending_text.empty()) {
                resetSession(msg.chat_id);
                tg.sendMessage(msg.chat_id, "Сессия устарела. Начни заново: «📝 Новая заметка».",
                               mainKeyboard());
                return;
            }
            saveNoteWithRemind(tg, store, msg.chat_id, snap.pending_text, *spec);
            return;
        }
        tg.sendMessage(msg.chat_id,
                       "Нажми <b>Да</b> или <b>Нет</b> под сообщением.\n"
                       "Можно просто написать время, например <code>12:35</code>.",
                       yesNoKeyboard());
        return;
    }

    tg.sendMessage(msg.chat_id,
                   "Выбери действие кнопкой ниже или нажми /help",
                   mainKeyboard());
}

void processUpdate(TelegramClient& tg, NotesStore& store, const TgUpdate& upd) {
    if (!upd.has_chat) {
        return;  // отвечать некуда (callback без message)
    }
    if (upd.is_callback) {
        handleCallback(tg, store, upd);
    } else {
        handleMessage(tg, store, upd);
    }
}

void reminderLoop(TelegramClient& tg, NotesStore& store) {
    while (!stopping()) {
        try {
            const auto due = store.dueReminders(nowUnix());
            for (const auto& n : due) {
                const std::string text = "⏰ <b>Напоминание #" + std::to_string(n.id) +
                                         "</b>\n" + escapeHtml(n.text);
                if (tg.sendMessage(n.chat_id, text, mainKeyboard())) {
                    store.markReminded(n.id);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "reminder loop: " << e.what() << "\n";
        }
        pruneSessions();
        interruptibleSleep(5000);
    }
}

}  // namespace

int main() {
    try {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            std::cerr << "Не удалось инициализировать curl\n";
            return 1;
        }
        struct SigGuard {
            ~SigGuard() {
                std::signal(SIGINT, SIG_DFL);
                std::signal(SIGTERM, SIG_DFL);
            }
        } sig_guard;
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        const std::string token = requireToken();
        TelegramClient tg(token);
        NotesStore store("notes.db");

        std::atomic<bool> running{true};
        std::thread reminder_thread([&] {
            reminderLoop(tg, store);
            running = false;
        });

        std::cout << "Бот запущен. Ctrl+C для остановки.\n";

        // offset держим только в памяти: после перезапуска Telegram перешлёт
        // неподтверждённые апдейты, и бот ответит на старые сообщения.
        int64_t offset = 0;
        int poll_errors = 0;

        while (running.load() && !stopping()) {
            try {
                const auto updates = tg.getUpdates(offset, kPollTimeoutSec);
                poll_errors = 0;
                for (const auto& upd : updates) {
                    try {
                        processUpdate(tg, store, upd);
                    } catch (const std::exception& e) {
                        // Ошибка в одном апдейте не должна ронять весь polling.
                        std::cerr << "update " << upd.update_id << ": " << e.what() << "\n";
                    }
                    // Сдвигаем offset после обработки, иначе апдейт потеряется
                    // вместе с exception'ом выше.
                    offset = upd.update_id + 1;
                }
            } catch (const std::exception& e) {
                if (stopping()) {
                    break;
                }
                std::cerr << "poll error: " << e.what() << "\n";
                // Неверный токен или сеть легла: не долбим API, а ждём с
                // растущей паузой (3, 6, 12, ... максимум 60 секунд).
                ++poll_errors;
                const int delay_ms = std::min(3000 << std::min(poll_errors - 1, 5), 60000);
                if (poll_errors == 1) {
                    std::cerr << "Повторю через " << delay_ms / 1000 << " с\n";
                }
                interruptibleSleep(delay_ms);
            }
        }

        std::cout << "Останавливаюсь (дождусь текущего long polling)...\n";
        running = false;
        reminder_thread.join();
        curl_global_cleanup();
        std::cout << "Остановлено.\n";
    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
