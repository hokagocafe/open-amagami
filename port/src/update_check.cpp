#include "update_check.hpp"

#include <windows.h>
#include <winhttp.h>

#include <array>
#include <cstdio>

#ifndef AMG_VERSION
#define AMG_VERSION "dev"
#endif

namespace amg::update_check {

namespace {

// "v1.2.3" as three numbers; false if it is not a release tag.
bool parse(const std::string& tag, std::array<int, 3>& out) {
    out = {0, 0, 0};
    return tag.size() > 1 && tag[0] == 'v' && std::sscanf(tag.c_str() + 1, "%d.%d.%d", &out[0], &out[1], &out[2]) >= 2;
}

}  // namespace

const char* current() { return AMG_VERSION; }

bool is_newer(const std::string& tag) {
    std::array<int, 3> mine, theirs;
    return parse(current(), mine) && parse(tag, theirs) && theirs > mine;
}

std::string latest() {
    std::array<int, 3> mine;
    if (!parse(current(), mine)) return "";  // a dev build has nothing to compare
    std::string body;
    HINTERNET session = WinHttpOpen(L"open-amagami", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", L"/repos/hokagocafe/open-amagami/releases/latest", nullptr,
                                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    if (request) {
        WinHttpSetTimeouts(request, 5000, 5000, 5000, 5000);
        if (WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, size = sizeof status;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                WINHTTP_NO_HEADER_INDEX);
            char chunk[4096];
            DWORD got = 0;
            while (status == 200 && body.size() < 512 * 1024 && WinHttpReadData(request, chunk, sizeof chunk, &got) && got > 0)
                body.append(chunk, got);
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    // "tag_name": "v0.1.4" — the one field needed, so no JSON library.
    const size_t key = body.find("\"tag_name\"");
    if (key == std::string::npos) return "";
    const size_t open = body.find('"', body.find(':', key) + 1);
    const size_t close = open == std::string::npos ? open : body.find('"', open + 1);
    if (close == std::string::npos) return "";
    const std::string tag = body.substr(open + 1, close - open - 1);
    std::array<int, 3> theirs;
    return parse(tag, theirs) ? tag : "";
}

}  // namespace amg::update_check
