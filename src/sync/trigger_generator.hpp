#pragma once

#include <cstdint>

class TriggerGenerator {
public:
    virtual ~TriggerGenerator() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    virtual std::uint64_t currentCycle() const = 0;
};
