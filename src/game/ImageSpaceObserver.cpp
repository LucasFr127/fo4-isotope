// SPDX-License-Identifier: GPL-3.0-or-later

#include "ImageSpaceObserver.hpp"
#include <RE/Fallout.h>
#include "GameObservationMemory.hpp"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <span>
#include <atomic>
#include <memory>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kMaximumImageSpaces{8192};
constexpr std::size_t kMaximumEditorIdEntries{1U << 20U};
constexpr std::string_view kNativeTexturePrefix{"Textures\\"};
static_assert(offsetof(RE::TESImageSpace, lutTexture) +
    offsetof(RE::TESTexture, textureName) == 0x80);

struct CatalogEntry
{
    std::uintptr_t dataIdentity{};
    std::uintptr_t formIdentity{};
    fo4hdr::ImageSpaceIdentity identity{};
    std::array<char, 260> lutPath{};
    bool ambiguousEditorId{};
};

using Catalog = std::vector<CatalogEntry>;

std::atomic<DWORD> g_ownerThread{};
std::unique_ptr<const Catalog> g_catalog;
std::uint64_t g_catalogGeneration{};

bool FormatKey(std::string_view basename, std::uint32_t localFormId,
    fo4hdr::ImageSpaceIdentity& identity)
{
    constexpr std::size_t suffixLength{7};
    std::array<char, 259 + 1> raw{};
    std::copy(basename.begin(), basename.end(), raw.begin());
    raw[basename.size()] = '|';
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 6; ++index)
    {
        const auto shift = static_cast<unsigned>((5 - index) * 4);
        raw[basename.size() + 1 + index] = hex[(localFormId >> shift) & 0xFU];
    }
    if (!fo4hdr::game::NormalizeObservedLutPath(
            {raw.data(), basename.size() + suffixLength}, identity.key)) return false;
    const std::string_view normalized(identity.key.data(), basename.size());
    return normalized.ends_with(".esm") || normalized.ends_with(".esp") || normalized.ends_with(".esl");
}

std::string_view PluginBasename(const RE::TESFile& origin)
{
    const auto filename = origin.GetFilename();
    const auto separator = filename.find_last_of("/\\");
    return separator == std::string_view::npos ? filename :
        filename.substr(separator + 1);
}

bool ValidPluginBasename(std::string_view basename)
{
    constexpr std::size_t suffixLength{7};
    return !basename.empty() && basename.find_first_of("<>:\"/\\|?*") == std::string_view::npos &&
        basename.size() + suffixLength <= 259;
}

bool BuildKey(RE::TESImageSpace& form, const RE::TESFile& origin,
    fo4hdr::ImageSpaceIdentity& identity)
{
    const auto basename = PluginBasename(origin);
    if (!ValidPluginBasename(basename)) return false;
    const auto localFormId = form.GetLocalFormID();
    if (localFormId == 0 || localFormId > 0x00FFFFFFU) return false;

    return FormatKey(basename, localFormId, identity);
}

bool CopyEditorId(const RE::BSFixedString& source,
    std::array<char, 128>& result) noexcept
{
    std::array<char, 128> bytes{};
    if (!fo4hdr::game::observation::ReadBoundedText(source.c_str(), bytes)) return false;
    const std::string_view value(bytes.data());
    std::array<char, 259 + 1> validated{};
    if (!fo4hdr::game::NormalizeObservedLutPath(value, validated)) return false;
    std::copy(value.begin(), value.end(), result.begin());
    return true;
}

void MergeEditorId(CatalogEntry& entry, const std::array<char, 128>& editorId)
{
    if (entry.identity.editorId.front() == '\0')
        entry.identity.editorId = editorId;
    else if (entry.identity.editorId != editorId)
    {
        entry.identity.editorId = {};
        entry.ambiguousEditorId = true;
    }
}

void EnrichEditorId(Catalog& catalog, const RE::TESForm* form, const RE::BSFixedString& name)
{
    const auto formIdentity = reinterpret_cast<std::uintptr_t>(form);
    const auto entry = std::lower_bound(catalog.begin(), catalog.end(), formIdentity,
        [](const CatalogEntry& candidate, std::uintptr_t target) {
            return candidate.formIdentity < target;
        });
    if (entry == catalog.end()) return;
    if (entry->formIdentity != formIdentity ||
        entry->ambiguousEditorId) return;
    std::array<char, 128> editorId{};
    if (!CopyEditorId(name, editorId)) return;
    MergeEditorId(*entry, editorId);
}

void ReadEditorMap(Catalog& catalog)
{
    auto [map, lock] = RE::TESForm::GetAllFormsByEditorID();
    if (map == nullptr) return;
    RE::BSAutoReadLock guard(lock.get());
    if (map->size() > kMaximumEditorIdEntries) return;
    for (const auto& mapped : *map) EnrichEditorId(catalog, mapped.second, mapped.first);
}

void EnrichEditorIds(Catalog& catalog) noexcept
{
    try
    {
        std::sort(catalog.begin(), catalog.end(),
            [](const CatalogEntry& left, const CatalogEntry& right) {
                return left.formIdentity < right.formIdentity;
            });
        ReadEditorMap(catalog);
    }
    catch (...)
    {
        for (auto& entry : catalog) entry.identity.editorId = {};
    }
}

void CopyLutPath(std::string_view textureNameSource, CatalogEntry& entry)
{
    const std::string_view textureName = textureNameSource;
    if (!textureName.empty() &&
        kNativeTexturePrefix.size() + textureName.size() <=
            259)
    {
        std::array<char, 260> rawPath{};
        auto output = std::copy(kNativeTexturePrefix.begin(),
            kNativeTexturePrefix.end(), rawPath.begin());
        std::copy(textureName.begin(), textureName.end(), output);
        (void)fo4hdr::game::NormalizeObservedLutPath(
            {rawPath.data(), kNativeTexturePrefix.size() + textureName.size()},
            entry.lutPath);
    }
}

const RE::TESFile* OriginFile(RE::TESImageSpace* form)
{
    if (form == nullptr ||
        !fo4hdr::game::observation::IsReadableMemory(form, sizeof(RE::TESImageSpace))) return nullptr;
    const auto formId = form->GetFormID();
    if ((formId >> 24U) == 0xFFU) return nullptr;
    return form->GetFile(0);
}

void AddCatalogEntry(Catalog& catalog, RE::TESImageSpace* form)
{
    const auto* origin = OriginFile(form);
    if (origin == nullptr) return;

    CatalogEntry entry{
        .dataIdentity = reinterpret_cast<std::uintptr_t>(
            std::addressof(form->data)),
        .formIdentity = reinterpret_cast<std::uintptr_t>(form)};
    if (entry.dataIdentity == 0 || !BuildKey(*form, *origin, entry.identity))
        return;
    entry.identity.formId = form->GetFormID();
    CopyLutPath(form->lutTexture.textureName, entry);
    catalog.push_back(entry);
}

template<class Projection>
bool SortUnique(Catalog& catalog, Projection project)
{
    std::ranges::sort(catalog, {}, project);
    return std::ranges::adjacent_find(catalog, {}, project) == catalog.end();
}
bool CollectCatalog(Catalog& catalog)
{
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (dataHandler == nullptr) return false;
    auto& forms = dataHandler->GetFormArray<RE::TESImageSpace>();
    if (forms.size() > kMaximumImageSpaces) return false;

    catalog.reserve(forms.size());
    for (auto* form : forms)
    {
        AddCatalogEntry(catalog, form);
    }
    return true;
}

bool BuildCatalog()
{
    auto catalog = std::make_unique<Catalog>();
    if (!CollectCatalog(*catalog)) return false;
    if (!SortUnique(*catalog, [](const CatalogEntry& entry) { return entry.identity.key; })) return false;

    EnrichEditorIds(*catalog);
    if (!SortUnique(*catalog, &CatalogEntry::dataIdentity)) return false;

    for (auto& entry : *catalog)
    {
        entry.formIdentity = 0;
        entry.ambiguousEditorId = false;
    }

    // Only the admitted game owner thread accesses or replaces this immutable
    // value. A foreign render thread cannot read this provenance.
    g_catalog = std::move(catalog);
    return true;
}

}  // namespace

namespace fo4hdr::game {

bool BuildImageSpaceCatalog(std::uint64_t resourceGeneration) noexcept
{
    const auto thread = ::GetCurrentThreadId();
    DWORD expected{};
    if (!g_ownerThread.compare_exchange_strong(expected, thread) && expected != thread) return false;
    g_catalogGeneration = 0;
    g_catalog.reset();
    try
    {
        if (!BuildCatalog()) return false;
        g_catalogGeneration = resourceGeneration;
        return true;
    }
    catch (...) { return false; }
}

static bool LookupImageSpaceIdentity(
    const RE::ImageSpaceBaseData* data, ImageSpaceIdentity& result,
    std::array<char, 260>& lutPath) noexcept
{
    result = {};
    lutPath = {};
    if (data == nullptr || ::GetCurrentThreadId() != g_ownerThread.load()) return false;
    const auto* catalog = g_catalog.get();
    if (catalog == nullptr) return false;
    const auto identity = reinterpret_cast<std::uintptr_t>(data);
    const auto entry = std::lower_bound(catalog->begin(), catalog->end(), identity,
        [](const CatalogEntry& candidate, std::uintptr_t target) {
            return candidate.dataIdentity < target;
        });
    if (entry == catalog->end() || entry->dataIdentity != identity) return false;
    result = entry->identity;
    lutPath = entry->lutPath;
    return true;
}


namespace {
std::size_t Utf8SequenceLength(unsigned char lead) noexcept
{
    constexpr std::array minimum{0xC2U, 0xE0U, 0xF0U};
    constexpr std::array maximum{0xDFU, 0xEFU, 0xF4U};
    for (std::size_t index = 0; index < minimum.size(); ++index)
        if (lead >= minimum[index] && lead <= maximum[index]) return index + 2;
    return 0;
}

bool ValidUtf8Second(unsigned char lead, unsigned char second) noexcept
{
    unsigned char minimum = 0x80U;
    unsigned char maximum = 0xBFU;
    struct Boundary { unsigned char lead, minimum, maximum; };
    constexpr std::array rules{Boundary{0xE0U, 0xA0U, 0xBFU}, Boundary{0xEDU, 0x80U, 0x9FU},
        Boundary{0xF0U, 0x90U, 0xBFU}, Boundary{0xF4U, 0x80U, 0x8FU}};
    for (const auto& rule : rules)
    {
        if (rule.lead != lead) continue;
        minimum = rule.minimum;
        maximum = rule.maximum;
    }
    return second >= minimum && second <= maximum;
}

std::size_t ValidUtf8Prefix(std::string_view text) noexcept
{
    const auto lead = static_cast<unsigned char>(text.front());
    const auto length = Utf8SequenceLength(lead);
    if (length == 0 || length > text.size()) return 0;
    if (!ValidUtf8Second(lead, static_cast<unsigned char>(text[1]))) return 0;
    const auto tail = text.substr(2, length - 2);
    if (!std::ranges::all_of(tail, [](unsigned char byte) {
            return (byte & 0xC0U) == 0x80U;
        })) return 0;
    return length;
}

char NormalizeGradeAscii(unsigned char byte) noexcept
{
    constexpr std::string_view reserved{"[]\x7F"};
    if (byte < 0x20U || reserved.find(static_cast<char>(byte)) != std::string_view::npos) return '\0';
    if (byte == '\\') return '/';
    constexpr std::string_view uppercase{"ABCDEFGHIJKLMNOPQRSTUVWXYZ"};
    const auto letter = uppercase.find(static_cast<char>(byte));
    if (letter != std::string_view::npos) return static_cast<char>('a' + letter);
    return static_cast<char>(byte);
}

bool ValidPathBounds(std::string_view path) noexcept
{
    if (path.empty() || path.size() > 259) return false;
    return path.front() != ' ' && path.back() != ' ';
}

bool CopyCodepoint(std::string_view path, std::array<char, 260>& normalized,
    std::size_t& index) noexcept
{
    const auto byte = static_cast<unsigned char>(path[index]);
    if (byte <= 0x7FU)
    {
        const auto character = NormalizeGradeAscii(byte);
        if (character == '\0') return false;
        normalized[index++] = character;
        return true;
    }
    const auto length = ValidUtf8Prefix(path.substr(index));
    if (length == 0) return false;
    std::copy_n(path.data() + index, length, normalized.data() + index);
    index += length;
    return true;
}

}  // namespace

[[nodiscard]] bool NormalizeObservedLutPath(
    std::string_view path,
    std::array<char, 260>& normalized) noexcept
{
    normalized.fill('\0');
    if (!ValidPathBounds(path)) return false;
    for (std::size_t index = 0; index < path.size();)
    {
        if (!CopyCodepoint(path, normalized, index)) return false;
    }
    return true;
}



namespace {
struct ManagerHeader
{
    std::uintptr_t base{}, overrideData{};

    bool Read(std::span<const std::byte> bytes) noexcept
    {
        std::uint8_t ready{};
        if (!observation::ReadSnapshotField(bytes, offsetof(RE::ImageSpaceManager, isReady), ready) ||
            ready != 1) return false;
        return observation::ReadSnapshotField(bytes, offsetof(RE::ImageSpaceManager, baseData), base) &&
            observation::ReadSnapshotField(bytes, offsetof(RE::ImageSpaceManager, overRideBaseData), overrideData);
    }
};

bool CurrentCatalog(std::uint64_t resourceGeneration) noexcept
{
    if (::GetCurrentThreadId() != g_ownerThread.load()) return false;
    return g_catalog != nullptr && g_catalogGeneration == resourceGeneration;
}

bool ReadActiveData(std::span<const std::byte> bytes, ImageSpaceActiveData& result) noexcept
{
    constexpr auto data = offsetof(RE::ImageSpaceManager, currentEOFData) +
        offsetof(RE::ImageSpaceData, baseData);
    constexpr auto hdr = data + offsetof(RE::ImageSpaceBaseData, hdrData);
    constexpr auto cinematic = data + offsetof(RE::ImageSpaceBaseData, cinematicData);
    constexpr std::array<std::size_t, 12> offsets{hdr, hdr + 4, hdr + 8, hdr + 12,
        hdr + 16, hdr + 20, hdr + 24, hdr + 28, hdr + 32, cinematic, cinematic + 4, cinematic + 8};
    return DecodeImageSpaceActiveData(bytes, offsets, result);
}
}  // namespace

bool ObserveImageSpace(const RE::ImageSpaceManager* manager,
    std::uint64_t resourceGeneration, ImageSpaceSnapshot& destination) noexcept
{
    destination = {};
    if (!CurrentCatalog(resourceGeneration) || manager == nullptr) return false;
    std::array<std::byte, sizeof(RE::ImageSpaceManager)> bytes{};
    if (!observation::GuardedReadValue(manager, bytes)) return false;
    ManagerHeader header{};
    if (!header.Read(bytes)) return false;
    ImageSpaceSnapshot candidate{};
    if (!ReadActiveData(bytes, candidate.active)) return false;
    candidate.baseMatched = LookupImageSpaceIdentity(
        reinterpret_cast<const RE::ImageSpaceBaseData*>(header.base), candidate.base, candidate.baseLutPath);
    candidate.managerIdentity = reinterpret_cast<std::uintptr_t>(manager);
    candidate.baseDataIdentity = header.base;
    candidate.overrideActive = header.overrideData != 0;
    candidate.threadId = ::GetCurrentThreadId();
    candidate.resourceGeneration = resourceGeneration;
    candidate.ready = true;
    destination = candidate;
    return true;
}

}  // namespace fo4hdr::game
