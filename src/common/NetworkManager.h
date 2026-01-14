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
#define MAX_PACKET_SIZE 1000000 // 1MB buffer for jumbo frames

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

    // =============================================================
    // SENDER (HOST) LOGIC
    // =============================================================
    bool WaitForReceiver(int& outClientSocket) {
        // 1. Setup TCP Listener
        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) {
            std::cerr << "[Network] Error: Failed to create socket: " << WSAGetLastError() << std::endl;
            return false;
        }

        sockaddr_in serverAddr = {};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        serverAddr.sin_port = htons(STREAM_PORT);
        
        // --- ERROR CHECKING ADDED HERE ---
        if (bind(listenSock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            std::cerr << "[Network] CRITICAL ERROR: Bind failed on port " << STREAM_PORT 
                      << ". Is the app already running? Error: " << WSAGetLastError() << std::endl;
            closesocket(listenSock);
            return false;
        }

        if (listen(listenSock, 1) == SOCKET_ERROR) {
            std::cerr << "[Network] Listen failed. Error: " << WSAGetLastError() << std::endl;
            closesocket(listenSock);
            return false;
        }
        // ---------------------------------

        // 2. Start UDP Broadcast in a background thread
        std::atomic<bool> searching = true;
        std::thread broadcaster([&]() {
            SOCKET udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            BOOL broadcast = TRUE;
            setsockopt(udpSock, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast));

            sockaddr_in broadcastAddr = {};
            broadcastAddr.sin_family = AF_INET;
            broadcastAddr.sin_port = htons(DISCOVERY_PORT);
            broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST; // 255.255.255.255

            const char* msg = "DISCOVER_DXGI_STREAM";
            
            std::cout << "[Network] Broadcasting on UDP..." << std::endl;

            while (searching) {
                int sent = sendto(udpSock, msg, (int)strlen(msg), 0, (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));
                if (sent == SOCKET_ERROR) {
                    // Optional: Print error if broadcast fails (often fails if no network cable is plugged in)
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            closesocket(udpSock);
        });

        std::cout << "[Network] TCP Listener Ready on Port " << STREAM_PORT << ". Waiting for Receiver..." << std::endl;

        // 3. Accept TCP Connection (Blocking)
        sockaddr_in clientAddr;
        int clientLen = sizeof(clientAddr);
        SOCKET client = accept(listenSock, (sockaddr*)&clientAddr, &clientLen);

        searching = false; // Stop broadcasting
        if (broadcaster.joinable()) broadcaster.join();
        closesocket(listenSock); // Stop listening

        if (client == INVALID_SOCKET) {
             std::cerr << "[Network] Accept failed. Error: " << WSAGetLastError() << std::endl;
             return false;
        }

        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
        std::cout << "[Network] Connected to Receiver: " << clientIP << std::endl;

        outClientSocket = (int)client;
        return true;
    }
    void SendFrame(int clientSock, const uint8_t* data, size_t size) {
        if (clientSock == INVALID_SOCKET) return;

        // Simple Protocol: [4 bytes Size] + [Data]
        uint32_t packetSize = htonl((uint32_t)size); // Network byte order
        
        // Send Header
        int sent = send((SOCKET)clientSock, (char*)&packetSize, sizeof(packetSize), 0);
        if (sent <= 0) return;

        // Send Body
        int totalSent = 0;
        while (totalSent < size) {
            sent = send((SOCKET)clientSock, (const char*)(data + totalSent), (int)(size - totalSent), 0);
            if (sent <= 0) break; // Error or disconnect
            totalSent += sent;
        }
    }

    // =============================================================
    // RECEIVER (CLIENT) LOGIC
    // =============================================================
    bool FindAndConnect(int& outServerSocket) {
    std::cout << "[Network] Looking for streams..." << std::endl;

    // 1. Listen for UDP Broadcast
    SOCKET udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    
    // Set a 2-second timeout for UDP discovery so we don't hang forever
    DWORD udpTimeout = 2000;
    setsockopt(udpSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&udpTimeout, sizeof(udpTimeout));

    sockaddr_in recvAddr = {};
    recvAddr.sin_family = AF_INET;
    recvAddr.sin_port = htons(DISCOVERY_PORT);
    recvAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(udpSock, (sockaddr*)&recvAddr, sizeof(recvAddr)) == SOCKET_ERROR) {
        std::cerr << "[Network] UDP Bind failed (Port " << DISCOVERY_PORT << " in use)." << std::endl;
        closesocket(udpSock);
        return false;
    }

    char buffer[1024];
    sockaddr_in senderAddr;
    int senderLen = sizeof(senderAddr);

    // Blocking wait (with timeout)
    int len = recvfrom(udpSock, buffer, sizeof(buffer), 0, (sockaddr*)&senderAddr, &senderLen);
    closesocket(udpSock);

    if (len > 0) {
        std::string msg(buffer, len);
        if (msg.find("DISCOVER_DXGI_STREAM") != std::string::npos) {
            char hostIP[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &senderAddr.sin_addr, hostIP, INET_ADDRSTRLEN);
            std::cout << "[Network] Found Stream at " << hostIP << ". Connecting..." << std::endl;

            // 2. Connect via TCP with Non-Blocking Timeout
            SOCKET tcpSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            senderAddr.sin_port = htons(STREAM_PORT); // Switch to Stream Port

            // Set Non-Blocking Mode
            u_long mode = 1;
            ioctlsocket(tcpSock, FIONBIO, &mode);

            // Start connection (will return immediately with WSAEWOULDBLOCK)
            connect(tcpSock, (sockaddr*)&senderAddr, sizeof(senderAddr));

            // Use select() to wait for connection for max 3 seconds
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(tcpSock, &writeSet);

            timeval timeout;
            timeout.tv_sec = 3;  // 3 Second Timeout
            timeout.tv_usec = 0;

            int result = select(0, NULL, &writeSet, NULL, &timeout);

            if (result > 0) {
                // Connection successful!
                // Set back to Blocking Mode for data transfer
                mode = 0;
                ioctlsocket(tcpSock, FIONBIO, &mode);
                
                // Disable Nagle
                BOOL nodelay = TRUE;
                setsockopt(tcpSock, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));

                outServerSocket = (int)tcpSock;
                return true;
            } else {
                std::cerr << "[Network] Connection timed out! (Firewall blocked port 8889)" << std::endl;
                closesocket(tcpSock);
                return false;
            }
        }
    }
    std::cerr << "[Network] No streams found (Timeout)." << std::endl;
    return false;
}
    bool ReceiveHeader(int serverSock, uint32_t& outSize) {
        uint32_t netSize;
        int bytesReceived = 0;
        char* ptr = (char*)&netSize;
        
        // Read exactly 4 bytes
        while (bytesReceived < 4) {
            int ret = recv((SOCKET)serverSock, ptr + bytesReceived, 4 - bytesReceived, 0);
            if (ret <= 0) return false;
            bytesReceived += ret;
        }

        outSize = ntohl(netSize);
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

    // Check if data is waiting to be read (Non-blocking)
    bool IsDataAvailable(int sock) {
        if (sock == -1) return false;
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        
        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0; // Return immediately
        
        // Returns > 0 if data is waiting
        return select(0, &readSet, nullptr, nullptr, &timeout) > 0;
    }

private:
    SOCKET socketFD = INVALID_SOCKET;
};