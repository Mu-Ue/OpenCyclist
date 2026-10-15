#include <winsock2.h>
#include <ws2tcpip.h>

#include "net/strava.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string_view>

#include <winrt/base.h>

#include "platform/settings.h"
#include "sim/tcx.h"

// The Strava API app, from the build. See the README.
#ifndef STRAVA_CLIENT_ID
#define STRAVA_CLIENT_ID ""
#endif
#ifndef STRAVA_CLIENT_SECRET
#define STRAVA_CLIENT_SECRET ""
#endif

namespace {

using namespace std::chrono_literals;

constexpr wchar_t kHost[] = L"www.strava.com";
constexpr ULONGLONG kSignInMs = 5 * 60 * 1000;  // how long the browser has to come back
constexpr ULONGLONG kProcessingMs = 90 * 1000;  // how long Strava has to process an upload
constexpr size_t kMaxReply = 1 << 20;

std::wstring Widen(std::string_view s) {
    std::wstring w(s.size(), L'\0');
    w.resize(MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size())));
    return w;
}

std::string Narrow(std::wstring_view w) {
    std::string s(w.size() * 3, '\0');
    s.resize(WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), static_cast<int>(s.size()),
                                 nullptr, nullptr));
    return s;
}

// Random hex from the system's secure generator, for the sign-in's state and the upload's boundary.
std::string RandomHex(size_t bytes) {
    uint8_t buf[32]{};
    bytes = std::min(bytes, sizeof(buf));
    BCryptGenRandom(nullptr, buf, static_cast<ULONG>(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::string hex;
    for (size_t i = 0; i < bytes; ++i) {
        char b[3];
        std::snprintf(b, sizeof(b), "%02x", buf[i]);
        hex += b;
    }
    return hex;
}

std::string UrlEncode(std::string_view s) {
    std::string out;
    for (const unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char b[4];
            std::snprintf(b, sizeof(b), "%%%02X", c);
            out += b;
        }
    }
    return out;
}

std::string UrlDecode(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// A parameter from a query string such as "state=x&code=y".
std::string QueryValue(std::string_view query, std::string_view key) {
    for (size_t at = 0; at < query.size();) {
        size_t end = query.find('&', at);
        if (end == std::string_view::npos) end = query.size();
        const std::string_view pair = query.substr(at, end - at);
        if (pair.size() > key.size() && pair.starts_with(key) && pair[key.size()] == '=')
            return UrlDecode(pair.substr(key.size() + 1));
        at = end + 1;
    }
    return {};
}

// Just enough JSON for Strava's replies: the first value of a key, wherever it is. Strava's keys are unique
// enough in the replies read here.
size_t JsonValue(std::string_view json, std::string_view key) {
    const std::string quoted = "\"" + std::string(key) + "\"";
    for (size_t at = json.find(quoted); at != std::string_view::npos; at = json.find(quoted, at + 1)) {
        size_t i = at + quoted.size();
        while (i < json.size() && isspace(static_cast<unsigned char>(json[i]))) ++i;
        if (i >= json.size() || json[i] != ':') continue;
        ++i;
        while (i < json.size() && isspace(static_cast<unsigned char>(json[i]))) ++i;
        return i;
    }
    return std::string_view::npos;
}

std::string JsonString(std::string_view json, std::string_view key) {
    size_t i = JsonValue(json, key);
    if (i == std::string_view::npos || i >= json.size() || json[i] != '"') return {};
    std::string out;
    for (++i; i < json.size() && json[i] != '"'; ++i) {
        if (json[i] != '\\' || i + 1 >= json.size()) {
            out += json[i];
            continue;
        }
        const char c = json[++i];
        if (c == 'n') out += '\n';
        else if (c == 't') out += '\t';
        else if (c == 'u' && i + 4 < json.size()) {
            // A UTF-16 unit, or half of a pair, as UTF-8.
            unsigned cp = std::stoul(std::string(json.substr(i + 1, 4)), nullptr, 16);
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < json.size() && json[i + 1] == '\\' && json[i + 2] == 'u') {
                const unsigned low = std::stoul(std::string(json.substr(i + 3, 4)), nullptr, 16);
                if (low >= 0xDC00 && low < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00), i += 6;
            }
            const wchar_t units[2] = {static_cast<wchar_t>(cp < 0x10000 ? cp : 0xD800 + ((cp - 0x10000) >> 10)),
                                      static_cast<wchar_t>(0xDC00 + ((cp - 0x10000) & 0x3FF))};
            out += Narrow({units, cp < 0x10000 ? 1u : 2u});
        } else out += c;  // \" \\ \/ and the rest
    }
    return out;
}

long long JsonNumber(std::string_view json, std::string_view key) {
    const size_t i = JsonValue(json, key);
    if (i == std::string_view::npos) return 0;
    return std::strtoll(std::string(json.substr(i, 24)).c_str(), nullptr, 10);
}

// One HTTPS request to Strava. Status 0 when it didn't get through.
struct Reply {
    DWORD status = 0;
    std::string body;
};

Reply Request(const wchar_t* verb, const std::wstring& path, std::string_view body = {},
              const std::wstring& headers = {}) {
    struct Handle {
        HINTERNET h;
        ~Handle() {
            if (h) WinHttpCloseHandle(h);
        }
    };
    Reply reply;
    Handle session{WinHttpOpen(L"OpenCyclist/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) return reply;
    Handle connection{WinHttpConnect(session.h, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection.h) return reply;
    Handle request{WinHttpOpenRequest(connection.h, verb, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request.h) return reply;
    WinHttpSetTimeouts(request.h, 10000, 15000, 60000, 60000);
    if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1L), const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        return reply;
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    char buf[8192];
    for (DWORD read = 0; WinHttpReadData(request.h, buf, sizeof(buf), &read) && read && reply.body.size() < kMaxReply;)
        reply.body.append(buf, read);
    reply.status = status;
    return reply;
}

Reply PostForm(const wchar_t* path, const std::string& form) {
    return Request(L"POST", path, form, L"Content-Type: application/x-www-form-urlencoded");
}

std::wstring TokenPath() { return AppDataDir() + L"\\strava.dat"; }

std::string ReadFile(const std::wstring& path, size_t limit) {
    std::ifstream in(path, std::ios::binary);
    std::string data;
    char buf[65536];
    while (in && data.size() < limit) {
        in.read(buf, sizeof(buf));
        data.append(buf, static_cast<size_t>(in.gcount()));
    }
    return data;
}

// The page the browser lands on after signing in.
std::string Page(bool ok) {
    const std::string body =
        std::string("<!doctype html><meta charset=utf-8><title>OpenCyclist</title><body style=\"background:#0a0a0a;"
                    "color:#f2f2f2;font:18px 'Segoe UI',sans-serif;display:grid;place-items:center;height:90vh\"><p>") +
        (ok ? "OpenCyclist is connected to Strava. You can close this tab." : "Strava wasn't connected. You can close "
                                                                             "this tab and try again in OpenCyclist.") +
        "</p>";
    char head[160];
    std::snprintf(head, sizeof(head),
                  "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nConnection: "
                  "close\r\n\r\n",
                  body.size());
    return head + body;
}

}  // namespace

struct Strava::Shared {
    HWND window = nullptr;
    std::atomic<bool> closed{false};
    mutable std::mutex mutex;
    bool loaded = false;  // the saved sign-in is read on first use, to keep it off the startup path
    Status status;
    std::string access, refresh;
    long long expires = 0;  // when the access token runs out, in seconds since 1970
    unsigned job = 0;       // counts uploads and sign-outs, so older background work knows to stop
    SOCKET listener = INVALID_SOCKET;

    void Notify() const {
        if (!closed) PostMessageW(window, WM_STRAVA, 0, 0);
    }

    // Reads the saved sign-in. Call with the lock held.
    void Load() {
        if (loaded) return;
        loaded = true;
        std::string blob = ReadFile(TokenPath(), 1 << 16);
        DATA_BLOB in{static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE*>(blob.data())}, out{};
        if (blob.empty() || !CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
            return;
        const std::string text(reinterpret_cast<char*>(out.pbData), out.cbData);
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        // refresh, access, expiry and athlete, one per line
        size_t a = text.find('\n'), b = text.find('\n', a + 1), c = text.find('\n', b + 1);
        if (c == std::string::npos) return;
        refresh = text.substr(0, a);
        access = text.substr(a + 1, b - a - 1);
        expires = std::strtoll(text.c_str() + b + 1, nullptr, 10);
        status.athlete = Widen(std::string_view(text).substr(c + 1));
        if (!refresh.empty()) status.state = State::Ready;
    }

    // Encrypted for this Windows user, so the file is no use copied elsewhere. Call with the lock held.
    void Save() const {
        const std::wstring path = TokenPath();
        if (refresh.empty()) {
            DeleteFileW(path.c_str());
            return;
        }
        std::string text = refresh + "\n" + access + "\n" + std::to_string(expires) + "\n" + Narrow(status.athlete);
        DATA_BLOB in{static_cast<DWORD>(text.size()), reinterpret_cast<BYTE*>(text.data())}, out{};
        const BOOL ok = CryptProtectData(&in, L"OpenCyclist Strava", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
        SecureZeroMemory(text.data(), text.size());
        if (!ok) return;
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(out.pbData), out.cbData);
        LocalFree(out.pbData);
    }

    // Takes the tokens from a token reply. Call with the lock held.
    bool TakeTokens(const std::string& json) {
        std::string a = JsonString(json, "access_token"), r = JsonString(json, "refresh_token");
        if (a.empty() || r.empty()) return false;
        access = std::move(a);
        refresh = std::move(r);
        expires = JsonNumber(json, "expires_at");
        const std::string first = JsonString(json, "firstname"), last = JsonString(json, "lastname");
        if (!first.empty() || !last.empty()) status.athlete = Widen(first + (first.empty() || last.empty() ? "" : " ") + last);
        Save();
        return true;
    }

    void CloseListener() {
        if (listener != INVALID_SOCKET) closesocket(listener);
        listener = INVALID_SOCKET;
    }

    bool Current(unsigned j) const { return !closed && job == j; }
};

namespace {

using Shared = std::shared_ptr<Strava::Shared>;

void Fail(const Shared& shared, unsigned job, std::wstring error) {
    {
        std::lock_guard lock(shared->mutex);
        if (!shared->Current(job)) return;
        shared->status.state = Strava::State::Failed;
        shared->status.error = std::move(error);
    }
    shared->Notify();
}

// Waits for the browser to come back to the listener with the sign-in code. Returns empty when the sign-in was
// refused, cancelled or timed out.
std::string WaitForCode(const Shared& shared, unsigned job, SOCKET listener, const std::string& state) {
    const ULONGLONG deadline = GetTickCount64() + kSignInMs;
    while (GetTickCount64() < deadline) {
        {
            std::lock_guard lock(shared->mutex);
            if (!shared->Current(job)) return {};
        }
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(listener, &ready);
        timeval wait{0, 500000};
        const int n = select(0, &ready, nullptr, nullptr, &wait);
        if (n < 0) return {};
        if (n == 0) continue;
        const SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        // A browser may open a connection and send nothing, so reads give up after a while.
        const DWORD timeout = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::string request;
        char buf[2048];
        while (request.find("\r\n") == std::string::npos && request.size() < 8192) {
            const int got = recv(client, buf, sizeof(buf), 0);
            if (got <= 0) break;
            request.append(buf, got);
        }
        // GET /?state=...&code=...&scope=... HTTP/1.1
        std::string code;
        bool ours = false;
        if (const size_t end = request.find_first_of(" \r\n", 6);
            request.starts_with("GET /?") && end != std::string::npos) {
            const std::string_view line(request.data() + 6, end - 6);
            ours = QueryValue(line, "state") == state;
            if (ours && QueryValue(line, "scope").find("activity:write") != std::string::npos)
                code = QueryValue(line, "code");
        }
        const std::string reply = ours ? Page(!code.empty()) : "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send(client, reply.data(), static_cast<int>(reply.size()), 0);
        shutdown(client, SD_SEND);
        closesocket(client);
        if (ours) return code;  // empty when refused, or when uploads weren't allowed
    }
    return {};
}

std::string Credentials() {
    return "client_id=" + UrlEncode(STRAVA_CLIENT_ID) + "&client_secret=" + UrlEncode(STRAVA_CLIENT_SECRET);
}

// Why a reply failed, for the screen.
std::wstring Problem(const Reply& reply) {
    if (!reply.status) return L"Couldn't reach Strava. Check the internet connection.";
    if (reply.status == 429) return L"Strava is busy. Try again in a few minutes.";
    const std::string message = JsonString(reply.body, "message");
    wchar_t buf[64];
    swprintf_s(buf, L"Strava said no (%lu)", reply.status);
    return message.empty() ? buf : std::wstring(buf) + L": " + Widen(message);
}

// The access token, refreshed when it's about to run out. Empty with `error` set when that failed.
std::string AccessToken(const Shared& shared, unsigned job, std::wstring& error) {
    std::string refresh;
    {
        std::lock_guard lock(shared->mutex);
        if (shared->expires - 600 > static_cast<long long>(time(nullptr))) return shared->access;
        refresh = shared->refresh;
    }
    const Reply reply =
        PostForm(L"/oauth/token", Credentials() + "&grant_type=refresh_token&refresh_token=" + UrlEncode(refresh));
    std::lock_guard lock(shared->mutex);
    if (!shared->Current(job)) return {};
    if (reply.status == 200 && shared->TakeTokens(reply.body)) return shared->access;
    if (reply.status == 400 || reply.status == 401) {
        // The sign-in was revoked on Strava's side.
        shared->access.clear();
        shared->refresh.clear();
        shared->status.athlete.clear();
        shared->Save();
        error = L"Strava signed this app out. Upload again to sign in.";
    } else {
        error = Problem(reply);
    }
    return {};
}

// The activity a duplicate upload matches, from an error such as "... duplicate of <a href='/activities/123'>".
uint64_t DuplicateOf(const std::string& error) {
    if (error.find("duplicate") == std::string::npos) return 0;
    size_t at = error.rfind("activities/");
    if (at == std::string::npos) at = error.rfind("activity ");
    if (at == std::string::npos) return 0;
    at = error.find_first_of("0123456789", at);
    return at == std::string::npos ? 0 : std::strtoull(error.c_str() + at, nullptr, 10);
}

void Finish(const Shared& shared, unsigned job, uint64_t activity) {
    {
        std::lock_guard lock(shared->mutex);
        if (!shared->Current(job)) return;
        shared->status.state = Strava::State::Uploaded;
        shared->status.activity = activity;
    }
    shared->Notify();
}

// The ride as a multipart form for Strava's upload endpoint.
std::string UploadForm(const std::wstring& csvPath, const std::string& tcx, const std::string& boundary) {
    // The file name makes a stable external id, so the same ride isn't uploaded twice.
    const size_t slash = csvPath.find_last_of(L"\\/");
    std::string name = Narrow(csvPath.substr(slash == std::wstring::npos ? 0 : slash + 1));
    if (name.ends_with(".csv")) name.resize(name.size() - 4);
    auto field = [&](std::string_view key, std::string_view value) {
        return "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + std::string(key) + "\"\r\n\r\n" +
               std::string(value) + "\r\n";
    };
    std::string body = field("data_type", "tcx") + field("trainer", "1") +
                       field("description", "Uploaded with OpenCyclist") + field("external_id", "opencyclist-" + name);
    body.reserve(body.size() + tcx.size() + 256);
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + name +
            ".tcx\"\r\nContent-Type: application/xml\r\n\r\n";
    body += tcx;
    body += "\r\n--" + boundary + "--\r\n";
    return body;
}

// Signs in when there's a listener waiting for the browser, then uploads the ride and waits for Strava to make
// an activity of it.
winrt::fire_and_forget Work(Shared shared, unsigned job, std::wstring csvPath, SOCKET listener, std::string state) {
    co_await winrt::resume_background();
    if (listener != INVALID_SOCKET) {
        const std::string code = WaitForCode(shared, job, listener, state);
        {
            std::lock_guard lock(shared->mutex);
            if (shared->listener == listener) shared->CloseListener();
            if (!shared->Current(job)) co_return;
        }
        if (code.empty()) {
            Fail(shared, job, L"Strava wasn't connected. Allow uploads when signing in.");
            co_return;
        }
        const Reply reply =
            PostForm(L"/oauth/token", Credentials() + "&grant_type=authorization_code&code=" + UrlEncode(code));
        bool ok;
        {
            std::lock_guard lock(shared->mutex);
            if (!shared->Current(job)) co_return;
            ok = reply.status == 200 && shared->TakeTokens(reply.body);
            shared->status.state = ok ? Strava::State::Uploading : Strava::State::Failed;
            if (!ok) shared->status.error = Problem(reply);
        }
        shared->Notify();
        if (!ok) co_return;
    }

    std::wstring error;
    const std::string token = AccessToken(shared, job, error);
    if (token.empty()) {
        if (!error.empty()) Fail(shared, job, error);
        co_return;
    }
    const std::string tcx = RideToTcx(ReadFile(csvPath, 64u << 20));
    if (tcx.empty()) {
        Fail(shared, job, L"This ride has nothing to upload");
        co_return;
    }
    const std::string boundary = "OpenCyclist" + RandomHex(16);
    const std::wstring auth = L"Authorization: Bearer " + Widen(token);
    const Reply upload = Request(L"POST", L"/api/v3/uploads", UploadForm(csvPath, tcx, boundary),
                                 auth + L"\r\nContent-Type: multipart/form-data; boundary=" + Widen(boundary));
    if (upload.status != 201 && upload.status != 200) {
        Fail(shared, job, Problem(upload));
        co_return;
    }

    // Strava takes a few seconds to turn the file into an activity.
    const std::wstring path = L"/api/v3/uploads/" + std::to_wstring(JsonNumber(upload.body, "id"));
    std::string reply = upload.body;
    for (const ULONGLONG deadline = GetTickCount64() + kProcessingMs;;) {
        if (const auto activity = static_cast<uint64_t>(JsonNumber(reply, "activity_id"))) {
            Finish(shared, job, activity);
            co_return;
        }
        const std::string problem = JsonString(reply, "error");
        if (!problem.empty()) {
            if (const uint64_t activity = DuplicateOf(problem)) Finish(shared, job, activity);
            else Fail(shared, job, L"Strava couldn't read the ride: " + Widen(problem));
            co_return;
        }
        if (GetTickCount64() > deadline) {
            Fail(shared, job, L"Strava is still processing the ride. It shows up there soon.");
            co_return;
        }
        co_await winrt::resume_after(2s);
        {
            std::lock_guard lock(shared->mutex);
            if (!shared->Current(job)) co_return;
        }
        const Reply poll = Request(L"GET", path, {}, auth);
        if (poll.status == 200) {
            reply = poll.body;
        } else if (poll.status != 0 && poll.status < 500) {
            Fail(shared, job, Problem(poll));
            co_return;
        }
    }
}

// Tells Strava to forget this app's sign-in. Best effort: the tokens are gone from this PC either way.
winrt::fire_and_forget Deauthorize(std::string access) {
    co_await winrt::resume_background();
    if (!access.empty()) PostForm(L"/oauth/deauthorize", "access_token=" + UrlEncode(access));
}

}  // namespace

Strava::Strava(HWND window) : shared_(std::make_shared<Shared>()) { shared_->window = window; }

Strava::~Strava() {
    std::lock_guard lock(shared_->mutex);
    shared_->closed = true;
    shared_->CloseListener();
}

bool Strava::Available() { return STRAVA_CLIENT_ID[0] && STRAVA_CLIENT_SECRET[0]; }

Strava::Status Strava::status() const {
    std::lock_guard lock(shared_->mutex);
    shared_->Load();
    Status status = shared_->status;
    status.signedIn = !shared_->refresh.empty();
    return status;
}

void Strava::Clear() {
    std::lock_guard lock(shared_->mutex);
    Shared& s = *shared_;
    s.Load();
    if (s.status.state == State::Uploading) return;
    ++s.job;
    s.CloseListener();
    s.status.state = s.refresh.empty() ? State::SignedOut : State::Ready;
    s.status.error.clear();
    s.status.activity = 0;
}

void Strava::Upload(std::wstring csvPath) {
    if (!Available()) return;
    std::lock_guard lock(shared_->mutex);
    Shared& s = *shared_;
    s.Load();
    if (s.status.state == State::Uploading) return;
    ++s.job;
    s.status.error.clear();
    s.status.activity = 0;
    std::string state;
    if (s.refresh.empty()) {
        // Sign in through the browser, which comes back to a listener on this PC with a one-time code.
        static const bool started = [] {
            WSADATA data;
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int length = sizeof(address);
        s.CloseListener();
        s.listener = started ? socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) : INVALID_SOCKET;
        if (s.listener == INVALID_SOCKET || bind(s.listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
            listen(s.listener, 4) || getsockname(s.listener, reinterpret_cast<sockaddr*>(&address), &length)) {
            s.CloseListener();
            s.status.state = State::Failed;
            s.status.error = L"Couldn't start the Strava sign-in";
            s.Notify();
            return;
        }
        state = RandomHex(16);
        const std::string url = "https://www.strava.com/oauth/authorize?client_id=" + UrlEncode(STRAVA_CLIENT_ID) +
                                "&response_type=code&approval_prompt=auto&scope=activity%3Awrite&state=" + state +
                                "&redirect_uri=" +
                                UrlEncode("http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/");
        ShellExecuteW(nullptr, nullptr, Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        s.status.state = State::SigningIn;
    } else {
        s.status.state = State::Uploading;
    }
    s.Notify();
    Work(shared_, s.job, std::move(csvPath), s.listener, std::move(state));
}

void Strava::SignOut() {
    std::string access;
    {
        std::lock_guard lock(shared_->mutex);
        Shared& s = *shared_;
        s.Load();
        ++s.job;
        s.CloseListener();
        access = std::move(s.access);
        s.access.clear();
        s.refresh.clear();
        s.expires = 0;
        s.status = {};
        s.Save();
        s.Notify();
    }
    Deauthorize(std::move(access));
}

void Strava::OpenActivity() const {
    const uint64_t activity = status().activity;
    if (!activity) return;
    const std::wstring url = L"https://www.strava.com/activities/" + std::to_wstring(activity);
    ShellExecuteW(nullptr, nullptr, url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
