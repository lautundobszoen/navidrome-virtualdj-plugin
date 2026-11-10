#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <wincred.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include "vdjOnlineSource.h"
#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>
#include <cwctype>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "credui.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace {
using nlohmann::json;
constexpr wchar_t kSettingsSubkey[] = L"Software\\Cue\\VirtualDJ\\Navidrome";
constexpr wchar_t kServerUrlValue[] = L"ServerUrl";
constexpr char kApiVersion[] = "1.16.1";
constexpr char kClientName[] = "VirtualDJ-Navidrome-Cue";

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string normalizeServerUrl(const std::wstring& input) {
    size_t first = 0, last = input.size();
    while (first < last && iswspace(input[first])) ++first;
    while (last > first && iswspace(input[last - 1])) --last;
    if (last <= first || last - first > 2048) return {};
    const std::wstring trimmed = input.substr(first, last - first);
    for (wchar_t ch : trimmed) if (iswspace(ch) || ch < 0x20) return {};

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUserNameLength = static_cast<DWORD>(-1);
    parts.dwPasswordLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(trimmed.c_str(), 0, 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
        parts.dwHostNameLength == 0 || parts.dwUserNameLength != 0 ||
        parts.dwPasswordLength != 0 || parts.dwExtraInfoLength != 0) return {};

    std::wstring scheme = parts.nScheme == INTERNET_SCHEME_HTTPS ? L"https" : L"http";
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::transform(host.begin(), host.end(), host.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    if (host.find(L':') != std::wstring::npos && host.front() != L'[') host = L"[" + host + L"]";

    std::wstring result = scheme + L"://" + host;
    const INTERNET_PORT defaultPort = parts.nScheme == INTERNET_SCHEME_HTTPS ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    if (parts.nPort != defaultPort) result += L":" + std::to_wstring(parts.nPort);
    std::wstring path;
    if (parts.dwUrlPathLength) path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    while (path.size() > 1 && path.back() == L'/') path.pop_back();
    if (!path.empty() && path != L"/") result += path;
    return wideToUtf8(result);
}

std::string readServerUrl();
bool writeServerUrl(const std::string& url);
std::wstring credentialTargetForServer(const std::string& url);

std::string urlEncode(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
    }
    return out;
}

std::string makeSalt() {
    unsigned char bytes[16]{};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        std::random_device rd;
        for (auto& b : bytes) b = static_cast<unsigned char>(rd());
    }
    static const char hex[] = "0123456789abcdef";
    std::string salt;
    salt.reserve(sizeof(bytes) * 2);
    for (unsigned char b : bytes) {
        salt.push_back(hex[b >> 4]);
        salt.push_back(hex[b & 0x0f]);
    }
    SecureZeroMemory(bytes, sizeof(bytes));
    return salt;
}

std::string md5Hex(const std::string& value) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[16]{};
    DWORD hashObjectSize = 0, bytesRead = 0;
    std::vector<unsigned char> hashObject;
    std::string result;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) == 0 &&
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&hashObjectSize), sizeof(hashObjectSize), &bytesRead, 0) == 0) {
        hashObject.resize(hashObjectSize);
        if (BCryptCreateHash(algorithm, &hash, hashObject.data(), hashObjectSize, nullptr, 0, 0) == 0 &&
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
                static_cast<ULONG>(value.size()), 0) == 0 &&
            BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            static const char hex[] = "0123456789abcdef";
            result.reserve(sizeof(digest) * 2);
            for (unsigned char b : digest) {
                result.push_back(hex[b >> 4]);
                result.push_back(hex[b & 0x0f]);
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!hashObject.empty()) SecureZeroMemory(hashObject.data(), hashObject.size());
    SecureZeroMemory(digest, sizeof(digest));
    return result;
}

std::string readServerUrl() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsSubkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return {};
    DWORD type = 0, size = 0;
    LONG status = RegQueryValueExW(key, kServerUrlValue, nullptr, &type, nullptr, &size);
    if (status != ERROR_SUCCESS || type != REG_SZ || size < sizeof(wchar_t) || size > 8192) {
        RegCloseKey(key);
        return {};
    }
    std::vector<wchar_t> value(size / sizeof(wchar_t) + 1, L'\0');
    status = RegQueryValueExW(key, kServerUrlValue, nullptr, &type,
        reinterpret_cast<LPBYTE>(value.data()), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) return {};
    return normalizeServerUrl(value.data());
}

bool writeServerUrl(const std::string& url) {
    const std::wstring value = utf8ToWide(url);
    if (value.empty()) return false;
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsSubkey, 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key, &disposition) != ERROR_SUCCESS) return false;
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG status = RegSetValueExW(key, kServerUrlValue, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

std::wstring credentialTargetForServer(const std::string& url) {
    const std::string hash = md5Hex(url);
    return hash.empty() ? L"Cue.VirtualDJ.Navidrome" : L"Cue.VirtualDJ.Navidrome." + utf8ToWide(hash);
}

struct Credentials {
    std::wstring username;
    std::wstring password;
    std::string serverUrl;
    ~Credentials() {
        if (!password.empty()) SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
    }
};

std::shared_ptr<Credentials> readSavedCredentials(const std::string& serverUrl) {
    PCREDENTIALW raw = nullptr;
    const std::wstring target = credentialTargetForServer(serverUrl);
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &raw) || !raw) return {};
    auto result = std::make_shared<Credentials>();
    result->serverUrl = serverUrl;
    if (raw->UserName) result->username = raw->UserName;
    if (raw->CredentialBlob && raw->CredentialBlobSize >= sizeof(wchar_t)) {
        const auto* p = reinterpret_cast<const wchar_t*>(raw->CredentialBlob);
        const size_t n = raw->CredentialBlobSize / sizeof(wchar_t);
        result->password.assign(p, p + n);
        while (!result->password.empty() && result->password.back() == L'\0') result->password.pop_back();
    }
    CredFree(raw);
    if (result->username.empty() || result->password.empty()) return {};
    return result;
}

void saveCredentials(const Credentials& credentials) {
    CREDENTIALW item{};
    const std::wstring target = credentialTargetForServer(credentials.serverUrl);
    item.Type = CRED_TYPE_GENERIC;
    item.TargetName = const_cast<LPWSTR>(target.c_str());
    item.UserName = const_cast<LPWSTR>(credentials.username.c_str());
    item.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(credentials.password.data()));
    item.CredentialBlobSize = static_cast<DWORD>(credentials.password.size() * sizeof(wchar_t));
    item.Persist = CRED_PERSIST_LOCAL_MACHINE;
    item.Comment = const_cast<LPWSTR>(L"VirtualDJ Navidrome plugin");
    CredWriteW(&item, 0);
}

void deleteSavedCredentials(const std::string& serverUrl) {
    const std::wstring target = credentialTargetForServer(serverUrl);
    CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
}

struct ServerUrlPromptState {
    std::wstring initialUrl;
    std::string resultUrl;
};

void appendDialogBytes(std::vector<BYTE>& bytes, const void* source, size_t length) {
    const size_t offset = bytes.size();
    bytes.resize(offset + length);
    memcpy(bytes.data() + offset, source, length);
}

void appendDialogWord(std::vector<BYTE>& bytes, WORD value) {
    appendDialogBytes(bytes, &value, sizeof(value));
}

void appendDialogDword(std::vector<BYTE>& bytes, DWORD value) {
    appendDialogBytes(bytes, &value, sizeof(value));
}

void appendDialogShort(std::vector<BYTE>& bytes, short value) {
    appendDialogBytes(bytes, &value, sizeof(value));
}

void appendDialogString(std::vector<BYTE>& bytes, const wchar_t* value) {
    const size_t length = wcslen(value) + 1;
    appendDialogBytes(bytes, value, length * sizeof(wchar_t));
}

void appendDialogControl(std::vector<BYTE>& bytes, DWORD style, short x, short y,
    short width, short height, WORD id, WORD classAtom, const wchar_t* title) {
    while (bytes.size() % 4) bytes.push_back(0);
    appendDialogDword(bytes, style);
    appendDialogDword(bytes, 0);
    appendDialogShort(bytes, x);
    appendDialogShort(bytes, y);
    appendDialogShort(bytes, width);
    appendDialogShort(bytes, height);
    appendDialogWord(bytes, id);
    appendDialogWord(bytes, 0xFFFF);
    appendDialogWord(bytes, classAtom);
    appendDialogString(bytes, title);
    appendDialogWord(bytes, 0);
}

INT_PTR CALLBACK serverUrlDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ServerUrlPromptState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<ServerUrlPromptState*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SetDlgItemTextW(dialog, 1002, state->initialUrl.c_str());
        SendDlgItemMessageW(dialog, 1002, EM_LIMITTEXT, 2048, 0);
        SendDlgItemMessageW(dialog, 1002, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dialog, 1002));
        return FALSE;
    }
    if (message == WM_COMMAND && state) {
        if (LOWORD(wParam) == IDOK) {
            std::vector<wchar_t> text(2049, L'\0');
            GetDlgItemTextW(dialog, 1002, text.data(), static_cast<int>(text.size()));
            state->resultUrl = normalizeServerUrl(text.data());
            if (state->resultUrl.empty()) {
                MessageBoxW(dialog,
                    L"Bitte eine gültige http://- oder https://-Adresse ohne Abfrageparameter eingeben.",
                    L"Navidrome-Serveradresse", MB_OK | MB_ICONWARNING);
                SetFocus(GetDlgItem(dialog, 1002));
                return TRUE;
            }
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

bool promptForServerUrl(const std::string& currentUrl, std::string& selectedUrl) {
    std::vector<BYTE> bytes;
    appendDialogDword(bytes, WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | WS_VISIBLE);
    appendDialogDword(bytes, 0);
    appendDialogWord(bytes, 4);
    appendDialogShort(bytes, 0);
    appendDialogShort(bytes, 0);
    appendDialogShort(bytes, 250);
    appendDialogShort(bytes, 76);
    appendDialogWord(bytes, 0);
    appendDialogWord(bytes, 0);
    appendDialogString(bytes, L"Navidrome-Server verbinden");
    appendDialogWord(bytes, 9);
    appendDialogString(bytes, L"Segoe UI");
    appendDialogControl(bytes, WS_CHILD | WS_VISIBLE, 8, 7, 234, 12, 1001, 0x0082,
        L"Server-URL (z. B. https://music.example.com):");
    appendDialogControl(bytes, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
        8, 21, 234, 14, 1002, 0x0081, utf8ToWide(currentUrl).c_str());
    appendDialogControl(bytes, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        134, 49, 52, 14, IDOK, 0x0080, L"Verbinden");
    appendDialogControl(bytes, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        190, 49, 52, 14, IDCANCEL, 0x0080, L"Abbrechen");

    ServerUrlPromptState state{utf8ToWide(currentUrl), {}};
    HWND owner = GetActiveWindow();
    const INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
        reinterpret_cast<LPCDLGTEMPLATEW>(bytes.data()), owner, serverUrlDialogProc,
        reinterpret_cast<LPARAM>(&state));
    if (result != IDOK) return false;
    selectedUrl = state.resultUrl;
    return !selectedUrl.empty();
}

std::vector<json> asArray(const json& value) {
    std::vector<json> result;
    if (value.is_array()) {
        result.reserve(value.size());
        for (const auto& item : value) result.push_back(item);
    } else if (value.is_object()) {
        result.push_back(value);
    }
    return result;
}

std::string stringValue(const json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || object[key].is_null()) return {};
    const auto& value = object[key];
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number() || value.is_boolean()) return value.dump();
    return {};
}

int intValue(const json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || object[key].is_null()) return 0;
    try {
        if (object[key].is_number()) return object[key].get<int>();
        if (object[key].is_string()) return std::stoi(object[key].get<std::string>());
    } catch (...) {}
    return 0;
}

class NavidromeApi {
public:
    explicit NavidromeApi(std::shared_ptr<Credentials> credentials) : credentials_(std::move(credentials)) {}

    bool call(const std::string& endpoint,
              const std::vector<std::pair<std::string, std::string>>& params,
              json& response, std::string* error = nullptr) const {
        if (!credentials_ || credentials_->serverUrl.empty() ||
            credentials_->username.empty() || credentials_->password.empty()) {
            if (error) *error = "Navidrome-Zugangsdaten fehlen.";
            return false;
        }
        const std::string username = wideToUtf8(credentials_->username);
        std::string password = wideToUtf8(credentials_->password);
        const std::string salt = makeSalt();
        const std::string token = md5Hex(password + salt);
        if (!password.empty()) SecureZeroMemory(password.data(), password.size());
        if (token.empty()) {
            if (error) *error = "Die API-Anmeldung konnte nicht vorbereitet werden.";
            return false;
        }

        std::string url = credentials_->serverUrl + "/rest/" + endpoint + ".view?u=" + urlEncode(username) +
            "&t=" + token + "&s=" + salt + "&v=" + kApiVersion + "&c=" + kClientName + "&f=json";
        for (const auto& param : params) url += "&" + urlEncode(param.first) + "=" + urlEncode(param.second);
        std::string body;
        if (!httpGet(url, body)) {
            if (error) *error = "Navidrome ist nicht erreichbar. Bitte Netzwerk und Server prüfen.";
            return false;
        }
        try {
            const json root = json::parse(body);
            if (!root.is_object() || !root.contains("subsonic-response")) {
                if (error) *error = "Navidrome hat eine unerwartete API-Antwort gesendet.";
                return false;
            }
            response = root["subsonic-response"];
            const std::string status = stringValue(response, "status");
            if (status != "ok") {
                std::string message = "Navidrome-API-Fehler.";
                if (response.contains("error")) {
                    const std::string details = stringValue(response["error"], "message");
                    if (!details.empty()) message = details;
                }
                if (error) *error = message;
                return false;
            }
            return true;
        } catch (...) {
            if (error) *error = "Navidrome hat ungültige JSON-Daten gesendet.";
            return false;
        }
    }

    std::string signedUrl(const std::string& endpoint,
                         const std::vector<std::pair<std::string, std::string>>& params) const {
        if (!credentials_ || credentials_->serverUrl.empty()) return {};
        const std::string username = wideToUtf8(credentials_->username);
        std::string password = wideToUtf8(credentials_->password);
        const std::string salt = makeSalt();
        const std::string token = md5Hex(password + salt);
        if (!password.empty()) SecureZeroMemory(password.data(), password.size());
        if (token.empty()) return {};
        std::string url = credentials_->serverUrl + "/rest/" + endpoint + ".view?u=" + urlEncode(username) +
            "&t=" + token + "&s=" + salt + "&v=" + kApiVersion + "&c=" + kClientName + "&f=json";
        for (const auto& param : params) url += "&" + urlEncode(param.first) + "=" + urlEncode(param.second);
        return url;
    }

private:
    static bool httpGet(const std::string& url, std::string& body) {
        const std::wstring wideUrl = utf8ToWide(url);
        if (wideUrl.empty()) return false;
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwSchemeLength = static_cast<DWORD>(-1);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) return false;
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

        HINTERNET session = WinHttpOpen(L"VirtualDJ Navidrome Online Source/1.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;
        WinHttpSetTimeouts(session, 5000, 5000, 10000, 20000);
        HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
        if (!connection) { WinHttpCloseHandle(session); return false; }
        const DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request) {
            WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return false;
        }
        bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
        if (ok) {
            DWORD status = 0, statusSize = sizeof(status);
            if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) || status < 200 || status >= 300) {
                ok = false;
            }
        }
        if (ok) {
            char buffer[16384];
            DWORD bytes = 0;
            while (WinHttpReadData(request, buffer, sizeof(buffer), &bytes) && bytes > 0) {
                body.append(buffer, bytes);
                if (body.size() > 64 * 1024 * 1024) { ok = false; break; }
            }
        }
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return ok;
    }

    std::shared_ptr<Credentials> credentials_;
};

enum class FolderType { Playlist, Artist, Genre };
struct FolderRecord {
    FolderType type;
    std::string id;
    std::string name;
};

} // namespace

class NavidromeVirtualDJSource final : public IVdjPluginOnlineSource {
public:
    NavidromeVirtualDJSource() { oauth = nullptr; }

    HRESULT VDJ_API OnGetPluginInfo(TVdjPluginInfo8* info) override {
        if (!info) return E_FAIL;
        info->Bitmap = 0;
        info->Flags = 0;
        info->PluginName = "Navidrome";
        info->Author = "Cue";
        info->Description = "Browse and stream a Navidrome library in VirtualDJ";
        info->Version = "1.1.0";
        return S_OK;
    }

    HRESULT VDJ_API IsLogged() override {
        const std::string serverUrl = readServerUrl();
        if (serverUrl.empty()) return S_FALSE;
        auto credentials = currentCredentials();
        if (!credentials || credentials->serverUrl != serverUrl) {
            credentials = readSavedCredentials(serverUrl);
            if (!credentials) return S_FALSE;
        }
        json response;
        NavidromeApi api(credentials);
        if (!api.call("ping", {}, response)) return S_FALSE;
        setCredentials(credentials);
        return S_OK;
    }

    HRESULT VDJ_API OnLogin() override {
        std::string serverUrl;
        if (!promptForServerUrl(readServerUrl(), serverUrl)) return S_FALSE;
        if (!writeServerUrl(serverUrl)) {
            MessageBoxW(nullptr, L"Die Serveradresse konnte nicht gespeichert werden.",
                L"VirtualDJ – Navidrome", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
            return S_FALSE;
        }
        if (serverUrl.compare(0, 7, "http://") == 0) {
            MessageBoxW(nullptr,
                L"Diese Verbindung verwendet unverschlüsseltes HTTP. Verwende HTTP nur in einem vertrauenswürdigen lokalen Netzwerk; für den Zugriff über das Internet wird HTTPS empfohlen.",
                L"Navidrome-Verbindung", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        }

        auto credentials = currentCredentials();
        if (!credentials || credentials->serverUrl != serverUrl) credentials = readSavedCredentials(serverUrl);
        if (credentials) {
            json response;
            if (NavidromeApi(credentials).call("ping", {}, response)) {
                setCredentials(credentials);
                return S_OK;
            }
        }

        std::vector<wchar_t> user(CRED_MAX_USERNAME_LENGTH + 1, L'\0');
        std::vector<wchar_t> password(CREDUI_MAX_PASSWORD_LENGTH + 1, L'\0');
        if (credentials && !credentials->username.empty()) {
            wcsncpy_s(user.data(), user.size(), credentials->username.c_str(), _TRUNCATE);
        }
        CREDUI_INFOW info{};
        info.cbSize = sizeof(info);
        info.pszCaptionText = L"Navidrome mit VirtualDJ verbinden";
        const std::wstring credentialMessage = L"Bitte Benutzername und Passwort für " + utf8ToWide(serverUrl) + L" eingeben.";
        info.pszMessageText = credentialMessage.c_str();
        BOOL save = FALSE;
        const DWORD flags = CREDUI_FLAGS_GENERIC_CREDENTIALS | CREDUI_FLAGS_ALWAYS_SHOW_UI |
            CREDUI_FLAGS_SHOW_SAVE_CHECK_BOX;
        const std::wstring credentialTarget = credentialTargetForServer(serverUrl);
        const DWORD result = CredUIPromptForCredentialsW(&info, credentialTarget.c_str(), nullptr, 0,
            user.data(), static_cast<ULONG>(user.size()), password.data(),
            static_cast<ULONG>(password.size()), &save, flags);
        if (result != NO_ERROR) {
            SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
            SecureZeroMemory(user.data(), user.size() * sizeof(wchar_t));
            return S_FALSE;
        }

        auto entered = std::make_shared<Credentials>();
        entered->username = user.data();
        entered->password = password.data();
        entered->serverUrl = serverUrl;
        SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
        SecureZeroMemory(user.data(), user.size() * sizeof(wchar_t));
        json response;
        std::string error;
        if (!NavidromeApi(entered).call("ping", {}, response, &error)) {
            MessageBoxW(nullptr, L"Die Navidrome-Anmeldung ist fehlgeschlagen. Bitte Zugangsdaten und Server prüfen.",
                L"VirtualDJ – Navidrome", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
            return S_FALSE;
        }
        if (save) saveCredentials(*entered);
        else deleteSavedCredentials(serverUrl);
        setCredentials(entered);
        return S_OK;
    }

    HRESULT VDJ_API OnLogout() override {
        auto credentials = currentCredentials();
        const std::string configuredUrl = readServerUrl();
        const std::string serverUrl = !configuredUrl.empty() ? configuredUrl :
            (credentials ? credentials->serverUrl : std::string{});
        if (!serverUrl.empty()) deleteSavedCredentials(serverUrl);
        std::lock_guard<std::mutex> guard(credentialsMutex_);
        credentials_.reset();
        return S_OK;
    }

    HRESULT VDJ_API OnSearch(const char* search, IVdjTracksList* tracksList) override {
        if (!search || !tracksList) return E_FAIL;
        // This implementation is synchronous; the SDK's async OnSearch completion path is not used.
        cancelled_.store(false);
        auto credentials = currentCredentials();
        if (!credentials) return E_FAIL;
        NavidromeApi api(credentials);
        const std::string query(search);
        size_t offset = 0;
        constexpr size_t pageSize = 500;
        while (!cancelled_.load()) {
            json response;
            if (!api.call("search3", {{"query", query}, {"songCount", std::to_string(pageSize)},
                    {"songOffset", std::to_string(offset)}, {"albumCount", "0"}, {"artistCount", "0"}}, response))
                return E_FAIL;
            json result = response.value("searchResult3", json::object());
            const auto songs = asArray(result.value("song", json{}));
            for (const auto& song : songs) {
                if (cancelled_.load()) break;
                addSong(api, song, tracksList);
            }
            if (songs.size() < pageSize) break;
            offset += songs.size();
        }
        return S_OK;
    }

    HRESULT VDJ_API OnSearchCancel() override {
        cancelled_.store(true);
        return S_OK;
    }

    HRESULT VDJ_API GetFolderList(IVdjSubfoldersList* subfoldersList) override {
        if (!subfoldersList) return E_FAIL;
        auto credentials = currentCredentials();
        if (!credentials) return E_FAIL;
        NavidromeApi api(credentials);
        std::map<std::string, FolderRecord> found;
        json response;
        if (!api.call("getPlaylists", {}, response)) return E_FAIL;
        const auto playlists = asArray(response.value("playlists", json::object()).value("playlist", json{}));
        for (const auto& playlist : playlists) {
            const std::string id = stringValue(playlist, "id");
            const std::string name = stringValue(playlist, "name");
            if (id.empty() || name.empty()) continue;
            const std::string key = "playlist:" + id;
            found.emplace(key, FolderRecord{FolderType::Playlist, id, name});
        }

        if (!api.call("getArtists", {}, response)) return E_FAIL;
        const auto indexes = asArray(response.value("artists", json::object()).value("index", json{}));
        for (const auto& index : indexes) {
            const auto artists = asArray(index.value("artist", json{}));
            for (const auto& artist : artists) {
                const std::string id = stringValue(artist, "id");
                const std::string name = stringValue(artist, "name");
                if (id.empty() || name.empty()) continue;
                const std::string key = "artist:" + id;
                found.emplace(key, FolderRecord{FolderType::Artist, id, name});
            }
        }

        if (!api.call("getGenres", {}, response)) return E_FAIL;
        const auto genres = asArray(response.value("genres", json::object()).value("genre", json{}));
        for (const auto& genre : genres) {
            const std::string name = stringValue(genre, "value");
            if (name.empty()) continue;
            const std::string key = "genre:" + name;
            found.emplace(key, FolderRecord{FolderType::Genre, name, name});
        }

        {
            std::lock_guard<std::mutex> guard(foldersMutex_);
            folders_ = found;
        }
        for (const auto& item : found) {
            const FolderRecord& folder = item.second;
            std::string label;
            switch (folder.type) {
                case FolderType::Playlist: label = "Playlist: "; break;
                case FolderType::Artist: label = "Künstler: "; break;
                case FolderType::Genre: label = "Genre: "; break;
            }
            label += folder.name;
            subfoldersList->add(item.first.c_str(), label.c_str());
        }
        return S_OK;
    }

    HRESULT VDJ_API GetFolder(const char* folderUniqueId, IVdjTracksList* tracksList) override {
        if (!folderUniqueId || !tracksList) return E_FAIL;
        cancelled_.store(false);
        FolderRecord folder;
        {
            std::lock_guard<std::mutex> guard(foldersMutex_);
            const auto it = folders_.find(folderUniqueId);
            if (it == folders_.end()) return E_FAIL;
            folder = it->second;
        }
        auto credentials = currentCredentials();
        if (!credentials) return E_FAIL;
        NavidromeApi api(credentials);
        if (folder.type == FolderType::Playlist) return listPlaylist(api, folder.id, tracksList);
        if (folder.type == FolderType::Artist) return listArtist(api, folder.id, tracksList);
        return listGenre(api, folder.id, tracksList);
    }

    HRESULT VDJ_API GetStreamUrl(const char* uniqueId, IVdjString& url, IVdjString& errorMessage) override {
        if (!uniqueId || !*uniqueId) {
            errorMessage = "Ungültige Navidrome-Track-ID.";
            return S_FALSE;
        }
        auto credentials = currentCredentials();
        if (!credentials) {
            errorMessage = "Bitte zuerst bei Navidrome anmelden.";
            return S_FALSE;
        }
        const std::string stream = NavidromeApi(credentials).signedUrl("stream", {{"id", uniqueId}});
        if (stream.empty()) {
            errorMessage = "Stream-Adresse konnte nicht erzeugt werden.";
            return S_FALSE;
        }
        url = stream.c_str();
        return S_OK;
    }

private:
    std::shared_ptr<Credentials> currentCredentials() {
        std::lock_guard<std::mutex> guard(credentialsMutex_);
        return credentials_;
    }

    void setCredentials(std::shared_ptr<Credentials> value) {
        std::lock_guard<std::mutex> guard(credentialsMutex_);
        credentials_ = std::move(value);
    }

    static std::string preferred(const json& song, const char* primary, const char* fallback) {
        std::string value = stringValue(song, primary);
        return value.empty() ? stringValue(song, fallback) : value;
    }

    static void addSong(const NavidromeApi& api, const json& song, IVdjTracksList* tracksList) {
        const std::string id = stringValue(song, "id");
        const std::string title = stringValue(song, "title");
        if (id.empty() || title.empty()) return;
        const std::string artist = preferred(song, "displayArtist", "artist");
        const std::string genre = stringValue(song, "genre");
        const std::string album = stringValue(song, "album");
        const std::string coverId = stringValue(song, "coverArt");
        std::string coverUrl;
        if (!coverId.empty()) coverUrl = api.signedUrl("getCoverArt", {{"id", coverId}});
        const float duration = static_cast<float>(intValue(song, "duration"));
        const float bpm = static_cast<float>(intValue(song, "bpm"));
        const int year = intValue(song, "year");
        tracksList->add(id.c_str(), title.c_str(), artist.c_str(), nullptr,
            genre.empty() ? nullptr : genre.c_str(), nullptr,
            album.empty() ? nullptr : album.c_str(),
            coverUrl.empty() ? nullptr : coverUrl.c_str(), nullptr,
            duration, bpm, 0, year, false, false);
    }

    static HRESULT listPlaylist(const NavidromeApi& api, const std::string& id, IVdjTracksList* tracksList) {
        json response;
        if (!api.call("getPlaylist", {{"id", id}}, response)) return E_FAIL;
        const json playlist = response.value("playlist", json::object());
        const auto songs = asArray(playlist.value("entry", json{}));
        for (const auto& song : songs) addSong(api, song, tracksList);
        return S_OK;
    }

    HRESULT listArtist(const NavidromeApi& api, const std::string& id, IVdjTracksList* tracksList) {
        json response;
        if (!api.call("getArtist", {{"id", id}}, response)) return E_FAIL;
        const json artist = response.value("artist", json::object());
        const auto albums = asArray(artist.value("album", json{}));
        std::vector<std::string> seen;
        for (const auto& album : albums) {
            if (cancelled_.load()) break;
            const std::string albumId = stringValue(album, "id");
            if (albumId.empty()) continue;
            json albumResponse;
            if (!api.call("getAlbum", {{"id", albumId}}, albumResponse)) continue;
            const json fullAlbum = albumResponse.value("album", json::object());
            const auto songs = asArray(fullAlbum.value("song", json{}));
            for (const auto& song : songs) {
                if (cancelled_.load()) break;
                const std::string trackId = stringValue(song, "id");
                if (trackId.empty() || std::find(seen.begin(), seen.end(), trackId) != seen.end()) continue;
                seen.push_back(trackId);
                addSong(api, song, tracksList);
            }
        }
        return S_OK;
    }

    HRESULT listGenre(const NavidromeApi& api, const std::string& genre, IVdjTracksList* tracksList) {
        constexpr size_t pageSize = 500;
        size_t offset = 0;
        while (!cancelled_.load()) {
            json response;
            if (!api.call("getSongsByGenre", {{"genre", genre}, {"count", std::to_string(pageSize)},
                    {"offset", std::to_string(offset)}}, response)) return E_FAIL;
            const json result = response.value("songsByGenre", json::object());
            const auto songs = asArray(result.value("song", json{}));
            for (const auto& song : songs) {
                if (cancelled_.load()) break;
                addSong(api, song, tracksList);
            }
            if (songs.size() < pageSize) break;
            offset += songs.size();
        }
        return S_OK;
    }

    std::mutex credentialsMutex_;
    std::shared_ptr<Credentials> credentials_;
    std::mutex foldersMutex_;
    std::map<std::string, FolderRecord> folders_;
    std::atomic<bool> cancelled_{false};
};

extern "C" VDJ_EXPORT HRESULT VDJ_API DllGetClassObject(const GUID& rclsid, const GUID& riid, void** ppObject) {
    if (!ppObject) return E_FAIL;
    *ppObject = nullptr;
    if (memcmp(&rclsid, &CLSID_VdjPlugin8, sizeof(GUID)) == 0 &&
        memcmp(&riid, &IID_IVdjPluginOnlineSource, sizeof(GUID)) == 0) {
        *ppObject = new NavidromeVirtualDJSource();
        return NO_ERROR;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}
