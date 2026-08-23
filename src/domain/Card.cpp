#include "Card.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace desto::domain {
namespace {

constexpr std::int64_t DaysFromCivil(std::int32_t year, std::int32_t month, std::int32_t day) noexcept {
    year -= month <= 2;
    const auto era = (year >= 0 ? year : year - 399) / 400;
    const auto yearOfEra = year - era * 400;
    const auto dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const auto dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return static_cast<std::int64_t>(era) * 146097 + dayOfEra - 719468;
}

TodoDate CivilFromDays(std::int64_t days) noexcept {
    days += 719468;
    const auto era = (days >= 0 ? days : days - 146096) / 146097;
    const auto dayOfEra = days - era * 146097;
    const auto yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524
        - dayOfEra / 146096) / 365;
    auto year = static_cast<std::int32_t>(yearOfEra + era * 400);
    const auto dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const auto monthPart = (5 * dayOfYear + 2) / 153;
    const auto day = static_cast<std::uint8_t>(dayOfYear - (153 * monthPart + 2) / 5 + 1);
    const auto month = static_cast<std::uint8_t>(monthPart + (monthPart < 10 ? 3 : -9));
    year += month <= 2;
    return {year, month, day};
}

void ValidateAppearance(const CardAppearancePreferences& appearance) {
    if (appearance.opacity < 0 || appearance.opacity > 1
        || appearance.cornerRadius < 0 || appearance.cornerRadius > 128) {
        throw std::invalid_argument(
            "Card opacity must be between 0 and 1 and corner radius must be between 0 and 128.");
    }
}

void ValidateContent(const CardContentPreferences& content) {
    switch (content.itemSize) {
    case CardItemSize::Small:
    case CardItemSize::Medium:
    case CardItemSize::Large:
    case CardItemSize::ExtraLarge:
        break;
    default:
        throw std::invalid_argument("Card item size is invalid.");
    }
    switch (content.sizeMode) {
    case CardSizeMode::Adaptive:
    case CardSizeMode::Fixed:
        break;
    default:
        throw std::invalid_argument("Card size mode is invalid.");
    }
    if (content.widthSpan < MinimumCardWidthSpan(content.itemSize)
        || content.widthSpan > 64) {
        throw std::invalid_argument(
            "Card width span is outside the range supported by its item size.");
    }
    if (content.fixedColumns == 0 || content.fixedColumns > 64
        || content.fixedRows == 0 || content.fixedRows > 64) {
        throw std::invalid_argument("Card fixed grid dimensions must be between 1 and 64.");
    }
    if (content.maximumVisibleRows.has_value()
        && (*content.maximumVisibleRows == 0 || *content.maximumVisibleRows > 64)) {
        throw std::invalid_argument("Card maximum visible rows must be between 1 and 64.");
    }
}

std::wstring FileNameKey(const std::filesystem::path& path) {
    auto result = path.filename().wstring();
    std::ranges::transform(result, result.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return result;
}

void ValidateSortMode(ApplicationItemSortMode sortMode) {
    switch (sortMode) {
    case ApplicationItemSortMode::Custom:
    case ApplicationItemSortMode::Name:
    case ApplicationItemSortMode::Size:
    case ApplicationItemSortMode::ItemType:
    case ApplicationItemSortMode::ModifiedDate:
        return;
    }
    throw std::invalid_argument("Application Card sort mode is invalid.");
}

void NormalizeAndValidatePlacements(std::vector<ApplicationItemPlacement>& placements) {
    std::unordered_set<std::wstring> unique;
    std::unordered_set<std::uint64_t> occupied;
    for (auto& placement : placements) {
        auto& item = placement.fileName;
        item = item.lexically_normal();
        const auto slot = (static_cast<std::uint64_t>(placement.row) << 32)
            | placement.column;
        if (item.empty() || item.is_absolute() || item != item.filename()
            || item == "." || item == ".." || !unique.insert(FileNameKey(item)).second
            || !occupied.insert(slot).second) {
            throw std::invalid_argument(
                "Application card placements must contain unique file names and slots.");
        }
    }
}

void NormalizeAndValidateMappingPlacements(
    std::vector<ApplicationItemPlacement>& placements) {
    std::unordered_set<std::wstring> unique;
    std::unordered_set<std::uint64_t> occupied;
    for (auto& placement : placements) {
        auto& path = placement.fileName;
        path = path.lexically_normal();
        const auto slot = (static_cast<std::uint64_t>(placement.row) << 32)
            | placement.column;
        auto key = path.wstring();
        std::ranges::transform(key, key.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        if (path.empty() || !path.is_absolute()
            || !unique.insert(std::move(key)).second
            || !occupied.insert(slot).second) {
            throw std::invalid_argument(
                "Mapping card placements must contain unique absolute paths and slots.");
        }
    }
}

} // namespace

bool IsValidTodoDate(TodoDate date) noexcept {
    if (date.month < 1 || date.month > 12 || date.day < 1) return false;
    constexpr std::array<std::uint8_t, 12> daysInMonth{
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    auto maximum = daysInMonth[date.month - 1];
    const auto leap = date.year % 4 == 0
        && (date.year % 100 != 0 || date.year % 400 == 0);
    if (date.month == 2 && leap) ++maximum;
    return date.day <= maximum;
}

TodoDate CurrentSystemTodoDate() noexcept {
    return CurrentTodoDate();
}

TodoDate CurrentTodoDate(std::optional<std::int32_t> offsetMinutes) noexcept {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return TodoDateAtUnixMilliseconds(now, offsetMinutes);
}

TodoDate TodoDateAtUnixMilliseconds(
    std::int64_t unixMilliseconds,
    std::optional<std::int32_t> offsetMinutes) noexcept {
    auto seconds = static_cast<std::time_t>(unixMilliseconds / 1000);
    std::tm value{};
#if defined(_WIN32)
    const auto failed = offsetMinutes.has_value()
        ? (seconds += static_cast<std::time_t>(*offsetMinutes) * 60,
            gmtime_s(&value, &seconds) != 0)
        : localtime_s(&value, &seconds) != 0;
#else
    const auto* converted = offsetMinutes.has_value()
        ? (seconds += static_cast<std::time_t>(*offsetMinutes) * 60, std::gmtime(&seconds))
        : std::localtime(&seconds);
    const auto failed = converted == nullptr;
    if (!failed) value = *converted;
#endif
    if (failed) return {1970, 1, 1};
    return {value.tm_year + 1900, static_cast<std::uint8_t>(value.tm_mon + 1),
        static_cast<std::uint8_t>(value.tm_mday)};
}

TodoDate AddTodoDays(TodoDate date, std::int32_t days) noexcept {
    return CivilFromDays(DaysFromCivil(date.year, date.month, date.day) + days);
}

std::int32_t CompareTodoDates(TodoDate left, TodoDate right) noexcept {
    const auto leftDays = DaysFromCivil(left.year, left.month, left.day);
    const auto rightDays = DaysFromCivil(right.year, right.month, right.day);
    return leftDays < rightDays ? -1 : leftDays > rightDays ? 1 : 0;
}

std::string ToString(TodoDate date) {
    if (!IsValidTodoDate(date)) throw std::invalid_argument("Todo date is invalid.");
    std::ostringstream result;
    result << std::setfill('0') << std::setw(4) << date.year << '-'
        << std::setw(2) << static_cast<int>(date.month) << '-'
        << std::setw(2) << static_cast<int>(date.day);
    return result.str();
}

bool IsTodoItemArchived(
    const TodoItem& item,
    TodoDate currentDate,
    std::optional<std::int32_t> offsetMinutes) noexcept {
    if (item.archived) return true;
    if (!item.completed || item.completedAtUnixMilliseconds <= 0) return false;
    return TodoDateAtUnixMilliseconds(item.completedAtUnixMilliseconds, offsetMinutes)
        != currentDate;
}

TodoDate TodoItemArchiveDate(
    const TodoItem& item,
    TodoDate today,
    std::optional<std::int32_t> offsetMinutes) noexcept {
    const auto timestamp = item.completedAtUnixMilliseconds > 0
        ? item.completedAtUnixMilliseconds : item.createdAtUnixMilliseconds;
    if (timestamp > 0) return TodoDateAtUnixMilliseconds(timestamp, offsetMinutes);
    return item.scheduledDate.value_or(today);
}

std::vector<TodoDateViewItem> ResolveTodoDateView(
    std::span<const TodoItem> items,
    TodoDate today,
    std::int32_t dateOffset,
    std::optional<std::int32_t> timeZoneOffsetMinutes) {
    const auto selected = AddTodoDays(today, dateOffset);
    const auto historical = dateOffset != 0 && dateOffset != 1;
    std::vector<TodoDateViewItem> overdue;
    std::vector<TodoDateViewItem> selectedItems;
    std::vector<TodoDateViewItem> archived;
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        const auto scheduled = item.scheduledDate.value_or(today);
        if (!IsTodoItemArchived(item, today, timeZoneOffsetMinutes)) {
            if (dateOffset == 0 && CompareTodoDates(scheduled, today) < 0) {
                overdue.push_back({index, true, false, scheduled});
            } else if (scheduled == selected) {
                selectedItems.push_back({index, false, false, selected});
            }
            continue;
        }
        if (!historical) continue;
        const auto archiveDate = TodoItemArchiveDate(item, today, timeZoneOffsetMinutes);
        if (archiveDate == selected || scheduled == selected) {
            archived.push_back({index, false, true, selected});
        }
    }
    std::ranges::stable_sort(overdue, [&](const TodoDateViewItem& left,
                                          const TodoDateViewItem& right) {
        return left.date == right.date ? left.index < right.index : left.date < right.date;
    });
    std::vector<TodoDateViewItem> result;
    result.reserve(overdue.size() + selectedItems.size() + archived.size());
    result.insert(result.end(), overdue.begin(), overdue.end());
    result.insert(result.end(), selectedItems.begin(), selectedItems.end());
    result.insert(result.end(), archived.begin(), archived.end());
    return result;
}

Card::Card(CardId id, CardType type)
    : id_(std::move(id)), type_(type) {
    if (id_.empty()) {
        throw std::invalid_argument("Card id must not be empty.");
    }
}

void Card::setName(std::string name) {
    if (name.empty() || name.size() > 192) {
        throw std::invalid_argument("Card name must contain 1-192 UTF-8 bytes.");
    }
    name_ = std::move(name);
}

void Card::setChrome(CardChromePreferences preferences) {
    chrome_ = preferences;
}

void Card::setAppearance(CardAppearancePreferences preferences) {
    ValidateAppearance(preferences);
    appearance_ = std::move(preferences);
}

void Card::setContent(CardContentPreferences preferences) {
    ValidateContent(preferences);
    content_ = preferences;
}

CardDeletionPreview Card::deletionPreview() const noexcept {
    return {
        .cardId = id_,
        .cardType = type_,
        .effect = deletionEffect(),
        .requiresConfirmation = true,
    };
}

ApplicationCard::ApplicationCard(CardId id, std::filesystem::path relativeStoragePath)
    : Card(std::move(id), CardType::Application),
      relativeStoragePath_(std::move(relativeStoragePath)) {
    if (relativeStoragePath_.empty() || relativeStoragePath_.is_absolute()) {
        throw std::invalid_argument("Application card storage path must be relative and non-empty.");
    }
}

void ApplicationCard::setRelativeStoragePath(std::filesystem::path relativeStoragePath) {
    if (relativeStoragePath.empty() || relativeStoragePath.is_absolute()) {
        throw std::invalid_argument("Application card storage path must be relative and non-empty.");
    }
    relativeStoragePath_ = std::move(relativeStoragePath);
}

void ApplicationCard::setItemPlacements(std::vector<ApplicationItemPlacement> placements) {
    setLayout(sortMode_, std::move(placements));
}

void ApplicationCard::setSortMode(ApplicationItemSortMode sortMode) {
    setLayout(sortMode, itemPlacements_);
}

void ApplicationCard::setLayout(
    ApplicationItemSortMode sortMode,
    std::vector<ApplicationItemPlacement> placements) {
    ValidateSortMode(sortMode);
    NormalizeAndValidatePlacements(placements);
    sortMode_ = sortMode;
    itemPlacements_ = std::move(placements);
}

void ApplicationCard::validateContentPreferences(
    const CardContentPreferences& preferences) const {
    ValidateContent(preferences);
    if (preferences.sizeMode != CardSizeMode::Fixed) return;
    const auto columns = ProjectCardColumns(preferences.widthSpan, preferences.itemSize);
    const auto outside = std::ranges::any_of(
        itemPlacements_, [&](const ApplicationItemPlacement& placement) {
            return placement.column >= columns
                || placement.row >= preferences.fixedRows;
        });
    if (outside) {
        throw std::invalid_argument(
            "Application Card custom positions do not fit the fixed grid.");
    }
}

MappingCard::MappingCard(CardId id)
    : Card(std::move(id), CardType::Mapping) {
}

void MappingCard::setFolderSource(std::filesystem::path sourceRoot) {
    if (sourceRoot.empty() || !sourceRoot.is_absolute()) {
        throw std::invalid_argument("Mapping folder source must be an absolute path.");
    }
    references_.clear();
    itemPlacements_.clear();
    sourceRoot_ = sourceRoot.lexically_normal();
    mode_ = MappingMode::Folder;
}

void MappingCard::setReferences(std::vector<FileReference> references) {
    for (const auto& reference : references) {
        if (reference.id.empty() || reference.path.empty() || !reference.path.is_absolute()) {
            throw std::invalid_argument(
                "Mapping references must have an id and an absolute path.");
        }
    }
    for (std::size_t index = 0; index < references.size(); ++index) {
        const auto duplicate = std::find_if(
            references.begin() + static_cast<std::ptrdiff_t>(index + 1),
            references.end(),
            [&](const FileReference& candidate) {
                return candidate.id == references[index].id
                    || candidate.path.lexically_normal()
                        == references[index].path.lexically_normal();
            });
        if (duplicate != references.end()) {
            throw std::invalid_argument("Mapping references must be unique.");
        }
        references[index].path = references[index].path.lexically_normal();
    }
    sourceRoot_.clear();
    itemPlacements_.clear();
    references_ = std::move(references);
    mode_ = MappingMode::References;
}

void MappingCard::setSortMode(ApplicationItemSortMode mode) {
    setLayout(mode, itemPlacements_);
}

void MappingCard::setItemPlacements(
    std::vector<ApplicationItemPlacement> placements) {
    setLayout(sortMode_, std::move(placements));
}

void MappingCard::setLayout(
    ApplicationItemSortMode mode,
    std::vector<ApplicationItemPlacement> placements) {
    ValidateSortMode(mode);
    NormalizeAndValidateMappingPlacements(placements);
    sortMode_ = mode;
    itemPlacements_ = std::move(placements);
}

void MappingCard::setMode(MappingMode mode) {
    if (mode == MappingMode::Empty) {
        throw std::invalid_argument("Mapping source mode must be Folder or References.");
    }
    if (mode_ == mode) return;
    sourceRoot_.clear();
    references_.clear();
    itemPlacements_.clear();
    mode_ = mode;
}

void MappingCard::clearSource() noexcept {
    sourceRoot_.clear();
    references_.clear();
    itemPlacements_.clear();
}

TodoCard::TodoCard(CardId id)
    : Card(std::move(id), CardType::Todo) {
}

void TodoCard::setItems(std::vector<TodoItem> items) {
    std::unordered_set<std::string> ids;
    for (const auto& item : items) {
        const auto hasVisibleTitle = item.title.find_first_not_of(" \t\r\n")
            != std::string::npos;
        if (item.id.empty() || !hasVisibleTitle || item.title.size() > 512
            || item.createdAtUnixMilliseconds < 0
            || item.completedAtUnixMilliseconds < 0
            || (!item.completed && item.completedAtUnixMilliseconds != 0)
            || (item.scheduledDate.has_value() && !IsValidTodoDate(*item.scheduledDate))
            || !ids.insert(item.id).second) {
            throw std::invalid_argument(
                "Todo items must have unique ids and non-empty titles up to 512 bytes.");
        }
    }
    items_ = std::move(items);
}

std::string_view ToString(CardType type) noexcept {
    switch (type) {
    case CardType::Application:
        return "application";
    case CardType::Mapping:
        return "mapping";
    case CardType::Todo:
        return "todo";
    }
    return "unknown";
}

std::string_view ToString(CardItemSize size) noexcept {
    switch (size) {
    case CardItemSize::Small:
        return "small";
    case CardItemSize::Medium:
        return "medium";
    case CardItemSize::Large:
        return "large";
    case CardItemSize::ExtraLarge:
        return "extraLarge";
    }
    return "medium";
}

std::uint32_t MinimumCardWidthSpan(CardItemSize size) noexcept {
    return size == CardItemSize::Small || size == CardItemSize::Medium ? 3u : 2u;
}

std::size_t ProjectCardColumns(
    std::uint32_t widthSpan,
    CardItemSize size) noexcept {
    const auto span = std::max(widthSpan, MinimumCardWidthSpan(size));
    double scale = 1.0;
    switch (size) {
    case CardItemSize::Small:
        scale = 1.5;
        break;
    case CardItemSize::Medium:
        scale = 1.25;
        break;
    case CardItemSize::Large:
        break;
    case CardItemSize::ExtraLarge:
        scale = 0.75;
        break;
    }
    return static_cast<std::size_t>(
        std::floor(static_cast<double>(span) * scale + 0.5));
}

std::uint32_t InferCardWidthSpan(
    std::size_t columns,
    CardItemSize size) noexcept {
    constexpr std::uint32_t maximumWidthSpan = 64;
    const auto minimumWidthSpan = MinimumCardWidthSpan(size);
    if (columns == 0) return minimumWidthSpan;

    auto bestSpan = minimumWidthSpan;
    auto bestDistance = std::numeric_limits<std::size_t>::max();
    for (auto span = minimumWidthSpan; span <= maximumWidthSpan; ++span) {
        const auto projected = ProjectCardColumns(span, size);
        const auto distance = projected > columns
            ? projected - columns : columns - projected;
        if (distance < bestDistance) {
            bestSpan = span;
            bestDistance = distance;
        }
        if (distance == 0) break;
    }
    return bestSpan;
}

std::uint32_t FitCardWidthSpan(
    std::size_t minimumColumns,
    CardItemSize size) noexcept {
    constexpr std::uint32_t maximumWidthSpan = 64;
    for (auto span = MinimumCardWidthSpan(size); span < maximumWidthSpan; ++span) {
        if (ProjectCardColumns(span, size) >= minimumColumns) return span;
    }
    return maximumWidthSpan;
}

std::string_view ToString(CardSizeMode mode) noexcept {
    switch (mode) {
    case CardSizeMode::Adaptive:
        return "adaptive";
    case CardSizeMode::Fixed:
        return "fixed";
    }
    return "adaptive";
}

std::string_view ToString(ApplicationItemSortMode mode) noexcept {
    switch (mode) {
    case ApplicationItemSortMode::Custom:
        return "custom";
    case ApplicationItemSortMode::Name:
        return "name";
    case ApplicationItemSortMode::Size:
        return "size";
    case ApplicationItemSortMode::ItemType:
        return "itemType";
    case ApplicationItemSortMode::ModifiedDate:
        return "modifiedDate";
    }
    return "custom";
}

} // namespace desto::domain
