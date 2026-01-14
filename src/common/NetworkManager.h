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

// Packet Types
#define PACKET_TYPE_VIDEO 0
#define PACKET_TYPE_AUDIO 1

struct PacketHeader {
    uint32_t packetType; // 0=Video, 1=Audio
    uint32_t payloadSize;
    int32_t  cursorX;    // Ignored for Audio
    int32_t  cursorY;    // Ignored for Audio
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

    bool IsDataAvailable(int sock) {
        if (sock == -1) return false;
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval timeout = { 0, 0 };
        return select(0, &readSet, nullptr, nullptr, &timeout) > 0;
    }

    bool WaitForReceiver(int& outClientSocket) {
        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) return false;

        sockaddr_in serverAddr = {};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        serverAddr.sin_port = htons(STREAM_PORT);
        
        if (bind(listenSock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            closesocket(listenSock);
            return false;
        }
        listen(listenSock, 1);

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

    // UPDATED: Now accepts packetType
    void SendPacket(int clientSock, uint32_t type, const uint8_t* data, size_t size, int x = -1, int y = -1) {
        if (clientSock == INVALID_SOCKET) return;

        PacketHeader header;
        header.packetType  = htonl(type);
        header.payloadSize = htonl((uint32_t)size);
        header.cursorX     = htonl(x);
        header.cursorY     = htonl(y);
        
        send((SOCKET)clientSock, (char*)&header, sizeof(header), 0);
        
        int totalSent = 0;
        while (totalSent < (int)size) {
            int sent = send((SOCKET)clientSock, (const char*)(data + totalSent), (int)(size - totalSent), 0);
            if (sent <= 0) break;
            totalSent += sent;
        }
    }

    bool FindAndConnect(int& outServerSocket) {
        SOCKET udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        DWORD udpTimeout = 2000;
        setsockopt(udpSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&udpTimeout, sizeof(udpTimeout));
        sockaddr_in recvAddr = {};
        recvAddr.sin_family = AF_INET;
        recvAddr.sin_port = htons(DISCOVERY_PORT);
        recvAddr.sin_addr.s_addr = INADDR_ANY;
        if (bind(udpSock, (sockaddr*)&recvAddr, sizeof(recvAddr)) == SOCKET_ERROR) {
            closesocket(udpSock); return false;
        }

        char buffer[1024];
        sockaddr_in senderAddr;
        int senderLen = sizeof(senderAddr);
        int len = recvfrom(udpSock, buffer, sizeof(buffer), 0, (sockaddr*)&senderAddr, &senderLen);
        closesocket(udpSock);

        if (len > 0) {
            SOCKET tcpSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            senderAddr.sin_port = htons(STREAM_PORT);
            connect(tcpSock, (sockaddr*)&senderAddr, sizeof(senderAddr));
            BOOL nodelay = TRUE;
            setsockopt(tcpSock, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));
            outServerSocket = (int)tcpSock;
            return true;
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
        outHeader.packetType  = ntohl(outHeader.packetType);
        outHeader.payloadSize = ntohl(outHeader.payloadSize);
        outHeader.cursorX     = ntohl(outHeader.cursorX);
        outHeader.cursorY     = ntohl(outHeader.cursorY);
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