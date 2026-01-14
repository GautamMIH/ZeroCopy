#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")

#define DISCOVERY_PORT 8888
#define STREAM_PORT 8889

struct PacketHeader {
    uint32_t frameSize;
    int32_t  cursorX;
    int32_t  cursorY;
};

class NetworkManager {
public:
    NetworkManager() {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
    }

    ~NetworkManager() {
        if (socketFD != INVALID_SOCKET) closesocket(socketFD);
        WSACleanup();
    }

    // Check if socket has data waiting (Non-Blocking)
    bool IsDataAvailable(int sock) {
        if (sock == -1) return false;
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval timeout = { 0, 0 };
        return select(0, &readSet, nullptr, nullptr, &timeout) > 0;
    }

    // SENDER: Wait for Client (With Error Checking)
    bool WaitForReceiver(int& outClientSocket) {
        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) return false;

        sockaddr_in serverAddr = {};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        serverAddr.sin_port = htons(STREAM_PORT);
        
        if (bind(listenSock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            std::cerr << "[Network] Bind failed. Port " << STREAM_PORT << " in use." << std::endl;
            closesocket(listenSock);
            return false;
        }

        if (listen(listenSock, 1) == SOCKET_ERROR) {
            closesocket(listenSock);
            return false;
        }

        std::atomic<bool> searching = true;
        std::thread broadcaster([&]() {
            SOCKET udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            BOOL broadcast = TRUE;
            setsockopt(udpSock, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast));

            sockaddr_in broadcastAddr = {};
            broadcastAddr.sin_family = AF_INET;
            broadcastAddr.sin_port = htons(DISCOVERY_PORT);
            broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;
            const char* msg = "DISCOVER_DXGI_STREAM";
            
            while (searching) {
                sendto(udpSock, msg, (int)strlen(msg), 0, (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            closesocket(udpSock);
        });

        sockaddr_in clientAddr;
        int clientLen = sizeof(clientAddr);
        SOCKET client = accept(listenSock, (sockaddr*)&clientAddr, &clientLen);

        searching = false;
        if (broadcaster.joinable()) broadcaster.join();
        closesocket(listenSock);

        if (client == INVALID_SOCKET) return false;
        outClientSocket = (int)client;
        return true;
    }

    // SENDER: Send Data + Cursor
    void SendFrame(int clientSock, const uint8_t* data, size_t size, int x, int y) {
        if (clientSock == INVALID_SOCKET) return;

        PacketHeader header;
        header.frameSize = htonl((uint32_t)size);
        header.cursorX   = htonl(x);
        header.cursorY   = htonl(y);
        
        int sent = send((SOCKET)clientSock, (char*)&header, sizeof(header), 0);
        if (sent <= 0) return;

        int totalSent = 0;
        while (totalSent < (int)size) {
            sent = send((SOCKET)clientSock, (const char*)(data + totalSent), (int)(size - totalSent), 0);
            if (sent <= 0) break;
            totalSent += sent;
        }
    }

    // RECEIVER: Discovery + Connect (With Timeouts)
    bool FindAndConnect(int& outServerSocket) {
        SOCKET udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        DWORD udpTimeout = 2000;
        setsockopt(udpSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&udpTimeout, sizeof(udpTimeout));

        sockaddr_in recvAddr = {};
        recvAddr.sin_family = AF_INET;
        recvAddr.sin_port = htons(DISCOVERY_PORT);
        recvAddr.sin_addr.s_addr = INADDR_ANY;

        if (bind(udpSock, (sockaddr*)&recvAddr, sizeof(recvAddr)) == SOCKET_ERROR) {
            closesocket(udpSock);
            return false;
        }

        char buffer[1024];
        sockaddr_in senderAddr;
        int senderLen = sizeof(senderAddr);
        int len = recvfrom(udpSock, buffer, sizeof(buffer), 0, (sockaddr*)&senderAddr, &senderLen);
        closesocket(udpSock);

        if (len > 0) {
            std::string msg(buffer, len);
            if (msg.find("DISCOVER_DXGI_STREAM") != std::string::npos) {
                SOCKET tcpSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                senderAddr.sin_port = htons(STREAM_PORT);

                u_long mode = 1; // Non-blocking
                ioctlsocket(tcpSock, FIONBIO, &mode);
                connect(tcpSock, (sockaddr*)&senderAddr, sizeof(senderAddr));

                fd_set writeSet;
                FD_ZERO(&writeSet);
                FD_SET(tcpSock, &writeSet);
                timeval timeout = { 3, 0 }; // 3s Timeout

                if (select(0, NULL, &writeSet, NULL, &timeout) > 0) {
                    mode = 0; // Blocking
                    ioctlsocket(tcpSock, FIONBIO, &mode);
                    BOOL nodelay = TRUE;
                    setsockopt(tcpSock, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));
                    outServerSocket = (int)tcpSock;
                    return true;
                }
                closesocket(tcpSock);
            }
        }
        return false;
    }

    bool ReceiveHeader(int serverSock, PacketHeader& outHeader) {
        int bytesReceived = 0;
        char* ptr = (char*)&outHeader;
        while (bytesReceived < sizeof(PacketHeader)) {
            int ret = recv((SOCKET)serverSock, ptr + bytesReceived, sizeof(PacketHeader) - bytesReceived, 0);
            if (ret <= 0) return false;
            bytesReceived += ret;
        }
        outHeader.frameSize = ntohl(outHeader.frameSize);
        outHeader.cursorX   = ntohl(outHeader.cursorX);
        outHeader.cursorY   = ntohl(outHeader.cursorY);
        return true;
    }

    bool ReceiveBody(int serverSock, std::vector<uint8_t>& buffer, uint32_t size) {
        if (buffer.size() < size) buffer.resize(size);
        uint32_t totalRead = 0;
        while (totalRead < size) {
            int ret = recv((SOCKET)serverSock, (char*)buffer.data() + totalRead, size - totalRead, 0);
            if (ret <= 0) return false;
            totalRead += ret;
        }
        return true;
    }

private:
    SOCKET socketFD = INVALID_SOCKET;
};