#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <csignal>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

int server_fd = -1; // глобально, чтобы можно было закрыть в обработчике сигнала

std::string load_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void stop_server(int signum) {
    std::cout << "\nStopping server...\n";
#ifdef _WIN32
    if (server_fd != -1) closesocket(server_fd);
    WSACleanup();
#else
    if (server_fd != -1) close(server_fd);
#endif
    exit(0);
}

int main() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    signal(SIGINT, stop_server); // обработка Ctrl+C

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Socket creation error\n";
        return 1;
    }

    // Разрешаем повторное использование порта
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

    while (true) {
        int addrlen = sizeof(address);
        int client = accept(server_fd, (sockaddr*)&address, (socklen_t*)&addrlen);
        if (client < 0) continue;

        char buffer[4096] = {0};
        recv(client, buffer, sizeof(buffer), 0);

        std::string html = load_file("index.html");
        if (html.empty()) html = "<h1>index.html not found</h1>";

        std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: " + std::to_string(html.size()) + "\r\n"
            "Connection: close\r\n\r\n" +
            html;

        send(client, response.c_str(), response.size(), 0);

#ifdef _WIN32
        closesocket(client);
#else
        close(client);
#endif
    }

    // Никогда сюда не дойдём, но на всякий случай
#ifdef _WIN32
    closesocket(server_fd);
    WSACleanup();
#else
    close(server_fd);
#endif

    return 0;
}

