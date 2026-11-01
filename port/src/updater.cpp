#include "updater.hpp"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <system_error>
#include <vector>

#include "inflate.hpp"
#include "paths.hpp"
#include "ui_text.hpp"

namespace amg::updater {

namespace {

namespace fs = std::filesystem;

constexpr size_t MAX_DOWNLOAD = 64u * 1024 * 1024;  // a release is a few megabytes: anything bigger is not one

std::string base_url(const std::string& tag) {
    if (const char* b = std::getenv("AMG_UPDATE_BASE")) return b;
    return "https://github.com/hokagocafe/open-amagami/releases/download/" + tag + "/";
}

const char* zip_name() { return sizeof(void*) == 8 ? "open-amagami-windows-x64.zip" : "open-amagami-windows-x86.zip"; }

// A GET that follows redirects (GitHub sends downloads to its storage). "" and an `error` on failure.
std::string http_get(const std::string& url, std::string& error) {
    const std::wstring wide = ui::wide(url);
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) {
        error = "bad address";
        return "";
    }
    std::string body;
    HINTERNET session = WinHttpOpen(L"open-amagami", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags) : nullptr;
    bool ok = false;
    if (request) {
        WinHttpSetTimeouts(request, 10000, 10000, 20000, 20000);
        if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, size = sizeof status;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                WINHTTP_NO_HEADER_INDEX);
            if (status != 200) {
                error = "the server answered " + std::to_string(status);
            } else {
                char chunk[16384];
                DWORD got = 0;
                ok = true;
                while (WinHttpReadData(request, chunk, sizeof chunk, &got) && got > 0) {
                    body.append(chunk, got);
                    if (body.size() > MAX_DOWNLOAD) {
                        ok = false;
                        error = "the download is too big";
                        break;
                    }
                }
            }
        } else {
            error = "no connection";
        }
    } else {
        error = "no connection";
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok ? body : "";
}

std::string sha256_hex(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    UCHAR digest[32] = {};
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), ULONG(data.size()), 0) == 0 &&
        BCryptFinishHash(hash, digest, sizeof digest, 0) == 0) {
        static const char* HEX = "0123456789abcdef";
        for (UCHAR b : digest) {
            out += HEX[b >> 4];
            out += HEX[b & 15];
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

// The hash that SHA256SUMS.txt gives for `name` ("<hash>  <name>" per line), lowercase; "" if it is not listed.
std::string listed_hash(const std::string& sums, const std::string& name) {
    size_t at = 0;
    while (at < sums.size()) {
        size_t end = sums.find('\n', at);
        if (end == std::string::npos) end = sums.size();
        std::string line = sums.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t gap = line.find(' ');
        if (gap == std::string::npos) continue;
        std::string file = line.substr(line.find_first_not_of(' ', gap));
        if (!file.empty() && file[0] == '*') file.erase(0, 1);
        if (file != name) continue;
        std::string hash = line.substr(0, gap);
        for (char& c : hash) c = char(std::tolower(static_cast<unsigned char>(c)));
        return hash;
    }
    return "";
}

uint32_t le32(const std::string& s, size_t at) {
    return uint32_t(uint8_t(s[at])) | uint32_t(uint8_t(s[at + 1])) << 8 | uint32_t(uint8_t(s[at + 2])) << 16 | uint32_t(uint8_t(s[at + 3])) << 24;
}
uint16_t le16(const std::string& s, size_t at) { return uint16_t(uint8_t(s[at]) | uint8_t(s[at + 1]) << 8); }

// Every file of a zip, by its name inside it. False (and a reason) if the zip is not one this code reads (stored or deflate).
bool read_zip(const std::string& z, std::map<std::string, std::string>& files, std::string& error) {
    if (z.size() < 22) {
        error = "the download is not a zip";
        return false;
    }
    size_t end = std::string::npos;
    for (size_t at = z.size() - 22, stop = z.size() > 70000 ? z.size() - 70000 : 0;; --at) {
        if (le32(z, at) == 0x06054b50) {
            end = at;
            break;
        }
        if (at == stop) break;
    }
    if (end == std::string::npos) {
        error = "the download is not a zip";
        return false;
    }
    const size_t count = le16(z, end + 10);
    size_t at = le32(z, end + 16);
    for (size_t i = 0; i < count; ++i) {
        if (at + 46 > z.size() || le32(z, at) != 0x02014b50) {
            error = "the zip is damaged";
            return false;
        }
        const uint16_t method = le16(z, at + 10);
        const uint32_t packed = le32(z, at + 20), size = le32(z, at + 24);
        const size_t name_len = le16(z, at + 28), extra_len = le16(z, at + 30), comment_len = le16(z, at + 32);
        const size_t local = le32(z, at + 42);
        if (at + 46 + name_len > z.size() || local + 30 > z.size() || le32(z, local) != 0x04034b50) {
            error = "the zip is damaged";
            return false;
        }
        std::string name = z.substr(at + 46, name_len);
        for (char& c : name)
            if (c == '\\') c = '/';
        at += 46 + name_len + extra_len + comment_len;
        if (name.empty() || name.back() == '/') continue;  // a folder
        const size_t data = local + 30 + le16(z, local + 26) + le16(z, local + 28);
        if (data + packed > z.size()) {
            error = "the zip is damaged";
            return false;
        }
        std::string content;
        if (method == 0) {
            content = z.substr(data, packed);
        } else if (method == 8) {
            content = inflate_raw(reinterpret_cast<const uint8_t*>(z.data()) + data, packed, size);
        } else {
            error = "the zip uses a compression this program does not read";
            return false;
        }
        if (content.size() != size) {
            error = "the zip is damaged";
            return false;
        }
        files[name] = std::move(content);
    }
    return true;
}

// "open-amagami/skins/kbd/a.png" -> "skins/kbd/a.png"; "" for a name that must not be written (outside the folder, a
// drive or the player's own data).
fs::path beside(fs::path p, const wchar_t* suffix) {
    p += suffix;
    return p;
}

std::string inside_name(const std::string& name) {
    const size_t slash = name.find('/');
    if (slash == std::string::npos) return "";
    std::string rel = name.substr(slash + 1);
    if (rel.empty() || rel[0] == '/' || rel.find(':') != std::string::npos) return "";
    for (size_t at = 0; at <= rel.size();) {
        size_t end = rel.find('/', at);
        if (end == std::string::npos) end = rel.size();
        const std::string part = rel.substr(at, end - at);
        if (part == ".." || part == ".") return "";
        at = end + 1;
    }
    if (rel == "portable.txt" || rel.rfind("portable-data/", 0) == 0) return "";  // the player's own
    return rel;
}

}  // namespace

std::string install(const std::string& tag, const std::function<void(const std::string&)>& progress) {
    const std::string dir = paths::exe_dir();
    if (dir.empty()) return ui::tr("The program's folder is not known.");
    const std::string base = base_url(tag);
    std::string error;
    progress(ui::tr("Downloading..."));
    const std::string sums = http_get(base + "SHA256SUMS.txt", error);
    if (sums.empty()) return ui::fmt("The update could not be downloaded ({0}).", {error});
    const std::string zip = http_get(base + zip_name(), error);
    if (zip.empty()) return ui::fmt("The update could not be downloaded ({0}).", {error});
    progress(ui::tr("Checking..."));
    const std::string wanted = listed_hash(sums, zip_name());
    if (wanted.empty() || wanted != sha256_hex(zip)) return ui::tr("The download did not match its checksum, so it was not installed.");
    std::map<std::string, std::string> files;
    if (!read_zip(zip, files, error)) return ui::fmt("The update could not be read ({0}).", {error});
    std::map<std::string, std::string> todo;
    for (auto& [name, content] : files) {
        const std::string rel = inside_name(name);
        if (!rel.empty()) todo[rel] = std::move(content);
    }
    if (!todo.count("amagami.exe")) return ui::tr("The download does not hold the program, so it was not installed.");
    progress(ui::tr("Installing..."));
    // Each file is written beside its place first; only when all are written are they moved in, the old ones to `.old`.
    std::vector<std::string> written;
    auto path_of = [&](const std::string& rel) { return fs::u8path(dir) / fs::u8path(rel); };
    std::error_code ec;
    for (const auto& [rel, content] : todo) {
        const fs::path target = path_of(rel);
        fs::create_directories(target.parent_path(), ec);
        const fs::path temp = beside(target, L".new");
        std::FILE* f = _wfopen(temp.c_str(), L"wb");
        if (!f || std::fwrite(content.data(), 1, content.size(), f) != content.size()) {
            if (f) std::fclose(f);
            for (const std::string& w : written) fs::remove(beside(path_of(w), L".new"), ec);
            fs::remove(temp, ec);
            return ui::tr("The update could not be written to the program's folder (is it in a protected place?).");
        }
        std::fclose(f);
        written.push_back(rel);
    }
    std::vector<std::string> moved;  // the files whose old copy went to `.old`
    for (const std::string& rel : written) {
        const fs::path target = path_of(rel);
        const fs::path old = beside(target, L".old");
        const fs::path temp = beside(target, L".new");
        bool had = fs::exists(target, ec);
        if (had && !MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) had = false, ec = std::make_error_code(std::errc::permission_denied);
        if (ec || !MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            // put everything back as it was
            if (had) MoveFileExW(old.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
            for (const std::string& m : moved) {
                const fs::path t = path_of(m);
                MoveFileExW(beside(t, L".old").c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
            for (const std::string& w : written) fs::remove(beside(path_of(w), L".new"), ec);
            return ui::tr("The update could not be installed, and the program was left as it was.");
        }
        if (had) moved.push_back(rel);
    }
    return "";
}

bool restart() {
    wchar_t exe[MAX_PATH * 2] = {};
    if (!GetModuleFileNameW(nullptr, exe, DWORD(sizeof exe / sizeof exe[0]))) return false;
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    std::wstring command = L"\"" + std::wstring(exe) + L"\"";
    const bool ok = CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) != 0;
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return ok;
}

void cleanup() {
    const std::string dir = paths::exe_dir();
    if (dir.empty()) return;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(fs::u8path(dir), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (it.depth() > 4) it.disable_recursion_pending();
        if (it->is_regular_file(ec) && it->path().extension() == L".old") fs::remove(it->path(), ec);
    }
}

}  // namespace amg::updater
