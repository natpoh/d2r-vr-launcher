// process.h - "is the game running": an interface, a Windows implementation and a stand-in for tests.
#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "util.h"

namespace vrml {

class ProcessProbe {
public:
    virtual ~ProcessProbe() = default;
    // Which of `names` (exe names, case-insensitive) are running right now.
    virtual std::vector<std::string> Running(const std::vector<std::string>& names) = 0;
};

// The machine's process list (Toolhelp on Windows; on other systems nothing is ever running).
std::unique_ptr<ProcessProbe> MakeSystemProbe();

// A stand-in: tests say what "runs".
class FakeProbe : public ProcessProbe {
public:
    std::set<std::string> running;   // lower-case exe names
    std::vector<std::string> Running(const std::vector<std::string>& names) override {
        std::vector<std::string> out;
        for (const std::string& n : names)
            if (running.count(Lower(n))) out.push_back(n);
        return out;
    }
    void Start(const std::string& n) { running.insert(Lower(n)); }
    void Stop(const std::string& n) { running.erase(Lower(n)); }
};

}  // namespace vrml
