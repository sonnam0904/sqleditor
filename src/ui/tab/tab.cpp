#include "ui/tab/tab.hpp"
#include <atomic>
#include <format>
#include <utility>

namespace {
    std::atomic_uint64_t g_nextTabId{1};
}

Tab::Tab(std::string name, const TabType type)
    : id_(g_nextTabId.fetch_add(1, std::memory_order_relaxed)), name(std::move(name)), type(type) {
    refreshWindowName();
}

void Tab::refreshWindowName() {
    windowName_ = std::format("{}###tab_{}", name, id_);
}
