#pragma once

#include "AuthCrypt.hpp"
#include "Socket.hpp"
#include "Warden.hpp"
#include "World.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace reclient {

struct Config {
    std::string host = "127.0.0.1";
    int authPort = 3724;
    std::string account;
    std::string password;
    std::string character;
    std::string clientPath;
    int controlPort = 8766;
    bool diagnostics = false;
    bool diagnosticsStrict = false;
};

Config loadConfig(const std::string& path);

// Headless 2.4.3 session. One calling thread runs run(); other threads may enqueue().
class GameClient {
public:
    explicit GameClient(Config config);

    using Logger = std::function<void(const std::string&)>;
    void setLogger(Logger logger) { logger_ = std::move(logger); }

    // Blocks until quit. script lines run in order; interactive and the control
    // port can enqueue more. "wait <seconds>" yields without blocking the socket.
    int run(std::vector<std::string> script, bool serve, bool stdinMode);

    // Windowed mode drives the same session one slice at a time.
    void connectSession();
    bool pump(int timeoutMs);

    struct View {
        bool connected = false;
        bool authed = false;
        bool inWorld = false;
        bool warden = false;
        std::string account;
        std::string error;
        std::string clientPath;
        uint32_t map = 0;
        Movement self;
        uint64_t selfGuid = 0;
        std::vector<Character> characters;
        std::vector<ChatLine> chat;
        std::vector<std::string> channels;
        GossipMenu gossip;
        std::vector<WowObject> objects;
    };
    View view() const;
    const std::unordered_map<uint64_t, WowObject>& objects() const { return objects_.all(); }

    void enqueue(std::string command);
    std::string submit(std::string command);
    std::string execute(const std::string& command);

    const std::vector<std::string>& discrepancies() const { return discrepancies_; }
    int controlPort() const { return config_.controlPort; }

private:
    void log(const std::string& line);
    void note(const std::string& line);
    void loginRealm();
    void connectWorld();
    void sendWorld(uint16_t opcode, const ByteBuffer& body);
    void onWarden(ByteBuffer& body);
    bool pollWorld(int timeoutMs);
    void onWorld(uint16_t opcode, ByteBuffer& body);
    void enterWorld(const std::string& name);
    void sendMove(uint16_t opcode);
    void requestNames();
    uint32_t nowMs() const;

    Config config_;
    Logger logger_;
    Socket auth_;
    Socket world_;
    AuthCrypt crypt_;
    WardenStream warden_;
    bool wardenSeen_ = false;
    bool wardenModuleRequested_ = false;
    size_t wardenModuleBytes_ = 0;
    std::string lastError_;
    ObjectStore objects_;
    std::vector<Character> characters_;
    std::vector<Realm> realms_;
    std::vector<ChatLine> chat_;
    uint32_t chatSeq_ = 0;
    std::vector<std::string> channels_;
    GossipMenu gossip_;
    std::vector<std::string> discrepancies_;
    std::vector<uint8_t> sessionKey_;
    Movement self_;
    uint64_t selfGuid_ = 0;
    uint32_t mapId_ = 0;
    uint32_t clientSeed_ = 0;
    bool authed_ = false;
    bool inWorld_ = false;
    bool entered_ = false;
    std::string enterName_;
    bool moverSent_ = false;
    bool walking_ = false;
    bool quit_ = false;
    int pendingWaitMs_ = 0;
    std::chrono::steady_clock::time_point lastBeat_{};
    std::chrono::steady_clock::time_point lastPing_{};
    std::chrono::steady_clock::time_point nextWait_{};
    std::vector<uint64_t> nameAsked_;
    std::vector<uint32_t> creatureAsked_;
    std::vector<uint32_t> objectAsked_;

    struct Job {
        std::string command;
        std::shared_ptr<std::promise<std::string>> reply;
    };
    std::mutex queueMu_;
    std::vector<Job> queue_;
};

}  // namespace reclient
