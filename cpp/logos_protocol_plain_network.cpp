#include "logos_protocol_plain_network.h"

#include "logos_reserved_events.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <set>
#include <utility>

namespace logos::plain::abi {

bool isControlMethod(const std::string& method)
{
    return method == "informModuleToken" || method == "revokeModuleToken"
        || method == "getPluginMethods" || method == "getPluginEvents"
        || method == "getPluginInterface";
}

namespace {
constexpr std::chrono::seconds kBusinessIdle{5};
}

EndpointWorkers::EndpointWorkers(std::function<std::size_t()> capacity)
    : capacity_(capacity ? std::move(capacity) : [] { return std::size_t{1}; })
    , controlThread_([this] { controlLoop(); })
{
}

EndpointWorkers::~EndpointWorkers() { stop(); }

void EndpointWorkers::control(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return;
        controlJobs_.push_back(std::move(job));
    }
    changed_.notify_all();
}

void EndpointWorkers::business(std::function<void()> job)
{
    const std::size_t capacity = std::max<std::size_t>(1, capacity_());
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    businessJobs_.push_back(std::move(job));
    if (businessIdle_ < businessJobs_.size() && businessWorkers_ < capacity) {
        // Detached: stop() waits for the count to reach zero instead of joining,
        // so threads that exited while idle leave nothing behind.
        std::thread([this] { businessLoop(); }).detach();
        ++businessWorkers_;
    }
    changed_.notify_all();
}

void EndpointWorkers::controlLoop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        changed_.wait(lock, [this] { return stopped_ || !controlJobs_.empty(); });
        if (controlJobs_.empty()) return; // stopped and drained
        auto job = std::move(controlJobs_.front());
        controlJobs_.pop_front();
        lock.unlock();
        try { job(); } catch (...) {}
        job = nullptr;
        lock.lock();
    }
}

void EndpointWorkers::businessLoop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        ++businessIdle_;
        const bool woke = changed_.wait_for(lock, kBusinessIdle,
            [this] { return stopped_ || !businessJobs_.empty(); });
        --businessIdle_;
        if (businessJobs_.empty()) {
            // Idle too long, or stopped with nothing left: this thread is done.
            if (stopped_ || !woke) break;
            continue;
        }
        auto job = std::move(businessJobs_.front());
        businessJobs_.pop_front();
        lock.unlock();
        try { job(); } catch (...) {}
        job = nullptr;
        lock.lock();
    }
    --businessWorkers_;
    workersDone_.notify_all();
}

void EndpointWorkers::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }
    changed_.notify_all();
    if (controlThread_.joinable()) controlThread_.join();
    std::unique_lock<std::mutex> lock(mutex_);
    workersDone_.wait(lock, [this] { return businessWorkers_ == 0; });
}

void SinkTable::subscribe(const std::string& object, const std::string& event,
                          const void* connection, IncomingCallHandler::EventSink sink)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_[object][event][connection] = std::move(sink);
}

void SinkTable::unsubscribe(const std::string& object, const std::string& event,
                            const void* connection)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = sinks_.find(object);
    if (found == sinks_.end()) return;
    auto named = found->second.find(event);
    if (named == found->second.end()) return;
    named->second.erase(connection);
    if (named->second.empty()) found->second.erase(named);
    if (found->second.empty()) sinks_.erase(found);
}

void SinkTable::dropConnection(const void* connection)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto object = sinks_.begin(); object != sinks_.end();) {
        for (auto event = object->second.begin(); event != object->second.end();) {
            event->second.erase(connection);
            if (event->second.empty()) event = object->second.erase(event);
            else ++event;
        }
        if (object->second.empty()) object = sinks_.erase(object);
        else ++object;
    }
}

void SinkTable::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.clear();
}

void SinkTable::emit(const std::string& object, const std::string& event,
                     const std::vector<RpcValue>& data)
{
    std::vector<IncomingCallHandler::EventSink> sinks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = sinks_.find(object);
        if (found == sinks_.end()) return;
        std::set<const void*> seen;
        for (const auto& name : {event, std::string{}}) {
            auto group = found->second.find(name);
            if (group != found->second.end())
                for (const auto& [id, sink] : group->second)
                    if (seen.insert(id).second) sinks.push_back(sink);
            if (event.empty() || logos::isReservedEventName(event)) break;
        }
    }
    EventMessage message{object, event, data};
    for (const auto& sink : sinks) sink(message);
}

} // namespace logos::plain::abi
