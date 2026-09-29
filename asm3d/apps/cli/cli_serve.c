/*
 * ASM3D - cli_serve.c
 * `asm3d_cli serve [folder] [--port 8080] [--open] [--max-requests N]`:
 * a minimal static file server for the browser editor (build/web), so no
 * other web server is needed. Local use only: it binds to 127.0.0.1, serves
 * GET/HEAD for files inside the folder and nothing else.
 *
 * Prints one JSON line when it starts listening (or fails), then serves
 * until stopped with Ctrl+C (or after --max-requests, used by the tests).
 */
#include "../../engine/core/a3_base.h"
#include "../../engine/core/a3_string.h"
#include "../../engine/core/a3_format.h"
#include "../../engine/core/a3_memory.h"
#include "../../engine/platform/a3_platform.h"
#include <stdio.h>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET Sock;
#  define SOCK_BAD INVALID_SOCKET
#  define sock_close closesocket
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <signal.h>
typedef int Sock;
#  define SOCK_BAD (-1)
#  define sock_close close
#endif

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void say(const char *json) { fputs(json, stdout); fputc('\n', stdout); fflush(stdout); }

static const char *mime(const char *path) {
    const char *ext = a3_path_extension(path);
    if (a3_streq(ext, ".html")) return "text/html; charset=utf-8";
    if (a3_streq(ext, ".js") || a3_streq(ext, ".mjs")) return "text/javascript; charset=utf-8";
    if (a3_streq(ext, ".wasm")) return "application/wasm";
    if (a3_streq(ext, ".json") || a3_streq(ext, ".a3scene") || a3_streq(ext, ".a3proj")) return "application/json";
    if (a3_streq(ext, ".css")) return "text/css";
    if (a3_streq(ext, ".png")) return "image/png";
    if (a3_streq(ext, ".jpg") || a3_streq(ext, ".jpeg")) return "image/jpeg";
    if (a3_streq(ext, ".svg")) return "image/svg+xml";
    if (a3_streq(ext, ".wav")) return "audio/wav";
    return "application/octet-stream";
}

static void send_all(Sock s, const void *data, usize n) {
    const char *p = (const char *)data;
    while (n) {
        int k = (int)send(s, p, (int)(n > 65536 ? 65536 : n), 0);
        if (k <= 0) return;
        p += k;
        n -= (usize)k;
    }
}

static void reply(Sock s, int code, const char *status, const char *type, const void *body, usize len, b32 head) {
    char hdr[512];
    int n = (int)a3_snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
        code, status, type, (unsigned long long)len);
    send_all(s, hdr, (usize)n);
    if (!head && body && len) send_all(s, body, len);
}

/* percent-decoding of the request path; rejects anything outside the folder */
static b32 safe_path(const char *url, char *out, usize cap) {
    usize o = 0;
    for (const char *p = url; *p && *p != '?' && *p != '#' && o + 1 < cap; ++p) {
        char c = *p;
        if (c == '%' && hexv(p[1]) >= 0 && hexv(p[2]) >= 0) {
            int hi = hexv(p[1]), lo = hexv(p[2]);
            c = (char)(hi * 16 + lo);
            p += 2;
        }
        if (c == '\\' || c == 0) return 0;
        out[o++] = c;
    }
    out[o] = 0;
    if (out[0] != '/' || a3_strstr(out, "..")) return 0;
    return 1;
}

static void handle(Sock c, const char *root) {
    char req[4096];
    int n = (int)recv(c, req, sizeof(req) - 1, 0);
    if (n <= 0) return;
    req[n] = 0;
    b32 head = a3_str_starts_with(req, "HEAD ");
    if (!a3_str_starts_with(req, "GET ") && !head) { reply(c, 405, "Method Not Allowed", "text/plain", "method not allowed\n", 19, 0); return; }
    char url[1024];
    const char *u = req + (head ? 5 : 4);
    usize i = 0;
    while (u[i] && u[i] != ' ' && i + 1 < sizeof(url)) { url[i] = u[i]; ++i; }
    url[i] = 0;
    char rel[1024], full[2048];
    if (!safe_path(url, rel, sizeof(rel))) { reply(c, 400, "Bad Request", "text/plain", "bad path\n", 9, head); return; }
    if (rel[a3_strlen(rel) - 1] == '/') a3_strcat(rel, sizeof(rel), "index.html");
    a3_snprintf(full, sizeof(full), "%s%s", root, rel);
    if (a3_dir_exists(full)) { a3_strcat(full, sizeof(full), "/index.html"); }
    A3FileData fd;
    if (a3_file_read_all(full, A3_MEM_TEMP, &fd) != A3_OK) { reply(c, 404, "Not Found", "text/plain", "not found\n", 10, head); return; }
    reply(c, 200, "OK", mime(full), fd.data, fd.size, head);
    a3_free(fd.data);
}

int a3_cli_serve(const char *root, int port, b32 open_browser, int max_requests) {
    char json[1024];
    char dir[1024];
    a3_strcpy(dir, sizeof(dir), root && *root ? root : ".");
    usize dl = a3_strlen(dir);
    while (dl > 1 && (dir[dl - 1] == '/' || dir[dl - 1] == '\\')) dir[--dl] = 0;
    char index[1100];
    a3_snprintf(index, sizeof(index), "%s/index.html", dir);
    if (!a3_file_exists(index)) {
        a3_snprintf(json, sizeof(json), "{\"ok\":false,\"command\":\"serve\",\"error\":\"no index.html in %s\",\"hint\":\"Serve the browser editor folder: build/web (or browser-editor in the Windows zip).\",\"log\":[]}", dir);
        say(json);
        return 1;
    }
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { say("{\"ok\":false,\"command\":\"serve\",\"error\":\"could not start Windows sockets\",\"hint\":\"\",\"log\":[]}"); return 1; }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    Sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == SOCK_BAD) { say("{\"ok\":false,\"command\":\"serve\",\"error\":\"could not create a socket\",\"hint\":\"\",\"log\":[]}"); return 1; }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    struct sockaddr_in addr;
    a3_memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(s, 16) != 0) {
        a3_snprintf(json, sizeof(json), "{\"ok\":false,\"command\":\"serve\",\"error\":\"port %d is busy\",\"hint\":\"Pick another port with --port, e.g. --port 8123.\",\"log\":[]}", port);
        say(json);
        sock_close(s);
        return 1;
    }
    char url[64];
    a3_snprintf(url, sizeof(url), "http://localhost:%d/", port);
    a3_snprintf(json, sizeof(json), "{\"ok\":true,\"command\":\"serve\",\"result\":{\"url\":\"%s\",\"folder\":\"%s\",\"stop\":\"Ctrl+C\"},\"log\":[]}", url, dir);
    for (char *p = json; *p; ++p) if (*p == '\\') *p = '/';
    say(json);
    if (open_browser) {
#if defined(_WIN32)
        const char *av[] = { "cmd", "/c", "start", "", url, 0 };
#else
        const char *av[] = { "xdg-open", url, 0 };
#endif
        a3_process_spawn_detached(av);
    }
    for (int served = 0; max_requests <= 0 || served < max_requests; ++served) {
        Sock c = accept(s, 0, 0);
        if (c == SOCK_BAD) continue;
        handle(c, dir);
        sock_close(c);
    }
    sock_close(s);
    return 0;
}
