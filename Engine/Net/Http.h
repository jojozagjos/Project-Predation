#pragma once

#include <future>
#include <string>

namespace pred
{

// One web request, off the game thread.
//
// The lobby server is a small web service (a Cloudflare Worker), so the game needs to ask it things
// over HTTPS. That happens on a thread of its own because a request can take a second, or many seconds
// when the network is down, and the frame cannot wait for it. The answer arrives through the future.
//
// Windows' own WinHTTP does the work -- it comes with Windows, handles HTTPS and proxies, and adds
// nothing to download or ship. On other systems every request fails with a message saying so.
struct HttpResult
{
    int status = 0;    // 0 when there was no answer at all
    std::string body;
    std::string error; // why there was no answer, in words
};

// `method` is "GET" or "POST"; a POST body is sent as JSON.
std::future<HttpResult> HttpRequest(const std::string& method, const std::string& url, const std::string& body = {},
                                    int timeoutMs = 8000);

} // namespace pred
