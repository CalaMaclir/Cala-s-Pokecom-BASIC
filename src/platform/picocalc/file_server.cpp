#include "program_file_guard.hpp"
#include "file_server.hpp"

#include "http_file_server_core.hpp"
#include "network.hpp"
#include "safe_file.hpp"
#include "storage.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "pico/stdlib.h"

namespace rmb::network {
namespace {

constexpr u16_t kPort = 80;
constexpr std::size_t kHeaderLimit = 2048;
constexpr std::size_t kIoSize = 2048;
constexpr std::uint32_t kClientTimeoutMs = 30000;

const char kWebUi[] = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>PicoCalc SD File Manager</title><style>body{font:16px system-ui;margin:auto;max-width:760px;padding:18px;background:#eef4fa;color:#123}h1{font-size:1.5rem}button,input{font:inherit;padding:.55rem;margin:.2rem}table{width:100%;border-collapse:collapse;background:white}td,th{padding:.55rem;border-bottom:1px solid #ccd;text-align:left}.actions{white-space:nowrap}.status{min-height:1.5em;color:#245}@media(max-width:520px){body{padding:10px}th:nth-child(2),td:nth-child(2){display:none}.actions button{display:block;width:100%}}</style></head><body><h1>PicoCalc SD File Manager</h1><p><input id="pick" type="file" multiple><button id="upload">Upload</button><button id="refresh">Refresh</button></p><p class="status" id="status"></p><table><thead><tr><th>File</th><th>Size</th><th>Actions</th></tr></thead><tbody id="files"></tbody></table><script>'use strict';const k=new URLSearchParams(location.search).get('k')||'';const api=(p,n='')=>p+'?k='+encodeURIComponent(k)+(n?'&name='+encodeURIComponent(n):'');const status=t=>document.querySelector('#status').textContent=t;const size=n=>n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':(n/1048576).toFixed(1)+' MB';async function refresh(){status('Loading...');const r=await fetch(api('/api/files'),{cache:'no-store'});if(!r.ok)throw Error(await r.text());const list=await r.json(),body=document.querySelector('#files');body.textContent='';for(const f of list){const tr=document.createElement('tr'),name=document.createElement('td'),bytes=document.createElement('td'),actions=document.createElement('td'),download=document.createElement('button'),del=document.createElement('button');name.textContent=f.name;bytes.textContent=size(f.size);actions.className='actions';download.textContent='Download';download.onclick=()=>location.href=api('/api/file',f.name);del.textContent='Delete';del.onclick=async()=>{if(confirm('Delete '+f.name+'?')){const x=await fetch(api('/api/file',f.name),{method:'DELETE'});if(!x.ok)throw Error(await x.text());await refresh()}};actions.append(download,del);tr.append(name,bytes,actions);body.append(tr)}status(list.length+' file(s)')}document.querySelector('#refresh').onclick=()=>refresh().catch(e=>status('Error: '+e.message));document.querySelector('#upload').onclick=async()=>{try{const files=document.querySelector('#pick').files;for(const f of files){status('Uploading '+f.name+'...');const r=await fetch(api('/api/file',f.name),{method:'PUT',headers:{'Content-Type':'application/octet-stream'},body:f});if(!r.ok)throw Error(await r.text())}await refresh()}catch(e){status('Error: '+e.message)}};refresh().catch(e=>status('Error: '+e.message));</script></body></html>)HTML";

enum class Operation { Headers, Static, List, Download, Upload };

struct State {
    tcp_pcb* listener = nullptr;
    tcp_pcb* client = nullptr;
    pbuf* incoming = nullptr;
    volatile bool peer_closed = false;
    volatile bool connection_error = false;
    bool running = false;
    bool storage_held = false;
    char token[7] = {};
    char error[96] = "STOPPED";
    std::uint32_t last_activity = 0;

    Operation operation = Operation::Headers;
    char request_header[kHeaderLimit + 1] = {};
    std::size_t request_length = 0;
    http::Request request;

    char response_header[512] = {};
    std::size_t response_header_length = 0;
    std::size_t response_header_sent = 0;
    char response_body[384] = {};
    const char* static_body = nullptr;
    std::size_t static_length = 0;
    std::size_t static_sent = 0;

    FILE* download = nullptr;
    std::uint32_t download_remaining = 0;
    DIR* directory = nullptr;
    bool list_started = false;
    bool list_complete = false;
    SafeFileWriter upload;
    std::uint32_t upload_received = 0;
    std::uint8_t io[kIoSize] = {};
    std::size_t io_length = 0;
    std::size_t io_sent = 0;
};

State server;

std::uint32_t now_ms() {
    return to_ms_since_boot(get_absolute_time());
}

const char* reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Busy";
        default: return "Error";
    }
}

void release_storage() {
    if (server.storage_held) {
        storage::unlock();
        server.storage_held = false;
    }
}

void cleanup_transaction() {
    if (server.download) { std::fclose(server.download); server.download = nullptr; }
    if (server.directory) { closedir(server.directory); server.directory = nullptr; }
    server.upload.abort();
    release_storage();
    server.operation = Operation::Headers;
    server.request_length = 0;
    server.response_header_length = server.response_header_sent = 0;
    server.static_body = nullptr;
    server.static_length = server.static_sent = 0;
    server.io_length = server.io_sent = 0;
    server.upload_received = 0;
}

void prepare_header(int status, const char* type, long length = -1, const char* extra = "") {
    if (length >= 0) {
        server.response_header_length = std::snprintf(
            server.response_header, sizeof(server.response_header),
            "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\n%sConnection: close\r\nCache-Control: no-store\r\n\r\n",
            status, reason(status), type, length, extra);
    } else {
        server.response_header_length = std::snprintf(
            server.response_header, sizeof(server.response_header),
            "HTTP/1.0 %d %s\r\nContent-Type: %s\r\n%sConnection: close\r\nCache-Control: no-store\r\n\r\n",
            status, reason(status), type, extra);
    }
    server.response_header_sent = 0;
}

void prepare_static(int status, const char* type, const char* body, std::size_t length) {
    prepare_header(status, type, static_cast<long>(length));
    server.static_body = body;
    server.static_length = length;
    server.static_sent = 0;
    server.operation = Operation::Static;
}

void prepare_error(int status, const char* detail = nullptr) {
    cleanup_transaction();
    std::snprintf(server.response_body, sizeof(server.response_body), "%d %s%s%s\n",
        status, reason(status), detail ? ": " : "", detail ? detail : "");
    prepare_static(status, "text/plain; charset=utf-8", server.response_body,
                   std::strlen(server.response_body));
}

bool take_storage() {
    if (!storage::init()) { prepare_error(500, storage::last_error()); return false; }
    if (!storage::try_lock()) { prepare_error(503, "STORAGE BUSY"); return false; }
    server.storage_held = true;
    return true;
}

void begin_request() {
    int status = 400;
    server.request = {};
    if (!http::parse_request(server.request_header, server.token, server.request, status)) {
        prepare_error(status);
        return;
    }
    using http::Method;
    using http::Resource;
    if (server.request.resource == Resource::Index) {
        prepare_static(200, "text/html; charset=utf-8", kWebUi, sizeof(kWebUi) - 1);
        return;
    }
    if (server.request.resource == Resource::Files) {
        if (!take_storage()) return;
        server.directory = opendir("/");
        if (!server.directory) { prepare_error(500, "CANNOT OPEN SD DIRECTORY"); return; }
        prepare_header(200, "application/json; charset=utf-8");
        server.operation = Operation::List;
        server.list_started = false;
        server.list_complete = false;
        return;
    }

    if (program_files::reserved(server.request.filename) ||
        (server.request.method != Method::Get && program_files::in_use(server.request.filename))) {
        prepare_error(409, "FILE IS OPEN BY BASIC"); return;
    }
    char path[96] = {};
    std::snprintf(path, sizeof(path), "/%s", server.request.filename);
    if (server.request.method == Method::Put) {
        if (!take_storage()) return;
        if (!server.upload.open(server.request.filename, "HTTP")) {
            prepare_error(std::strstr(server.upload.error(), "EXISTS") ? 409 : 500,
                          server.upload.error());
            return;
        }
        server.operation = Operation::Upload;
        if (server.request.content_length == 0) {
            if (!server.upload.commit()) { prepare_error(500, server.upload.error()); return; }
            release_storage();
            prepare_static(201, "text/plain", "CREATED\n", 8);
        }
        return;
    }
    if (!take_storage()) return;
    struct stat value;
    if (stat(path, &value) != 0 || !S_ISREG(value.st_mode)) {
        prepare_error(404);
        return;
    }
    if (server.request.method == Method::Delete) {
        if (std::remove(path) != 0) { prepare_error(500, "DELETE FAILED"); return; }
        release_storage();
        prepare_static(204, "text/plain", "", 0);
        return;
    }
    server.download = std::fopen(path, "rb");
    if (!server.download) { prepare_error(500, "SD READ ERROR"); return; }
    server.download_remaining = static_cast<std::uint32_t>(value.st_size);
    char disposition[160] = {};
    std::snprintf(disposition, sizeof(disposition),
        "Content-Disposition: attachment; filename=\"%s\"\r\n", server.request.filename);
    prepare_header(200, "application/octet-stream", value.st_size, disposition);
    server.operation = Operation::Download;
}

bool flush_upload_buffer() {
    if (!server.io_length) return true;
    if (!server.upload.write(server.io, server.io_length)) return false;
    server.io_length = 0;
    return true;
}

void consume(const std::uint8_t* data, std::size_t length) {
    for (std::size_t i = 0; i < length; ++i) {
        if (server.operation == Operation::Headers) {
            if (server.request_length >= kHeaderLimit) { prepare_error(400, "HEADERS TOO LARGE"); return; }
            server.request_header[server.request_length++] = static_cast<char>(data[i]);
            server.request_header[server.request_length] = '\0';
            if (server.request_length >= 4 &&
                !std::memcmp(server.request_header + server.request_length - 4, "\r\n\r\n", 4))
                begin_request();
        } else if (server.operation == Operation::Upload) {
            if (server.upload_received >= server.request.content_length) {
                prepare_error(400, "BODY TOO LONG");
                return;
            }
            server.io[server.io_length++] = data[i];
            ++server.upload_received;
            if (server.io_length == sizeof(server.io) && !flush_upload_buffer()) {
                prepare_error(500, server.upload.error());
                return;
            }
            if (server.upload_received == server.request.content_length) {
                if (!flush_upload_buffer() || !server.upload.commit()) {
                    prepare_error(500, server.upload.error());
                    return;
                }
                release_storage();
                prepare_static(201, "text/plain", "CREATED\n", 8);
            }
        }
    }
}

err_t received(void*, tcp_pcb*, pbuf* packet, err_t error) {
    if (error != ERR_OK) return error;
    if (!packet) { server.peer_closed = true; return ERR_OK; }
    if (server.incoming) pbuf_cat(server.incoming, packet);
    else server.incoming = packet;
    server.last_activity = now_ms();
    return ERR_OK;
}

void connection_failed(void*, err_t) {
    server.client = nullptr;
    server.connection_error = true;
}

err_t accepted(void*, tcp_pcb* client, err_t error) {
    if (error != ERR_OK || !client) return ERR_VAL;
    if (server.client) {
        static const char busy[] =
            "HTTP/1.0 503 Busy\r\nContent-Type: text/plain\r\n"
            "Content-Length: 9\r\nConnection: close\r\n\r\n503 BUSY\n";
        if (tcp_write(client, busy, sizeof(busy) - 1, TCP_WRITE_FLAG_COPY) == ERR_OK) {
            tcp_output(client);
            if (tcp_close(client) == ERR_OK) return ERR_OK;
        }
        tcp_abort(client);
        return ERR_ABRT;
    }
    server.client = client;
    server.peer_closed = false;
    server.connection_error = false;
    server.last_activity = now_ms();
    server.operation = Operation::Headers;
    server.request_length = 0;
    tcp_arg(client, nullptr);
    tcp_recv(client, received);
    tcp_err(client, connection_failed);
    tcp_nagle_disable(client);
    return ERR_OK;
}

bool send_bytes(const void* data, std::size_t length, std::size_t& sent) {
    if (!server.client || sent >= length) return sent >= length;
    cyw43_arch_lwip_begin();
    if (!server.client) { cyw43_arch_lwip_end(); return false; }
    const u16_t room = tcp_sndbuf(server.client);
    const u16_t amount = static_cast<u16_t>(std::min<std::size_t>(length - sent, room));
    err_t result = ERR_MEM;
    if (amount) {
        result = tcp_write(server.client,
            static_cast<const std::uint8_t*>(data) + sent, amount, TCP_WRITE_FLAG_COPY);
        if (result == ERR_OK) { sent += amount; tcp_output(server.client); }
    }
    cyw43_arch_lwip_end();
    return sent >= length;
}

bool generate_list_chunk() {
    if (!server.list_started) {
        server.io[0] = '['; server.io_length = 1; server.io_sent = 0;
        server.list_started = true;
        return true;
    }
    while (dirent* entry = readdir(server.directory)) {
        if (program_files::reserved(entry->d_name)) continue;
        if (!SafeFileWriter::valid_root_name(entry->d_name)) continue;
        char path[96] = {}, escaped[180] = {};
        std::snprintf(path, sizeof(path), "/%s", entry->d_name);
        struct stat value;
        if (stat(path, &value) != 0 || !S_ISREG(value.st_mode)) continue;
        if (!http::json_escape(entry->d_name, escaped, sizeof(escaped))) continue;
        const int n = std::snprintf(reinterpret_cast<char*>(server.io), sizeof(server.io),
            "%s{\"name\":\"%s\",\"size\":%lu}",
            server.list_complete ? "," : "", escaped,
            static_cast<unsigned long>(value.st_size));
        server.list_complete = true;
        server.io_length = static_cast<std::size_t>(n);
        server.io_sent = 0;
        return true;
    }
    closedir(server.directory); server.directory = nullptr;
    server.io[0] = ']'; server.io_length = 1; server.io_sent = 0;
    server.operation = Operation::Static;
    server.static_body = nullptr;
    server.static_length = server.static_sent = 0;
    release_storage();
    return true;
}

void close_client() {
    cleanup_transaction();
    cyw43_arch_lwip_begin();
    if (server.client) {
        tcp_arg(server.client, nullptr);
        tcp_recv(server.client, nullptr);
        tcp_err(server.client, nullptr);
        if (tcp_close(server.client) != ERR_OK) tcp_abort(server.client);
        server.client = nullptr;
    }
    cyw43_arch_lwip_end();
}

void drive_output() {
    if (!server.client || server.operation == Operation::Headers || server.operation == Operation::Upload) return;
    if (!send_bytes(server.response_header, server.response_header_length,
                    server.response_header_sent)) return;

    if (server.io_sent < server.io_length) {
        if (!send_bytes(server.io, server.io_length, server.io_sent)) return;
        server.io_length = server.io_sent = 0;
    }
    if (server.operation == Operation::Static) {
        if (server.static_body &&
            !send_bytes(server.static_body, server.static_length, server.static_sent)) return;
        close_client();
        return;
    }
    if (server.operation == Operation::List) {
        generate_list_chunk();
        return;
    }
    if (server.operation == Operation::Download) {
        if (!server.download_remaining) { close_client(); return; }
        const std::size_t wanted = std::min<std::size_t>(sizeof(server.io), server.download_remaining);
        server.io_length = std::fread(server.io, 1, wanted, server.download);
        server.io_sent = 0;
        if (!server.io_length) { prepare_error(500, "SD READ ERROR"); return; }
        server.download_remaining -= server.io_length;
    }
}

} // namespace

bool file_server_start() {
    if (server.running) return true;
    if (!connected()) { std::snprintf(server.error, sizeof(server.error), "WIFI NOT CONNECTED"); return false; }
    if (!storage::init()) { std::snprintf(server.error, sizeof(server.error), "%s", storage::last_error()); return false; }
    cleanup_transaction();
    std::snprintf(server.token, sizeof(server.token), "%06lX",
                  static_cast<unsigned long>(get_rand_32() & 0xffffffu));
    cyw43_arch_lwip_begin();
    server.listener = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!server.listener || tcp_bind(server.listener, IP_ADDR_ANY, kPort) != ERR_OK) {
        if (server.listener) { tcp_abort(server.listener); server.listener = nullptr; }
        cyw43_arch_lwip_end();
        std::snprintf(server.error, sizeof(server.error), "PORT 80 UNAVAILABLE");
        server.token[0] = '\0';
        return false;
    }
    server.listener = tcp_listen(server.listener);
    if (!server.listener) {
        cyw43_arch_lwip_end();
        std::snprintf(server.error, sizeof(server.error), "HTTP LISTEN FAILED");
        server.token[0] = '\0';
        return false;
    }
    tcp_accept(server.listener, accepted);
    cyw43_arch_lwip_end();
    server.running = true;
    std::snprintf(server.error, sizeof(server.error), "RUNNING");
    return true;
}

void file_server_stop() {
    if (!server.running && !server.listener && !server.client) return;
    cyw43_arch_lwip_begin();
    if (server.client) {
        tcp_arg(server.client, nullptr); tcp_recv(server.client, nullptr); tcp_err(server.client, nullptr);
        tcp_abort(server.client); server.client = nullptr;
    }
    if (server.listener) { tcp_accept(server.listener, nullptr); tcp_close(server.listener); server.listener = nullptr; }
    pbuf* pending = server.incoming; server.incoming = nullptr;
    if (pending) pbuf_free(pending);
    cyw43_arch_lwip_end();
    cleanup_transaction();
    server.running = false;
    server.token[0] = '\0';
    std::snprintf(server.error, sizeof(server.error), "STOPPED");
}

void file_server_poll() {
    if (!server.running) return;
    if (!connected()) { file_server_stop(); return; }
    cyw43_arch_lwip_begin();
    pbuf* packets = server.incoming; server.incoming = nullptr;
    const bool failed = server.connection_error;
    server.connection_error = false;
    cyw43_arch_lwip_end();
    if (failed) cleanup_transaction();
    if (packets) {
        const u16_t received_bytes = packets->tot_len;
        if (!failed) {
            for (pbuf* part = packets; part; part = part->next)
                consume(static_cast<const std::uint8_t*>(part->payload), part->len);
        }
        cyw43_arch_lwip_begin();
        if (server.client) tcp_recved(server.client, received_bytes);
        pbuf_free(packets);
        cyw43_arch_lwip_end();
    }
    if (server.client && now_ms() - server.last_activity > kClientTimeoutMs &&
        (server.operation == Operation::Headers || server.operation == Operation::Upload))
        prepare_error(408);
    if (server.peer_closed &&
        (server.operation == Operation::Headers || server.operation == Operation::Upload))
        prepare_error(400, "CONNECTION CLOSED");
    drive_output();
}

bool file_server_running() { return server.running; }
const char* file_server_token() { return server.token; }
const char* file_server_last_error() { return server.error; }

} // namespace rmb::network
