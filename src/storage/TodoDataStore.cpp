#include "TodoDataStore.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace desto::storage {
namespace {

using Json = nlohmann::json;

std::string ToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path ItemPath(const std::filesystem::path& directory,
    const domain::CardId& cardId) {
    if (cardId.empty() || cardId.find_first_of("\\/:*?\"<>|") != std::string::npos) {
        throw std::invalid_argument("Todo Card id is invalid for storage.");
    }
    return directory / (cardId + ".json");
}

Json SerializeItem(const domain::TodoItem& item) {
    Json result{
        {"id", item.id},
        {"title", item.title},
        {"completed", item.completed},
        {"createdAt", item.createdAtUnixMilliseconds},
        {"completedAt", item.completedAtUnixMilliseconds},
        {"archived", item.archived},
    };
    if (item.scheduledDate.has_value()) {
        result["scheduledDate"] = domain::ToString(*item.scheduledDate);
    }
    return result;
}

domain::TodoItem ParseItem(const Json& value) {
    domain::TodoItem item;
    item.id = value.value("id", std::string{});
    item.title = value.value("title", std::string{});
    item.completed = value.value("completed", false);
    item.createdAtUnixMilliseconds = value.value("createdAt", std::int64_t{0});
    item.completedAtUnixMilliseconds = value.value("completedAt", std::int64_t{0});
    item.archived = value.value("archived", false);
    if (value.contains("scheduledDate") && value["scheduledDate"].is_string()) {
        const auto text = value["scheduledDate"].get<std::string>();
        if (text.size() == 10 && text[4] == '-' && text[7] == '-') {
            item.scheduledDate = domain::TodoDate{
                std::stoi(text.substr(0, 4)),
                static_cast<std::uint8_t>(std::stoi(text.substr(5, 2))),
                static_cast<std::uint8_t>(std::stoi(text.substr(8, 2))),
            };
        }
    }
    return item;
}

} // namespace

TodoDataStore::TodoDataStore(std::filesystem::path configDirectory)
    : directory_(std::move(configDirectory) / "todos") {
    if (directory_.empty() || !directory_.is_absolute()) {
        throw std::invalid_argument("Todo storage directory must be absolute.");
    }
}

void TodoDataStore::loadInto(ApplicationConfig& config) const {
    if (!std::filesystem::exists(directory_)) return;
    for (auto& card : config.cards) {
        if (card.type != domain::CardType::Todo) continue;
        const auto path = ItemPath(directory_, card.id);
        if (!std::filesystem::exists(path)) continue;
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("Unable to open Todo data file.");
        const auto document = Json::parse(input);
        if (!document.is_object() || document.value("schemaVersion", 0) != 1
            || !document.contains("items") || !document["items"].is_array()) {
            throw std::runtime_error("Todo data file has an invalid schema.");
        }
        std::vector<domain::TodoItem> items;
        for (const auto& value : document["items"]) items.push_back(ParseItem(value));
        card.todoItems = std::move(items);
    }
}

void TodoDataStore::save(const ApplicationConfig& config) const {
    std::filesystem::create_directories(directory_);
    for (const auto& card : config.cards) {
        if (card.type != domain::CardType::Todo) continue;
        const auto path = ItemPath(directory_, card.id);
        const auto temporary = path.string() + ".tmp";
        Json document{{"schemaVersion", 1}, {"cardId", card.id}, {"items", Json::array()}};
        for (const auto& item : card.todoItems) document["items"].push_back(SerializeItem(item));
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("Unable to write Todo data file.");
            output << document.dump(2);
            output.flush();
            if (!output) throw std::runtime_error("Unable to flush Todo data file.");
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(temporary, path, error);
        }
        if (error) throw std::runtime_error("Unable to publish Todo data file.");
    }
}

} // namespace desto::storage
