#include "telegram_client.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <stdexcept>
#include <sstream>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace {

// Запас на случай, если Telegram поменяет лимит.
constexpr size_t kChunkLen = kTgMaxMessageLen;

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// Освобождает curl-ресурсы при любом выходе из request().
struct CurlGuard {
    CURL* handle{nullptr};
    struct curl_slist* headers{nullptr};

    ~CurlGuard() {
        if (headers) {
            curl_slist_free_all(headers);
        }
        if (handle) {
            curl_easy_cleanup(handle);
        }
    }
};

// Режем по строкам, чтобы не разрывать HTML-теги. Если одна строка длиннее
// лимита — режем её, но не по середине многобайтового UTF-8 символа.
std::vector<std::string> splitForTelegram(const std::string& text) {
    std::vector<std::string> chunks;
    if (text.empty()) {
        return {std::string()};
    }

    size_t start = 0;
    while (start < text.size()) {
        if (text.size() - start <= kChunkLen) {
            chunks.push_back(text.substr(start));
            break;
        }

        size_t end = start + kChunkLen;
        const size_t nl = text.rfind('\n', end);
        if (nl != std::string::npos && nl > start) {
            end = nl + 1;
        }

        // Не режем многобайтовый UTF-8 символ: если первый байт следующего
        // чанка — продолжение, отступаем назад до его ведущего байта.
        if (end < text.size()) {
            while (end > start &&
                   (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
                --end;
            }
        }
        if (end == start) {
            end = start + kChunkLen;  // символ длиннее лимита — режем как есть
        }

        chunks.push_back(text.substr(start, end - start));
        start = end;
    }
    return chunks;
}

}  // namespace

TelegramClient::TelegramClient(std::string token) : token_(std::move(token)) {
    if (token_.empty()) {
        throw std::runtime_error("BOT_TOKEN is empty");
    }
}

std::string TelegramClient::request(const std::string& method,
                                    const std::optional<std::string>& json_body,
                                    long timeout_sec) const {
    // В method может быть query-строка — в ошибках она только мешает.
    const std::string short_method = method.substr(0, method.find('?'));

    CurlGuard guard;
    guard.handle = curl_easy_init();
    if (!guard.handle) {
        throw std::runtime_error("curl_easy_init failed");
    }

    const std::string url = api(method);
    std::string response;

    curl_easy_setopt(guard.handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(guard.handle, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(guard.handle, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(guard.handle, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(guard.handle, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(guard.handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(guard.handle, CURLOPT_USERAGENT, "tg-notes-bot/1.0");
    curl_easy_setopt(guard.handle, CURLOPT_ACCEPT_ENCODING, "");

    if (json_body) {
        guard.headers = curl_slist_append(nullptr, "Content-Type: application/json");
        curl_easy_setopt(guard.handle, CURLOPT_HTTPHEADER, guard.headers);
        curl_easy_setopt(guard.handle, CURLOPT_POSTFIELDS, json_body->c_str());
        curl_easy_setopt(guard.handle, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(json_body->size()));
    } else {
        curl_easy_setopt(guard.handle, CURLOPT_HTTPGET, 1L);
    }

    const CURLcode rc = curl_easy_perform(guard.handle);
    long http_code = 0;
    curl_easy_getinfo(guard.handle, CURLINFO_RESPONSE_CODE, &http_code);

    if (rc != CURLE_OK) {
        throw std::runtime_error(short_method + " failed: " + curl_easy_strerror(rc));
    }
    if (http_code >= 400) {
        throw std::runtime_error(short_method + " status " + std::to_string(http_code) +
                                 ": " + response);
    }
    return response;
}

std::vector<TgUpdate> TelegramClient::getUpdates(int64_t offset, int timeout_sec) {
    std::ostringstream method;
    method << "getUpdates?timeout=" << timeout_sec << "&offset=" << offset
           << "&allowed_updates=%5B%22message%22%2C%22callback_query%22%5D";

    // Даём HTTP-запросу пережить long polling с запасом.
    const auto body = request(method.str(), std::nullopt, static_cast<long>(timeout_sec) + 10L);
    const auto root = json::parse(body);
    if (!root.value("ok", false)) {
        throw std::runtime_error("getUpdates not ok: " + body);
    }

    std::vector<TgUpdate> updates;
    for (const auto& upd : root.at("result")) {
        TgUpdate u;
        u.update_id = upd.at("update_id").get<int64_t>();

        if (upd.contains("callback_query")) {
            const auto& cb = upd["callback_query"];
            u.is_callback = true;
            u.callback_id = cb.value("id", "");
            u.text = cb.value("data", "");
            if (cb.contains("from")) {
                u.user_id = cb["from"].value("id", static_cast<int64_t>(0));
            }
            // В инлайн-режиме message нет — отвечать тогда некуда.
            if (cb.contains("message") && cb["message"].contains("chat")) {
                const auto& chat = cb["message"]["chat"];
                if (chat.contains("id")) {
                    u.chat_id = chat.at("id").get<int64_t>();
                    u.has_chat = true;
                }
                if (cb["message"].contains("message_id")) {
                    u.message_id = cb["message"].at("message_id").get<int64_t>();
                }
            }
            updates.push_back(std::move(u));
            continue;
        }

        if (!upd.contains("message") || !upd["message"].contains("text")) {
            continue;
        }
        const auto& msg = upd["message"];
        if (!msg.contains("chat") || !msg["chat"].contains("id")) {
            continue;
        }
        u.chat_id = msg["chat"].at("id").get<int64_t>();
        u.has_chat = true;
        if (msg.contains("from")) {
            u.user_id = msg["from"].value("id", static_cast<int64_t>(0));
        }
        u.message_id = msg.value("message_id", static_cast<int64_t>(0));
        u.text = msg.at("text").get<std::string>();
        updates.push_back(std::move(u));
    }
    return updates;
}

bool TelegramClient::sendMessage(int64_t chat_id, const std::string& text,
                                 const std::optional<std::string>& reply_markup_json) {
    const auto chunks = splitForTelegram(text);
    bool all_ok = true;
    for (size_t i = 0; i < chunks.size(); ++i) {
        try {
            json body = {
                {"chat_id", chat_id},
                {"text", chunks[i]},
                {"parse_mode", "HTML"},
            };
            // Клавиатуру вешаем только на первую часть, иначе пользователь
            // увидит её несколько раз.
            if (i == 0 && reply_markup_json) {
                body["reply_markup"] = json::parse(*reply_markup_json);
            }
            const auto resp = request(api("sendMessage"), body.dump(), 30L);
            if (!json::parse(resp).value("ok", false)) {
                all_ok = false;
            }
        } catch (const std::exception&) {
            // Дальше слать бессмысленно: чат заблокирован или сеть легла.
            return false;
        }
    }
    return all_ok;
}

bool TelegramClient::answerCallback(const std::string& callback_id,
                                    const std::string& text) {
    if (callback_id.empty()) {
        return false;
    }
    try {
        json body = {{"callback_query_id", callback_id}};
        if (!text.empty()) {
            body["text"] = text;
        }
        const auto resp = request(api("answerCallbackQuery"), body.dump(), 30L);
        return json::parse(resp).value("ok", false);
    } catch (const std::exception&) {
        return false;
    }
}

std::string TelegramClient::api(const std::string& method) const {
    return "https://api.telegram.org/bot" + token_ + "/" + method;
}
