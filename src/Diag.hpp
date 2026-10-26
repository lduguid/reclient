#pragma once

#include <stdexcept>
#include <string>

namespace reclient::diag {

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Off until configure() or toggle(). While off, event() and check() do nothing.
void configure(bool enabled, bool strict, std::string logPath);
bool enabled();
bool strict();
bool toggle();
void setEnabled(bool on);

// One line in the log. Ignored while diagnostics are off.
void event(const char* objective, const std::string& detail);

// Records a pass silently. A failure is logged, shown on the HUD, and thrown
// as Failure when strict mode is on. The same objective is logged at most once
// every two seconds.
void check(const char* objective, bool ok, const std::string& detail);

int failures();
std::string lastFailure();
std::string logPath();

}  // namespace reclient::diag
