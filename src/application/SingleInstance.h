#pragma once

namespace desto::application {

enum class InstanceAcquireResult {
    Acquired,
    AlreadyRunning,
    Failed,
};

class SingleInstanceGate {
public:
    virtual ~SingleInstanceGate() = default;

    [[nodiscard]] virtual InstanceAcquireResult acquire() noexcept = 0;
    virtual void release() noexcept = 0;
};

class MemorySingleInstanceDomain {
public:
    [[nodiscard]] bool held() const noexcept { return held_; }

private:
    friend class MemorySingleInstanceGate;
    bool held_ = false;
};

class MemorySingleInstanceGate final : public SingleInstanceGate {
public:
    explicit MemorySingleInstanceGate(MemorySingleInstanceDomain& domain) noexcept
        : domain_(domain) {
    }

    [[nodiscard]] InstanceAcquireResult acquire() noexcept override;
    void release() noexcept override;

private:
    MemorySingleInstanceDomain& domain_;
    bool acquired_ = false;
};

} // namespace desto::application
