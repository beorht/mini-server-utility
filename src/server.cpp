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

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

int server_fd = -1;
std::atomic<bool> running(true);

std::string html_cache;
std::string css_cache;
time_t html_mod_time = 0;
time_t css_mod_time = 0;

// Загрузка текстового файла
std::string load_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Загрузка бинарного файла
std::vector<char> load_binary_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(size);
    if (file.read(buffer.data(), size)) return buffer;
    return {};
}

// Определение Content-Type по расширению
std::string get_content_type(const std::string& path) {
    if (path.rfind(".html") == path.size() - 5) return "text/html";
    if (path.rfind(".css") == path.size() - 4) return "text/css";
    if (path.rfind(".js") == path.size() - 3) return "application/javascript";
    if (path.rfind(".png") == path.size() - 4) return "image/png";
    if (path.rfind(".jpg") == path.size() - 4) return "image/jpeg";
    if (path.rfind(".jpeg") == path.size() - 5) return "image/jpeg";
    if (path.rfind(".gif") == path.size() - 4) return "image/gif";
    if (path.rfind(".svg") == path.size() - 4) return "image/svg+xml";
    if (path.rfind(".ico") == path.size() - 4) return "image/x-icon";
    return "application/octet-stream";
}

std::string get_request_path(const std::string& request) {
    size_t start = request.find("GET /") + 5;
    size_t end = request.find(" HTTP/1.1");
    if (start == std::string::npos || end == std::string::npos) return "/";
    std::string path = request.substr(start, end - start);
    return path.empty() ? "/" : path;
}


time_t get_file_mod_time(const std::string& path) {
    struct stat result{};
    if (stat(path.c_str(), &result) == 0) return result.st_mtime;
    return 0;
}

void stop_server(int signum) {
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

// Поток для отслеживания изменений файлов
void watch_files() {
    while (running) {
        time_t new_html_time = get_file_mod_time("index.html");
        time_t new_css_time  = get_file_mod_time("style.css");

        if (new_html_time != html_mod_time) {
            html_mod_time = new_html_time;
            html_cache = load_file("index.html");
            std::cout << "[HTML] Reloaded\n";
        }
        if (new_css_time != css_mod_time) {
            css_mod_time = new_css_time;
            css_cache = load_file("style.css");
            std::cout << "[CSS] Reloaded\n";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

int main() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    signal(SIGINT, stop_server);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Socket creation error\n";
        return 1;
    }

    int opt = 1;
#ifdef _WIN32
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) < 0) {
#else
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
#endif
        std::cerr << "setsockopt failed\n";
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8080);

    if (bind(server_fd, (sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed\n";
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        std::cerr << "Listen failed\n";
        return 1;
    }

    std::cout << "Server started: http://localhost:8080\n";

    // Изначальная загрузка файлов
    html_cache = load_file("index.html");
    html_mod_time = get_file_mod_time("index.html");

    css_cache = load_file("style.css");
    css_mod_time = get_file_mod_time("style.css");

    // Запускаем поток для наблюдения изменений
    std::thread watcher(watch_files);
    watcher.detach();

    while (running) {
        int addrlen = sizeof(address);
        int client = accept(server_fd, (sockaddr*)&address, (socklen_t*)&addrlen);
        if (client < 0) continue;

        char buffer[4096] = {0};
        recv(client, buffer, sizeof(buffer) - 1, 0);

        std::string request(buffer);
        std::string path = get_request_path(request);
        if (path == "/") path = "index.html";


        std::string response;
        std::vector<char> content_binary;
        std::string content_text;

        bool is_binary = false;
        std::string content_type = get_content_type(path);
        if (content_type.rfind("text/") == 0) {
            if (path == "index.html") {
                content_text = html_cache;
            } else if (path == "style.css") {
                content_text = css_cache;
            } else {
                content_text = load_file(path);
            }
        } else {
            is_binary = true;
            content_binary = load_binary_file(path);
        }

        size_t content_size = is_binary ? content_binary.size() : content_text.size();

        if (content_size > 0) {
            std::stringstream ss;
            ss << "HTTP/1.1 200 OK\r\n";
            ss << "Content-Type: " << content_type << "\r\n";
            ss << "Content-Length: " << content_size << "\r\n";
            ss << "Connection: close\r\n\r\n";
            response = ss.str();
            
            send(client, response.c_str(), response.size(), 0);
            if(is_binary) {
                send(client, content_binary.data(), content_binary.size(), 0);
            } else {
                send(client, content_text.c_str(), content_text.size(), 0);
            }

        } else {
            std::string not_found_content = "<h1>404 Not Found</h1>";
            std::stringstream ss;
            ss << "HTTP/1.1 404 Not Found\r\n";
            ss << "Content-Type: text/html\r\n";
            ss << "Content-Length: " << not_found_content.size() << "\r\n";
            ss << "Connection: close\r\n\r\n";
            ss << not_found_content;
            response = ss.str();
            send(client, response.c_str(), response.size(), 0);
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


