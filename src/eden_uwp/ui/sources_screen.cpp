// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// The Windows headers come first: the Eden headers #undef the A/W macros of the Win32 calls they
// name their methods after.
#include <windows.h>

// wingdi.h defines GetObject as a macro for GetObjectW, which would rename JsonValue::GetObject().
#ifdef GetObject
#undef GetObject
#endif

#include "eden_uwp/ui/sources_screen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/hle/service/acc/profile_manager.h"
#include "eden_uwp/await_bounded.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/game_download.h"
#include "eden_uwp/ui/anim.h"
#include "eden_uwp/ui/art.h"
#include "eden_uwp/ui/art_colors.h"
#include "eden_uwp/ui/source_catalog.h"
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/text_entry.h"
#include "eden_uwp/ui/theme.h"
#include "eden_uwp/ui/tinfoil_index.h"
#include "eden_uwp/ui/widgets.h"
#include "eden_uwp/usb_library.h"

namespace EdenXbox::Ui {
namespace {

namespace fs = std::filesystem;

using D2D1::RectF;
using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;
using winrt::Windows::Foundation::Uri;
using winrt::Windows::Storage::FileIO;
using winrt::Windows::Storage::KnownFolders;
using winrt::Windows::Storage::StorageFile;
using winrt::Windows::UI::Core::CoreProcessEventsOption;
using winrt::Windows::UI::Core::CoreWindow;
using winrt::Windows::Web::Http::HttpClient;

// Layout, in canvas units. The rows follow the USB import list.
constexpr float kMargin = kScreenMargin;
constexpr float kRight = kCanvasWidth - kScreenMargin;
constexpr float kTitleTop = 150.0f;
constexpr float kTitleHeight = 76.0f;
constexpr float kSubTop = 232.0f;
constexpr float kSubHeight = 30.0f;
constexpr float kListTop = 330.0f;
constexpr float kRowHeight = 104.0f;
constexpr float kRowPitch = 110.0f;
constexpr float kRowRadius = 22.0f;
constexpr float kRowPadding = 28.0f;
constexpr float kStatWidth = 220.0f;
constexpr int kVisibleRows = 5;

constexpr float kSheetLeft = 420.0f;
constexpr float kSheetWidth = 1080.0f;
constexpr float kSheetPadding = 56.0f;
constexpr float kConfirmTop = 340.0f;
constexpr float kConfirmHeight = 360.0f;

constexpr auto kToastDuration = std::chrono::milliseconds(4200);
constexpr auto kRateWindow = std::chrono::milliseconds(500);

constexpr std::size_t kMaxEntries = Tinfoil::kMaxEntries;
constexpr auto kRequestTimeout = std::chrono::seconds(30);

struct Source {
    std::string name;
    std::string url;
};

using Kind = Tinfoil::Kind;

struct Entry {
    std::wstring name; // what the source calls it, for display
    std::wstring file; // the name it is saved under
    std::string url;   // absolute, without the #name fragment
    std::uint64_t size = 0;
    Kind kind = Kind::Other;
    Tinfoil::Headers headers;
    std::string icon_url;
    std::string banner_url;
    std::string art_origin;
    Tinfoil::Headers art_headers;
    std::wstring key; // case-folded destination for Windows queue collision checks
};

enum class Failure { None, Unreachable, Unsupported, SourceError };

struct FetchResult {
    std::vector<Entry> entries;
    std::wstring message; // the source's own "success" or "error" text
    Failure failure = Failure::None;
    std::optional<Tinfoil::Traversal> traversal;
    bool partial = false;
    std::set<std::string> encrypted_sources;
    Tinfoil::Unsupported unsupported = Tinfoil::Unsupported::None;
    std::vector<std::string> locations;
    std::map<std::string, std::wstring> errors;
};

struct StorageResult {
    fs::space_info internal{};
    fs::space_info external{};
};

struct ImportResult {
    std::vector<Source> sources;
};

// State a detached worker shares with the screen. The worker owns a reference, so leaving the
// screen never has to wait for a request that is still in flight.
template <typename Result>
struct Task {
    std::atomic<bool> done{false};
    std::atomic<bool> cancel{false};
    Result result; // written by the worker before `done` is set, read by the screen after
};

// ---- Small string helpers (UTF-8 unless noted) ----

std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && static_cast<unsigned char>(text[begin]) <= ' ') {
        ++begin;
    }
    while (end > begin && static_cast<unsigned char>(text[end - 1]) <= ' ') {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return text;
}

// The text between "://" and the next "/", "?" or "#", without credentials.
std::string HostOf(const std::string& url) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return url;
    }
    const std::size_t start = scheme + 3;
    const std::size_t end = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, end == std::string::npos ? end : end - start);
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        authority.erase(0, at + 1);
    }
    return authority;
}

// For the diagnostic log and the list: no credentials and no query string, which may carry a token.
std::string RedactedUrl(const std::string& url) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return url;
    }
    const std::size_t path = url.find_first_of("/?#", scheme + 3);
    std::string result = url.substr(0, scheme + 3) + HostOf(url);
    if (path != std::string::npos && url[path] == '/') {
        const std::size_t cut = url.find_first_of("?#", path);
        result += url.substr(path, cut == std::string::npos ? cut : cut - path);
    }
    return result;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string UrlDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && HexValue(text[i + 1]) >= 0 &&
            HexValue(text[i + 2]) >= 0) {
            out.push_back(static_cast<char>(HexValue(text[i + 1]) * 16 + HexValue(text[i + 2])));
            i += 2;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::string Utf8(const std::wstring& text) {
    return winrt::to_string(text);
}

const wchar_t* KindLabel(Kind kind) {
    switch (kind) {
    case Kind::Game:
        return Tr(Text::SrcTypeGame);
    case Kind::Update:
        return Tr(Text::SrcTypeUpdate);
    case Kind::Dlc:
        return Tr(Text::SrcTypeDlc);
    case Kind::Other:
        break;
    }
    return Tr(Text::SrcOther);
}

bool IsGameExtension(const std::wstring& extension) {
    return extension == L".nsp" || extension == L".nsz" || extension == L".xci" ||
           extension == L".xcz";
}

std::wstring LowerWide(std::wstring text) {
    if (text.empty())
        return text;
    std::wstring lower(text.size(), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(),
                      static_cast<int>(text.size()), lower.data(), static_cast<int>(lower.size()),
                      nullptr, nullptr, 0) > 0)
        return lower;
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

std::wstring ExtensionOf(const std::wstring& name) {
    const std::size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? std::wstring() : LowerWide(name.substr(dot));
}

// The name a download is saved under: characters Windows rejects become "_", and a name without a
// game extension borrows the one of the URL so the library recognizes the file.
std::wstring DiskName(const std::wstring& display_name, const std::string& clean_url) {
    std::wstring name = display_name;
    for (wchar_t& c : name) {
        if (c < 0x20 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) {
            c = L'_';
        }
    }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) {
        name.pop_back();
    }
    if (name.size() > 180) {
        name.resize(180);
    }
    if (!IsGameExtension(ExtensionOf(name))) {
        const std::size_t query = clean_url.find_first_of("?#");
        const std::wstring url_name = Widen(UrlDecode(clean_url.substr(0, query)));
        const std::wstring extension = ExtensionOf(url_name);
        if (IsGameExtension(extension)) {
            name += extension;
        }
    }
    return name.empty() ? std::wstring(L"download") : name;
}

// ---- JSON access that tolerates missing and null fields ----

std::string StringAt(const JsonObject& object, const wchar_t* key) {
    if (object.HasKey(key)) {
        const IJsonValue value = object.GetNamedValue(key);
        if (value.ValueType() == JsonValueType::String) {
            return winrt::to_string(value.GetString());
        }
    }
    return {};
}

JsonArray ArrayAt(const JsonObject& object, const wchar_t* key) {
    if (object.HasKey(key)) {
        const IJsonValue value = object.GetNamedValue(key);
        if (value.ValueType() == JsonValueType::Array) {
            return value.GetArray();
        }
    }
    return JsonArray{};
}

bool ParseObject(const std::string& text, JsonObject& object) {
    try {
        return JsonObject::TryParse(winrt::to_hstring(text), object);
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

// ---- The saved list ----

fs::path SourcesFile(const fs::path& local_state) {
    return local_state / L"sources.json";
}

std::vector<Source> LoadSources(const fs::path& local_state) {
    std::vector<Source> sources;
    try {
        std::ifstream in(SourcesFile(local_state), std::ios::binary);
        if (!in) {
            return sources;
        }
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        JsonObject root;
        if (!ParseObject(text, root)) {
            return sources;
        }
        for (const IJsonValue& value : ArrayAt(root, L"sources")) {
            if (value.ValueType() != JsonValueType::Object) {
                continue;
            }
            const JsonObject item = value.GetObject();
            Source source;
            source.name = StringAt(item, L"name");
            source.url = StringAt(item, L"url");
            if (Tinfoil::IsLocation(source.url)) {
                if (source.name.empty()) {
                    source.name = HostOf(source.url);
                }
                sources.push_back(std::move(source));
            }
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic("SOURCES_LOAD failed " + winrt::to_string(error.message()));
    }
    return sources;
}

bool SaveSources(const fs::path& local_state, const std::vector<Source>& sources) {
    try {
        JsonArray list;
        for (const Source& source : sources) {
            JsonObject item;
            item.SetNamedValue(L"name", JsonValue::CreateStringValue(winrt::to_hstring(source.name)));
            item.SetNamedValue(L"url", JsonValue::CreateStringValue(winrt::to_hstring(source.url)));
            list.Append(item);
        }
        JsonObject root;
        root.SetNamedValue(L"sources", list);
        std::error_code ec;
        fs::create_directories(local_state, ec);
        const fs::path file = SourcesFile(local_state);
        const fs::path temporary = local_state / L"sources.json.tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out << winrt::to_string(root.Stringify());
            out.flush();
            if (!out) {
                Diagnostic("SOURCES_SAVE write failed");
                return false;
            }
        }
        fs::rename(temporary, file, ec);
        if (ec) {
            // rename refused to replace an existing file: remove it and try once more.
            fs::remove(file, ec);
            ec.clear();
            fs::rename(temporary, file, ec);
        }
        if (ec) {
            Diagnostic("SOURCES_SAVE rename failed " + ec.message());
            return false;
        }
        return true;
    } catch (const winrt::hresult_error& error) {
        Diagnostic("SOURCES_SAVE failed " + winrt::to_string(error.message()));
        return false;
    }
}

// False when the address is already in the list (compared without case or a trailing slash).
bool AddSource(std::vector<Source>& sources, Source source) {
    const auto key = [](const std::string& url) {
        std::string lower = Lower(url);
        while (!lower.empty() && lower.back() == '/') {
            lower.pop_back();
        }
        return lower;
    };
    const std::string wanted = key(source.url);
    for (const Source& existing : sources) {
        if (key(existing.url) == wanted) {
            return false;
        }
    }
    if (source.name.empty()) {
        source.name = HostOf(source.url);
    }
    sources.push_back(std::move(source));
    return true;
}

// sources.txt: one URL per line, or "name|url". Blank lines and lines starting with # are skipped.
std::vector<Source> ParseSourcesText(const std::string& text) {
    std::vector<Source> found;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = Trim(text.substr(start, end - start));
        start = end + 1;
        if (!line.empty() && static_cast<unsigned char>(line[0]) == 0xEF && line.size() >= 3 &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line = Trim(line.substr(3)); // a UTF-8 byte order mark
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        Source source;
        const std::size_t bar = line.find('|');
        if (!Tinfoil::IsLocation(line) && bar != std::string::npos) {
            source.name = Trim(line.substr(0, bar));
            source.url = Trim(line.substr(bar + 1));
        } else {
            source.url = line;
        }
        if (Tinfoil::IsLocation(source.url)) {
            found.push_back(std::move(source));
        }
    }
    return found;
}

// Looks for sources.txt at the root of each removable drive and in its switch\ folder.
ImportResult ScanDrivesForSources(const std::atomic<bool>& cancel) {
    ImportResult result;
    try {
        const auto drives = AwaitBounded(KnownFolders::RemovableDevices().GetFoldersAsync(),
                                         std::chrono::seconds(5));
        if (!drives) {
            return result;
        }
        for (const auto& drive : *drives) {
            for (const wchar_t* relative : {L"sources.txt", L"switch\\sources.txt"}) {
                if (cancel.load()) {
                    return result;
                }
                try {
                    const auto item =
                        AwaitBounded(drive.TryGetItemAsync(relative), std::chrono::seconds(3));
                    const auto file = item && *item ? item->try_as<StorageFile>() : nullptr;
                    if (!file) {
                        continue;
                    }
                    const auto text =
                        AwaitBounded(FileIO::ReadTextAsync(file), std::chrono::seconds(10));
                    if (!text) {
                        continue;
                    }
                    for (Source& source : ParseSourcesText(winrt::to_string(*text))) {
                        result.sources.push_back(std::move(source));
                    }
                } catch (const winrt::hresult_error& error) {
                    Diagnostic("SOURCES_IMPORT skip " + winrt::to_string(error.message()));
                }
            }
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic("SOURCES_IMPORT failed " + winrt::to_string(error.message()));
    }
    Diagnostic("SOURCES_IMPORT found=" + std::to_string(result.sources.size()));
    return result;
}

// Claims a .partial path for one download thread at a time.
struct PartialClaim {
    explicit PartialClaim(fs::path path_) : path{std::move(path_)} {
        std::scoped_lock lock{Mutex()};
        owned = Paths().insert(path).second;
    }
    ~PartialClaim() {
        if (!owned)
            return;
        std::scoped_lock lock{Mutex()};
        Paths().erase(path);
    }
    PartialClaim(const PartialClaim&) = delete;
    PartialClaim& operator=(const PartialClaim&) = delete;
    static std::mutex& Mutex() {
        static std::mutex mutex;
        return mutex;
    }
    static std::set<fs::path>& Paths() {
        static std::set<fs::path> paths;
        return paths;
    }
    fs::path path;
    bool owned{};
};

// ---- Reading a source ----

// GETs a text body. False, with a log line, on a network error or an unsuccessful status.
bool HttpGetText(const HttpClient& client, const Tinfoil::Request& request, std::string& body,
                 std::string& effective_url) {
    Tinfoil::Headers headers = request.headers;
    const std::string url = Tinfoil::Authenticate(request.url, headers);
    try {
        const winrt::Windows::Web::Http::HttpRequestMessage message{
            winrt::Windows::Web::Http::HttpMethod::Get(), Uri{winrt::to_hstring(url)}};
        for (const auto& [name, value] : headers) {
            void(message.Headers().TryAppendWithoutValidation(winrt::to_hstring(name),
                                                              winrt::to_hstring(value)));
        }
        const auto response = AwaitBounded(
            client.SendRequestAsync(
                message, winrt::Windows::Web::Http::HttpCompletionOption::ResponseHeadersRead),
            kRequestTimeout);
        if (!response) {
            Diagnostic("SOURCES_FETCH " + RedactedUrl(url) + " timed out");
            return false;
        }
        if (!response->IsSuccessStatusCode()) {
            Diagnostic(fmt::format("SOURCES_FETCH {} status={}", RedactedUrl(url),
                                   static_cast<int>(response->StatusCode())));
            return false;
        }
        effective_url = winrt::to_string(response->RequestMessage().RequestUri().AbsoluteUri());
        // Preserve binary bytes for encrypted-index detection, but never buffer an unlimited
        // response (a mistaken package URL may be gigabytes). Autoindex traversal is bounded too.
        using namespace winrt::Windows::Storage::Streams;
        const auto stream =
            AwaitBounded(response->Content().ReadAsInputStreamAsync(), kRequestTimeout);
        if (!stream)
            return false;
        constexpr std::size_t max_body = 16u << 20;
        Buffer buffer{64u << 10};
        body.clear();
        for (;;) {
            const auto read = AwaitBounded(
                stream->ReadAsync(buffer, buffer.Capacity(), InputStreamOptions::Partial),
                kRequestTimeout);
            if (!read || read->Length() > max_body - body.size())
                return false;
            if (read->Length() == 0)
                break;
            body.append(reinterpret_cast<const char*>(read->data()), read->Length());
        }
        return true;
    } catch (const winrt::hresult_error&) {
        // HTTP exception messages can include request header values.
        Diagnostic("SOURCES_FETCH " + RedactedUrl(url) + " failed");
        return false;
    }
}

Text UnsupportedText(Tinfoil::Unsupported reason) {
    switch (reason) {
    case Tinfoil::Unsupported::Encrypted:
        return Text::ShopEncrypted;
    case Tinfoil::Unsupported::Ftp:
        return Text::ShopFtpUnsupported;
    case Tinfoil::Unsupported::GoogleDrive:
        return Text::ShopDriveUnsupported;
    case Tinfoil::Unsupported::OneFichier:
        return Text::ShopFichierUnsupported;
    default:
        return Text::SrcUnsupported;
    }
}

FetchResult FetchSource(Tinfoil::Traversal traversal, std::size_t remaining,
                        const std::atomic<bool>& cancel) {
    FetchResult result;
    HttpClient client;
    // A failure to add the header is not worth failing the request for.
    void(client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"NXbox/1.0"));
    // A page reads one nonempty index, or at most four empty/failed indexes.
    int requests = 0;
    while (!cancel.load() && result.entries.empty() && requests++ < 4) {
        const auto request = traversal.Next();
        if (!request) {
            break;
        }
        const auto support = Tinfoil::LocationSupport(request->url);
        if (support != Tinfoil::Unsupported::None) {
            result.failure = Failure::Unsupported;
            result.unsupported = support;
            result.errors[request->url] = Tr(UnsupportedText(support));
            if (!traversal.HasNext())
                break;
            result.partial = true;
            continue;
        }
        std::string body;
        std::string effective_url;
        if (!HttpGetText(client, *request, body, effective_url)) {
            if (request->depth == 0) {
                result.failure = Failure::Unreachable;
                result.errors[request->url] = Tr(Text::SrcUnreachable);
                if (!traversal.HasNext())
                    return result;
            }
            result.partial = true;
            continue;
        }
        const Tinfoil::Index index = Tinfoil::ParseIndex(body, effective_url);
        if (index.status != Tinfoil::Status::Ok) {
            if (request->depth == 0) {
                result.failure = index.status == Tinfoil::Status::SourceError
                                     ? Failure::SourceError
                                     : Failure::Unsupported;
                result.message = Widen(index.error);
                result.unsupported = index.unsupported;
                if (index.unsupported == Tinfoil::Unsupported::Encrypted)
                    result.encrypted_sources.insert(request->url);
                result.errors[request->url] = index.status == Tinfoil::Status::SourceError
                                                  ? Widen(index.error)
                                                  : Tr(UnsupportedText(index.unsupported));
                Diagnostic("SOURCES_FETCH " + RedactedUrl(request->url) + " unusable index");
                if (!traversal.HasNext())
                    return result;
            }
            result.partial = true;
            continue;
        }
        result.locations.insert(result.locations.end(), index.locations.begin(),
                                index.locations.end());
        for (const auto& location : index.locations)
            traversal.AddRoot(location);
        result.failure = Failure::None;
        result.message = Widen(index.success);
        for (const Tinfoil::File& file : index.files) {
            if (result.entries.size() >= remaining) {
                break;
            }
            Entry entry;
            entry.name = Widen(file.name);
            entry.url = file.url;
            entry.icon_url = file.icon_url;
            entry.banner_url = file.banner_url;
            entry.art_origin = effective_url;
            entry.headers = Tinfoil::Origin(effective_url) == Tinfoil::Origin(request->url)
                                ? request->headers
                                : Tinfoil::Headers{};
            for (const auto& header : index.headers) {
                entry.headers.erase(std::remove_if(entry.headers.begin(), entry.headers.end(),
                                                   [&](const auto& old) {
                                                       return Lower(old.first) ==
                                                              Lower(header.first);
                                                   }),
                                    entry.headers.end());
                entry.headers.push_back(header);
            }
            entry.art_headers = entry.headers;
            if (Tinfoil::Origin(entry.url) != Tinfoil::Origin(request->url))
                entry.headers.clear(); // Never send one shop's credentials to another host.
            entry.file = DiskName(entry.name, entry.url);
            entry.key = LowerWide(entry.file);
            entry.size = file.size;
            entry.kind = file.kind;
            result.entries.push_back(std::move(entry));
        }
        traversal.Follow(*request, index);
    }
    if (traversal.HasNext() && result.entries.size() < remaining)
        result.traversal = std::move(traversal);
    Diagnostic("SOURCES_FETCH page files=" + std::to_string(result.entries.size()));
    return result;
}

// The shared downloader has no request-header argument. Keep its storage/resume behavior for
// ordinary links and stream authenticated shop packages here without buffering them in memory.
bool DownloadShopPackage(const Entry& entry, fs::path& destination,
                         const std::function<void(std::uint64_t, std::uint64_t)>& progress,
                         const std::atomic<bool>& cancel) {
    auto headers = entry.headers;
    const auto url = Tinfoil::Authenticate(entry.url, headers);
    if (headers.empty())
        return DownloadFile(url, destination, [](const fs::path&) {}, progress, &cancel);
    using namespace winrt::Windows::Web::Http;
    using namespace winrt::Windows::Storage::Streams;
    for (int attempt = 0; attempt < 3 && !cancel.load(); ++attempt) {
        try {
            std::error_code ec;
            fs::create_directories(destination.parent_path(), ec);
            auto have = fs::exists(destination, ec) ? fs::file_size(destination, ec) : 0;
            if (ec)
                return false;
            HttpClient client;
            HttpRequestMessage request{HttpMethod::Get(), Uri{winrt::to_hstring(url)}};
            for (const auto& [name, value] : headers) {
                // Range and framing are owned by the transfer, never by the index.
                const auto key = Lower(name);
                if (key != "range" && key != "host" && key != "content-length")
                    void(request.Headers().TryAppendWithoutValidation(winrt::to_hstring(name),
                                                                      winrt::to_hstring(value)));
            }
            if (have)
                void(request.Headers().TryAppendWithoutValidation(
                    L"Range", winrt::to_hstring("bytes=" + std::to_string(have) + "-")));
            const auto response = AwaitBounded(
                client.SendRequestAsync(request, HttpCompletionOption::ResponseHeadersRead),
                kRequestTimeout);
            if (!response)
                continue;
            if (static_cast<int>(response->StatusCode()) == 416 && have > 0) {
                const auto range = response->Content().Headers().ContentRange();
                if (range && range.Length() && range.Length().Value() == have) {
                    progress(have, have);
                    return true;
                }
            }
            if (!response->IsSuccessStatusCode())
                continue;
            if (response->StatusCode() == HttpStatusCode::PartialContent) {
                const auto range = response->Content().Headers().ContentRange();
                if (!range || !range.FirstBytePosition() ||
                    range.FirstBytePosition().Value() != have)
                    return false;
            } else {
                have = 0;
            }
            const auto length = response->Content().Headers().ContentLength();
            if (!length)
                return false;
            if (length.Value() > std::numeric_limits<std::uint64_t>::max() - have)
                return false;
            const auto total = have + length.Value();
            const auto free = FreeSpace(destination.parent_path());
            if (!free)
                return false;
            if (*free < total - have) {
                bool relocated = false;
                for (const auto& folder : ExternalGameFolders()) {
                    const auto candidate = folder / destination.filename();
                    if (candidate == destination)
                        continue;
                    const auto size = fs::exists(candidate, ec) ? fs::file_size(candidate, ec) : 0;
                    const auto available = FreeSpace(folder);
                    if (!ec && size <= total && available && *available >= total - size) {
                        destination = candidate;
                        relocated = true;
                        break;
                    }
                }
                if (relocated)
                    continue; // reopen with the selected drive's Range, not the previous offset
                return false;
            }
            const auto stream =
                AwaitBounded(response->Content().ReadAsInputStreamAsync(), kRequestTimeout);
            if (!stream)
                continue;
            std::ofstream out(destination,
                              std::ios::binary | (have ? std::ios::app : std::ios::trunc));
            if (!out)
                return false;
            Buffer buffer{1u << 20};
            progress(have, total);
            while (have < total && !cancel.load()) {
                const auto read = AwaitBounded(
                    stream->ReadAsync(buffer, buffer.Capacity(), InputStreamOptions::Partial),
                    kRequestTimeout);
                if (!read || read->Length() == 0 || read->Length() > total - have)
                    break;
                out.write(reinterpret_cast<const char*>(read->data()), read->Length());
                if (!out)
                    return false;
                have += read->Length();
                progress(have, total);
            }
            out.flush();
            if (out && have == total && !cancel.load())
                return true;
        } catch (const winrt::hresult_error&) {
            Diagnostic("SOURCES_DOWNLOAD retry"); // never log headers or credentials
        }
    }
    return false;
}

// Runs `body(task)` on a detached thread with its own MTA apartment.
template <typename Result, typename Body>
std::shared_ptr<Task<Result>> Spawn(Body body) {
    auto task = std::make_shared<Task<Result>>();
    std::thread([task, body = std::move(body)]() mutable {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Apartment {
                ~Apartment() {
                    winrt::uninit_apartment();
                }
            } apartment;
            body(*task);
        } catch (const winrt::hresult_error& error) {
            Diagnostic("SOURCES worker failed " + winrt::to_string(error.message()));
        } catch (const std::exception& error) {
            Diagnostic(std::string("SOURCES worker failed ") + error.what());
        }
        task->done.store(true);
    }).detach();
    return task;
}

enum class Mode { List, Keyboard, Importing, Browse, Detail, Queue, Confirm };
enum class Outcome { Pending, Done, Failed, Cancelled };

struct Cursor {
    int selected = 0;
    int top = 0;
};

class SourcesScreen {
public:
    SourcesScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                  const fs::path& local_state)
        : renderer_(renderer), window_(window), input_(input), local_state_(local_state) {}

    ~SourcesScreen() {
        if (fetch_) {
            fetch_->cancel.store(true);
        }
        if (import_) {
            import_->cancel.store(true);
        }
        cancel_.store(true);
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
    }

    // Returns true when the window was closed.
    bool Run() {
        const auto closed_token =
            window_.Closed([this](const auto&, const auto&) { closed_ = true; });
        SCOPE_EXIT {
            window_.Closed(closed_token);
        };
        Diagnostic("UI sources screen open");
        sources_ = LoadSources(local_state_);
        while (!closed_ && !leaving_) {
            const Clock::time_point frame_start = Clock::now();
            window_.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            input_.Update();
            const Clock::time_point now = Clock::now();
            Update(now);
            renderer_.BeginFrame();
            Draw(now);
            renderer_.EndFrame();
            const Clock::duration spent = Clock::now() - frame_start;
            if (spent < std::chrono::milliseconds(8)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16) - spent);
            }
        }
        Diagnostic("UI sources screen closed");
        return closed_;
    }

    void InitializeShop() {
        shop_ = true;
        sources_ = LoadSources(local_state_);
        ScanInstalled();
        // Read the selected profile through existing APIs. Never mutate account state.
        const Service::Account::ProfileManager profiles;
        const auto user =
            profiles.GetUser(static_cast<std::size_t>(Settings::values.current_user.GetValue()));
        Service::Account::ProfileBase profile{};
        if (user && profiles.GetProfileBase(*user, profile)) {
            const auto end = std::find(profile.username.begin(), profile.username.end(), 0);
            profile_name_ = Widen(std::string(profile.username.begin(), end));
            const auto path =
                Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) /
                ("system/save/8000000000000010/su/avators/" + user->FormattedString() + ".jpg");
            Pixels pixels;
            if (renderer_.DecodeImage(ReadFileBytes(path), pixels))
                renderer_.UploadImage(pixels, profile_avatar_);
        }
        ReloadShop();
    }

    void ReloadShop() {
        // A settings round-trip may invalidate a detail page kept as Queue's return target.
        queue_return_ = Mode::Browse;
        queue_section_ = tab_;
        detail_items_.clear();
        sidebar_focus_ = true;
        sources_ = LoadSources(local_state_);
        if (!sources_.empty()) {
            browse_url_ = sources_.front().url;
            LoadBrowse();
        } else {
            if (fetch_)
                fetch_->cancel.store(true);
            ++fetch_serial_;
            fetch_.reset();
            failure_ = Failure::None;
            unsupported_ = Tinfoil::Unsupported::None;
            fetch_message_.clear();
            partial_ = false;
            art_.clear();
            source_errors_.clear();
            encrypted_sources_.clear();
            entries_.clear();
            packages_.clear();
            games_.clear();
            visible_.clear();
            traversal_.reset();
        }
        SetMode(Mode::Browse);
    }

    void TickShop(Clock::time_point now, bool active) {
        if (mode_ != Mode::Detail && !(mode_ == Mode::Queue && queue_return_ == Mode::Detail))
            AdoptFetch();
        UpdateDownload(now);
        // Fetch the bounded traversal incrementally without requiring a Load more tile.
        if (!Loading() && traversal_ && entries_.size() < kMaxEntries && mode_ != Mode::Detail &&
            !(mode_ == Mode::Queue && queue_return_ == Mode::Detail))
            LoadBrowse(true);
        if (storage_ && storage_->done.load()) {
            internal_space_ = storage_->result.internal;
            external_space_ = storage_->result.external;
            storage_.reset();
        }
        if (!storage_ && now >= storage_refresh_) {
            storage_refresh_ = now + std::chrono::seconds(5);
            storage_ = Spawn<StorageResult>([path = local_state_](Task<StorageResult>& task) {
                std::error_code ec;
                task.result.internal = fs::space(path, ec);
                if (ec)
                    task.result.internal = {};
                if (const auto free = FreeSpace(path))
                    task.result.internal.available = *free;
                const auto folders = ExternalGameFolders();
                if (!folders.empty()) {
                    task.result.external = fs::space(folders.front(), ec);
                    if (ec)
                        task.result.external = {};
                    if (const auto free = FreeSpace(folders.front()))
                        task.result.external.available = *free;
                }
            });
        }
        if (!active)
            return;
        if (mode_ == Mode::Keyboard) {
            UpdateKeyboard(now);
            return;
        }
        if (mode_ == Mode::Detail) {
            UpdateDetail(now);
            return;
        }
        if (sidebar_focus_) {
            if (mode_ != Mode::Browse)
                SetMode(Mode::Browse);
            queue_return_ = Mode::Browse;
            if (input_.Pressed(Button::Up)) {
                if (section_ == 0)
                    navigation_requested_ = true;
                else
                    --section_;
            }
            if (input_.Pressed(Button::Down))
                section_ = std::min(section_ + 1, 6);
            if (input_.Pressed(Button::B))
                navigation_requested_ = true;
            if (section_ < 4 && tab_ != section_) {
                tab_ = section_;
                query_.clear(); // A search only filters the Search section.
                browse_cursor_ = {};
                FilterCatalog();
            }
            if (input_.Pressed(Button::Right) || input_.Pressed(Button::A)) {
                if ((sources_.empty() && section_ != 5) ||
                    (section_ == 6 && input_.Pressed(Button::A))) {
                    sources_requested_ = true;
                } else if (section_ == 4) {
                    tab_ = 3;
                    FilterCatalog();
                    searching_ = true;
                    entry_ = std::make_unique<TextEntry>(Tr(Text::SrcSearch), query_);
                    SetMode(Mode::Keyboard);
                    sidebar_focus_ = false;
                } else {
                    sidebar_focus_ = false;
                }
            }
            return;
        }
        if (input_.Pressed(Button::B) ||
            (section_ < 5 && input_.Pressed(Button::Left) && browse_cursor_.selected % 4 == 0)) {
            sidebar_focus_ = true;
            queue_return_ = Mode::Browse;
            SetMode(Mode::Browse);
            return;
        }
        if (sources_.empty() && section_ != 5) {
            if (input_.Pressed(Button::A))
                sources_requested_ = true;
            return;
        }
        if (mode_ == Mode::Queue && input_.Pressed(Button::View)) {
            section_ = queue_section_;
            SetMode(queue_return_);
            return;
        }
        if (section_ == 6) {
            if (input_.Pressed(Button::Up))
                source_cursor_ = std::max(0, source_cursor_ - 1);
            if (input_.Pressed(Button::Down))
                source_cursor_ = std::min(std::max(0, static_cast<int>(sources_.size()) - 1),
                                          source_cursor_ + 1);
            if (input_.Pressed(Button::A))
                sources_requested_ = true;
        } else if (section_ == 5) {
            if (mode_ != Mode::Queue)
                SetMode(Mode::Queue);
            UpdateQueue();
        } else {
            if (mode_ != Mode::Browse && mode_ != Mode::Detail && mode_ != Mode::Keyboard)
                SetMode(Mode::Browse); // SetMode resets the scroll tween.
            UpdateBrowse(now);
        }
    }

    void DrawShop(Clock::time_point now, bool navigation_focused) {
        // Background fetches/downloads must not consume a message's display time on another tab.
        if ((toast_.empty() || now >= toast_until_) && !pending_toasts_.empty()) {
            toast_ = std::move(pending_toasts_.front());
            pending_toasts_.erase(pending_toasts_.begin());
            toast_until_ = now + kToastDuration;
        }
        navigation_focused_ = navigation_focused;
        Draw(now);
    }
    void SetTinfoilAvailable(bool available) {
        tinfoil_available_ = available;
    }
    bool TakeTinfoilRequest() {
        return std::exchange(tinfoil_requested_, false);
    }
    bool TakeSourcesRequest() {
        return std::exchange(sources_requested_, false);
    }
    bool TakeNavigationRequest() {
        return std::exchange(navigation_requested_, false);
    }
    bool Typing() const {
        return mode_ == Mode::Keyboard;
    }

private:
    // ---- State ----

    int ListRows() const { return 2 + static_cast<int>(sources_.size()); }
    int BrowseRows() const {
        return static_cast<int>(visible_.size()) +
               ((!shop_ && traversal_) || HasEncryptedShop() ? 1 : 0);
    }
    bool HasEncryptedShop() const {
        return shop_ && !encrypted_sources_.empty();
    }
    bool InBrowse() const {
        return mode_ == Mode::Browse || mode_ == Mode::Detail ||
               (mode_ == Mode::Queue && queue_return_ != Mode::List) ||
               (mode_ == Mode::Keyboard && searching_);
    }
    Cursor& CurrentCursor() { return InBrowse() ? browse_cursor_ : list_cursor_; }
    int CurrentRows() const { return InBrowse() ? BrowseRows() : ListRows(); }
    bool Loading() const { return fetch_ && !fetch_->done.load(); }

    // Which source row is focused, or -1 on one of the two action rows.
    int FocusedSource() const { return list_cursor_.selected - 2; }

    void Toast(const std::wstring& text, Clock::time_point now) {
        if (text.empty()) {
            return;
        }
        if (shop_) {
            if (pending_toasts_.size() < 128)
                pending_toasts_.push_back(text);
            return;
        }
        toast_ = text;
        toast_until_ = now + kToastDuration;
    }

    void SetMode(Mode mode) {
        mode_ = mode;
        scroll_ = Tween(static_cast<float>(CurrentCursor().top));
    }

    void OpenKeyboard(const std::wstring& initial) {
        entry_ = std::make_unique<TextEntry>(Tr(Text::SrcEnterUrl), initial);
        SetMode(Mode::Keyboard);
    }

    void LoadBrowse(bool more = false) {
        if (fetch_) {
            fetch_->cancel.store(true);
        }
        ++fetch_serial_;
        if (!more) {
            shop_art_ = std::make_unique<ShopArtSource>(local_state_);
            source_errors_.clear();
            encrypted_sources_.clear();
            entries_.clear();
            packages_.clear();
            games_.clear();
            visible_.clear();
            art_.clear();
            browse_cursor_ = {};
            shelf_ = Tween(0.0f);
            fetch_message_.clear();
            partial_ = false;
            traversal_ = Tinfoil::Traversal(browse_url_);
            if (shop_)
                for (const auto& source : sources_)
                    traversal_->AddRoot(source.url);
        }
        failure_ = Failure::None;
        auto traversal = std::move(*traversal_);
        traversal_.reset();
        fetch_ = Spawn<FetchResult>(
            [traversal = std::move(traversal),
             remaining = kMaxEntries - entries_.size()](Task<FetchResult>& task) mutable {
                task.result = FetchSource(std::move(traversal), remaining, task.cancel);
            });
    }

    void RebuildCatalog() {
        std::vector<std::uint64_t> sizes;
        packages_.clear();
        for (const auto& entry : entries_) {
            packages_.push_back(Tinfoil::ParsePackage(Utf8(entry.name)));
            sizes.push_back(entry.size);
        }
        games_ = Tinfoil::Group(packages_, sizes);
        FilterCatalog();
        // A source with only updates or DLC would open on an empty "New games" tab.
        while (!shop_ && visible_.empty() && query_.empty() && tab_ < 3 && !games_.empty()) {
            ++tab_;
            FilterCatalog();
        }
    }

    void FilterCatalog() {
        visible_ = Tinfoil::Select(games_, packages_, static_cast<Tinfoil::Tab>(tab_),
                                   static_cast<Tinfoil::Sort>(sort_));
        const auto query = LowerWide(query_);
        visible_.erase(std::remove_if(visible_.begin(), visible_.end(),
                                      [&](auto index) {
                                          return LowerWide(Widen(games_[index].name)).find(query) ==
                                                 std::wstring::npos;
                                      }),
                       visible_.end());
        browse_cursor_.selected =
            std::clamp(browse_cursor_.selected, 0, std::max(BrowseRows() - 1, 0));
    }

    void ScanInstalled() {
        installed_.clear();
        installed_files_.clear();
        installed_sizes_.clear();
        installed_paths_.clear();
        std::error_code ec;
        // Same scope as the library scan: games\ and its subfolders.
        for (fs::recursive_directory_iterator it(local_state_ / L"games",
                                                 fs::directory_options::skip_permission_denied, ec),
             end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) &&
                IsGameExtension(LowerWide(it->path().extension().wstring()))) {
                const auto size = it->file_size(ec);
                if (ec || size == 0)
                    continue;
                installed_paths_.push_back(it->path());
                installed_sizes_.push_back(size);
                installed_files_.push_back(LowerWide(it->path().filename().wstring()));
                installed_.push_back(Tinfoil::ParsePackage(Utf8(it->path().filename().wstring())));
            }
        }
        // The library can identify base games whose filenames contain no title ID. Trust a
        // cache record only while its path, size and modification time still match a local file.
        ec.clear();
        for (fs::directory_iterator it(local_state_ / L"library", ec), end; !ec && it != end;
             it.increment(ec)) {
            if (it->path().extension() != L".json" || it->path().stem().wstring().size() != 16)
                continue;
            std::ifstream in(it->path(), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            JsonObject record;
            if (!ParseObject(text, record))
                continue;
            const fs::path path = Widen(StringAt(record, L"path"));
            const auto id = StringAt(record, L"title_id");
            for (std::size_t i = 0; i < installed_paths_.size(); ++i) {
                if (!installed_[i].title_id.empty() ||
                    LowerWide(path.lexically_normal().wstring()) !=
                        LowerWide(installed_paths_[i].lexically_normal().wstring()))
                    continue;
                if (!record.HasKey(L"size") || !record.HasKey(L"mtime"))
                    continue;
                const auto size = record.GetNamedValue(L"size");
                const auto mtime = record.GetNamedValue(L"mtime");
                if (size.ValueType() != JsonValueType::Number ||
                    mtime.ValueType() != JsonValueType::Number)
                    continue;
                const auto modified = fs::last_write_time(installed_paths_[i], ec);
                if (ec || size.GetNumber() != static_cast<double>(installed_sizes_[i]) ||
                    mtime.GetNumber() !=
                        static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(
                                                modified.time_since_epoch())
                                                .count()))
                    continue;
                const auto package = Tinfoil::ParsePackage("[" + id + "]");
                if (package.kind == Kind::Game)
                    installed_[i] = package;
            }
        }
    }

    bool Installed(std::size_t index) const {
        const auto& entry = entries_[index];
        for (std::size_t i = 0; i < installed_.size(); ++i) {
            const bool same_file = installed_files_[i] == LowerWide(entry.file);
            const bool same_package = Tinfoil::IsInstalled(packages_[index], installed_[i]);
            // The advertised size distinguishes an old incomplete file with the same name.
            // Different formats and newer local versions can naturally have different sizes.
            const bool newer = installed_[i].version && packages_[index].version &&
                               *installed_[i].version > *packages_[index].version;
            if (same_file && entry.size > 0 && !newer && entry.size != installed_sizes_[i])
                continue;
            if (same_file || same_package)
                return true;
        }
        return false;
    }

    void AdoptFetch() {
        if (!fetch_ || !fetch_->done.load() || fetch_adopted_ == fetch_serial_)
            return;
        fetch_adopted_ = fetch_serial_;
        std::string selected_id;
        if (browse_cursor_.selected >= 0 &&
            static_cast<std::size_t>(browse_cursor_.selected) < visible_.size())
            selected_id =
                games_[visible_[static_cast<std::size_t>(browse_cursor_.selected)]].base_id;
        for (auto& entry : fetch_->result.entries)
            entries_.push_back(std::move(entry));
        if (!fetch_->result.message.empty())
            fetch_message_ = fetch_->result.message;
        if (fetch_->result.failure != Failure::None) // A later page must not hide a failure.
            failure_ = fetch_->result.failure;
        unsupported_ = fetch_->result.unsupported;
        encrypted_sources_.insert(fetch_->result.encrypted_sources.begin(),
                                  fetch_->result.encrypted_sources.end());
        for (const auto& [url, error] : fetch_->result.errors) {
            source_errors_[url] = error;
            if (!encrypted_sources_.count(url) && announced_.insert(error).second)
                Toast(error, Clock::now());
        }
        if (!fetch_->result.message.empty() && failure_ == Failure::None &&
            announced_.insert(fetch_->result.message).second)
            Toast(fetch_->result.message, Clock::now());
        // Index "locations" are followed for this load only; persisting them would let any
        // index add sources the user never chose.
        partial_ = partial_ || fetch_->result.partial;
        traversal_ = std::move(fetch_->result.traversal);
        RebuildCatalog();
        if (!selected_id.empty())
            for (std::size_t i = 0; i < visible_.size(); ++i)
                if (games_[visible_[i]].base_id == selected_id) {
                    browse_cursor_.selected = static_cast<int>(i);
                    break;
                }
    }

    void StartImport() {
        import_ = Spawn<ImportResult>([](Task<ImportResult>& task) {
            task.result = ScanDrivesForSources(task.cancel);
        });
        SetMode(Mode::Importing);
    }

    void AdoptImport(Clock::time_point now) {
        if (!import_ || !import_->done.load()) {
            return;
        }
        int added = 0;
        for (Source& source : import_->result.sources) {
            if (AddSource(sources_, std::move(source))) {
                ++added;
            }
        }
        import_.reset();
        if (added > 0) {
            SaveSources(local_state_, sources_);
            Diagnostic("SOURCES_ADD imported=" + std::to_string(added));
            Toast(std::to_wstring(added) + L" " + Tr(Text::SrcImported), now);
        } else {
            Toast(Tr(Text::SrcImportNone), now);
        }
        SetMode(Mode::List);
    }

    void StartDownload(const Entry& entry) {
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
        cancel_.store(false);
        bytes_done_.store(0);
        bytes_total_.store(entry.size);
        rate_.store(0.0);
        outcome_.store(Outcome::Pending);
        const fs::path destination = local_state_ / L"games" / entry.file;
        download_thread_ = std::thread([this, package = entry, name = Utf8(entry.name),
                                        expected_size = entry.size, destination]() mutable {
            Outcome outcome = Outcome::Failed;
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                struct Apartment {
                    ~Apartment() {
                        winrt::uninit_apartment();
                    }
                } apartment;
                Clock::time_point sample_at = Clock::now();
                std::uint64_t sample_bytes = 0;
                bool first = true;
                const auto progress = [&](std::uint64_t have, std::uint64_t total) {
                    bytes_done_.store(have);
                    bytes_total_.store(total);
                    const Clock::time_point at = Clock::now();
                    if (first) {
                        first = false;
                        sample_at = at;
                        sample_bytes = have;
                        return;
                    }
                    if (at - sample_at >= kRateWindow) {
                        const double seconds = std::chrono::duration<double>(at - sample_at).count();
                        rate_.store(static_cast<double>(have - sample_bytes) / seconds);
                        sample_at = at;
                        sample_bytes = have;
                    }
                };
                // Keep incomplete packages out of the library and the installed inventory.
                // DownloadFile still resumes this partial path, including on removable storage.
                fs::path target = destination;
                target += L".partial";
                std::error_code ec;
                if (expected_size > 0 && fs::exists(destination, ec) &&
                    fs::file_size(destination, ec) < expected_size && !fs::exists(target, ec)) {
                    fs::rename(destination, target, ec); // resume a pre-catalog partial download
                }
                // The Shop tab and Settings > Sources each own a downloader; only one may
                // write a given .partial file.
                const PartialClaim claim{target};
                bool ok = claim.owned && DownloadShopPackage(package, target, progress, cancel_);
                if (ok) {
                    fs::path complete = target;
                    complete.replace_extension(); // removes only the trailing .partial
                    fs::rename(target, complete, ec);
                    ok = !ec;
                }
                outcome = ok ? Outcome::Done : (cancel_.load() ? Outcome::Cancelled : Outcome::Failed);
            } catch (const winrt::hresult_error& error) {
                Diagnostic("SOURCES_DOWNLOAD worker failed " + winrt::to_string(error.message()));
            } catch (const std::exception& error) {
                Diagnostic(std::string("SOURCES_DOWNLOAD worker failed ") + error.what());
            }
            Diagnostic("SOURCES_DOWNLOAD " + name +
                       (outcome == Outcome::Done        ? " ok"
                        : outcome == Outcome::Cancelled ? " cancelled"
                                                        : " failed"));
            outcome_.store(outcome);
        });
    }

    // ---- Input ----

    void Update(Clock::time_point now) {
        AdoptFetch();
        UpdateDownload(now);
        switch (mode_) {
        case Mode::Keyboard:
            UpdateKeyboard(now);
            return;
        case Mode::Importing:
            if (input_.Pressed(Button::B)) {
                import_->cancel.store(true);
                import_.reset();
                SetMode(Mode::List);
            } else {
                AdoptImport(now);
            }
            return;
        case Mode::Detail:
            UpdateDetail(now);
            return;
        case Mode::Queue:
            UpdateQueue();
            return;
        case Mode::Confirm:
            UpdateConfirm(now);
            return;
        case Mode::Browse:
            UpdateBrowse(now);
            break;
        case Mode::List:
            UpdateList(now);
            break;
        }
        Scroll(now);
    }

    void Scroll(Clock::time_point now) {
        Cursor& cursor = CurrentCursor();
        cursor.selected = std::clamp(cursor.selected, 0, std::max(CurrentRows() - 1, 0));
        if (cursor.selected < cursor.top) {
            cursor.top = cursor.selected;
        } else if (cursor.selected > cursor.top + kVisibleRows - 1) {
            cursor.top = cursor.selected - (kVisibleRows - 1);
        }
        cursor.top = std::max(cursor.top, 0);
        if (scroll_.Target() != static_cast<float>(cursor.top)) {
            scroll_.To(static_cast<float>(cursor.top), now, kDurationPanel);
        }
    }

    void MoveCursor(int rows) {
        Cursor& cursor = CurrentCursor();
        if (input_.Pressed(Button::Up)) {
            cursor.selected = std::max(cursor.selected - 1, 0);
        }
        if (input_.Pressed(Button::Down)) {
            cursor.selected = std::min(cursor.selected + 1, std::max(rows - 1, 0));
        }
    }

    void UpdateList(Clock::time_point now) {
        if (input_.Pressed(Button::View) && !queue_.Empty()) {
            OpenQueue();
            return;
        }
        if (input_.Pressed(Button::B)) {
            leaving_ = true;
            return;
        }
        MoveCursor(ListRows());
        if (input_.Pressed(Button::X) && FocusedSource() >= 0) {
            confirm_focus_ = 1; // the safe answer
            SetMode(Mode::Confirm);
            return;
        }
        if (!input_.Pressed(Button::A)) {
            return;
        }
        if (list_cursor_.selected == 0) {
            OpenKeyboard({});
        } else if (list_cursor_.selected == 1) {
            StartImport();
        }
        (void)now;
    }

    void UpdateKeyboard(Clock::time_point now) {
        const TextEntry::Result result = entry_->Update(input_);
        if (result == TextEntry::Result::Editing) {
            return;
        }
        if (searching_) {
            if (result == TextEntry::Result::Confirmed) {
                query_ = entry_->Text();
                browse_cursor_ = {};
                shelf_ = Tween(0.0f);
                FilterCatalog();
            }
            searching_ = false;
            entry_.reset();
            SetMode(Mode::Browse);
            return;
        }
        const std::string typed = Trim(Utf8(entry_->Text()));
        if (result == TextEntry::Result::Cancelled || typed.empty()) {
            entry_.reset();
            SetMode(Mode::List);
            return;
        }
        if (!Tinfoil::IsLocation(typed)) {
            Toast(Tr(Text::SrcInvalidUrl), now);
            return; // the keyboard stays open with the text, to fix it
        }
        Source source;
        source.url = typed;
        if (!AddSource(sources_, std::move(source))) {
            Toast(Tr(Text::SrcDuplicate), now);
            entry_.reset();
            SetMode(Mode::List);
            return;
        }
        SaveSources(local_state_, sources_);
        Diagnostic("SOURCES_ADD " + RedactedUrl(typed));
        Toast(Tr(Text::SrcAdded), now);
        list_cursor_.selected = ListRows() - 1;
        entry_.reset();
        SetMode(Mode::List);
    }

    void UpdateConfirm(Clock::time_point now) {
        if (input_.Pressed(Button::B)) {
            SetMode(Mode::List);
            return;
        }
        if (input_.Pressed(Button::Left) || input_.Pressed(Button::Right)) {
            confirm_focus_ = 1 - confirm_focus_;
        }
        if (!input_.Pressed(Button::A)) {
            return;
        }
        const int index = FocusedSource();
        if (confirm_focus_ == 0 && index >= 0 && index < static_cast<int>(sources_.size())) {
            Diagnostic("SOURCES_REMOVE " + RedactedUrl(sources_[static_cast<std::size_t>(index)].url));
            sources_.erase(sources_.begin() + index);
            SaveSources(local_state_, sources_);
            Toast(Tr(Text::SrcRemoved), now);
        }
        SetMode(Mode::List);
    }

    void OpenQueue() {
        if (shop_) {
            queue_section_ = section_;
            section_ = 5;
        }
        queue_cursor_ = 0;
        queue_return_ = mode_;
        SetMode(Mode::Queue);
    }

    void UpdateBrowse(Clock::time_point now) {
        if (input_.Pressed(Button::B)) {
            if (fetch_)
                fetch_->cancel.store(true);
            SetMode(Mode::List);
            return;
        }
        if (input_.Pressed(Button::View)) {
            OpenQueue();
            return;
        }
        if (!shop_ &&
            (input_.Pressed(Button::LeftShoulder) || input_.Pressed(Button::RightShoulder))) {
            tab_ = (tab_ + (input_.Pressed(Button::RightShoulder) ? 1 : 3)) % 4;
            browse_cursor_ = {};
            shelf_ = Tween(0.0f);
            FilterCatalog();
        }
        if (input_.Pressed(Button::Y)) {
            if (shop_) {
                tab_ = 3;
                section_ = 4;
                FilterCatalog();
            }
            searching_ = true;
            entry_ = std::make_unique<TextEntry>(Tr(Text::SrcSearch), query_);
            SetMode(Mode::Keyboard);
            return;
        }
        if (input_.Pressed(Button::X)) {
            sort_ = (sort_ + 1) % 3;
            browse_cursor_ = {};
            shelf_ = Tween(0.0f);
            FilterCatalog();
        }
        if (input_.Pressed(Button::Menu) && !Loading() && !sources_.empty()) {
            LoadBrowse();
            return;
        }
        // Left/Right moves by one tile; Up/Down moves by a four-column grid row.
        if (input_.Pressed(Button::Left))
            browse_cursor_.selected = std::max(0, browse_cursor_.selected - 1);
        if (input_.Pressed(Button::Right))
            browse_cursor_.selected =
                std::min(std::max(0, BrowseRows() - 1), browse_cursor_.selected + 1);
        if (shop_) {
            if (input_.Pressed(Button::Up))
                browse_cursor_.selected = std::max(0, browse_cursor_.selected - 4);
            if (input_.Pressed(Button::Down))
                browse_cursor_.selected =
                    std::min(std::max(0, BrowseRows() - 1), browse_cursor_.selected + 4);
        }
        if (input_.Pressed(Button::A)) {
            const auto selected = static_cast<std::size_t>(browse_cursor_.selected);
            if (HasEncryptedShop() && selected == visible_.size()) {
                if (tinfoil_available_)
                    tinfoil_requested_ = true;
            } else if (!shop_ && selected == visible_.size() && traversal_) {
                LoadBrowse(true);
            } else if (selected < visible_.size()) {
                detail_game_ = visible_[selected];
                detail_items_ = games_[detail_game_].items;
                detail_cursor_ = 0;
                for (std::size_t i = 0; i < detail_items_.size(); ++i) {
                    if (Tinfoil::Matches(packages_[detail_items_[i]].kind,
                                         static_cast<Tinfoil::Tab>(tab_))) {
                        detail_cursor_ = static_cast<int>(i);
                        break;
                    }
                }
                panel_ = Tween(0.0f);
                panel_.To(1.0f, now, kDurationPanel);
                SetMode(Mode::Detail);
            }
        }
    }

    void UpdateDetail(Clock::time_point now) {
        if (input_.Pressed(Button::B)) {
            SetMode(Mode::Browse);
            return;
        }
        if (input_.Pressed(Button::View)) {
            OpenQueue();
            return;
        }
        if (input_.Pressed(Button::Up))
            detail_cursor_ = std::max(0, detail_cursor_ - 1);
        if (input_.Pressed(Button::Down))
            detail_cursor_ =
                std::min(static_cast<int>(detail_items_.size()) - 1, detail_cursor_ + 1);
        if (input_.Pressed(Button::A) && !detail_items_.empty()) {
            const auto index = detail_items_[static_cast<std::size_t>(detail_cursor_)];
            if (Installed(index)) {
                Toast(Tr(Text::SrcInstalled), now);
                return;
            }
            if (!queue_.Add(entries_[index])) {
                Toast(Tr(Text::SrcQueueFull), now);
                return;
            }
            Toast(Tr(Text::SrcQueued), now);
            if (!download_thread_.joinable())
                StartDownload(queue_.Front());
        }
    }

    void UpdateQueue() {
        if (input_.Pressed(Button::View) || (queue_.Empty() && input_.Pressed(Button::B))) {
            SetMode(queue_return_);
            return;
        }
        if (input_.Pressed(Button::Up))
            queue_cursor_ = std::max(0, queue_cursor_ - 1);
        if (input_.Pressed(Button::Down))
            queue_cursor_ =
                std::min(static_cast<int>(queue_.Items().size()) - 1, queue_cursor_ + 1);
        queue_cursor_ =
            std::clamp(queue_cursor_, 0, std::max(0, static_cast<int>(queue_.Items().size()) - 1));
        if (input_.Pressed(shop_ ? Button::X : Button::B) && !queue_.Empty()) {
            if (queue_cursor_ == 0)
                cancel_.store(true);
            else
                queue_.Remove(static_cast<std::size_t>(queue_cursor_));
        }
    }

    void UpdateDownload(Clock::time_point now) {
        if (!download_thread_.joinable() || outcome_.load() == Outcome::Pending)
            return;
        const Outcome outcome = outcome_.load();
        download_thread_.join();
        if (outcome == Outcome::Done) {
            // DownloadFile may choose another storage location; only mark an existing local file.
            ScanInstalled();
        }
        Toast(outcome == Outcome::Done        ? Tr(Text::SrcDownloadDone)
              : outcome == Outcome::Cancelled ? Tr(Text::SrcDownloadCancelled)
                                              : Tr(Text::SrcDownloadFailed),
              now);
        queue_.Remove(0);
        if (!queue_.Empty())
            StartDownload(queue_.Front());
    }

    // ---- Drawing ----

    void Draw(Clock::time_point now) {
        DrawHeader();
        if (shop_) {
            DrawSidebar();
            if (section_ == 6)
                DrawShopSources();
            else if (sources_.empty())
                DrawShopEmpty();
            else if (section_ != 5 && mode_ != Mode::Detail)
                DrawBrowse(now);
        } else if (InBrowse()) {
            DrawBrowse(now);
        } else {
            DrawListRows(now);
        }
        DrawTabs(renderer_, shop_ ? 1 : 2, shop_ && navigation_focused_);
        DrawClock(renderer_);
        if (shop_) {
            DrawStorage(Tr(Text::ShopInternalStorage), internal_space_, 1180.0f);
            DrawStorage(Tr(Text::ShopExternalStorage), external_space_, 1490.0f);
        }
        if (!shop_ && !queue_.Empty() && (mode_ == Mode::Browse || mode_ == Mode::List)) {
            const auto total = bytes_total_.load();
            const auto percent =
                total > 0 ? std::min<std::uint64_t>(
                                100, static_cast<std::uint64_t>(
                                         100.0 * static_cast<double>(bytes_done_.load()) /
                                         static_cast<double>(total)))
                          : 0;
            const std::wstring status = std::wstring(Tr(Text::SrcQueue)) + L" · " +
                                        std::to_wstring(queue_.Items().size()) + L" · " +
                                        std::to_wstring(percent) + L"%";
            renderer_.DrawString(status, Font::MetaMono, RectF(760.0f, 50.0f, 1470.0f, 98.0f),
                                 Theme::kTextSecondary, HAlign::Right, VAlign::Middle);
        }
        switch (mode_) {
        case Mode::Keyboard:
            entry_->Draw(renderer_, now);
            DrawToast(now);
            return; // the keyboard draws its own hints
        case Mode::Confirm:
            DrawConfirm();
            break;
        case Mode::Importing:
            DrawBusy(Tr(Text::SrcImporting));
            break;
        case Mode::Detail:
            DrawDetail(now);
            break;
        case Mode::Queue:
            DrawQueue();
            break;
        default:
            break;
        }
        if (shop_ && section_ == 5 && mode_ != Mode::Queue)
            DrawQueue();
        DrawToast(now);
        if (shop_)
            DrawProfile();
        DrawHints(renderer_, CurrentHints(), shop_);
    }

    void DrawHeader() {
        const Text sections[] = {Text::SrcNewGames, Text::SrcUpdates, Text::SrcDlc,
                                 Text::SrcAll,      Text::SrcSearch,  Text::SrcQueue,
                                 Text::ShopSources};
        const std::wstring title =
            shop_ ? std::wstring(Tr(Text::ShopBrand)) + L" - " + Tr(sections[section_])
                  : std::wstring(Tr(Text::SourcesRowTitle));
        renderer_.DrawString(
            title, Font::ModsTitle,
            RectF(kMargin, kTitleTop, shop_ ? 1150.0f : kRight, kTitleTop + kTitleHeight),
            Theme::kText, HAlign::Left, VAlign::Middle);
        std::wstring line = Tr(Text::SrcSubtitle);
        D2D1_COLOR_F color = Theme::kTextSecondary;
        if (shop_ || InBrowse()) {
            if (Loading()) {
                line = Tr(Text::SrcLoading);
            } else if (failure_ == Failure::Unreachable) {
                line = Tr(Text::SrcUnreachable);
                color = Theme::kDangerText;
            } else if (failure_ == Failure::Unsupported) {
                line = Tr(UnsupportedText(unsupported_));
                color = Theme::kDangerText;
            } else if (failure_ == Failure::SourceError) {
                line = fetch_message_;
                color = Theme::kDangerText;
            } else {
                line = std::to_wstring(games_.size()) + L" " + Tr(Text::SrcGamesCount);
                if (partial_)
                    line += L"  ·  " + std::wstring(Tr(Text::SrcPartial));
            }
        }
        renderer_.DrawString(
            line, Font::Meta,
            RectF(kMargin, kSubTop, shop_ ? 1150.0f : kRight, kSubTop + kSubHeight), color,
            HAlign::Left, VAlign::Middle);
    }

    struct RowText {
        std::wstring title;
        std::wstring sub;
        std::wstring stat;
    };

    void DrawListRows(Clock::time_point now) {
        const float scroll = scroll_.Value(now);
        renderer_.PushClip(RectF(kMargin - 20.0f, kListTop - 10.0f, kRight + 20.0f,
                                 kListTop + static_cast<float>(kVisibleRows) * kRowPitch - 1.0f));
        for (int i = 0; i < ListRows(); ++i) {
            const float y = kListTop + (static_cast<float>(i) - scroll) * kRowPitch;
            if (y + kRowHeight < kListTop - 12.0f ||
                y > kListTop + static_cast<float>(kVisibleRows) * kRowPitch) {
                continue;
            }
            RowText row;
            if (i == 0) {
                row = {Tr(Text::SrcAdd), Tr(Text::SrcAddHint), L""};
            } else if (i == 1) {
                row = {Tr(Text::SrcImportUsb), Tr(Text::SrcImportHint), L""};
            } else {
                const Source& source = sources_[static_cast<std::size_t>(i - 2)];
                row = {Widen(source.name), Widen(RedactedUrl(source.url)), L""};
            }
            DrawRow(row, i, y, mode_ == Mode::List && i == list_cursor_.selected);
        }
        renderer_.PopClip();
        if (sources_.empty()) {
            renderer_.DrawString(Tr(Text::SrcEmpty), Font::Body,
                                 RectF(kMargin, kListTop + 2.0f * kRowPitch + 20.0f, kRight,
                                       kListTop + 2.0f * kRowPitch + 140.0f),
                                 Theme::kTextSecondary);
        }
    }

    struct Art {
        Image image;
        std::string url;
        bool final = false;
        Tween fade = Tween(); // explicit ctor: Art{} needs a default member initializer
    };

    Image* VisibleArt(const Tinfoil::Game& game, Clock::time_point now, bool detail = false) {
        if (!shop_art_)
            return nullptr;
        const Entry* source = nullptr;
        std::string url;
        for (const auto item : game.items) {
            const auto& entry = entries_[item];
            const auto& preferred = detail ? entry.banner_url : entry.icon_url;
            const auto& fallback = detail ? entry.icon_url : entry.banner_url;
            if (!preferred.empty() || !fallback.empty()) {
                source = &entry;
                url = preferred.empty() ? fallback : preferred;
                break;
            }
        }
        if (!source)
            return nullptr;
        const auto key = game.base_id + (detail ? ":banner" : ":icon");
        auto& art = art_[key];
        if (art.url != url) {
            art = Art{};
            art.url = url;
        }
        shop_art_->Request(url, source->art_origin, source->art_headers);
        if (!art.final && decodes_ < 2) {
            fs::path file;
            const auto state = shop_art_->Get(url, file);
            if (state != ArtState::Pending) {
                art.final = true;
                if (state == ArtState::Ready) {
                    ++decodes_;
                    Pixels pixels;
                    if (renderer_.DecodeImage(ReadFileBytes(file), pixels) &&
                        renderer_.UploadImage(pixels, art.image)) {
                        art.fade = Tween(0.0f);
                        art.fade.To(1.0f, now, kDurationScreen);
                    } else
                        shop_art_->Discard(url);
                }
            }
        }
        return art.image.bitmap ? &art.image : nullptr;
    }

    void DrawArtPlaceholder(const Tinfoil::Game& game, const D2D1_RECT_F& rect) {
        const auto colors =
            EdenXbox::Ui::Art::Seeded(game.base_id.empty() ? game.name : game.base_id);
        D2D1_GRADIENT_STOP stops[3];
        for (std::size_t i = 0; i < 3; ++i) {
            auto c = colors[i];
            // Maximum linear luminance .10: white title text has at least 7:1 contrast.
            while (EdenXbox::Ui::Art::Luminance(c) > .10f)
                c = EdenXbox::Ui::Art::Scale(c, .95f);
            stops[i] = {EdenXbox::Ui::Art::kGradientStops[i], D2D1::ColorF(c.r, c.g, c.b, 1)};
        }
        renderer_.FillGradient(rect, 22, stops, 3, D2D1::Point2F(rect.left, rect.top),
                               D2D1::Point2F(rect.left, rect.bottom));
        renderer_.DrawString(
            Widen(game.name), Font::TileLabel,
            RectF(rect.left + 14, rect.top + 14, rect.right - 14, rect.bottom - 14), Theme::kText,
            HAlign::Center, VAlign::Middle);
    }

    void DrawEncryptedShop(float x, float y, float right, bool focused) {
        renderer_.DrawString(
            Tr(tinfoil_available_ ? Text::ShopEncrypted : Text::ShopInstallTinfoil), Font::RowSub,
            RectF(x, y, right, y + 160.0f), Theme::kTextSecondary);
        if (tinfoil_available_)
            DrawChoicePill(renderer_,
                           RectF(x, y + 174.0f, (std::min)(right, x + 620.0f), y + 238.0f),
                           Tr(Text::ShopOpenTinfoil), true, focused);
    }

    void DrawBrowse(Clock::time_point now) {
        decodes_ = 0;
        const Text sorts[] = {Text::SrcSortName, Text::SrcSortSize, Text::SrcSortNewest};
        std::wstring description = Tr(sorts[sort_]);
        if (!query_.empty())
            description += L" · " + query_;
        renderer_.DrawString(description, Font::Meta, RectF(500.0f, 292.0f, kRight, 334.0f),
                             Theme::kTextSecondary);
        const int selected = browse_cursor_.selected;
        const int first = (selected / 4 / 2) * 8;
        std::vector<std::string> keep;
        for (int i = first; i < std::min(BrowseRows(), first + 8); ++i) {
            const float x = 500.0f + static_cast<float>((i - first) % 4) * 326.0f;
            const float y = 370.0f + static_cast<float>((i - first) / 4) * 272.0f;
            const auto rect = RectF(x, y, x + 294.0f, y + 165.0f);
            if (HasEncryptedShop() && static_cast<std::size_t>(i) == visible_.size()) {
                const bool empty = visible_.empty();
                DrawEncryptedShop(empty ? 540.0f : x, empty ? 430.0f : y,
                                  empty ? kRight : x + 294.0f,
                                  i == selected && mode_ == Mode::Browse && !sidebar_focus_ &&
                                      !navigation_focused_);
                continue;
            }
            renderer_.FillRounded(rect, 22.0f, Theme::kSurface);
            std::wstring name;
            std::wstring meta;
            if (static_cast<std::size_t>(i) < visible_.size()) {
                const auto& game = games_[visible_[static_cast<std::size_t>(i)]];
                name = Widen(game.name);
                keep.push_back(game.base_id + ":icon");
                if (auto* image = VisibleArt(game, now))
                    renderer_.DrawImage(*image, rect, 22.0f,
                                        art_[game.base_id + ":icon"].fade.Value(now));
                else
                    DrawArtPlaceholder(game, rect);
                meta = game.size ? FormatBytes(game.size) : Tr(Text::SrcUnknownSize);
            } else {
                name = Tr(Text::SrcLoadMore);
            }
            renderer_.DrawString(name, Font::Caption, RectF(x, y + 180.0f, x + 294.0f, y + 228.0f),
                                 Theme::kText);
            renderer_.DrawString(meta, Font::RowSub, RectF(x, y + 230.0f, x + 294.0f, y + 264.0f),
                                 Theme::kTextSecondary);
            if (i == selected && mode_ == Mode::Browse &&
                (!shop_ || (!sidebar_focus_ && !navigation_focused_)))
                DrawRing(renderer_, rect, 28.0f, 1.0f);
        }
        for (auto it = art_.begin(); it != art_.end();) {
            if (std::find(keep.begin(), keep.end(), it->first) == keep.end())
                it = art_.erase(it);
            else
                ++it;
        }
        if (BrowseRows() == 0) {
            const auto text =
                Loading()                          ? std::wstring(Tr(Text::SrcLoading))
                : failure_ == Failure::Unsupported ? std::wstring(Tr(UnsupportedText(unsupported_)))
                : failure_ == Failure::SourceError ? fetch_message_
                : failure_ == Failure::Unreachable
                    ? std::wstring(Tr(Text::SrcUnreachable))
                    : std::wstring(Tr(entries_.empty() ? Text::SrcNoFiles : Text::SrcNoMatches));
            renderer_.DrawString(text, Font::Body, RectF(540.0f, 430.0f, kRight, 780.0f),
                                 Theme::kTextSecondary);
        }
        renderer_.DrawString(Tr(Text::SrcBrowseHelp), Font::RowSub,
                             RectF(500.0f, 916.0f, kRight, 954.0f), Theme::kTextSecondary);
    }

    void DrawDetail(Clock::time_point now) {
        decodes_ = 0;
        const auto& game = games_[detail_game_];
        const float shift = (1.0f - panel_.Value(now)) * 28.0f;
        const auto sheet = RectF(shop_ ? 0.0f : 300.0f, 150.0f + shift,
                                 shop_ ? kCanvasWidth : 1620.0f, 930.0f + shift);
        if (shop_)
            renderer_.FillRounded(sheet, 0.0f, Theme::kBackground);
        else
            DrawSheet(renderer_, sheet);
        renderer_.PushClip(sheet);
        if (auto* image = VisibleArt(game, now, true))
            renderer_.DrawImage(*image, RectF(96.0f, 185.0f + shift, 576.0f, 455.0f + shift), 22.0f,
                                1.0f);
        else
            DrawArtPlaceholder(game, RectF(96.0f, 185.0f + shift, 576.0f, 455.0f + shift));
        renderer_.DrawString(Widen(game.name), Font::Heading,
                             RectF(624.0f, 208.0f + shift, 1824.0f, 320.0f + shift), Theme::kText);
        renderer_.DrawString(game.size ? FormatBytes(game.size) : Tr(Text::SrcUnknownSize),
                             Font::MetaMono, RectF(624.0f, 330.0f + shift, 1824.0f, 374.0f + shift),
                             Theme::kTextSecondary);
        renderer_.DrawString(Widen(game.base_id), Font::MetaMono,
                             RectF(624.0f, 378.0f + shift, 1824.0f, 410.0f + shift),
                             Theme::kTextSecondary);
        // Availability is independent of the active tab, including locally installed add-ons
        // that this source does not carry.
        const Kind kinds[] = {Kind::Game, Kind::Update, Kind::Dlc};
        for (int i = 0; i < 3; ++i) {
            const auto kind = kinds[i];
            const bool available =
                std::any_of(game.items.begin(), game.items.end(),
                            [&](auto item) { return packages_[item].kind == kind; });
            const bool installed =
                std::any_of(installed_.begin(), installed_.end(), [&](const auto& local) {
                    return !game.base_id.empty() && local.base_id == game.base_id &&
                           local.kind == kind;
                });
            const float x = 96.0f + static_cast<float>(i) * 576.0f;
            renderer_.DrawString(KindLabel(kind), Font::Meta,
                                 RectF(x, 480.0f + shift, x + 540.0f, 512.0f + shift),
                                 Theme::kText);
            std::wstring label = Tr(available ? Text::SrcAvailable : Text::SrcUnavailable);
            if (installed)
                label += L" · " + std::wstring(Tr(Text::SrcInstalled));
            renderer_.DrawString(label, Font::RowSub,
                                 RectF(x, 514.0f + shift, x + 540.0f, 546.0f + shift),
                                 Theme::kTextSecondary);
        }
        const int first = std::max(0, detail_cursor_ - 4);
        for (int row = first; row < std::min(first + 5, static_cast<int>(detail_items_.size()));
             ++row) {
            const auto item = detail_items_[static_cast<std::size_t>(row)];
            const auto& package = packages_[item];
            const float y = 568.0f + shift + static_cast<float>(row - first) * 66.0f;
            const auto rect = RectF(96.0f, y, 1824.0f, y + 60.0f);
            std::wstring label = Widen(package.title_id) + L" · " + KindLabel(package.kind);
            label += L" · " + (package.version ? std::wstring(Tr(Text::SrcVersion)) + L" " +
                                                     std::to_wstring(*package.version)
                                               : Tr(Text::SrcUnknownVersion));
            label += L" · " + (entries_[item].size ? FormatBytes(entries_[item].size)
                                                   : Tr(Text::SrcUnknownSize));
            if (Installed(item))
                label += L" · " + std::wstring(Tr(Text::SrcInstalled));
            renderer_.DrawString(
                std::wstring(Tr(Text::HintDownload)) + L" · " + entries_[item].name, Font::RowSub,
                RectF(114.0f, y + 2.0f, 1806.0f, y + 30.0f), Theme::kText);
            renderer_.DrawString(label, Font::RowSub, RectF(114.0f, y + 32.0f, 1806.0f, y + 58.0f),
                                 Theme::kTextSecondary);
            if (row == detail_cursor_ && (!shop_ || !navigation_focused_))
                DrawRing(renderer_, rect, 22.0f, 1.0f);
        }
        renderer_.PopClip();
    }

    void DrawQueue() {
        if (shop_)
            renderer_.FillRounded(RectF(490.0f, 280.0f, kRight, 930.0f), 0.0f, Theme::kBackground);
        else
            DrawSheet(renderer_, RectF(300.0f, 170.0f, 1620.0f, 930.0f));
        renderer_.DrawString(Tr(Text::SrcQueue), Font::Heading,
                             RectF(500.0f, 290.0f, 1572.0f, 350.0f), Theme::kText);
        DrawProgress();
        if (queue_.Empty())
            renderer_.DrawString(Tr(Text::SrcQueueEmpty), Font::Body,
                                 RectF(500.0f, 400.0f, 1572.0f, 550.0f), Theme::kTextSecondary);
        for (std::size_t i = 0; i < queue_.Items().size(); ++i) {
            const float y = 430.0f + static_cast<float>(i) * 66.0f;
            renderer_.DrawString(queue_.Items()[i].name, Font::RowSub,
                                 RectF(518.0f, y, 1300.0f, y + 48.0f), Theme::kText, HAlign::Left,
                                 VAlign::Middle);
            const auto total = bytes_total_.load();
            const auto percent =
                i == 0 && total ? std::min<std::uint64_t>(
                                      100, static_cast<std::uint64_t>(
                                               100.0 * static_cast<double>(bytes_done_.load()) /
                                               static_cast<double>(total)))
                                : 0;
            const auto state = std::wstring(Tr(i == 0 ? Text::SrcDownloading : Text::SrcWaiting)) +
                               L" · " + std::to_wstring(percent) + L"%";
            renderer_.DrawString(state, Font::RowSub, RectF(1320.0f, y, 1554.0f, y + 48.0f),
                                 Theme::kTextSecondary, HAlign::Right, VAlign::Middle);
            if (static_cast<int>(i) == queue_cursor_ &&
                (!shop_ || (!sidebar_focus_ && !navigation_focused_)))
                DrawRing(renderer_, RectF(500.0f, y, 1572.0f, y + 48.0f), 22.0f, 1.0f);
        }
    }

    void DrawRow(const RowText& row, int index, float y, bool focused) {
        const D2D1_RECT_F rect = RectF(kMargin, y, kRight, y + kRowHeight);
        const Cursor& cursor = InBrowse() ? browse_cursor_ : list_cursor_;
        if (focused) {
            renderer_.FillRounded(rect, kRowRadius, Theme::kSurfaceStrong);
        } else if (index > 0 && index - 1 != cursor.selected) {
            renderer_.FillRounded(RectF(kMargin, y - 3.0f, kRight, y - 2.0f), 0.0f,
                                  Theme::kHairline);
        }
        const float left = kMargin + kRowPadding;
        const float stat_right = kRight - kRowPadding;
        const float info_right = row.stat.empty() ? stat_right : stat_right - kStatWidth - 24.0f;
        renderer_.DrawString(row.title, Font::RowTitle,
                             RectF(left, y + 20.0f, info_right, y + 54.0f), Theme::kText,
                             HAlign::Left, VAlign::Middle);
        renderer_.DrawString(row.sub, Font::RowSub, RectF(left, y + 58.0f, info_right, y + 84.0f),
                             Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
        if (!row.stat.empty()) {
            renderer_.DrawString(row.stat, Font::Stat,
                                 RectF(stat_right - kStatWidth, y, stat_right, y + kRowHeight),
                                 Theme::kText, HAlign::Right, VAlign::Middle);
        }
        if (focused) {
            DrawRing(renderer_, rect, kRowRadius + kRingGap, 1.0f);
        }
    }

    void DrawConfirm() {
        const int index = FocusedSource();
        const D2D1_RECT_F sheet =
            RectF(kSheetLeft, kConfirmTop, kSheetLeft + kSheetWidth, kConfirmTop + kConfirmHeight);
        DrawSheet(renderer_, sheet);
        const float left = sheet.left + kSheetPadding;
        const float right = sheet.right - kSheetPadding;
        renderer_.DrawString(Tr(Text::SrcRemoveTitle), Font::Heading,
                             RectF(left, sheet.top + 36.0f, right, sheet.top + 100.0f),
                             Theme::kText, HAlign::Left, VAlign::Middle);
        if (index >= 0 && index < static_cast<int>(sources_.size())) {
            renderer_.DrawString(Widen(sources_[static_cast<std::size_t>(index)].name), Font::Meta,
                                 RectF(left, sheet.top + 108.0f, right, sheet.top + 142.0f),
                                 Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
        }
        renderer_.DrawString(Tr(Text::SrcRemoveBody), Font::Body,
                             RectF(left, sheet.top + 156.0f, right, sheet.top + 250.0f),
                             Theme::kTextSecondary);
        const std::wstring remove = Tr(Text::SrcRemove);
        const std::wstring keep = Tr(Text::HintCancel);
        const float remove_width = PillWidth(renderer_, remove, false);
        const float keep_width = PillWidth(renderer_, keep, false);
        const float top = sheet.top + 262.0f;
        DrawChoicePill(renderer_, RectF(left, top, left + remove_width, top + kPillHeight), remove,
                       false, confirm_focus_ == 0);
        const float keep_left = left + remove_width + kPillGap;
        DrawChoicePill(renderer_, RectF(keep_left, top, keep_left + keep_width, top + kPillHeight),
                       keep, true, confirm_focus_ == 1);
    }

    void DrawBusy(const std::wstring& text) {
        const D2D1_RECT_F sheet =
            RectF(kSheetLeft, kConfirmTop, kSheetLeft + kSheetWidth, kConfirmTop + 220.0f);
        DrawSheet(renderer_, sheet);
        renderer_.DrawString(text, Font::Heading,
                             RectF(sheet.left + kSheetPadding, sheet.top, sheet.right - kSheetPadding,
                                   sheet.bottom),
                             Theme::kText, HAlign::Left, VAlign::Middle);
    }

    // The active transfer uses the existing progress bar, percentage, rate and byte counters.
    void DrawProgress() {
        const std::uint64_t done = bytes_done_.load();
        const std::uint64_t total = bytes_total_.load();
        const double fraction = total > 0 ? static_cast<double>(done) / static_cast<double>(total) : 0.0;
        if (queue_.Empty())
            return;
        const float left = 500.0f;
        const float right = 1572.0f;
        DrawProgressBar(renderer_, RectF(left, 366.0f, right, 378.0f),
                        std::clamp(fraction, 0.0, 1.0));
        const double rate = rate_.load();
        wchar_t speed[32];
        swprintf_s(speed, std::size(speed), L"%.1f", rate / (1024.0 * 1024.0));
        std::wstring stats = std::to_wstring(static_cast<int>(fraction * 100.0)) + L"%   " + speed +
                             L" " + Tr(Text::SrcRate);
        if (CurrentLanguage() == Language::Portuguese) {
            std::replace(stats.begin(), stats.end(), L'.', L',');
        }
        stats += L"   " + FormatBytes(done) + L" / " + FormatBytes(total);
        renderer_.DrawString(stats, Font::MetaMono, RectF(left, 388.0f, right, 424.0f),
                             Theme::kText, HAlign::Left, VAlign::Middle);
    }

    void DrawToast(Clock::time_point now) {
        if (toast_.empty() || now >= toast_until_) {
            return;
        }
        const float remaining = std::chrono::duration<float>(toast_until_ - now).count();
        const float elapsed = std::chrono::duration<float>(kToastDuration).count() - remaining;
        DrawToastCapsule(renderer_, toast_,
                         std::clamp(std::min(elapsed / 0.15f, remaining / 0.22f), 0.0f, 1.0f));
    }

    std::vector<Hint> CurrentHints() const {
        if (shop_ && navigation_focused_)
            return {{Theme::kButtonA, L"A", Tr(Text::HintSelect)},
                    {Theme::kButtonB, L"B", Tr(Text::HintQuit)}};
        if (shop_ && section_ == 6)
            return {{Theme::kButtonA, L"A", Tr(Text::SourcesRowTitle)},
                    {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        if (shop_ && sidebar_focus_)
            return {{Theme::kButtonA, L"A",
                     Tr(section_ == 6 || (sources_.empty() && section_ != 5) ? Text::SourcesRowTitle
                                                                             : Text::HintSelect)},
                    {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        switch (mode_) {
        case Mode::Queue:
            return {{Theme::kButtonX, L"X", Tr(Text::SrcCancelEntry)},
                    {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        case Mode::Detail:
            return {{Theme::kButtonA, L"A", Tr(Text::HintDownload)},
                    {Theme::kTextSecondary, L"≡", Tr(Text::SrcQueue)},
                    {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        case Mode::Importing:
            return {{Theme::kButtonB, L"B", Tr(Text::HintCancel)}};
        case Mode::Confirm:
            return {{Theme::kButtonA, L"A", Tr(Text::HintSelect)},
                    {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        case Mode::Browse:
            return {{Theme::kButtonA, L"A", Tr(Text::HintSelect)},
                    {Theme::kButtonY, L"Y", Tr(Text::SrcSearch)},
                    {Theme::kTextSecondary, L"≡", Tr(Text::SrcQueue)},
                    {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        case Mode::Keyboard:
            break;
        case Mode::List:
            break;
        }
        std::vector<Hint> hints;
        if (FocusedSource() < 0)
            hints.push_back({Theme::kButtonA, L"A", Tr(Text::HintSelect)});
        if (!queue_.Empty())
            hints.push_back({Theme::kTextSecondary, L"≡", Tr(Text::SrcQueue)});
        if (FocusedSource() >= 0) {
            hints.push_back({Theme::kButtonX, L"X", Tr(Text::HintRemove)});
        }
        hints.push_back({Theme::kButtonB, L"B", Tr(Text::HintBack)});
        return hints;
    }

    void DrawStorage(const std::wstring& label, const fs::space_info& space, float x) {
        renderer_.DrawString(label, Font::RowSub, RectF(x, 154.0f, x + 280.0f, 190.0f),
                             Theme::kTextSecondary);
        DrawProgressBar(renderer_, RectF(x, 202.0f, x + 280.0f, 210.0f),
                        space.capacity ? static_cast<double>(space.available) / space.capacity
                                       : 0.0);
        renderer_.DrawString(space.capacity ? FormatBytes(space.available) : L"—", Font::RowSub,
                             RectF(x, 218.0f, x + 280.0f, 252.0f), Theme::kTextSecondary);
    }
    void DrawSidebar() {
        const Text labels[] = {Text::SrcNewGames, Text::SrcUpdates, Text::SrcDlc,     Text::SrcAll,
                               Text::SrcSearch,   Text::SrcQueue,   Text::ShopSources};
        const wchar_t* icons[] = {L"+", L"↑", L"◇", L"▦", L"⌕", L"↓", L"≡"};
        for (int row = 0; row < 7; ++row) {
            const float y = 300.0f + row * 80.0f;
            const auto rect = RectF(kMargin, y, 446.0f, y + 64.0f);
            if (row == section_)
                renderer_.FillRounded(rect, 32.0f, Theme::kSurfaceStrong);
            renderer_.DrawString(icons[row], Font::Meta,
                                 RectF(kMargin + 18.0f, y, kMargin + 58.0f, y + 64.0f),
                                 Theme::kText, HAlign::Center, VAlign::Middle);
            renderer_.DrawString(Tr(labels[row]), Font::Caption,
                                 RectF(kMargin + 72.0f, y, 430.0f, y + 64.0f), Theme::kText,
                                 HAlign::Left, VAlign::Middle);
            if (sidebar_focus_ && !navigation_focused_ && row == section_)
                DrawRing(renderer_, rect, 38.0f, 1.0f);
        }
    }
    void DrawShopSources() {
        renderer_.DrawString(Tr(Text::ShopManageSources), Font::Body,
                             RectF(500.0f, 300.0f, kRight, 370.0f), Theme::kTextSecondary);
        const int first = std::max(0, source_cursor_ - 4);
        for (int i = first; i < std::min(first + 5, static_cast<int>(sources_.size())); ++i) {
            const float y = 400.0f + (i - first) * 78.0f;
            const auto& source = sources_[static_cast<std::size_t>(i)];
            renderer_.DrawString(Widen(source.name), Font::Caption,
                                 RectF(500.0f, y, kRight, y + 38.0f), Theme::kText);
            const auto status = source_errors_.find(source.url);
            renderer_.DrawString(
                encrypted_sources_.count(source.url)
                    ? Tr(tinfoil_available_ ? Text::ShopEncrypted : Text::ShopInstallTinfoil)
                : status == source_errors_.end() ? Widen(RedactedUrl(source.url))
                                                 : status->second,
                Font::RowSub, RectF(500.0f, y + 38.0f, kRight, y + 74.0f),
                status == source_errors_.end() ? Theme::kTextSecondary : Theme::kDangerText);
        }
        DrawChoicePill(renderer_, RectF(500.0f, 830.0f, 1150.0f, 894.0f), Tr(Text::SourcesRowTitle),
                       true, !sidebar_focus_ && !navigation_focused_);
    }
    void DrawShopEmpty() {
        renderer_.DrawString(Tr(Text::ShopEmptyTitle), Font::Heading,
                             RectF(540.0f, 410.0f, kRight, 490.0f), Theme::kText);
        renderer_.DrawString(Tr(Text::SrcEmpty), Font::Body, RectF(540.0f, 510.0f, kRight, 640.0f),
                             Theme::kTextSecondary);
        DrawChoicePill(renderer_, RectF(540.0f, 690.0f, 1160.0f, 754.0f), Tr(Text::SrcAdd), true,
                       !sidebar_focus_);
    }
    void DrawProfile() {
        const auto rect = RectF(kMargin, 970.0f, kMargin + 52.0f, 1022.0f);
        if (profile_avatar_.bitmap)
            renderer_.DrawImage(profile_avatar_, rect, 26.0f, 1.0f);
        else {
            renderer_.FillRounded(rect, 26.0f, Theme::kSurfaceStrong);
            renderer_.DrawString(profile_name_.empty() ? L"?" : profile_name_.substr(0, 1),
                                 Font::Caption, rect, Theme::kText, HAlign::Center, VAlign::Middle);
        }
        renderer_.DrawString(profile_name_, Font::RowSub,
                             RectF(kMargin + 68.0f, 970.0f, 490.0f, 1022.0f), Theme::kText,
                             HAlign::Left, VAlign::Middle);
    }

    // ---- Members ----

    std::shared_ptr<Task<StorageResult>> storage_;
    Clock::time_point storage_refresh_{};
    fs::space_info internal_space_{};
    fs::space_info external_space_{};
    bool navigation_focused_ = false;
    bool shop_ = false;
    bool sidebar_focus_ = true;
    int section_ = 0;
    int source_cursor_ = 0;
    std::set<std::string> encrypted_sources_;
    bool tinfoil_available_ = false;
    bool tinfoil_requested_ = false;
    bool sources_requested_ = false;
    bool navigation_requested_ = false;
    std::wstring profile_name_;
    Image profile_avatar_;
    std::set<std::wstring> announced_;
    std::map<std::string, std::wstring> source_errors_;
    Tinfoil::Unsupported unsupported_ = Tinfoil::Unsupported::None;
    Renderer& renderer_;
    CoreWindow window_;
    Input& input_;
    fs::path local_state_;

    Mode mode_ = Mode::List;
    std::vector<Source> sources_;
    Cursor list_cursor_;
    Cursor browse_cursor_;
    Tween scroll_;
    int confirm_focus_ = 1; // 0 remove, 1 keep
    std::unique_ptr<TextEntry> entry_;

    std::string browse_url_;
    std::vector<Entry> entries_;
    std::vector<Tinfoil::Package> packages_;
    std::vector<Tinfoil::Game> games_;
    std::vector<std::size_t> visible_;
    std::vector<Tinfoil::Package> installed_;
    std::vector<std::wstring> installed_files_;
    std::vector<std::uint64_t> installed_sizes_;
    std::vector<fs::path> installed_paths_;
    std::optional<Tinfoil::Traversal> traversal_;
    std::unique_ptr<ShopArtSource> shop_art_;
    std::map<std::string, Art> art_;
    int decodes_ = 0;
    int tab_ = 0;
    int sort_ = 0;
    bool partial_ = false;
    std::wstring query_;
    bool searching_ = false;
    Tween shelf_;
    Tween panel_;
    std::size_t detail_game_ = 0;
    std::vector<std::size_t> detail_items_;
    int detail_cursor_ = 0;
    Tinfoil::DownloadQueue<Entry> queue_;
    int queue_cursor_ = 0;
    int queue_section_ = 0;
    Mode queue_return_ = Mode::Browse;
    std::wstring fetch_message_;
    Failure failure_ = Failure::None;
    std::shared_ptr<Task<FetchResult>> fetch_;
    int fetch_serial_ = 0;   // which fetch the screen started last
    int fetch_adopted_ = 0;  // which fetch's result it already took
    std::shared_ptr<Task<ImportResult>> import_;

    std::vector<std::wstring> pending_toasts_;
    std::wstring toast_;
    Clock::time_point toast_until_{};
    bool closed_ = false;
    bool leaving_ = false;

    // The download worker. Joined by the screen, never detached: it writes into the file system.
    std::thread download_thread_;
    std::atomic<bool> cancel_{false};
    std::atomic<std::uint64_t> bytes_done_{0};
    std::atomic<std::uint64_t> bytes_total_{0};
    std::atomic<double> rate_{0.0};
    std::atomic<Outcome> outcome_{Outcome::Pending};
};

} // namespace

struct ShopScreen::Impl {
    SourcesScreen screen;
    Impl(Renderer& renderer, const CoreWindow& window, Input& input, const fs::path& local_state)
        : screen(renderer, window, input, local_state) {
        screen.InitializeShop();
    }
};
ShopScreen::ShopScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                       const fs::path& local_state)
    : impl_(std::make_unique<Impl>(renderer, window, input, local_state)) {}
ShopScreen::~ShopScreen() = default;
void ShopScreen::Tick(Clock::time_point now, bool active) {
    impl_->screen.TickShop(now, active);
}
void ShopScreen::Draw(Clock::time_point now, bool navigation_focused) {
    impl_->screen.DrawShop(now, navigation_focused);
}
void ShopScreen::ReloadSources() {
    impl_->screen.ReloadShop();
}
void ShopScreen::SetTinfoilAvailable(bool available) {
    impl_->screen.SetTinfoilAvailable(available);
}
bool ShopScreen::TakeTinfoilRequest() {
    return impl_->screen.TakeTinfoilRequest();
}
bool ShopScreen::TakeSourcesRequest() {
    return impl_->screen.TakeSourcesRequest();
}
bool ShopScreen::TakeNavigationRequest() {
    return impl_->screen.TakeNavigationRequest();
}
bool ShopScreen::Typing() const {
    return impl_->screen.Typing();
}

bool RunSourcesScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                      const std::filesystem::path& local_state) {
    try {
        SourcesScreen screen(renderer, window, input, local_state);
        return screen.Run();
    } catch (const winrt::hresult_error& error) {
        Diagnostic("UI sources screen failed " + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        Diagnostic(std::string("UI sources screen failed ") + error.what());
    }
    return false;
}

int CountSources(const std::filesystem::path& local_state) {
    return static_cast<int>(LoadSources(local_state).size());
}

} // namespace EdenXbox::Ui
