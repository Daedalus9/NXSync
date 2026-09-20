#include "nxsync/nextcloud_client.hpp"

#include <curl/curl.h>
#include <switch.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace nxsync {
namespace {

constexpr const char* UserAgent = "NXSync/0.30.12-rc2";
constexpr u64 EarliestPlausibleTlsTime = 1704067200ULL; // 2024-01-01 UTC
constexpr u64 LatestPlausibleTlsTime = 2366841599ULL;   // 2044-12-31 UTC
// The transient worker has a 128 KiB main-thread stack. Keep the hashing
// scratch buffer well below that limit: a 128 KiB automatic buffer overflowed
// the stack while preparing the first cloud index after a successful ZIP upload.
constexpr std::size_t HashBufferSize = 16 * 1024;

std::uint64_t nextCacheBuster() {
    // The system tick is shared across transient worker processes for the
    // duration of a boot.  Mixing in a per-process counter also guarantees
    // distinct URLs for multiple reads issued in the same tick.
    static std::uint64_t counter = 0;
    return (static_cast<std::uint64_t>(armGetSystemTick()) << 8U) ^ ++counter;
}

void appendNoCacheHeaders(curl_slist*& headers) {
    headers = curl_slist_append(
        headers,
        "Cache-Control: no-cache, no-store, max-age=0, must-revalidate");
    headers = curl_slist_append(headers, "Pragma: no-cache");
}

struct ResponseData {
    std::string sha256;
    std::string body;
};

struct UploadProgressContext {
    NextcloudProgress progress;
    NextcloudProgressCallback callback{nullptr};
    void* callbackContext{nullptr};
};

std::string trim(std::string value) {
    const auto notSpace = [](const unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::size_t discardBody(char*, std::size_t size, std::size_t count, void*) {
    return size * count;
}

std::size_t captureBody(
    char* buffer,
    const std::size_t size,
    const std::size_t count,
    void* rawResponse) {
    const std::size_t length = size * count;
    auto* response = static_cast<ResponseData*>(rawResponse);
    constexpr std::size_t MaximumResponseBytes = 4 * 1024 * 1024;
    if (response->body.size() + length > MaximumResponseBytes) {
        return 0;
    }
    response->body.append(buffer, length);
    return length;
}

std::size_t captureDiagnosticBody(
    char* buffer,
    const std::size_t size,
    const std::size_t count,
    void* rawResponse) {
    const std::size_t length = size * count;
    auto* response = static_cast<ResponseData*>(rawResponse);
    constexpr std::size_t MaximumDiagnosticBytes = 2048;
    if (response->body.size() < MaximumDiagnosticBytes) {
        const std::size_t available = MaximumDiagnosticBytes - response->body.size();
        response->body.append(buffer, std::min(length, available));
    }
    // A diagnostic response must never abort the WebDAV transfer merely
    // because the server returned a large HTML error page.
    return length;
}

std::string normalizeDiagnosticResponse(std::string value) {
    for (char& ch : value) {
        if (ch == '\r' || ch == '\n' || ch == '\t'
            || static_cast<unsigned char>(ch) < 0x20U) {
            ch = ' ';
        }
    }
    std::string normalized;
    normalized.reserve(value.size());
    bool previousSpace = false;
    for (const char ch : value) {
        const bool space = ch == ' ';
        if (!space || !previousSpace) normalized.push_back(ch);
        previousSpace = space;
    }
    normalized = trim(std::move(normalized));
    constexpr std::size_t MaximumStatusBytes = 512;
    if (normalized.size() > MaximumStatusBytes) {
        normalized.resize(MaximumStatusBytes);
        normalized += "...";
    }
    return normalized;
}

std::size_t writeFileBody(
    char* buffer,
    const std::size_t size,
    const std::size_t count,
    void* rawFile) {
    const std::size_t length = size * count;
    return std::fwrite(buffer, 1, length, static_cast<FILE*>(rawFile));
}

std::size_t captureHeader(
    char* buffer,
    const std::size_t size,
    const std::size_t count,
    void* rawResponse) {
    const std::size_t length = size * count;
    auto* response = static_cast<ResponseData*>(rawResponse);
    std::string line(buffer, length);
    const std::size_t separator = line.find(':');
    if (separator != std::string::npos) {
        const std::string name = lower(trim(line.substr(0, separator)));
        if (name == "x-hash-sha256") {
            response->sha256 = lower(trim(line.substr(separator + 1)));
            const std::string prefix = "sha256:";
            if (response->sha256.compare(0, prefix.size(), prefix) == 0) {
                response->sha256.erase(0, prefix.size());
            }
        }
    }
    return length;
}

int reportCurlProgress(
    void* rawContext,
    curl_off_t,
    curl_off_t,
    const curl_off_t uploadTotal,
    const curl_off_t uploaded) {
    auto* context = static_cast<UploadProgressContext*>(rawContext);
    context->progress.totalBytes = uploadTotal > 0
        ? static_cast<std::uint64_t>(uploadTotal)
        : context->progress.totalBytes;
    context->progress.bytesTransferred = uploaded > 0
        ? static_cast<std::uint64_t>(uploaded)
        : 0;
    if (context->callback != nullptr) {
        context->callback(context->progress, context->callbackContext);
    }
    return 0;
}

int reportDownloadProgress(
    void* rawContext,
    const curl_off_t downloadTotal,
    const curl_off_t downloaded,
    curl_off_t,
    curl_off_t) {
    auto* context = static_cast<UploadProgressContext*>(rawContext);
    context->progress.totalBytes = downloadTotal > 0
        ? static_cast<std::uint64_t>(downloadTotal)
        : context->progress.totalBytes;
    context->progress.bytesTransferred = downloaded > 0
        ? static_cast<std::uint64_t>(downloaded)
        : 0;
    if (context->callback != nullptr) {
        context->callback(context->progress, context->callbackContext);
    }
    return 0;
}

std::string describeCurlFailure(const char* prefix, const CURLcode code) {
    std::string message = std::string(prefix) + curl_easy_strerror(code);
    if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CONNECT_ERROR) {
        message += ". Check the Switch date/time and HTTPS certificate";
    }
    return message;
}

void setCommonOptions(
    CURL* curl,
    const AppConfig& config,
    const std::string& url,
    ResponseData& response) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, config.nextcloudUsername.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, config.nextcloudAppPassword.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, UserAgent);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardBody);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, captureHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);
}

NextcloudResult performDirectoryRequest(
    const AppConfig& config,
    const std::string& url,
    const char* method,
    const bool acceptExisting) {
    NextcloudResult result;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        result.message = "Unable to initialize a WebDAV request";
        return result;
    }

    ResponseData response;
    setCommonOptions(curl, config, url, response);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_slist* headers = nullptr;
    if (std::strcmp(method, "PROPFIND") == 0) {
        headers = curl_slist_append(headers, "Depth: 0");
        headers = curl_slist_append(headers, "Content-Type: application/xml; charset=utf-8");
        static constexpr char Body[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/>"
            "</d:prop></d:propfind>";
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, Body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(sizeof(Body) - 1));
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode curlCode = curl_easy_perform(curl);
    result.curlCode = static_cast<int>(curlCode);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpStatus);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    const bool expected = std::strcmp(method, "PROPFIND") == 0
        ? result.httpStatus == 207
        : result.httpStatus == 201 || (acceptExisting && result.httpStatus == 405);
    result.success = curlCode == CURLE_OK && expected;
    if (!result.success) {
        result.message = curlCode != CURLE_OK
            ? describeCurlFailure("WebDAV error: ", curlCode)
            : "Risposta HTTP WebDAV inattesa";
    }
    return result;
}

std::vector<std::string> remoteDirectoryPrefixes(const std::string& remoteFilePath) {
    const std::size_t finalSlash = remoteFilePath.find_last_of('/');
    const std::string directory = finalSlash == std::string::npos
        ? std::string()
        : remoteFilePath.substr(0, finalSlash);
    std::vector<std::string> prefixes;
    std::string current;
    std::string segment;
    for (std::size_t index = 0; index <= directory.size(); ++index) {
        const bool atEnd = index == directory.size();
        if (!atEnd && directory[index] != '/') {
            segment.push_back(directory[index]);
            continue;
        }
        if (!segment.empty()) {
            current += "/" + segment;
            prefixes.push_back(current);
            segment.clear();
        }
    }
    return prefixes;
}

std::string xmlDecode(std::string value) {
    const std::array<std::pair<const char*, const char*>, 5> entities{{
        {"&amp;", "&"},
        {"&lt;", "<"},
        {"&gt;", ">"},
        {"&quot;", "\""},
        {"&apos;", "'"},
    }};
    for (const auto& entity : entities) {
        std::size_t position = 0;
        while ((position = value.find(entity.first, position)) != std::string::npos) {
            value.replace(position, std::strlen(entity.first), entity.second);
            position += std::strlen(entity.second);
        }
    }
    return value;
}

int hexValue(const char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

std::string percentDecode(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const int high = hexValue(value[index + 1]);
            const int low = hexValue(value[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        decoded.push_back(value[index]);
    }
    return decoded;
}

std::string localTagName(const std::string& tag) {
    const std::size_t colon = tag.find(':');
    return lower(colon == std::string::npos ? tag : tag.substr(colon + 1));
}

std::string elementText(const std::string& block, const std::string& wanted) {
    std::size_t position = 0;
    while ((position = block.find('<', position)) != std::string::npos) {
        if (position + 1 >= block.size() || block[position + 1] == '/') {
            ++position;
            continue;
        }
        const std::size_t nameEnd = block.find_first_of(" >\t\r\n/", position + 1);
        if (nameEnd == std::string::npos) {
            return {};
        }
        const std::string fullName = block.substr(position + 1, nameEnd - position - 1);
        if (localTagName(fullName) != wanted) {
            position = nameEnd;
            continue;
        }
        const std::size_t contentStart = block.find('>', nameEnd);
        if (contentStart == std::string::npos) {
            return {};
        }
        const std::string closing = "</" + fullName + ">";
        const std::size_t contentEnd = block.find(closing, contentStart + 1);
        if (contentEnd == std::string::npos) {
            return {};
        }
        return xmlDecode(block.substr(contentStart + 1, contentEnd - contentStart - 1));
    }
    return {};
}

bool containsElement(const std::string& block, const std::string& wanted) {
    std::size_t position = 0;
    while ((position = block.find('<', position)) != std::string::npos) {
        if (position + 1 >= block.size()) {
            return false;
        }
        std::size_t nameStart = position + 1;
        if (block[nameStart] == '/') {
            ++nameStart;
        }
        const std::size_t nameEnd = block.find_first_of(" >\t\r\n/", nameStart);
        if (nameEnd == std::string::npos) {
            return false;
        }
        if (localTagName(block.substr(nameStart, nameEnd - nameStart)) == wanted) {
            return true;
        }
        position = nameEnd;
    }
    return false;
}

std::vector<std::string> responseBlocks(const std::string& xml) {
    std::vector<std::string> blocks;
    std::size_t position = 0;
    while ((position = xml.find('<', position)) != std::string::npos) {
        const std::size_t nameEnd = xml.find_first_of(" >\t\r\n", position + 1);
        if (nameEnd == std::string::npos) {
            break;
        }
        const std::string fullName = xml.substr(position + 1, nameEnd - position - 1);
        if (localTagName(fullName) != "response") {
            position = nameEnd;
            continue;
        }
        const std::string closing = "</" + fullName + ">";
        const std::size_t end = xml.find(closing, nameEnd);
        if (end == std::string::npos) {
            break;
        }
        blocks.push_back(xml.substr(position, end + closing.size() - position));
        position = end + closing.size();
    }
    return blocks;
}

std::string trimSlashes(std::string path) {
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    while (!path.empty() && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

std::string basenameFromHref(const std::string& href) {
    std::string path = percentDecode(href);
    const std::size_t query = path.find_first_of("?#");
    if (query != std::string::npos) {
        path.erase(query);
    }
    path = trimSlashes(path);
    const std::size_t separator = path.find_last_of('/');
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

bool hrefRepresentsDirectory(const std::string& href, const std::string& remotePath) {
    const std::string decoded = trimSlashes(percentDecode(href));
    const std::string remote = trimSlashes(remotePath);
    if (remote.empty() || decoded.size() < remote.size()) {
        return false;
    }
    const std::size_t position = decoded.size() - remote.size();
    return decoded.compare(position, remote.size(), remote) == 0
        && (position == 0 || decoded[position - 1] == '/');
}

std::string calculateSha256(const std::string& path) {
    FILE* input = std::fopen(path.c_str(), "rb");
    if (input == nullptr) {
        return {};
    }
    Sha256Context context{};
    sha256ContextCreate(&context);
    std::array<unsigned char, HashBufferSize> buffer{};
    while (true) {
        const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), input);
        if (count > 0) {
            sha256ContextUpdate(&context, buffer.data(), count);
        }
        if (count < buffer.size()) {
            break;
        }
    }
    const bool readFailed = std::ferror(input) != 0;
    std::fclose(input);
    if (readFailed) {
        return {};
    }
    std::array<unsigned char, SHA256_HASH_SIZE> digest{};
    sha256ContextGetHash(&context, digest.data());
    static constexpr char Hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(SHA256_HASH_SIZE * 2);
    for (const unsigned char byte : digest) {
        result.push_back(Hex[byte >> 4U]);
        result.push_back(Hex[byte & 0x0FU]);
    }
    return result;
}

} // namespace

std::string formatNextcloudFailure(const NextcloudResult& result) {
    std::string message = result.message.empty()
        ? "Nextcloud operation failed"
        : result.message;
    message += " (HTTP " + std::to_string(result.httpStatus)
        + ", curl " + std::to_string(result.curlCode) + ")";
    if (!result.remotePath.empty()) {
        message += " - " + result.remotePath;
    }
    if (!result.serverResponse.empty()) {
        message += " - risposta: " + result.serverResponse;
    }
    return message;
}

NextcloudClient::NextcloudClient(const AppConfig& config)
    : config_(config),
      davBaseUrl_(makeNextcloudDavBaseUrl(config.nextcloudUrl, config.nextcloudUsername)) {
    if (!config_.nextcloudConfigured()) {
        initializationResult_.message = "Nextcloud configuration is incomplete";
        return;
    }
    if (!isValidNextcloudUrl(config_.nextcloudUrl)) {
        initializationResult_.message = "Invalid Nextcloud URL: HTTPS is required";
        return;
    }

    u64 userClock = 0;
    const Result clockResult = timeGetCurrentTime(TimeType_UserSystemClock, &userClock);
    if (R_FAILED(clockResult)
        || userClock < EarliestPlausibleTlsTime
        || userClock > LatestPlausibleTlsTime) {
        initializationResult_.networkResult = clockResult;
        initializationResult_.message =
            "Invalid Switch date/time: correct it in System Settings before using TLS";
        return;
    }

    initializationResult_.networkResult = socketInitializeDefault();
    if (R_FAILED(initializationResult_.networkResult)) {
        initializationResult_.message = "Unable to initialize sockets";
        return;
    }
    socketsInitialized_ = true;

    const Result nifmResult = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(nifmResult)) {
        initializationResult_.networkResult = nifmResult;
        initializationResult_.message = "Unable to initialize the Switch network";
        return;
    }
    nifmInitialized_ = true;

    const CURLcode curlResult = curl_global_init(CURL_GLOBAL_DEFAULT);
    initializationResult_.curlCode = static_cast<int>(curlResult);
    if (curlResult != CURLE_OK) {
        initializationResult_.message = "Unable to initialize libcurl ("
            + std::to_string(static_cast<int>(curlResult)) + "): "
            + curl_easy_strerror(curlResult);
        return;
    }
    curlInitialized_ = true;
    initializationResult_.success = true;
    initializationResult_.message = "Nextcloud client initialized";
}

NextcloudClient::~NextcloudClient() {
    if (curlInitialized_) {
        curl_global_cleanup();
    }
    if (nifmInitialized_) {
        nifmExit();
    }
    if (socketsInitialized_) {
        socketExit();
    }
}

bool NextcloudClient::ready() const {
    return initializationResult_.success;
}

const NextcloudResult& NextcloudClient::initializationResult() const {
    return initializationResult_;
}

NextcloudResult NextcloudClient::testConnection() {
    if (!ready()) {
        return initializationResult_;
    }
    NextcloudResult result = performDirectoryRequest(
        config_,
        davBaseUrl_,
        "PROPFIND",
        false);
    result.remotePath = "/";
    result.message = result.success
        ? "Nextcloud connection succeeded"
        : result.message;
    return result;
}

NextcloudListResult NextcloudClient::listDirectory(const std::string& remotePath) {
    NextcloudListResult result;
    result.remotePath = remotePath;
    if (!ready()) {
        static_cast<NextcloudResult&>(result) = initializationResult_;
        result.remotePath = remotePath;
        return result;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        result.message = "Unable to initialize the WebDAV listing";
        return result;
    }

    ResponseData response;
    setCommonOptions(
        curl,
        config_,
        makeNextcloudResourceUrl(davBaseUrl_, remotePath),
        response);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, captureBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    static constexpr char Body[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
        "<d:displayname/><d:resourcetype/><d:getcontentlength/>"
        "<d:getlastmodified/></d:prop></d:propfind>";
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, Body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(sizeof(Body) - 1));
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Depth: 1");
    headers = curl_slist_append(headers, "Content-Type: application/xml; charset=utf-8");
    if (isMutableCloudIndexPath(remotePath)) {
        appendNoCacheHeaders(headers);
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode curlCode = curl_easy_perform(curl);
    result.curlCode = static_cast<int>(curlCode);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpStatus);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (curlCode != CURLE_OK) {
        result.message = describeCurlFailure("WebDAV listing failed: ", curlCode);
        return result;
    }
    if (result.httpStatus != 207) {
        result.message = "Nextcloud rejected the folder listing";
        return result;
    }

    for (const std::string& block : responseBlocks(response.body)) {
        const std::string href = elementText(block, "href");
        if (hrefRepresentsDirectory(href, remotePath)) {
            continue;
        }
        std::string name = trim(elementText(block, "displayname"));
        if (name.empty()) {
            name = basenameFromHref(href);
        }
        if (name.empty() || name == "." || name == ".."
            || name.find('/') != std::string::npos
            || name.find('\\') != std::string::npos) {
            continue;
        }

        NextcloudEntry entry;
        entry.name = name;
        entry.remotePath = trimSlashes(remotePath) + "/" + name;
        if (entry.remotePath.empty() || entry.remotePath.front() != '/') {
            entry.remotePath.insert(entry.remotePath.begin(), '/');
        }
        entry.directory = containsElement(block, "collection");
        entry.modified = trim(elementText(block, "getlastmodified"));
        const std::string size = trim(elementText(block, "getcontentlength"));
        if (!size.empty()) {
            char* end = nullptr;
            errno = 0;
            const unsigned long long parsed = std::strtoull(size.c_str(), &end, 10);
            if (errno == 0 && end != size.c_str() && *end == '\0') {
                entry.size = static_cast<std::uint64_t>(parsed);
            }
        }
        result.entries.push_back(std::move(entry));
    }

    std::sort(result.entries.begin(), result.entries.end(), [](const auto& left, const auto& right) {
        if (left.directory != right.directory) {
            return left.directory > right.directory;
        }
        return lower(left.name) < lower(right.name);
    });
    result.success = true;
    result.message = "Nextcloud folder read successfully";
    return result;
}

NextcloudTextResult NextcloudClient::downloadText(
    const std::string& remotePath,
    const std::size_t maximumBytes) {
    NextcloudTextResult result;
    result.remotePath = remotePath;
    if (!ready()) {
        static_cast<NextcloudResult&>(result) = initializationResult_;
        result.remotePath = remotePath;
        return result;
    }
    if (maximumBytes == 0 || maximumBytes > 4 * 1024 * 1024) {
        result.message = "Invalid text download limit";
        return result;
    }
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        result.message = "Unable to initialize the index download";
        return result;
    }
    ResponseData response;
    const bool mutableIndex = isMutableCloudIndexPath(remotePath);
    setCommonOptions(
        curl,
        config_,
        mutableIndex
            ? makeFreshNextcloudResourceUrl(
                davBaseUrl_, remotePath, nextCacheBuster())
            : makeNextcloudResourceUrl(davBaseUrl_, remotePath),
        response);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, captureBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "X-Hash: sha256");
    if (mutableIndex) {
        appendNoCacheHeaders(headers);
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    const CURLcode curlCode = curl_easy_perform(curl);
    result.curlCode = static_cast<int>(curlCode);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpStatus);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (curlCode != CURLE_OK || result.httpStatus != 200) {
        result.message = curlCode != CURLE_OK
            ? describeCurlFailure("Index download failed: ", curlCode)
            : "Nextcloud rejected the index download";
        return result;
    }
    if (response.body.size() > maximumBytes) {
        result.message = "Global index exceeds the safety limit";
        return result;
    }
    result.remoteSha256 = response.sha256;
    if (!response.sha256.empty()) {
        Sha256Context context{};
        sha256ContextCreate(&context);
        sha256ContextUpdate(&context, response.body.data(), response.body.size());
        std::array<unsigned char, SHA256_HASH_SIZE> hash{};
        sha256ContextGetHash(&context, hash.data());
        static constexpr char HexDigits[] = "0123456789abcdef";
        std::string localHash;
        localHash.reserve(64);
        for (const unsigned char byte : hash) {
            localHash.push_back(HexDigits[byte >> 4U]);
            localHash.push_back(HexDigits[byte & 0x0FU]);
        }
        if (lower(response.sha256) != localHash) {
            result.message = "Global index SHA-256 does not match";
            return result;
        }
    }
    result.success = true;
    result.text = std::move(response.body);
    result.bytesDownloaded = result.text.size();
    result.message = "Global index downloaded and verified";
    return result;
}

NextcloudResult NextcloudClient::deleteFile(const std::string& remotePath) {
    NextcloudResult result;
    result.remotePath = remotePath;
    if (!ready()) {
        result = initializationResult_;
        result.remotePath = remotePath;
        return result;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        result.message = "Unable to initialize WebDAV deletion";
        return result;
    }
    ResponseData response;
    setCommonOptions(
        curl,
        config_,
        makeNextcloudResourceUrl(davBaseUrl_, remotePath),
        response);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    const CURLcode curlCode = curl_easy_perform(curl);
    result.curlCode = static_cast<int>(curlCode);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpStatus);
    curl_easy_cleanup(curl);

    result.success = curlCode == CURLE_OK
        && (result.httpStatus == 200
            || result.httpStatus == 204
            || result.httpStatus == 404);
    result.message = result.success
        ? (result.httpStatus == 404
            ? "Nextcloud backup is already absent"
            : "Nextcloud backup deleted")
        : (curlCode != CURLE_OK
            ? describeCurlFailure("WebDAV deletion failed: ", curlCode)
            : "Nextcloud rejected the deletion");
    return result;
}

NextcloudResult NextcloudClient::downloadVerified(
    const std::string& remotePath,
    const std::string& localPath,
    NextcloudProgressCallback progressCallback,
    void* progressContext) {
    NextcloudResult result;
    result.remotePath = remotePath;
    if (!ready()) {
        result = initializationResult_;
        result.remotePath = remotePath;
        return result;
    }

    const std::string temporaryPath = localPath + ".partial";
    std::remove(temporaryPath.c_str());
    FILE* output = std::fopen(temporaryPath.c_str(), "wb");
    if (output == nullptr) {
        result.message = "Unable to create the temporary download";
        return result;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        std::fclose(output);
        std::remove(temporaryPath.c_str());
        result.message = "Unable to initialize the WebDAV download";
        return result;
    }

    ResponseData response;
    setCommonOptions(
        curl,
        config_,
        makeNextcloudResourceUrl(davBaseUrl_, remotePath),
        response);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFileBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, output);
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "X-Hash: sha256");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    UploadProgressContext downloadProgress;
    downloadProgress.progress.stage = "Downloading and verifying from Nextcloud";
    downloadProgress.progress.remotePath = remotePath;
    downloadProgress.callback = progressCallback;
    downloadProgress.callbackContext = progressContext;
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, reportDownloadProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &downloadProgress);

    const CURLcode curlCode = curl_easy_perform(curl);
    result.curlCode = static_cast<int>(curlCode);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpStatus);
    curl_off_t downloaded = 0;
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &downloaded);
    if (downloaded > 0) {
        result.bytesDownloaded = static_cast<std::uint64_t>(downloaded);
    }
    result.remoteSha256 = response.sha256;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    const bool closeSucceeded = std::fclose(output) == 0;

    if (curlCode != CURLE_OK || result.httpStatus != 200 || !closeSucceeded) {
        std::remove(temporaryPath.c_str());
        result.message = curlCode != CURLE_OK
            ? describeCurlFailure("Download failed: ", curlCode)
            : "Incomplete Nextcloud download";
        return result;
    }

    const std::string localHash = calculateSha256(temporaryPath);
    if (localHash.size() != 64) {
        std::remove(temporaryPath.c_str());
        result.message = "Unable to calculate the download SHA-256";
        return result;
    }
    const bool remoteHashAvailable = response.sha256.size() == 64;
    const bool hashMatches = downloadHashMatches(remotePath, localHash, response.sha256);
    if (!hashMatches) {
        std::remove(temporaryPath.c_str());
        result.message = "Downloaded SHA-256 does not match the backup name or server checksum";
        return result;
    }

    std::remove(localPath.c_str());
    if (std::rename(temporaryPath.c_str(), localPath.c_str()) != 0) {
        const int renameError = errno;
        std::remove(temporaryPath.c_str());
        result.message = "Download verified but not finalized (errno "
            + std::to_string(renameError) + ")";
        return result;
    }
    result.success = true;
    result.remoteSha256 = localHash;
    result.message = remoteHashAvailable
        ? "Nextcloud download verified with SHA-256"
        : "Download verified using the filename hash and ZIP";
    return result;
}

NextcloudResult NextcloudClient::uploadVerified(
    const std::string& localPath,
    const std::string& remotePath,
    const std::string& sha256,
    NextcloudProgressCallback progressCallback,
    void* progressContext) {
    NextcloudResult result;
    result.remotePath = remotePath;
    if (!ready()) {
        result = initializationResult_;
        result.remotePath = remotePath;
        return result;
    }
    if (sha256.size() != 64) {
        result.message = "Invalid local SHA-256";
        return result;
    }

    for (const auto& directory : remoteDirectoryPrefixes(remotePath)) {
        if (progressCallback != nullptr) {
            NextcloudProgress progress;
            progress.stage = "Creating Nextcloud folders";
            progress.remotePath = directory;
            progressCallback(progress, progressContext);
        }
        const NextcloudResult createResult = performDirectoryRequest(
            config_,
            makeNextcloudResourceUrl(davBaseUrl_, directory),
            "MKCOL",
            true);
        if (!createResult.success) {
            result.curlCode = createResult.curlCode;
            result.httpStatus = createResult.httpStatus;
            result.message = "Folder creation failed: " + createResult.message;
            return result;
        }
    }

    struct stat fileStat{};
    if (stat(localPath.c_str(), &fileStat) != 0 || !S_ISREG(fileStat.st_mode)) {
        result.message = "Local archive is unavailable for upload";
        return result;
    }
    FILE* input = std::fopen(localPath.c_str(), "rb");
    if (input == nullptr) {
        result.message = "Unable to open the local ZIP archive";
        return result;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        std::fclose(input);
        result.message = "Unable to initialize the WebDAV upload";
        return result;
    }

    ResponseData response;
    setCommonOptions(
        curl,
        config_,
        makeNextcloudResourceUrl(davBaseUrl_, remotePath),
        response);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, captureDiagnosticBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, input);
    curl_easy_setopt(
        curl,
        CURLOPT_INFILESIZE_LARGE,
        static_cast<curl_off_t>(fileStat.st_size));
    curl_easy_setopt(curl, CURLOPT_UPLOAD_BUFFERSIZE, 128L * 1024L);

    UploadProgressContext uploadProgress;
    uploadProgress.progress.stage = "Uploading and verifying on Nextcloud";
    uploadProgress.progress.remotePath = remotePath;
    uploadProgress.progress.totalBytes = static_cast<std::uint64_t>(fileStat.st_size);
    uploadProgress.callback = progressCallback;
    uploadProgress.callbackContext = progressContext;
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, reportCurlProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &uploadProgress);

    const std::string checksumHeader = "OC-Checksum: SHA256:" + lower(sha256);
    const std::string totalLengthHeader = "OC-Total-Length: "
        + std::to_string(static_cast<std::uint64_t>(fileStat.st_size));
    curl_slist* headers = nullptr;
    const bool jsonContent = remotePath.size() >= 5
        && lower(remotePath.substr(remotePath.size() - 5)) == ".json";
    headers = curl_slist_append(
        headers,
        jsonContent ? "Content-Type: application/json" : "Content-Type: application/zip");
    headers = curl_slist_append(headers, "Expect:");
    headers = curl_slist_append(headers, "X-Hash: sha256");
    headers = curl_slist_append(headers, checksumHeader.c_str());
    headers = curl_slist_append(headers, totalLengthHeader.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode curlCode = curl_easy_perform(curl);
    result.curlCode = static_cast<int>(curlCode);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpStatus);
    curl_off_t uploaded = 0;
    curl_easy_getinfo(curl, CURLINFO_SIZE_UPLOAD_T, &uploaded);
    if (uploaded > 0) {
        result.bytesUploaded = static_cast<std::uint64_t>(uploaded);
    }
    result.remoteSha256 = response.sha256;
    result.serverResponse = normalizeDiagnosticResponse(response.body);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    std::fclose(input);

    const bool httpSucceeded = result.httpStatus == 201 || result.httpStatus == 204;
    const bool sizeMatches = result.bytesUploaded
        == static_cast<std::uint64_t>(fileStat.st_size);
    const bool hashMatches = lower(result.remoteSha256) == lower(sha256);
    result.success = curlCode == CURLE_OK && httpSucceeded && sizeMatches && hashMatches;
    if (result.success) {
        result.message = "Nextcloud upload verified";
    } else if (curlCode != CURLE_OK) {
        result.message = describeCurlFailure("Upload failed: ", curlCode);
    } else if (!httpSucceeded) {
        result.message = "Nextcloud rejected the upload";
    } else if (!sizeMatches) {
        result.message = "Remote size was not verified";
    } else {
        result.message = result.remoteSha256.empty()
            ? "Nextcloud did not return X-Hash-SHA256"
            : "Remote SHA-256 does not match";
    }
    return result;
}

NextcloudResult NextcloudClient::uploadTextVerified(
    const std::string& text,
    const std::string& remotePath,
    NextcloudProgressCallback progressCallback,
    void* progressContext) {
    NextcloudResult result;
    result.remotePath = remotePath;
    if (text.empty()) {
        result.message = "Global index is empty";
        return result;
    }
    mkdir("sdmc:/switch/NXSync", 0777);
    const std::string temporaryPath = "sdmc:/switch/NXSync/.cloud-index-upload.tmp";
    FILE* output = std::fopen(temporaryPath.c_str(), "wb");
    if (output == nullptr) {
        result.message = "Unable to prepare the global index";
        return result;
    }
    const bool contentWritten =
        std::fwrite(text.data(), 1, text.size(), output) == text.size();
    const bool flushed = std::fflush(output) == 0;
    const bool closed = std::fclose(output) == 0;
    const bool written = contentWritten && flushed && closed;
    if (!written) {
        std::remove(temporaryPath.c_str());
        result.message = "Unable to write the temporary global index";
        return result;
    }
    const std::string sha256 = calculateSha256(temporaryPath);
    if (sha256.size() != 64) {
        std::remove(temporaryPath.c_str());
        result.message = "Unable to verify the local global index";
        return result;
    }
    result = uploadVerified(
        temporaryPath,
        remotePath,
        sha256,
        progressCallback,
        progressContext);
    std::remove(temporaryPath.c_str());
    if (result.success) {
        result.message = "Nextcloud global index verified";
    }
    return result;
}

} // namespace nxsync
