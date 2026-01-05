#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip> // For std::setw
#include <atomic>  // For thread-safe counters
#include <winsock2.h>
#include <ws2tcpip.h>
#include "video/DXGICapturer.h"
#include "video/HardwareEncoder.h"
// These tell the MSVC linker to include necessary Windows components
#pragma comment(lib, "ws2_32.lib")  // Winsock
#pragma comment(lib, "d3d11.lib")   // Direct3D 11
#pragma comment(lib, "dxgi.lib")    // DXGI
#pragma comment(lib, "winmm.lib")   // High Precision Timer


// Handles the TCP connection to the receiving computer

class NetworkSender {
    SOCKET sock = INVALID_SOCKET;
public:
    // Tries to connect to the target IP. Returns true if successful.
    bool Connect(const std::string& ip, int port) {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "[Network] WSAStartup Failed." << std::endl;
            return false;
        }

        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET) {
            std::cerr << "[Network] Socket creation failed." << std::endl;
            WSACleanup();
            return false;
        }

        // Set non-blocking mode optional, but keeping blocking for simplicity here
        sockaddr_in server;
        server.sin_family = AF_INET;
        server.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &server.sin_addr);

        std::cout << "[Network] Connecting to " << ip << ":" << port << "..." << std::endl;
        
        if (connect(sock, (sockaddr*)&server, sizeof(server)) != 0) {
            std::cerr << "[Network] Connection failed. (Is the receiver app running?)" << std::endl;
            closesocket(sock);
            sock = INVALID_SOCKET;
            WSACleanup();
            return false;
        }
        
        std::cout << "[Network] Connected!" << std::endl;
        
        // Optional: Disable Nagle's Algorithm for lower latency
        // int flag = 1;
        // setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));
        
        return true;
    }

    // Sends a simple packet: [4 bytes Size] + [N bytes Data]
    void SendPacket(const std::vector<uint8_t>& data) {
        if (sock == INVALID_SOCKET || data.empty()) return;

        // 1. Send Header (Size)
        int size = static_cast<int>(data.size());
        int sent = send(sock, (char*)&size, sizeof(int), 0);
        
        if (sent == SOCKET_ERROR) {
            std::cerr << "[Network] Send Error (Header)" << std::endl;
            return;
        }

        // 2. Send Body (Payload)
        // Note: For large packets, you might need a loop to ensure all bytes are sent.
        sent = send(sock, (char*)data.data(), size, 0);
        
        if (sent == SOCKET_ERROR) {
            std::cerr << "[Network] Send Error (Body)" << std::endl;
        }
    }

    ~NetworkSender() {
        if (sock != INVALID_SOCKET) closesocket(sock);
        WSACleanup();
    }
};


// MAIN APPLICATION
int main() {
    // 1. Configuration
    // CHANGE THIS IP to your second computer's IP address
    const std::string RECEIVER_IP = "127.0.0.1"; 
    const int RECEIVER_PORT = 8080;

    // 2. Instantiate Components
    DXGICapturer capturer;
    HardwareEncoder encoder;
    NetworkSender net;

    std::cout << "============================================" << std::endl;
    std::cout << "   ZeroCopy Software Capture Card (v1.0)    " << std::endl;
    std::cout << "============================================" << std::endl;

    // 3. Connect Network
    bool isOnline = net.Connect(RECEIVER_IP, RECEIVER_PORT);
    if (!isOnline) {
        std::cout << ">> OPERATING IN OFFLINE MODE (Capture & Encode Test Only) <<" << std::endl;
    }

    // 4. Initialize Capturer
    if (!capturer.Initialize()) {
        std::cerr << "[Main] Failed to initialize DXGI Capturer. Exiting." << std::endl;
        return -1;
    }

    // 5. Shared State for Stats
    bool encoderInitialized = false;
    std::atomic<int> framesProcessed = 0;
    std::atomic<long long> totalBytesSent = 0;
    auto lastReportTime = std::chrono::steady_clock::now();

    // 6. Define the Frame Callback
    // This lambda is called by DXGICapturer loop 60 times a second
    auto onFrameCallback = [&](ID3D11Texture2D* tex, ID3D11DeviceContext* ctx) {
        
        // A. Lazy Initialize the Hardware Encoder
        // We do this here because we need the texture to know the screen resolution
        if (!encoderInitialized) {
            D3D11_TEXTURE2D_DESC desc;
            tex->GetDesc(&desc);
            
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            ctx->GetDevice(&device);

            std::cout << "[Main] Initializing Hardware Encoder for " << desc.Width << "x" << desc.Height << "..." << std::endl;
            
            if (encoder.Initialize(device.Get(), desc.Width, desc.Height)) {
                encoderInitialized = true;
                std::cout << "[Main] Encoder Initialized Successfully." << std::endl;
            } else {
                std::cerr << "[Main] FATAL: Failed to initialize Hardware Encoder." << std::endl;
                exit(-1); // Stop the app
            }
        }

        // B. Encode the Frame
        if (encoderInitialized) {
            // This returns a compressed H.264 packet (std::vector<uint8_t>)
            // Ideally < 5ms latency
            std::vector<uint8_t> packet = encoder.EncodeFrame(tex, ctx);

            if (!packet.empty()) {
                // C. Send to Network
                if (isOnline) {
                    net.SendPacket(packet);
                }
                
                // Track Stats
                totalBytesSent += packet.size();
                framesProcessed++;
            }
        }

        // D. Print Stats (Once per second)
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = now - lastReportTime;

        if (elapsed.count() >= 1.0) {
            double fps = framesProcessed / elapsed.count();
            double mbps = (totalBytesSent * 8.0) / (1000.0 * 1000.0) / elapsed.count(); // Megabits per sec

            std::cout << "\rFPS: " << std::setw(4) << std::fixed << std::setprecision(1) << fps 
                      << " | Bitrate: " << std::setw(5) << std::setprecision(2) << mbps << " Mbps"
                      << " | Status: " << (isOnline ? "ONLINE " : "OFFLINE")
                      << "   " << std::flush;

            framesProcessed = 0;
            totalBytesSent = 0;
            lastReportTime = now;
        }
    };

    // 7. Start the Loop
    std::cout << "\nStarting Capture Loop (60 FPS)..." << std::endl;
    std::cout << "Press ENTER to stop.\n" << std::endl;
    
    capturer.Start(onFrameCallback);

    // 8. Keep Main Thread Alive
    std::cin.get();

    // 9. Cleanup
    std::cout << "\nStopping..." << std::endl;
    capturer.Stop();
    std::cout << "Goodbye." << std::endl;

    return 0;
}