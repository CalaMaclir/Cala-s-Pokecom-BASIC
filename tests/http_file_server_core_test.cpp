#include <cassert>
#include <cstring>
#include <iostream>
#include "http_file_server_core.hpp"

int main() {
    using namespace rmb::http;
    Request request; int status = 0;
    assert(parse_request("GET /api/file?name=TEST.BAS&k=7A31F2 HTTP/1.1\r\nHost: p\r\n\r\n", "7A31F2", request, status));
    assert(request.resource == Resource::File && !std::strcmp(request.filename, "TEST.BAS"));
    assert(!parse_request("DELETE /api/file?name=../BAD&k=7A31F2 HTTP/1.1\r\n\r\n", "7A31F2", request, status));
    assert(status == 400);
    assert(!parse_request("GET /api/files?k=BAD HTTP/1.1\r\n\r\n", "7A31F2", request, status));
    assert(status == 403);
    assert(parse_request("PUT /api/file?name=A.BMP&k=7A31F2 HTTP/1.1\r\nContent-Length: 1048576\r\n\r\n", "7A31F2", request, status));
    assert(request.method == Method::Put && request.content_length == 1048576);
    assert(!parse_request("PUT /api/file?name=A.BMP&k=7A31F2 HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", "7A31F2", request, status));
    assert(!parse_request("PUT /api/file?name=A.BMP&k=7A31F2 HTTP/1.1\r\nContent-Length: 999999999\r\n\r\n", "7A31F2", request, status));
    assert(status == 413);
    char escaped[64];
    assert(json_escape("A\"B\\C", escaped, sizeof(escaped)) != 0);
    assert(!std::strcmp(escaped, "A\\\"B\\\\C"));
    std::cout << "HTTP request, token, filename and JSON tests passed\n";
}
