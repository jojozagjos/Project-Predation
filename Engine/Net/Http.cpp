#include "Engine/Net/Http.h"

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    include <winhttp.h>
#endif

#include <string>
#include <vector>

namespace pred
{
namespace
{

#if defined(_WIN32)

std::wstring Widen(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

// Closes a WinHTTP handle however the function leaves.
struct Handle
{
    HINTERNET value = nullptr;
    ~Handle()
    {
        if (value != nullptr)
        {
            WinHttpCloseHandle(value);
        }
    }
};

std::string Describe(DWORD error)
{
    switch (error)
    {
    case ERROR_WINHTTP_TIMEOUT:
        return "no answer in time";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return "could not find the server's name (no internet, or the address is wrong)";
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return "could not connect to the server";
    case ERROR_WINHTTP_SECURE_FAILURE:
        return "the server's certificate was not accepted";
    default:
        return "network error " + std::to_string(error);
    }
}

HttpResult Perform(const std::string& method, const std::string& url, const std::string& body, int timeoutMs)
{
    HttpResult result;
    const std::wstring wideUrl = Widen(url);

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    // The query string is part of the request too; it is read into its own buffer and put back.
    wchar_t extra[1024] = {};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts))
    {
        result.error = "that is not a web address: " + url;
        return result;
    }
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    Handle session{WinHttpOpen(L"ProjectPredation", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0)};
    if (session.value == nullptr)
    {
        result.error = Describe(GetLastError());
        return result;
    }
    WinHttpSetTimeouts(session.value, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    Handle connection{WinHttpConnect(session.value, host, parts.nPort, 0)};
    if (connection.value == nullptr)
    {
        result.error = Describe(GetLastError());
        return result;
    }
    const std::wstring target = std::wstring(path) + extra;
    Handle request{WinHttpOpenRequest(connection.value, Widen(method).c_str(), target.c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      secure ? WINHTTP_FLAG_SECURE : 0)};
    if (request.value == nullptr)
    {
        result.error = Describe(GetLastError());
        return result;
    }
    const wchar_t* headers = L"Content-Type: application/json\r\n";
    const BOOL sent = WinHttpSendRequest(request.value, body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers,
                                         body.empty() ? 0 : static_cast<DWORD>(-1L),
                                         body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                         static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(request.value, nullptr))
    {
        result.error = Describe(GetLastError());
        return result;
    }

    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    result.status = static_cast<int>(status);

    // Read to the end, bounded: a lobby answer is a few hundred bytes, and a server that sends
    // megabytes is not one of ours.
    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available) || available == 0)
        {
            break;
        }
        std::vector<char> chunk(available);
        DWORD read = 0;
        if (!WinHttpReadData(request.value, chunk.data(), available, &read) || read == 0)
        {
            break;
        }
        result.body.append(chunk.data(), read);
        if (result.body.size() > 256 * 1024)
        {
            result.error = "the answer was far too long";
            result.status = 0;
            break;
        }
    }
    return result;
}

#else

HttpResult Perform(const std::string&, const std::string&, const std::string&, int)
{
    HttpResult result;
    result.error = "web requests are only built on Windows";
    return result;
}

#endif

} // namespace

std::future<HttpResult> HttpRequest(const std::string& method, const std::string& url, const std::string& body,
                                    int timeoutMs)
{
    return std::async(std::launch::async, Perform, method, url, body, timeoutMs);
}

} // namespace pred
