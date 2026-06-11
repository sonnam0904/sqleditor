#pragma once

#include <string>
#include <unordered_map>
#include <vector>

struct ProcessResult {
    bool success = false;
    int exitCode = -1;
    std::string output;
    std::string errorMessage;
};

struct ProcessSpec {
    std::vector<std::string> args;
    std::unordered_map<std::string, std::string> environment;
};

class ProcessRunner {
public:
    static ProcessResult run(const ProcessSpec& spec);
};
