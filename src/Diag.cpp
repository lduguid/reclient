#include "Diag.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace reclient::diag {
namespace {

std::mutex gate;
bool on = false;
bool strictMode = false;
int failureCount = 0;
std::string path = "diag.log";
std::string last;
std::ofstream out;
std::unordered_map<std::string, uint64_t> lastFailMs;

uint64_t nowMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

std::string stamp() {
    const std::time_t wall = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_s(&local, &wall);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
    return buf;
}

void writeLine(const std::string& line) {
    if (!out.is_open()) {
        out.open(path, std::ios::app);
        if (!out) return;
    }
    out << stamp() << " " << line << "\n";
    out.flush();
}

}  // namespace

void configure(bool enabled, bool strict, std::string logPath) {
    std::lock_guard<std::mutex> lock(gate);
    strictMode = strict;
    if (!logPath.empty()) path = std::move(logPath);
    on = enabled;
    if (!on) return;
    writeLine(std::string("diagnostics on") + (strictMode ? " strict" : ""));
}

bool enabled() {
    std::lock_guard<std::mutex> lock(gate);
    return on;
}

bool strict() {
    std::lock_guard<std::mutex> lock(gate);
    return strictMode;
}

void setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(gate);
    if (on == enabled) return;
    on = enabled;
    writeLine(on ? "diagnostics on" : "diagnostics off");
}

bool toggle() {
    std::lock_guard<std::mutex> lock(gate);
    on = !on;
    if (on) {
        failureCount = 0;
        last.clear();
        lastFailMs.clear();
    }
    writeLine(on ? "diagnostics on" : "diagnostics off");
    return on;
}

void event(const char* objective, const std::string& detail) {
    std::lock_guard<std::mutex> lock(gate);
    if (!on) return;
    writeLine(std::string("ok ") + objective + " " + detail);
}

void check(const char* objective, bool ok, const std::string& detail) {
    bool abort = false;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(gate);
        if (!on || ok) return;
        const uint64_t now = nowMs();
        const auto seen = lastFailMs.find(objective);
        if (seen != lastFailMs.end() && now - seen->second < 2000) return;
        lastFailMs[objective] = now;
        ++failureCount;
        last = std::string(objective) + " " + detail;
        message = "FAIL " + last;
        writeLine(message);
        abort = strictMode;
    }
    if (abort) throw Failure(message);
}

int failures() {
    std::lock_guard<std::mutex> lock(gate);
    return failureCount;
}

std::string lastFailure() {
    std::lock_guard<std::mutex> lock(gate);
    return last;
}

std::string logPath() {
    std::lock_guard<std::mutex> lock(gate);
    return path;
}

}  // namespace reclient::diag
