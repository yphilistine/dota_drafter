#pragma comment(lib, "ws2_32.lib")

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#include <iostream>
#include <mutex>
#include <string>
#include <thread>

static const int PORT = 3000;

static std::string g_last_state;
static std::mutex  g_mutex;

std::string now_hms() {
    SYSTEMTIME st; GetLocalTime(&st);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string json_get(const std::string& src, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = src.find(needle);
    if (pos == std::string::npos) return "";
    pos = src.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    while (++pos < src.size() && (src[pos] == ' ' || src[pos] == '\t'));
    if (pos >= src.size()) return "";
    if (src[pos] == '"') {
        pos++;
        std::string val;
        while (pos < src.size() && src[pos] != '"') {
            if (src[pos] == '\\') pos++;
            if (pos < src.size()) val += src[pos++];
        }
        return val;
    }
    auto end = src.find_first_of(",}\n]", pos);
    std::string val = src.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    while (!val.empty() && (val.back() == ' ' || val.back() == '\r')) val.pop_back();
    return val;
}

void handle_gsi(const std::string& body) {
    std::string state = json_get(body, "game_state");
    if (state.empty()) return;

    std::string prev;
    { std::lock_guard<std::mutex> lk(g_mutex); prev = g_last_state; g_last_state = state; }
    if (state == prev) return;

    if (state == "DOTA_GAMERULES_STATE_HERO_SELECTION")
        std::cout << "[" << now_hms() << "] >> HERO SELECTION\n";
    else if (state == "DOTA_GAMERULES_STATE_STRATEGY_TIME")
        std::cout << "[" << now_hms() << "] >> STRATEGY TIME\n";
}

void client_thread(SOCKET client) {
    std::string request;
    char buf[8192];
    int n;
    do {
        n = recv(client, buf, sizeof(buf), 0);
        if (n > 0) request.append(buf, n);
    } while (n == (int)sizeof(buf));

    if (!request.empty()) {
        auto sep = request.find("\r\n\r\n");
        if (sep != std::string::npos && request.substr(0, 4) == "POST")
            handle_gsi(request.substr(sep + 4));
    }

    const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
    send(client, resp, (int)strlen(resp), 0);
    closesocket(client);
}

int main() {
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(server, (sockaddr*)&addr, sizeof(addr));
    listen(server, SOMAXCONN);

    std::cout << "[GSI] Listening on port " << PORT << "\n";

    while (true) {
        SOCKET client = accept(server, nullptr, nullptr);
        if (client != INVALID_SOCKET)
            std::thread(client_thread, client).detach();
    }
}
