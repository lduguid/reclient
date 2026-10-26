#include "App.hpp"
#include "Client.hpp"

#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace {
std::string field(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const size_t at = json.find(needle);
    if (at == std::string::npos) return {};
    const size_t colon = json.find(':', at);
    if (colon == std::string::npos) return {};
    size_t i = colon + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    if (i >= json.size()) return {};
    if (json[i] == '"') {
        std::string out;
        for (size_t n = i + 1; n < json.size(); ++n) {
            if (json[n] == '\\' && n + 1 < json.size()) {
                out.push_back(json[++n]);
                continue;
            }
            if (json[n] == '"') break;
            out.push_back(json[n]);
        }
        return out;
    }
    size_t end = i;
    while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ' ') ++end;
    return json.substr(i, end - i);
}

std::string commandFromJson(const std::string& json) {
    const std::string method = field(json, "method");
    if (method == "say") return "say " + field(json, "text");
    if (method == "move") return "move " + field(json, "x") + " " + field(json, "y");
    if (method == "enter") return "enter " + field(json, "name");
    if (method == "wait") return "wait " + field(json, "seconds");
    return method;
}
}  // namespace

int main(int argc, char** argv) {
    std::string configPath = "config.json";
    std::string scriptPath;
    bool serve = false;
    bool stdinMode = false;
    bool window = false;
    bool shot = false;
    bool diag = false;
    bool diagStrict = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--serve") serve = true;
        else if (arg == "--stdin") stdinMode = true;
        else if (arg == "--window") window = true;
        else if (arg == "--shot") shot = true;
        else if (arg == "--config" && i + 1 < argc) configPath = argv[++i];
        else if (arg == "--script" && i + 1 < argc) scriptPath = argv[++i];
        else if (arg == "--diag") diag = true;
        else if (arg == "--diag-strict") {
            diag = true;
            diagStrict = true;
        } else if (arg == "--help") {
            std::cout << "reclient --config config.json [--script commands.txt] [--serve] [--stdin] [--window] [--shot]\n"
                         "        [--diag] [--diag-strict]\n"
                         "F3 toggles diagnostics in the window. The log is diag.log. Strict mode stops on a failed check.\n";
            return 0;
        }
    }

    try {
        reclient::Config config = reclient::loadConfig(configPath);
        if (diag) config.diagnostics = true;
        if (diagStrict) config.diagnosticsStrict = true;
        if (window || shot) return reclient::runWindow(config, shot);
        std::vector<std::string> script;
        if (!scriptPath.empty()) {
            std::ifstream in(scriptPath);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line[0] != '#') script.push_back(line);
            }
        } else if (!serve && !stdinMode && !config.character.empty()) {
            script = {"wait 1", "wait 8", "status", "nearby", "pos", "quit"};
        }

        reclient::GameClient client(std::move(config));
        client.setLogger([](const std::string& line) { std::cout << line << std::endl; });

        std::atomic<bool> running{true};
        std::thread control([&] {
            if (!serve) return;
            try {
                reclient::Listener listener;
                listener.listen(client.controlPort());
                std::cout << "control 127.0.0.1:" << client.controlPort() << std::endl;
                while (running) {
                    reclient::Socket peer;
                    if (!listener.acceptInto(peer, 200)) continue;
                    std::string line;
                    uint8_t byte = 0;
                    while (peer.recvExact(&byte, 1, 5000)) {
                        if (byte == '\n') break;
                        if (byte != '\r') line.push_back(char(byte));
                    }
                    const std::string id = field(line, "id");
                    const std::string result = client.submit(commandFromJson(line));
                    std::string payload = "{\"id\":" + (id.empty() ? "0" : id) + ",\"result\":";
                    if (!result.empty() && (result.front() == '{' || result.front() == '[')) payload += result;
                    else {
                        payload += "\"";
                        for (char c : result) {
                            if (c == '"' || c == '\\') payload.push_back('\\');
                            payload.push_back(c);
                        }
                        payload += "\"";
                    }
                    payload += "}\n";
                    peer.sendAll(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
                }
            } catch (const std::exception& ex) {
                std::cerr << "control: " << ex.what() << std::endl;
            }
        });

        const int code = client.run(std::move(script), serve, stdinMode);
        running = false;
        control.join();
        return code;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << std::endl;
        return 1;
    }
}
