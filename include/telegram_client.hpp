#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Ограничение Telegram на длину одного сообщения.
inline constexpr size_t kTgMaxMessageLen = 4096;

struct TgUpdate {
    int64_t update_id{};
    bool is_callback{false};
    bool has_chat{false};   // у callback без message (инлайн-режим) чата нет
    int64_t chat_id{};
    int64_t user_id{};
    std::string text;           // текст сообщения или callback_data
    std::string callback_id;    // для answerCallbackQuery
    int64_t message_id{};
};

class TelegramClient {
public:
    explicit TelegramClient(std::string token);

    std::vector<TgUpdate> getUpdates(int64_t offset, int timeout_sec = 25);

    // Длинный текст режется на части по 4096 символов; вернёт true, только
    // если отправились все части.
    bool sendMessage(int64_t chat_id, const std::string& text,
                     const std::optional<std::string>& reply_markup_json = std::nullopt);

    bool answerCallback(const std::string& callback_id,
                        const std::string& text = "");

private:
    std::string api(const std::string& method) const;
    std::string request(const std::string& method,
                        const std::optional<std::string>& json_body,
                        long timeout_sec) const;

    std::string token_;
};
