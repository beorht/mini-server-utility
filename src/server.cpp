#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <sys/stat.h>
#include <vector>
#include <ctime>
#include <unordered_map>
#include <mutex>
#include <cstring>
#include <cstdint>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <dirent.h>
#endif

// ─── SHA-1 ────────────────────────────────────────────────────────────────────

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_compress(uint32_t s[5], const uint8_t blk[64]) {
    uint32_t W[80];
    for (int i = 0; i < 16; i++)
        W[i] = ((uint32_t)blk[i*4]<<24)|((uint32_t)blk[i*4+1]<<16)|
               ((uint32_t)blk[i*4+2]<<8)|blk[i*4+3];
    for (int i = 16; i < 80; i++)
        W[i] = ROL32(W[i-3]^W[i-8]^W[i-14]^W[i-16], 1);
    uint32_t a=s[0],b=s[1],c=s[2],d=s[3],e=s[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if      (i < 20) { f=(b&c)|(~b&d);       k=0x5A827999u; }
        else if (i < 40) { f=b^c^d;               k=0x6ED9EBA1u; }
        else if (i < 60) { f=(b&c)|(b&d)|(c&d);  k=0x8F1BBCDCu; }
        else             { f=b^c^d;               k=0xCA62C1D6u; }
        uint32_t t=ROL32(a,5)+f+e+k+W[i];
        e=d; d=c; c=ROL32(b,30); b=a; a=t;
    }
    s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d; s[4]+=e;
}

static void sha1(const std::string& msg, uint8_t out[20]) {
    uint32_t s[5]={0x67452301u,0xEFCDAB89u,0x98BADCFEu,0x10325476u,0xC3D2E1F0u};
    std::vector<uint8_t> d(msg.begin(), msg.end());
    d.push_back(0x80);
    while (d.size() % 64 != 56) d.push_back(0);
    uint64_t bits = (uint64_t)msg.size() * 8;
    for (int i = 7; i >= 0; i--) d.push_back((bits >> (i*8)) & 0xff);
    for (size_t i = 0; i < d.size(); i += 64) sha1_compress(s, d.data()+i);
    for (int i = 0; i < 5; i++) {
        out[i*4+0]=(s[i]>>24)&0xff; out[i*4+1]=(s[i]>>16)&0xff;
        out[i*4+2]=(s[i]>>8)&0xff;  out[i*4+3]=s[i]&0xff;
    }
}

// ─── Base64 ──────────────────────────────────────────────────────────────────

static std::string base64_encode(const uint8_t* data, size_t len) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val=0, valb=-6;
    for (size_t i = 0; i < len; i++) {
        val = (val<<8)+data[i]; valb += 8;
        while (valb >= 0) { out.push_back(T[(val>>valb)&0x3F]); valb -= 6; }
    }
    if (valb > -6) out.push_back(T[((val<<8)>>(valb+8))&0x3F]);
    while (out.size()%4) out.push_back('=');
    return out;
}

// ─── Globals ─────────────────────────────────────────────────────────────────

int server_fd = -1;
std::atomic<bool> running(true);

std::unordered_map<std::string, std::string> text_cache;
std::unordered_map<std::string, time_t>      file_mod_times;
std::mutex cache_mutex;

std::vector<int> ws_clients;
std::mutex ws_mutex;

// ─── Logging ─────────────────────────────────────────────────────────────────

void log(int status, const std::string& path) {
    std::time_t now = std::time(nullptr);
    char ts[20];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::cout << "[" << ts << "] " << status << " /" << path << "\n";
}

// ─── File helpers ────────────────────────────────────────────────────────────

std::string load_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream buf; buf << file.rdbuf();
    return buf.str();
}

std::vector<char> load_binary_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buf(size);
    if (file.read(buf.data(), size)) return buf;
    return {};
}

std::string get_content_type(const std::string& path) {
    if (path.rfind(".html") == path.size()-5) return "text/html";
    if (path.rfind(".css")  == path.size()-4) return "text/css";
    if (path.rfind(".js")   == path.size()-3) return "application/javascript";
    if (path.rfind(".png")  == path.size()-4) return "image/png";
    if (path.rfind(".jpg")  == path.size()-4) return "image/jpeg";
    if (path.rfind(".jpeg") == path.size()-5) return "image/jpeg";
    if (path.rfind(".gif")  == path.size()-4) return "image/gif";
    if (path.rfind(".svg")  == path.size()-4) return "image/svg+xml";
    if (path.rfind(".ico")  == path.size()-4) return "image/x-icon";
    return "application/octet-stream";
}

time_t get_file_mod_time(const std::string& path) {
    struct stat result{};
    return (stat(path.c_str(), &result) == 0) ? result.st_mtime : 0;
}

bool is_watched_extension(const std::string& name) {
    auto ends = [&](const std::string& ext) {
        return name.size() >= ext.size() &&
               name.compare(name.size()-ext.size(), ext.size(), ext) == 0;
    };
    return ends(".html") || ends(".css") || ends(".js");
}

std::vector<std::string> scan_watched_files() {
    std::vector<std::string> files;
#ifdef _WIN32
    WIN32_FIND_DATA fd; HANDLE h = FindFirstFile("*", &fd);
    if (h == INVALID_HANDLE_VALUE) return files;
    do {
        std::string name(fd.cFileName);
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && is_watched_extension(name))
            files.push_back(name);
    } while (FindNextFile(h, &fd));
    FindClose(h);
#else
    DIR* dir = opendir(".");
    if (!dir) return files;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name(entry->d_name);
        if (is_watched_extension(name)) files.push_back(name);
    }
    closedir(dir);
#endif
    return files;
}

// ─── WebSocket ───────────────────────────────────────────────────────────────

static const std::string WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Скрипт, инжектируемый в конец каждого HTML-ответа
static const std::string WS_INJECT =
    "<script>(function(){"
    "function connect(){"
    "var ws=new WebSocket('ws://'+location.host+'/ws');"
    "ws.onmessage=function(){location.reload()};"
    "ws.onclose=function(){setTimeout(connect,1000)};"
    "}connect();"
    "})();</script>";

std::string ws_accept_key(const std::string& client_key) {
    uint8_t hash[20];
    sha1(client_key + WS_MAGIC, hash);
    return base64_encode(hash, 20);
}

bool is_ws_request(const std::string& req) {
    return req.find("Upgrade: websocket") != std::string::npos ||
           req.find("Upgrade: WebSocket") != std::string::npos;
}

std::string get_ws_key(const std::string& req) {
    const std::string hdr = "Sec-WebSocket-Key: ";
    size_t pos = req.find(hdr);
    if (pos == std::string::npos) return "";
    pos += hdr.size();
    size_t end = req.find("\r\n", pos);
    return req.substr(pos, end - pos);
}

void ws_send_frame(int fd, const std::string& msg) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81); // FIN=1, text
    if (msg.size() <= 125) {
        frame.push_back((uint8_t)msg.size());
    } else {
        frame.push_back(126);
        frame.push_back((msg.size() >> 8) & 0xff);
        frame.push_back(msg.size() & 0xff);
    }
    for (char c : msg) frame.push_back((uint8_t)c);
    send(fd, (char*)frame.data(), frame.size(), 0);
}

void ws_broadcast(const std::string& msg) {
    std::lock_guard<std::mutex> lock(ws_mutex);
    for (int fd : ws_clients) ws_send_frame(fd, msg);
}

void handle_ws_client(int fd) {
    uint8_t buf[256];
    while (running) {
        int n = recv(fd, (char*)buf, sizeof(buf), 0);
        if (n <= 0) break;
        uint8_t opcode = buf[0] & 0x0f;
        if (opcode == 0x8) break; // close frame
    }
    {
        std::lock_guard<std::mutex> lock(ws_mutex);
        ws_clients.erase(std::remove(ws_clients.begin(), ws_clients.end(), fd), ws_clients.end());
    }
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

// ─── File watcher ────────────────────────────────────────────────────────────

void watch_files() {
    while (running) {
        for (const auto& name : scan_watched_files()) {
            time_t mtime = get_file_mod_time(name);
            std::lock_guard<std::mutex> lock(cache_mutex);
            if (file_mod_times[name] != mtime) {
                file_mod_times[name] = mtime;
                text_cache[name] = load_file(name);
                std::cout << "[Reloaded] " << name << "\n";
                ws_broadcast("reload");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// ─── Request parsing ─────────────────────────────────────────────────────────

std::string get_request_path(const std::string& req) {
    size_t start = req.find("GET /") + 5;
    size_t end   = req.find(" HTTP/1.1");
    if (start == std::string::npos || end == std::string::npos) return "/";
    std::string path = req.substr(start, end - start);
    return path.empty() ? "/" : path;
}

void stop_server(int) {
    std::cout << "\nStopping server...\n";
    running = false;
#ifdef _WIN32
    if (server_fd != -1) closesocket(server_fd);
    WSACleanup();
#else
    if (server_fd != -1) close(server_fd);
#endif
    exit(0);
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { std::cerr << "WSAStartup failed\n"; return 1; }
#endif

    int port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
        if (port <= 0 || port > 65535) { std::cerr << "Invalid port: " << argv[1] << "\n"; return 1; }
    }

    signal(SIGINT, stop_server);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { std::cerr << "Socket creation error\n"; return 1; }

    int opt = 1;
#ifdef _WIN32
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) < 0) {
#else
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
#endif
        std::cerr << "setsockopt failed\n"; return 1;
    }

    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port        = htons(port);

    if (bind(server_fd, (sockaddr*)&address, sizeof(address)) < 0) { std::cerr << "Bind failed\n"; return 1; }
    if (listen(server_fd, 10) < 0) { std::cerr << "Listen failed\n"; return 1; }

    std::cout << "Server started: http://localhost:" << port << "\n";

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        for (const auto& name : scan_watched_files()) {
            text_cache[name]     = load_file(name);
            file_mod_times[name] = get_file_mod_time(name);
            std::cout << "[Loaded] " << name << "\n";
        }
    }

    std::thread(watch_files).detach();

    while (running) {
        int addrlen = sizeof(address);
        int client = accept(server_fd, (sockaddr*)&address, (socklen_t*)&addrlen);
        if (client < 0) continue;

        char buf[4096] = {0};
        recv(client, buf, sizeof(buf)-1, 0);
        std::string request(buf);

        // WebSocket upgrade
        if (is_ws_request(request)) {
            std::string key    = get_ws_key(request);
            std::string accept = ws_accept_key(key);
            std::string resp   = "HTTP/1.1 101 Switching Protocols\r\n"
                                 "Upgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
            send(client, resp.c_str(), resp.size(), 0);
            {
                std::lock_guard<std::mutex> lock(ws_mutex);
                ws_clients.push_back(client);
            }
            std::thread(handle_ws_client, client).detach();
            continue; // не закрываем клиент
        }

        // HTTP запрос
        std::string path = get_request_path(request);
        if (path == "/") path = "index.html";

        std::string        content_text;
        std::vector<char>  content_binary;
        bool is_binary = false;

        std::string content_type = get_content_type(path);
        if (content_type.rfind("text/") == 0 || content_type == "application/javascript") {
            std::lock_guard<std::mutex> lock(cache_mutex);
            auto it = text_cache.find(path);
            content_text = (it != text_cache.end()) ? it->second : load_file(path);
            // Инжектируем WS-скрипт в HTML
            if (content_type == "text/html" && !content_text.empty())
                content_text += WS_INJECT;
        } else {
            is_binary = true;
            content_binary = load_binary_file(path);
        }

        size_t content_size = is_binary ? content_binary.size() : content_text.size();

        if (content_size > 0) {
            std::stringstream ss;
            ss << "HTTP/1.1 200 OK\r\n"
               << "Content-Type: " << content_type << "\r\n"
               << "Content-Length: " << content_size << "\r\n"
               << "Connection: close\r\n\r\n";
            std::string header = ss.str();
            log(200, path);
            send(client, header.c_str(), header.size(), 0);
            if (is_binary)
                send(client, content_binary.data(), content_binary.size(), 0);
            else
                send(client, content_text.c_str(), content_text.size(), 0);
        } else {
            std::string body = "<h1>404 Not Found</h1>";
            std::stringstream ss;
            ss << "HTTP/1.1 404 Not Found\r\n"
               << "Content-Type: text/html\r\n"
               << "Content-Length: " << body.size() << "\r\n"
               << "Connection: close\r\n\r\n"
               << body;
            std::string resp = ss.str();
            log(404, path);
            send(client, resp.c_str(), resp.size(), 0);
        }

#ifdef _WIN32
        closesocket(client);
#else
        close(client);
#endif
    }

#ifdef _WIN32
    closesocket(server_fd);
    WSACleanup();
#else
    close(server_fd);
#endif
    return 0;
}
