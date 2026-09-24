#include "logos_protocol.h"

#include "implementations/qt_remote_plain/qtro_transport.h"
#include "logos_protocol_plain_network.h"
#include "logos_protocol_plain_tokens.h"
#include "logos_codec.h"
#include "logos_call_error.h"
#include "logos_transport_config_json.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

using logos::plain::RpcBytes;
using logos::plain::RpcList;
using logos::plain::RpcMap;
using logos::plain::RpcValue;
using logos::plain::RpcConnectionBase;
using logos::plain::CallMessage;
using logos::plain::ResultMessage;
using logos::plain::EventMessage;
using logos::plain::SubscribeMessage;
using logos::qt_remote_plain::Client;
using logos::qt_remote_plain::MetaType;
using logos::qt_remote_plain::Server;
using logos::qt_remote_plain::Variant;
using json = nlohmann::json;

namespace {

constexpr int kDefaultTimeoutMs = 20000;
constexpr const char* kCompletionEvent = "__logos_call_complete__";
constexpr const char* kPendingKey = "__logos_pending_call__";
constexpr const char* kStatusKey = "__logos_rpc_status__";

char* duplicate(const std::string& value)
{
    auto* result = static_cast<char*>(std::malloc(value.size() + 1));
    if (!result) return nullptr;
    std::memcpy(result, value.data(), value.size());
    result[value.size()] = '\0';
    return result;
}

int timeoutMs(int value) { return value > 0 ? value : kDefaultTimeoutMs; }

// Text crossing the C ABI: invalid UTF-8 in a name or message is replaced
// rather than thrown.
std::string dumpJson(const json& value)
{
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Members of JSON another party wrote, read without trusting their types.
std::string stringField(const json& object, const char* key, const std::string& fallback = {})
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : fallback;
}

bool boolField(const json& object, const char* key, bool fallback)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

std::string errorJson(const char* code, const std::string& message,
                      const std::string& origin)
{
    return dumpJson(json{{"code", code}, {"message", message}, {"origin", origin}});
}

RpcValue jsonToRpc(const json& value)
{
    if (value.is_null()) return {};
    if (value.is_boolean()) return RpcValue{value.get<bool>()};
    if (value.is_number_unsigned()) return RpcValue::makeInteger(value.get<std::uint64_t>());
    if (value.is_number_integer()) return RpcValue{value.get<std::int64_t>()};
    if (value.is_number_float()) return RpcValue{value.get<double>()};
    if (value.is_string()) return RpcValue{value.get<std::string>()};
    if (value.is_array()) {
        RpcList list;
        list.items.reserve(value.size());
        for (const auto& item : value) list.items.push_back(jsonToRpc(item));
        return RpcValue{std::move(list)};
    }
    if (value.is_object() && value.size() == 1) {
        const auto found = value.find("_bytes");
        if (found != value.end() && found->is_string()) {
            RpcBytes bytes;
            if (!logos::b64UrlDecodeChecked(found->get<std::string>(), bytes.data))
                return {};
            return RpcValue{std::move(bytes)};
        }
    }
    RpcMap map;
    for (auto it = value.begin(); it != value.end(); ++it)
        map.emplace(it.key(), jsonToRpc(it.value()));
    return RpcValue{std::move(map)};
}

std::string base64Url(const std::vector<std::uint8_t>& bytes)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    unsigned accumulator = 0;
    int bits = 0;
    for (const auto byte : bytes) {
        accumulator = (accumulator << 8) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(alphabet[(accumulator >> bits) & 63u]);
            accumulator &= (1u << bits) - 1u;
        }
    }
    if (bits) out.push_back(alphabet[(accumulator << (6 - bits)) & 63u]);
    return out;
}

json rpcToJson(const RpcValue& value)
{
    if (value.isNull()) return nullptr;
    if (value.isBool()) return value.asBool();
    if (value.isInt()) return value.asInt();
    if (value.isUInt()) return value.asUInt();
    if (value.isDouble()) return value.asDouble();
    if (value.isString()) return value.asString();
    if (value.isBytes()) return json{{"_bytes", base64Url(value.asBytes().data)}};
    if (value.isList()) {
        json out = json::array();
        for (const auto& item : value.asList().items) out.push_back(rpcToJson(item));
        return out;
    }
    json out = json::object();
    for (const auto& item : value.asMap().entries)
        out[item.first] = rpcToJson(item.second);
    return out;
}

// "result" is the LIDL spelling the C++ generator publishes; Rust publishes
// "LogosResult". Qt typed wrappers need the user type either way.
bool isLogosResultType(const std::string& type)
{
    return type == "LogosResult" || type == "result"
        || (type.size() > 13 && type.compare(type.size() - 13, 13, "::LogosResult") == 0);
}

// Non-negative JSON integers reach Qt as qulonglong, as nlohmannToQVariant
// sends them on the Qt path; RpcValue folds them into int64.
void markUnsigned(Variant& variant, const json& value)
{
    if (value.is_number_unsigned()) {
        variant.type = MetaType::ULongLong;
    } else if (value.is_array() && variant.type == MetaType::VariantList
               && variant.nestedValues.size() == value.size()) {
        for (std::size_t i = 0; i < value.size(); ++i)
            markUnsigned(variant.nestedValues[i], value[i]);
    } else if (value.is_object() && variant.type == MetaType::VariantMap
               && variant.nestedKeys.size() == variant.nestedValues.size()) {
        for (std::size_t i = 0; i < variant.nestedKeys.size(); ++i) {
            const auto found = value.find(variant.nestedKeys[i]);
            if (found != value.end()) markUnsigned(variant.nestedValues[i], *found);
        }
    }
}

Variant jsonToVariant(const json& value)
{
    Variant variant = Variant::fromRpc(jsonToRpc(value));
    markUnsigned(variant, value);
    return variant;
}

Variant resultVariant(const json& value, const std::string& declaredReturnType)
{
    if (isLogosResultType(declaredReturnType)
        && value.is_object() && value.size() == 3
        && value.contains("success") && value["success"].is_boolean()
        && value.contains("value") && value.contains("error")) {
        return Variant::logosResult(value["success"].get<bool>(),
                                    jsonToVariant(value["value"]),
                                    jsonToVariant(value["error"]));
    }
    return jsonToVariant(value);
}

std::string instanceId()
{
    if (const char* value = std::getenv("LOGOS_INSTANCE_ID"); value && *value)
        return value;
    // Twelve hex digits, as LogosInstance::id() makes them: the id is in every
    // socket path, and macOS caps those at 104 bytes.
    std::random_device random;
    const std::uint64_t bits = (static_cast<std::uint64_t>(random()) << 32 | random())
        ^ static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    char text[13];
    std::snprintf(text, sizeof text, "%012llx",
                  static_cast<unsigned long long>(bits & 0xffffffffffffULL));
    const std::string value = text;
#ifdef _WIN32
    _putenv_s("LOGOS_INSTANCE_ID", value.c_str());
#else
    ::setenv("LOGOS_INSTANCE_ID", value.c_str(), 1);
#endif
    return value;
}

std::string endpoint(const std::string& module)
{
    return "local:logos_" + module + "_" + instanceId();
}

std::mutex gDefaultMutex;
std::string gDefaultTransport = R"({"protocol":"qt_remote_plain"})";

// "local" is accepted too: here it can only mean qt_remote_plain.
bool acceptsPlainConfig(const char* text)
{
    if (!text || !*text || std::strcmp(text, "null") == 0) return true;
    LogosTransportSet set;
    return logos::parseTransportSet(std::string("[") + text + "]", &set) && set.size() == 1;
}

LogosTransportConfig parsePlainConfig(const char* text)
{
    LogosTransportConfig config;
    config.protocol = LogosProtocol::QtRemotePlain;
    std::string source;
    if (!text || !*text || std::strcmp(text, "null") == 0) {
        std::lock_guard<std::mutex> lock(gDefaultMutex);
        source = gDefaultTransport;
    } else source = text;
    const auto set = logos::transportSetFromJsonString(std::string("[") + source + "]");
    if (set.empty()) return config;
    LogosTransportConfig parsed = set.front();
    if (parsed.protocol == LogosProtocol::LocalSocket) parsed.protocol = LogosProtocol::QtRemotePlain;
    return parsed;
}

struct TokenFlight {
    bool finished = false;
    std::string token;
    std::string error;
};

struct TokenStore {
    std::mutex mutex;
    std::map<std::string, std::string> outbound;
    std::map<std::string, std::string> inbound;
    std::string credential;
    // One capability request per target at a time: each token it issues
    // replaces the previous one, so concurrent requests revoke each other.
    std::map<std::string, std::shared_ptr<TokenFlight>> flights;
    std::condition_variable flightDone;
};

std::mutex gTokenMutex;
std::shared_ptr<TokenStore> gSharedTokens = std::make_shared<TokenStore>();
std::map<std::string, std::shared_ptr<TokenStore>> gIdentityTokens;
std::set<std::string> gVendedIdentities;

std::shared_ptr<TokenStore> tokensFor(const std::string& identity)
{
    std::lock_guard<std::mutex> lock(gTokenMutex);
    if (const auto found = gIdentityTokens.find(identity); found != gIdentityTokens.end())
        return found->second;
    gVendedIdentities.insert(identity);
    return gSharedTokens;
}

std::string tokenGet(const std::shared_ptr<TokenStore>& store, const std::string& module)
{
    std::lock_guard<std::mutex> lock(store->mutex);
    const auto found = store->outbound.find(module);
    return found == store->outbound.end() ? std::string{} : found->second;
}

void tokenSave(const std::shared_ptr<TokenStore>& store,
               const std::string& module, const std::string& token)
{
    std::lock_guard<std::mutex> lock(store->mutex);
    store->outbound[module] = token;
    if (module == "core" || module == "capability_module") {
        store->credential = token;
        store->outbound["core"] = token;
        store->outbound["capability_module"] = token;
    }
}

bool isUnauthorized(const Variant& value)
{
    if (!value.value.isMap()) return false;
    const auto& entries = value.value.asMap().entries;
    if (entries.size() != 1) return false;
    const RpcValue* status = value.value.asMap().find(kStatusKey);
    return status && status->isString() && status->asString() == "unauthorized";
}

bool pendingId(const Variant& value, std::string& id)
{
    if (!value.value.isMap() || value.value.asMap().entries.size() != 1) return false;
    const RpcValue* found = value.value.asMap().find(kPendingKey);
    if (!found || !found->isString() || found->asString().empty()) return false;
    id = found->asString();
    return true;
}

struct SubscriptionState {
    enum class Phase { Pending, Armed, Held };

    std::recursive_mutex callbackMutex;
    std::atomic<bool> active{true};
    Phase phase = Phase::Pending; // protected by ClientState::mutex
    std::string event;
    lp_event_cb callback = nullptr;
    void* userData = nullptr;
};

struct ClientState {
    struct StatusNotification {
        int status = 0;
        unsigned long long generation = 0;
        std::string reason;
    };

    std::recursive_mutex callbackMutex;
    std::mutex mutex;
    std::timed_mutex connectionMutex;
    std::condition_variable completionChanged;
    std::condition_variable subscriptionChanged;
    std::condition_variable eventsChanged;
    std::mutex eventsMutex;
    // Network events in arrival order; an empty entry is the connection's loss.
    std::deque<std::optional<EventMessage>> events;
    std::shared_ptr<Client> wire = std::make_shared<Client>();
    std::shared_ptr<RpcConnectionBase> networkWire;
    LogosTransportConfig targetConfig;
    LogosTransportConfig capabilityConfig;
    std::string target;
    std::string origin;
    std::shared_ptr<TokenStore> tokens;
    std::vector<std::weak_ptr<SubscriptionState>> subscriptions;
    std::map<std::string, RpcValue> completions;
    std::deque<StatusNotification> statusNotifications;
    std::atomic<bool> alive{true};
    std::atomic<unsigned long long> generation{0};
    // An armed subscription was lost since the last arm; only then is the next
    // arm a new establishment.
    bool lostSinceArmed = false;
    lp_subscription_status_cb statusCallback = nullptr;
    void* statusUserData = nullptr;
    bool manualRestart = false;
    bool workerStop = false;
    std::thread subscriptionWorker;
    std::thread eventWorker;
    // lp_invoke_async calls start in order on a few threads that exit when idle.
    std::mutex asyncMutex;
    std::condition_variable asyncChanged;
    std::deque<std::function<void()>> asyncCalls;
    std::size_t asyncWorkers = 0;
    std::size_t asyncIdle = 0;
    bool asyncStop = false;
    // Their requests go out in the order the calls were made: each takes a
    // ticket and writes when it is the next one (asyncNextToSend).
    std::uint64_t asyncTickets = 0;
    std::uint64_t asyncNextToSend = 0;
    std::set<std::uint64_t> asyncSent;
    std::condition_variable asyncTurn;
};

constexpr std::size_t kMaxAsyncWorkers = 16;
constexpr std::chrono::seconds kAsyncWorkerIdle{5};

void asyncWorkerLoop(const std::shared_ptr<ClientState>& state)
{
    std::unique_lock<std::mutex> lock(state->asyncMutex);
    for (;;) {
        ++state->asyncIdle;
        state->asyncChanged.wait_for(lock, kAsyncWorkerIdle, [&] {
            return state->asyncStop || !state->asyncCalls.empty();
        });
        --state->asyncIdle;
        if (state->asyncStop || state->asyncCalls.empty()) break;
        std::function<void()> call = std::move(state->asyncCalls.front());
        state->asyncCalls.pop_front();
        lock.unlock();
        try {
            call();
        } catch (...) {
            // Nothing thrown here may end the process.
        }
        call = nullptr;
        lock.lock();
    }
    --state->asyncWorkers;
}

// Waits until `ticket` is the next async request to go out.
bool awaitSendTurn(const std::shared_ptr<ClientState>& state, std::uint64_t ticket,
                   std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock<std::mutex> lock(state->asyncMutex);
    return state->asyncTurn.wait_until(lock, deadline, [&] {
        return state->asyncStop || state->asyncNextToSend == ticket;
    }) && !state->asyncStop;
}

// Its request written, or the call abandoned before writing: the next ticket may go.
struct SendTurn {
    std::shared_ptr<ClientState> state;
    std::uint64_t ticket = 0;
    bool finished = false;
    void finish()
    {
        if (finished) return;
        finished = true;
        // Notified under the lock: on mingw a notify_all after unlocking was seen
        // to miss a waiter, and every later call waited out its deadline behind it.
        std::lock_guard<std::mutex> lock(state->asyncMutex);
        state->asyncSent.insert(ticket);
        while (state->asyncSent.erase(state->asyncNextToSend)) ++state->asyncNextToSend;
        state->asyncTurn.notify_all();
    }
    ~SendTurn() { finish(); }
};

// Destruction from a user callback cannot join workers that might be waiting
// to acquire callbackMutex. The workers retain ClientState until they exit.
thread_local const ClientState* activeCallbackState = nullptr;

struct CallbackScope {
    explicit CallbackScope(const ClientState* state)
        : previous(activeCallbackState) { activeCallbackState = state; }
    ~CallbackScope() { activeCallbackState = previous; }
    const ClientState* previous;
};

void connectionLost(const std::shared_ptr<ClientState>& state);

void recordNetworkCompletion(const std::shared_ptr<ClientState>& state,
                             const EventMessage& message)
{
    if (message.object != state->target || message.eventName != kCompletionEvent) return;
    if (message.data.size() == 2 && message.data[0].isString()) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->alive) return;
            state->completions[message.data[0].asString()] = message.data[1];
        }
        state->completionChanged.notify_all();
    }
}

void deliverNetworkEvent(const std::shared_ptr<ClientState>& state,
                         const EventMessage& message)
{
    if (message.object != state->target || message.eventName == kCompletionEvent) return;
    std::lock_guard<std::recursive_mutex> callbackLock(state->callbackMutex);
    if (!state->alive) return;
    std::vector<std::shared_ptr<SubscriptionState>> subscriptions;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        for (auto it = state->subscriptions.begin(); it != state->subscriptions.end();) {
            if (auto sub = it->lock()) {
                if (sub->active && sub->phase == SubscriptionState::Phase::Armed)
                    subscriptions.push_back(std::move(sub));
                ++it;
            } else it = state->subscriptions.erase(it);
        }
    }
    json data = json::array();
    for (const auto& value : message.data) data.push_back(rpcToJson(value));
    const std::string text = dumpJson(data);
    for (const auto& sub : subscriptions) {
        std::lock_guard<std::recursive_mutex> subLock(sub->callbackMutex);
        if (sub->active && (sub->event.empty() || sub->event == message.eventName)
            && sub->callback) {
            CallbackScope scope(state.get());
            sub->callback(message.eventName.c_str(), text.c_str(), sub->userData);
        }
        if (!state->alive) break;
    }
}

void queueNetworkEvent(const std::shared_ptr<ClientState>& state,
                       EventMessage message)
{
    {
        std::lock_guard<std::mutex> lock(state->eventsMutex);
        if (!state->alive) return;
        state->events.push_back(std::move(message));
    }
    state->eventsChanged.notify_one();
}

// Reported behind the events already received, as over QtRO.
void queueNetworkLoss(const std::shared_ptr<ClientState>& state)
{
    {
        std::lock_guard<std::mutex> lock(state->eventsMutex);
        if (!state->alive) return;
        state->events.push_back(std::nullopt);
    }
    state->eventsChanged.notify_one();
}

void networkEventLoop(const std::shared_ptr<ClientState>& state)
{
    for (;;) {
        std::optional<EventMessage> message;
        {
            std::unique_lock<std::mutex> lock(state->eventsMutex);
            state->eventsChanged.wait(lock, [&] {
                return !state->alive || !state->events.empty();
            });
            if (!state->alive) return;
            message = std::move(state->events.front());
            state->events.pop_front();
        }
        if (message)
            deliverNetworkEvent(state, *message);
        else
            connectionLost(state);
    }
}

void reportSubscriptionStatus(const std::shared_ptr<ClientState>& state,
                              int status, unsigned long long generation,
                              const char* reason)
{
    lp_subscription_status_cb callback = nullptr;
    void* userData = nullptr;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        callback = state->statusCallback;
        userData = state->statusUserData;
    }
    if (!callback) return;
    std::lock_guard<std::recursive_mutex> callbackLock(state->callbackMutex);
    if (state->alive) {
        CallbackScope scope(state.get());
        callback(status, generation, reason, userData);
    }
}

bool hasSubscriptionPhase(const std::shared_ptr<ClientState>& state,
                          SubscriptionState::Phase phase)
{
    for (auto it = state->subscriptions.begin(); it != state->subscriptions.end();) {
        if (auto subscription = it->lock()) {
            if (subscription->active && subscription->phase == phase) return true;
            ++it;
        } else {
            it = state->subscriptions.erase(it);
        }
    }
    return false;
}

const char* callErrorCode(Client::Failure failure)
{
    switch (failure) {
    case Client::Failure::Unavailable: return "object_unavailable";
    case Client::Failure::Timeout: return "timeout";
    case Client::Failure::Failed: return "call_failed";
    case Client::Failure::Transport: break;
    }
    return "transport_error";
}

// `code`, when given, names a failure in the protocol's call-error vocabulary.
bool ensureConnected(const std::shared_ptr<ClientState>& state,
                     int timeout, std::string& error, std::string* code = nullptr)
{
    const auto fail = [code](const char* value) {
        if (code) *code = value;
        return false;
    };
    if (timeout <= 0 || !state->alive) {
        error = "connection timed out";
        return fail(state->alive ? "timeout" : "transport_error");
    }
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout);
    std::unique_lock<std::timed_mutex> connectionLock(state->connectionMutex,
                                                      std::defer_lock);
    if (!connectionLock.try_lock_until(deadline)) {
        error = "connection timed out";
        return fail("timeout");
    }
    if (!state->alive) {
        error = "client destroyed";
        return fail("transport_error");
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
        error = "connection timed out";
        return fail("timeout");
    }
    if (state->targetConfig.protocol == LogosProtocol::Tcp
        || state->targetConfig.protocol == LogosProtocol::TcpSsl) {
        if (state->networkWire && state->networkWire->isOpen()) return true;
        if (state->networkWire) state->networkWire->stop("reconnecting");
        auto wire = logos::plain::abi::connect(
            state->targetConfig, remaining, error, &state->alive);
        if (!wire) return fail("object_unavailable");
        if (!state->alive) {
            wire->stop("client destroyed");
            error = "client destroyed";
            return fail("transport_error");
        }
        std::weak_ptr<ClientState> weak = state;
        wire->setErrorHandler([weak](const std::string&) {
            if (auto locked = weak.lock()) queueNetworkLoss(locked);
        });
        wire->sendSubscribe({state->target, kCompletionEvent},
            [weak](EventMessage event) {
                if (auto locked = weak.lock()) recordNetworkCompletion(locked, event);
            });
        wire->sendSubscribe({state->target, ""},
            [weak](EventMessage event) {
                if (event.eventName == kCompletionEvent) return;
                if (auto locked = weak.lock()) queueNetworkEvent(locked, std::move(event));
            });
        state->networkWire = std::move(wire);
        return true;
    }
    if (state->wire->isConnected()) return true;
    Client::Failure failure = Client::Failure::Transport;
    if (state->wire->connect(endpoint(state->target), remaining, &error, &failure)) return true;
    return fail(callErrorCode(failure));
}

void subscriptionLoop(const std::shared_ptr<ClientState>& state)
{
    // Each attempt itself waits up to 250 ms for the provider to listen, so a
    // returning provider is reached within about half a second, as over QtRO.
    const auto retry = std::chrono::milliseconds(250);
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->subscriptionChanged.wait(lock, [&] {
                return state->workerStop
                    || !state->statusNotifications.empty()
                    || hasSubscriptionPhase(state, SubscriptionState::Phase::Pending);
            });
            if (state->workerStop) return;
            if (!state->statusNotifications.empty()) {
                auto notification = std::move(state->statusNotifications.front());
                state->statusNotifications.pop_front();
                lock.unlock();
                reportSubscriptionStatus(state, notification.status,
                    notification.generation, notification.reason.c_str());
                continue;
            }
        }

        std::string error;
        // QtRO's local retry tick; a network dial (DNS, TCP, TLS) gets the
        // 5 s the Qt-side network client gave it.
        const bool network = state->targetConfig.protocol == LogosProtocol::Tcp
            || state->targetConfig.protocol == LogosProtocol::TcpSsl;
        bool acquired = ensureConnected(state, network ? 5000 : 250, error);
        if (acquired && state->targetConfig.protocol == LogosProtocol::QtRemotePlain) {
            std::lock_guard<std::timed_mutex> connectionLock(state->connectionMutex);
            acquired = state->wire->acquire(state->target, std::chrono::milliseconds(250),
                                             &error);
        }

        if (!acquired) {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (state->subscriptionChanged.wait_for(lock, retry, [&] {
                    return state->workerStop
                        || !state->statusNotifications.empty()
                        || !hasSubscriptionPhase(state, SubscriptionState::Phase::Pending);
                })) {
                if (state->workerStop) return;
            }
            continue;
        }

        bool established = false;
        unsigned long long generation = 0;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            const bool alreadyArmed =
                hasSubscriptionPhase(state, SubscriptionState::Phase::Armed);
            bool armedAny = false;
            for (auto it = state->subscriptions.begin(); it != state->subscriptions.end();) {
                if (auto subscription = it->lock()) {
                    if (subscription->active
                        && subscription->phase == SubscriptionState::Phase::Pending) {
                        subscription->phase = SubscriptionState::Phase::Armed;
                        armedAny = true;
                    }
                    ++it;
                } else {
                    it = state->subscriptions.erase(it);
                }
            }
            // Arming a subscription taken while none was armed is not a gap.
            established = armedAny && !alreadyArmed
                && (state->generation == 0 || state->lostSinceArmed);
            if (established) state->lostSinceArmed = false;
            generation = established ? ++state->generation : state->generation.load();
        }
        if (established)
            reportSubscriptionStatus(state, LP_SUB_ARMED, generation, nullptr);
    }
}

std::optional<RpcValue> networkCall(const std::shared_ptr<ClientState>& state,
                                    const std::string& object,
                                    const std::string& method,
                                    const json& args, const std::string& token,
                                    int timeout, std::string& error, std::string& code,
                                    const std::function<void()>* sent = nullptr)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout);
    if (!ensureConnected(state, timeout, error, &code)) return std::nullopt;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
        error = "invocation timed out";
        code = "timeout";
        return std::nullopt;
    }
    std::shared_ptr<RpcConnectionBase> wire;
    {
        std::unique_lock<std::timed_mutex> lock(state->connectionMutex,
                                                std::defer_lock);
        if (!lock.try_lock_until(deadline)) {
            error = "invocation timed out";
            code = "timeout";
            return std::nullopt;
        }
        wire = state->networkWire;
    }
    if (!wire || !wire->isOpen()) {
        error = "connection closed";
        code = "transport_error";
        return std::nullopt;
    }
    CallMessage request;
    request.id = wire->nextId();
    request.authToken = token;
    request.object = object;
    request.method = method;
    for (const auto& arg : args) request.args.push_back(jsonToRpc(arg));
    const auto id = request.id;
    auto future = wire->sendCall(std::move(request));
    if (sent && *sent) (*sent)();
    if (future.wait_until(deadline) != std::future_status::ready) {
        wire->cancelPending(id);
        error = "invocation timed out";
        code = "timeout";
        return std::nullopt;
    }
    auto result = future.get();
    if (!result.ok) {
        const logos::CallError failure = logos::callErrorFromWire(object, result.errCode, result.err);
        error = failure.message;
        code = failure.code;
        return std::nullopt;
    }
    return std::move(result.value);
}

// What getPluginInterface() answers, from the two calls a provider built before
// logos-cpp-sdk #71 has instead: its methods, then its events.
std::optional<json> legacyInterface(const std::optional<json>& methods,
                                    const std::optional<json>& events)
{
    if (!methods || !methods->is_array()) return std::nullopt;
    json entries = *methods;
    if (events && events->is_array()) {
        for (json entry : *events) {
            if (entry.is_object() && !entry.contains("type")) entry["type"] = "event";
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

void connectionLost(const std::shared_ptr<ClientState>& state)
{
    bool lost = false;
    bool held = false;
    unsigned long long generation = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->alive) return;
        held = state->manualRestart;
        for (auto it = state->subscriptions.begin(); it != state->subscriptions.end();) {
            if (auto subscription = it->lock()) {
                if (subscription->active
                    && subscription->phase == SubscriptionState::Phase::Armed) {
                    subscription->phase = held ? SubscriptionState::Phase::Held
                                               : SubscriptionState::Phase::Pending;
                    lost = true;
                }
                ++it;
            } else {
                it = state->subscriptions.erase(it);
            }
        }
        generation = state->generation.load();
        if (lost) {
            state->lostSinceArmed = true;
            state->statusNotifications.push_back({held ? LP_SUB_HELD : LP_SUB_LOST,
                                                  generation, "provider_unavailable"});
        }
    }
    if (!lost) return;
    state->subscriptionChanged.notify_all();
}

std::optional<Variant> directCall(const std::shared_ptr<ClientState>& state,
                                  const std::string& object,
                                  const std::string& signature,
                                  std::vector<Variant> arguments,
                                  int timeout, std::string& error, std::string& code,
                                  const std::function<void()>* sent = nullptr)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout);
    const auto remaining = [&] {
        return std::max(0, static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count()));
    };
    Client::Failure failure = Client::Failure::Transport;
    const auto failed = [&]() -> std::optional<Variant> {
        code = callErrorCode(failure);
        return std::nullopt;
    };
    if (object != state->target) {
        Client temporary;
        if (!temporary.connect(endpoint(object), std::chrono::milliseconds(remaining()), &error,
                               &failure))
            return failed();
        auto result = temporary.call(object, signature, std::move(arguments),
                                     std::chrono::milliseconds(remaining()), &error, &failure);
        return result ? result : failed();
    }
    if (!ensureConnected(state, remaining(), error, &code)) return std::nullopt;
    auto result = state->wire->call(object, signature, std::move(arguments),
                                    std::chrono::milliseconds(remaining()), &error, &failure,
                                    sent ? *sent : std::function<void()>{});
    return result ? result : failed();
}

// capability_module listens before anything asks it for a token, so when nothing
// listens there, none is coming: the exchange gives up after this, not at the deadline.
constexpr std::chrono::milliseconds kCapabilityListenerWait{250};

std::string mintToken(const std::shared_ptr<ClientState>& state,
                      int timeout, std::string& error)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout);
    const std::string credential = tokenGet(state->tokens, "capability_module");
    if (state->capabilityConfig.protocol == LogosProtocol::Tcp
        || state->capabilityConfig.protocol == LogosProtocol::TcpSsl) {
        auto wire = logos::plain::abi::connect(
            state->capabilityConfig, std::chrono::milliseconds(timeout), error,
            &state->alive);
        if (!wire) return {};
        CallMessage request;
        request.id = wire->nextId();
        request.authToken = credential;
        request.object = "capability_module";
        request.method = "requestModule";
        request.args = {RpcValue{state->origin}, RpcValue{state->target}};
        const auto id = request.id;
        auto future = wire->sendCall(std::move(request));
        if (future.wait_until(deadline) != std::future_status::ready) {
            wire->cancelPending(id);
            wire->stop("capability request timed out");
            error = "capability request timed out";
            return {};
        }
        auto result = future.get();
        wire->stop("capability request complete");
        if (!result.ok || !result.value.isString()) {
            error = result.err;
            return {};
        }
        const std::string token = result.value.asString();
        if (!token.empty()) tokenSave(state->tokens, state->target, token);
        return token;
    }
    Client capability;
    if (!capability.connect(endpoint("capability_module"), std::chrono::milliseconds(timeout),
                            &error, nullptr, kCapabilityListenerWait)) return {};
    Variant requestArgs = Variant::fromRpc(RpcValue{RpcList{{RpcValue{state->origin},
                                                             RpcValue{state->target}}}});
    auto result = capability.call("capability_module",
        "callRemoteMethod(QString,QString,QVariantList)",
        {Variant::fromRpc(RpcValue{credential}),
         Variant::fromRpc(RpcValue{"requestModule"}), std::move(requestArgs)},
        std::max(std::chrono::milliseconds::zero(),
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     deadline - std::chrono::steady_clock::now())), &error);
    if (!result || !result->value.isString()) return {};
    const std::string token = result->value.asString();
    if (!token.empty()) tokenSave(state->tokens, state->target, token);
    return token;
}

// The token to call the target with, requested at most once at a time.
// `rejected` is one the target just refused: it is replaced, unless another
// call has already replaced it.
std::string tokenFor(const std::shared_ptr<ClientState>& state, int timeout,
                     std::string& error, const std::string& rejected = {})
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout);
    TokenStore& store = *state->tokens;
    std::unique_lock<std::mutex> lock(store.mutex);
    for (;;) {
        const auto current = store.outbound.find(state->target);
        const std::string known = current == store.outbound.end() ? std::string()
                                                                  : current->second;
        if ((!known.empty() && known != rejected) || state->target == "capability_module")
            return known;
        const auto running = store.flights.find(state->target);
        if (running == store.flights.end()) break;
        const std::shared_ptr<TokenFlight> flight = running->second;
        if (!store.flightDone.wait_until(lock, deadline, [&] { return flight->finished; })) {
            error = "capability request timed out";
            return {};
        }
        if (flight->token.empty()) {
            error = flight->error;
            return {};
        }
    }
    const auto current = store.outbound.find(state->target);
    if (current != store.outbound.end() && current->second == rejected)
        store.outbound.erase(current);
    auto flight = std::make_shared<TokenFlight>();
    store.flights[state->target] = flight;
    lock.unlock();
    std::string token = mintToken(state, std::max(0, static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count())), error);
    lock.lock();
    flight->finished = true;
    flight->token = token;
    flight->error = error;
    const auto mine = store.flights.find(state->target);
    if (mine != store.flights.end() && mine->second == flight) store.flights.erase(mine);
    lock.unlock();
    store.flightDone.notify_all();
    return token;
}

// On failure `code` is the protocol's call-error code for it (logos_call_error.h).
// `sent`, if given, runs once the first request is written; a retry after a
// refused token is sent later, out of that order.
std::optional<Variant> invoke(const std::shared_ptr<ClientState>& state,
                              const std::string& method, const json& args,
                              int timeout, std::string& error, std::string& code,
                              const std::function<void()>* sent = nullptr)
{
    code = "transport_error";
    if (!args.is_array()) {
        error = "arguments must be a JSON array";
        code = "invalid_arg";
        return std::nullopt;
    }
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout);
    const auto remaining = [&] {
        return std::max(0, static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count()));
    };
    std::string token = tokenFor(state, remaining(), error);
    if (state->targetConfig.protocol == LogosProtocol::Tcp
        || state->targetConfig.protocol == LogosProtocol::TcpSsl) {
        auto result = networkCall(state, state->target, method, args, token,
                                  remaining(), error, code, sent);
        auto unauthorized = [](const std::optional<RpcValue>& value) {
            if (!value || !value->isMap()) return false;
            const RpcValue* status = value->asMap().find(kStatusKey);
            return status && status->isString() && status->asString() == "unauthorized";
        };
        if (unauthorized(result) && state->target != "capability_module") {
            token = tokenFor(state, remaining(), error, token);
            if (!token.empty())
                result = networkCall(state, state->target, method, args,
                                     token, remaining(), error, code);
        }
        if (unauthorized(result)) {
            error = "token not recognized";
            code = "unauthorized";
            return std::nullopt;
        }
        if (!result) return std::nullopt;
        Variant wrapped = Variant::fromRpc(*result);
        std::string completion;
        if (pendingId(wrapped, completion)) {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (!state->completionChanged.wait_until(lock, deadline, [&] {
                    return !state->alive || state->completions.count(completion) != 0;
                })) {
                error = "deferred invocation timed out";
                code = "timeout";
                return std::nullopt;
            }
            auto found = state->completions.find(completion);
            if (found == state->completions.end()) {
                error = "client destroyed";
                return std::nullopt;
            }
            Variant completed = Variant::fromRpc(std::move(found->second));
            state->completions.erase(found);
            return completed;
        }
        return wrapped;
    }
    Variant arguments = jsonToVariant(args);
    auto result = directCall(state, state->target,
        "callRemoteMethod(QString,QString,QVariantList)",
        {Variant::fromRpc(RpcValue{token}), Variant::fromRpc(RpcValue{method}),
         std::move(arguments)}, remaining(), error, code, sent);
    if (result && isUnauthorized(*result) && state->target != "capability_module") {
        token = tokenFor(state, remaining(), error, token);
        if (!token.empty()) {
            arguments = jsonToVariant(args);
            result = directCall(state, state->target,
                "callRemoteMethod(QString,QString,QVariantList)",
                {Variant::fromRpc(RpcValue{token}), Variant::fromRpc(RpcValue{method}),
                 std::move(arguments)}, remaining(), error, code);
        }
    }
    if (result && isUnauthorized(*result)) {
        error = "token not recognized";
        code = "unauthorized";
        return std::nullopt;
    }
    if (result && result->opaque) {
        code = "call_failed";
        error = "reply carries a Qt type the plain runtime cannot represent (metatype "
            + std::to_string(static_cast<std::uint32_t>(result->type))
            + (result->customType.empty() ? "" : " " + result->customType) + ")";
        return std::nullopt;
    }
    if (result) {
        std::string completion;
        if (pendingId(*result, completion)) {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (!state->completionChanged.wait_until(lock, deadline, [&] {
                    return !state->alive || state->completions.count(completion) != 0;
                })) {
                error = "deferred invocation timed out";
                code = "timeout";
                return std::nullopt;
            }
            auto found = state->completions.find(completion);
            if (found == state->completions.end()) {
                error = "client destroyed";
                return std::nullopt;
            }
            Variant completed = Variant::fromRpc(std::move(found->second));
            state->completions.erase(found);
            return completed;
        }
    }
    return result;
}

std::atomic<unsigned> gHostServices{0};
constexpr unsigned kTokenRegistry = 1;
constexpr unsigned kTokenDelivery = 2;
thread_local std::string gCurrentCaller = R"({"kind":"unknown"})";

} // namespace

struct lp_client { std::shared_ptr<ClientState> state; };
struct lp_subscription {
    std::shared_ptr<ClientState> owner;
    std::shared_ptr<SubscriptionState> state;
};

struct lp_provider {
    std::string moduleName;
    std::string transportSetJson;
    Server server;
    bool qtroStarted = false;
    std::vector<std::unique_ptr<logos::plain::abi::ServerEndpoint>> networkEndpoints;
    lp_dispatch_cb dispatch = nullptr;
    lp_getmethods_cb getMethods = nullptr;
    lp_token_cb onToken = nullptr;
    lp_validate_token_cb validateToken = nullptr;
    void* validatorUserData = nullptr;
    void* userData = nullptr;
    std::mutex mutex;
    std::map<std::string, std::string> inbound;
    std::map<std::string, std::string> returnTypes;
    // Deferred calls of LogosResult methods: call id -> declared return type.
    std::map<std::string, std::string> pendingResultTypes;
    std::string credential;
    std::atomic<bool> prepared{false};
    std::atomic<bool> registered{false};
};

namespace {

std::atomic<unsigned long long> gTokenComparisons{0};

// The Qt provider's comparison (module_proxy.cpp): a different length is a
// mismatch, but the scan still covers the longer value.
bool constantTimeEquals(const std::string& a, const std::string& b)
{
    gTokenComparisons.fetch_add(1, std::memory_order_relaxed);
    const std::size_t n = std::max(a.size(), b.size());
    std::size_t diff = a.size() ^ b.size();
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<std::size_t>(ca ^ cb);
    }
    return diff == 0;
}

bool isAnchorKey(const std::string& key)
{
    return key == "core" || key == "capability_module";
}

// The caller document for `token`, empty when unauthorized. As on the Qt path
// (module_proxy.cpp), every entry is compared, an anchor key authorizes but
// never names, and a value two callers share names neither; a token the host's
// validator accepts is authorized too, and named by no one.
std::string providerCaller(lp_provider* provider, const std::string& token,
                           const char* protocol = "local")
{
    if (token.empty()) return {};
    lp_validate_token_cb validate = nullptr;
    void* validatorData = nullptr;
    {
        std::lock_guard<std::mutex> lock(provider->mutex);
        const bool host = !provider->credential.empty()
            && constantTimeEquals(provider->credential, token);
        bool anchor = false;
        int named = 0;
        std::string name;
        for (const auto& entry : provider->inbound) {
            const bool match = constantTimeEquals(entry.second, token);
            if (isAnchorKey(entry.first)) {
                anchor = anchor || match;
            } else if (match) {
                ++named;
                name = entry.first;
            }
        }
        if (host) return R"({"kind":"host"})";
        if (named == 1 && !anchor) return dumpJson(json{{"kind", "module"}, {"name", name}});
        if (named > 0 || anchor) return R"({"kind":"unknown"})";
        validate = provider->validateToken;
        validatorData = provider->validatorUserData;
    }
    if (validate && validate(token.c_str(), protocol, validatorData) == LP_OK)
        return R"({"kind":"unknown"})";
    return {};
}

json providerMetadata(lp_provider* provider, const std::string& requested)
{
    if (!provider->getMethods) return json::array();
    char* text = provider->getMethods(provider->userData);
    const json metadata = text ? json::parse(text, nullptr, false) : json::array();
    lp_string_free(text);
    if (!metadata.is_array() || requested == "interface")
        return metadata.is_array() ? metadata : json::array();
    json filtered = json::array();
    for (const auto& entry : metadata) {
        if (!entry.is_object()) continue;
        const bool event = stringField(entry, "type") == "event";
        if ((requested == "events") == event) filtered.push_back(entry);
    }
    return filtered;
}

void cacheReturnTypes(lp_provider* provider)
{
    const json metadata = providerMetadata(provider, "methods");
    std::map<std::string, std::string> returnTypes;
    for (const auto& entry : metadata) {
        if (!entry.is_object() || stringField(entry, "type", "method") != "method")
            continue;
        const std::string name = stringField(entry, "name");
        const std::string returnType = stringField(entry, "returnType");
        if (!name.empty()) returnTypes[name] = returnType;
    }
    std::lock_guard<std::mutex> lock(provider->mutex);
    provider->returnTypes = std::move(returnTypes);
}

std::string providerReturnType(lp_provider* provider, const std::string& method)
{
    std::lock_guard<std::mutex> lock(provider->mutex);
    const auto found = provider->returnTypes.find(method);
    return found == provider->returnTypes.end() ? std::string{} : found->second;
}

// Recorded only once the module took it: a refused push must not later
// authorize, or name, whoever presents that token.
bool providerAcceptToken(lp_provider* provider, const std::string& auth,
                         const std::string& module, const std::string& token)
{
    bool trusted = false;
    {
        std::lock_guard<std::mutex> lock(provider->mutex);
        trusted = !provider->credential.empty() && constantTimeEquals(provider->credential, auth);
    }
    if (!trusted) return false;
    const bool accepted = !provider->onToken
        || provider->onToken(module.c_str(), token.c_str(), provider->userData) == LP_OK;
    if (accepted && !module.empty() && !token.empty()) {
        std::lock_guard<std::mutex> lock(provider->mutex);
        provider->inbound[module] = token;
    }
    return accepted;
}

// A deferred LogosResult method answers with the pending sentinel; the
// completion that carries its result must carry the user type too.
void rememberPendingResult(lp_provider* provider, const json& result, const std::string& type)
{
    if (!isLogosResultType(type) || !result.is_object() || result.size() != 1) return;
    const auto found = result.find(kPendingKey);
    if (found == result.end() || !found->is_string() || found->get<std::string>().empty()) return;
    std::lock_guard<std::mutex> lock(provider->mutex);
    provider->pendingResultTypes[found->get<std::string>()] = type;
}

Variant providerInvoke(lp_provider* provider, bool handshake,
                       std::int32_t index, const std::vector<Variant>& arguments)
{
    if (handshake || index == 3) {
        if (arguments.size() != 3 || !arguments[0].value.isString()
            || !arguments[1].value.isString() || !arguments[2].value.isString())
            return Variant::fromRpc(RpcValue{false});
        return Variant::fromRpc(RpcValue{providerAcceptToken(provider,
            arguments[0].value.asString(), arguments[1].value.asString(),
            arguments[2].value.asString())});
    }
    if (index >= 4 && index <= 6) {
        const json metadata = providerMetadata(
            provider, index == 4 ? "methods" : index == 5 ? "events" : "interface");
        return Variant{MetaType::JsonArray, false,
                       jsonToRpc(metadata), {}};
    }
    if (index < 0 || index > 2 || arguments.size() < 2
        || !arguments[0].value.isString() || !arguments[1].value.isString()) return {};
    const std::string auth = arguments[0].value.asString();
    const std::string method = arguments[1].value.asString();
    if (arguments.size() >= 3 && arguments[2].opaque) return {};
    const json args = arguments.size() >= 3 ? rpcToJson(arguments[2].value) : json::array();
    if ((method == "getPluginMethods" || method == "getPluginEvents"
         || method == "getPluginInterface") && args.empty()) {
        const json metadata = providerMetadata(provider,
            method == "getPluginMethods" ? "methods"
            : method == "getPluginEvents" ? "events" : "interface");
        return Variant{MetaType::JsonArray, false,
                       jsonToRpc(metadata), {}};
    }
    const std::string caller = providerCaller(provider, auth);
    if (caller.empty())
        return Variant::fromRpc(jsonToRpc(json{{kStatusKey, "unauthorized"}}));
    if (!provider->dispatch) return {};
    const std::string argsText = dumpJson(args);
    const std::string previousCaller = std::move(gCurrentCaller);
    gCurrentCaller = caller;
    char* text = provider->dispatch(method.c_str(), argsText.c_str(), provider->userData);
    gCurrentCaller = previousCaller;
    if (!text) return {};
    const json result = json::parse(text, nullptr, false);
    lp_string_free(text);
    if (result.is_discarded()) return {};
    const std::string type = providerReturnType(provider, method);
    rememberPendingResult(provider, result, type);
    return resultVariant(result, type);
}


ResultMessage providerNetworkCall(lp_provider* provider,
                                  const CallMessage& request,
                                  const char* protocol)
{
    ResultMessage reply;
    reply.id = request.id;
    if (request.object != provider->moduleName) {
        reply.err = "object not published: " + request.object;
        reply.errCode = "MODULE_NOT_LOADED";
        return reply;
    }
    if (request.method == "informModuleToken") {
        if (request.args.size() != 2 || !request.args[0].isString()
            || !request.args[1].isString()) {
            reply.err = "invalid token arguments";
            reply.errCode = "INVALID_ARGUMENT";
            return reply;
        }
        reply.ok = true;
        reply.value = RpcValue{providerAcceptToken(provider, request.authToken,
            request.args[0].asString(), request.args[1].asString())};
        return reply;
    }
    if (!provider->registered) {
        reply.err = "object not published: " + request.object;
        reply.errCode = "MODULE_NOT_LOADED";
        return reply;
    }
    if (request.method == "getPluginMethods" || request.method == "getPluginEvents"
        || request.method == "getPluginInterface") {
        reply.ok = true;
        reply.value = jsonToRpc(providerMetadata(provider,
            request.method == "getPluginMethods" ? "methods"
            : request.method == "getPluginEvents" ? "events" : "interface"));
        return reply;
    }
    const std::string caller = providerCaller(provider, request.authToken, protocol);
    if (caller.empty()) {
        reply.ok = true;
        reply.value = jsonToRpc(json{{kStatusKey, "unauthorized"}});
        return reply;
    }
    json args = json::array();
    for (const auto& value : request.args) args.push_back(rpcToJson(value));
    const std::string argsText = dumpJson(args);
    const std::string previousCaller = std::move(gCurrentCaller);
    gCurrentCaller = caller;
    char* text = provider->dispatch(request.method.c_str(), argsText.c_str(),
                                    provider->userData);
    gCurrentCaller = previousCaller;
    if (!text) {
        reply.err = "method failed";
        reply.errCode = "METHOD_FAILED";
        return reply;
    }
    const json result = json::parse(text, nullptr, false);
    lp_string_free(text);
    if (result.is_discarded()) {
        reply.err = "invalid method result";
        reply.errCode = "METHOD_FAILED";
        return reply;
    }
    reply.ok = true;
    reply.value = jsonToRpc(result);
    return reply;
}

logos::plain::MethodsResultMessage providerNetworkMethods(
    lp_provider* provider, const logos::plain::MethodsMessage& request)
{
    logos::plain::MethodsResultMessage reply;
    reply.id = request.id;
    if (request.object != provider->moduleName || !provider->registered) {
        reply.err = "object not published";
        return reply;
    }
    const json metadata = providerMetadata(provider, "methods");
    for (const auto& entry : metadata) {
        if (!entry.is_object()) continue;
        logos::plain::MethodMetadata method;
        method.name = stringField(entry, "name");
        method.signature = stringField(entry, "signature");
        method.returnType = stringField(entry, "returnType");
        method.isInvokable = boolField(entry, "isInvokable", true);
        if (entry.contains("parameters") && entry["parameters"].is_array())
            for (const auto& parameter : entry["parameters"])
                method.parameters.items.push_back(jsonToRpc(parameter));
        reply.methods.push_back(std::move(method));
    }
    reply.ok = true;
    return reply;
}

} // namespace

namespace logos::plain::abi {
unsigned long long tokenComparisonCount()
{
    return gTokenComparisons.load(std::memory_order_relaxed);
}
} // namespace logos::plain::abi

extern "C" {

const char* lp_protocol_version(void) { return LOGOS_PROTOCOL_VERSION_STRING; }
int lp_protocol_abi_major(void) { return LOGOS_PROTOCOL_VERSION_MAJOR; }
void lp_string_free(char* value) { std::free(value); }
char* lp_string_copy(const char* value) { return value ? duplicate(value) : nullptr; }
const char* lp_current_caller_json(void) { return gCurrentCaller.c_str(); }

int lp_set_mode(const char* mode)
{
    if (!mode) return LP_ERR_INVALID_ARG;
    if (std::strcmp(mode, "remote") == 0) return LP_OK;
    if (std::strcmp(mode, "local") == 0 || std::strcmp(mode, "mock") == 0)
        return LP_ERR_UNSUPPORTED;
    return LP_ERR_INVALID_ARG;
}
const char* lp_get_mode(void) { return "remote"; }

int lp_set_default_transport(const char* transportJson)
try {
    if (!transportJson || !acceptsPlainConfig(transportJson)) return LP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(gDefaultMutex);
    gDefaultTransport = transportJson;
    return LP_OK;
} catch (...) {
    return LP_ERR_INTERNAL;
}

lp_client* lp_client_create(const char* targetModule, const char* originModule,
                            const char* targetTransportJson,
                            const char* capabilityTransportJson)
try {
    if (!targetModule || !*targetModule || !originModule
        || !acceptsPlainConfig(targetTransportJson)
        || !acceptsPlainConfig(capabilityTransportJson)) return nullptr;
    auto state = std::make_shared<ClientState>();
    state->target = targetModule;
    state->origin = originModule;
    state->targetConfig = parsePlainConfig(targetTransportJson);
    state->capabilityConfig = parsePlainConfig(capabilityTransportJson);
    state->tokens = tokensFor(originModule);
    std::weak_ptr<ClientState> weak = state;
    state->wire->setInternalEventHandler(
        [weak](const std::string& object, std::int32_t signal,
               const std::vector<Variant>& arguments) {
            auto state = weak.lock();
            if (!state || !state->alive || object != state->target
                || signal != 0 || arguments.size() != 2
                || !arguments[0].value.isString()
                || arguments[0].value.asString() != kCompletionEvent
                || arguments[1].opaque)
                return false;
            const json data = rpcToJson(arguments[1].value);
            if (!data.is_array() || data.size() != 2 || !data[0].is_string())
                return true;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->completions[data[0].get<std::string>()] = jsonToRpc(data[1]);
            }
            state->completionChanged.notify_all();
            return true;
        });
    state->wire->setEventHandler(
        [weak](const std::string& object, std::int32_t signal,
               std::vector<Variant> arguments) {
            auto state = weak.lock();
            if (!state) return;
            std::lock_guard<std::recursive_mutex> callbackLock(state->callbackMutex);
            if (!state->alive || object != state->target
                || signal != 0 || arguments.size() != 2
                || !arguments[0].value.isString() || arguments[1].opaque) return;
            const std::string event = arguments[0].value.asString();
            const json data = rpcToJson(arguments[1].value);
            std::vector<std::shared_ptr<SubscriptionState>> subscriptions;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                for (auto it = state->subscriptions.begin(); it != state->subscriptions.end();) {
                    if (auto sub = it->lock()) {
                        if (sub->active && sub->phase == SubscriptionState::Phase::Armed)
                            subscriptions.push_back(std::move(sub));
                        ++it;
                    }
                    else it = state->subscriptions.erase(it);
                }
            }
            const std::string text = dumpJson(data.is_array() ? data : json::array());
            for (const auto& sub : subscriptions) {
                std::lock_guard<std::recursive_mutex> subLock(sub->callbackMutex);
                if (sub->active && (sub->event.empty() || sub->event == event) && sub->callback) {
                    CallbackScope scope(state.get());
                    sub->callback(event.c_str(), text.c_str(), sub->userData);
                }
                if (!state->alive) break;
            }
        });
    state->wire->setDisconnectHandler([weak](const std::string&) {
        if (auto state = weak.lock()) connectionLost(state);
    });
    state->subscriptionWorker = std::thread([state] { subscriptionLoop(state); });
    if (state->targetConfig.protocol == LogosProtocol::Tcp
        || state->targetConfig.protocol == LogosProtocol::TcpSsl)
        state->eventWorker = std::thread([state] { networkEventLoop(state); });
    return new lp_client{std::move(state)};
} catch (...) {
    return nullptr;
}

void lp_client_destroy(lp_client* client)
{
    if (!client) return;
    const bool fromCallback = activeCallbackState == client->state.get();
    {
        std::lock_guard<std::recursive_mutex> lock(client->state->callbackMutex);
        client->state->alive = false;
    }
    {
        std::lock_guard<std::mutex> lock(client->state->mutex);
        client->state->workerStop = true;
    }
    {
        // Calls that have not started never will; running ones finish unseen.
        std::lock_guard<std::mutex> lock(client->state->asyncMutex);
        client->state->asyncStop = true;
        client->state->asyncTurn.notify_all();
        client->state->asyncCalls.clear();
    }
    client->state->asyncChanged.notify_all();
    client->state->completionChanged.notify_all();
    client->state->subscriptionChanged.notify_all();
    client->state->eventsChanged.notify_all();
    std::shared_ptr<RpcConnectionBase> networkWire;
    {
        std::lock_guard<std::timed_mutex> lock(client->state->connectionMutex);
        networkWire = std::move(client->state->networkWire);
    }
    if (networkWire) networkWire->stop("client destroyed");
    if (fromCallback)
        client->state->wire->closeWithoutWaitingForCallbacks();
    else
        client->state->wire->close();
    if (client->state->subscriptionWorker.joinable()) {
        if (fromCallback
            || client->state->subscriptionWorker.get_id() == std::this_thread::get_id())
            client->state->subscriptionWorker.detach();
        else
            client->state->subscriptionWorker.join();
    }
    if (client->state->eventWorker.joinable()) {
        if (fromCallback
            || client->state->eventWorker.get_id() == std::this_thread::get_id())
            client->state->eventWorker.detach();
        else
            client->state->eventWorker.join();
    }
    delete client;
}

int lp_invoke(lp_client* client, const char* method, const char* argsJson,
              int timeout, char** outResultJson, char** outErrorJson)
try {
    if (outResultJson) *outResultJson = nullptr;
    if (outErrorJson) *outErrorJson = nullptr;
    if (!client || !method || !*method) return LP_ERR_INVALID_ARG;
    const json args = json::parse(argsJson && *argsJson ? argsJson : "[]", nullptr, false);
    if (args.is_discarded() || !args.is_array()) {
        if (outErrorJson) *outErrorJson = duplicate(errorJson(
            "invalid_arg", "arguments must be a JSON array", client->state->target));
        return LP_ERR_INVALID_ARG;
    }
    std::string error;
    std::string code;
    auto result = invoke(client->state, method, args, timeoutMs(timeout), error, code);
    if (!result) {
        if (outErrorJson) *outErrorJson = duplicate(errorJson(code.c_str(), error, client->state->target));
        return LP_ERR_UNAVAILABLE;
    }
    if (outResultJson) *outResultJson = duplicate(dumpJson(rpcToJson(result->value)));
    return LP_OK;
} catch (...) {
    return LP_ERR_INTERNAL;
}

int lp_invoke_async(lp_client* client, const char* method, const char* argsJson,
                    int timeout, lp_result_cb callback, void* userData)
try {
    if (!client || !method || !*method || !callback) return LP_ERR_INVALID_ARG;
    const json args = json::parse(argsJson && *argsJson ? argsJson : "[]", nullptr, false);
    if (args.is_discarded() || !args.is_array()) return LP_ERR_INVALID_ARG;
    auto state = client->state;
    const std::string methodName = method;
    std::lock_guard<std::mutex> queueLock(state->asyncMutex);
    if (state->asyncStop) return LP_ERR_INVALID_ARG;
    const std::uint64_t ticket = state->asyncTickets++;
    std::function<void()> call = [state, ticket, methodName, args, timeout, callback, userData] {
        SendTurn turn{state, ticket};
        std::string error;
        std::string code = "timeout";
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeoutMs(timeout));
        std::optional<Variant> result;
        if (awaitSendTurn(state, ticket, deadline)) {
            const int left = std::max(0, static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count()));
            const std::function<void()> sent = [&turn] { turn.finish(); };
            result = invoke(state, methodName, args, left, error, code, &sent);
        } else {
            error = "invocation timed out behind earlier calls";
        }
        turn.finish();
        std::lock_guard<std::recursive_mutex> lock(state->callbackMutex);
        if (!state->alive) return;
        const std::string text = result
            ? dumpJson(rpcToJson(result->value))
            : errorJson(code.c_str(), error, state->target);
        CallbackScope scope(state.get());
        callback(result ? 1 : 0, text.c_str(), userData);
    };
    // The ticket is taken and queued under one lock, so tickets are dequeued in
    // order and the next one to go out always has a worker.
    state->asyncCalls.push_back(std::move(call));
    if (state->asyncIdle < state->asyncCalls.size()
        && state->asyncWorkers < kMaxAsyncWorkers) {
        try {
            std::thread([state] { asyncWorkerLoop(state); }).detach();
            ++state->asyncWorkers;
        } catch (...) {
            if (state->asyncWorkers == 0) {
                state->asyncCalls.pop_back();
                --state->asyncTickets;
                return LP_ERR_INTERNAL;
            }
        }
    }
    state->asyncChanged.notify_one();
    return LP_OK;
} catch (...) {
    return LP_ERR_INTERNAL;
}

lp_subscription* lp_subscribe(lp_client* client, const char* eventName,
                              lp_event_cb callback, void* userData)
try {
    if (!client || !eventName || !callback
        || std::strcmp(eventName, kCompletionEvent) == 0) return nullptr;
    auto state = std::make_shared<SubscriptionState>();
    state->event = eventName;
    state->callback = callback;
    state->userData = userData;
    {
        std::lock_guard<std::mutex> lock(client->state->mutex);
        client->state->subscriptions.push_back(state);
    }
    client->state->subscriptionChanged.notify_all();
    return new lp_subscription{client->state, std::move(state)};
} catch (...) {
    return nullptr;
}

int lp_client_set_subscription_status_cb(lp_client* client,
                                         lp_subscription_status_cb callback,
                                         void* userData)
{
    if (!client) return 0;
    // The replay may destroy the client; its state must outlive the callback.
    const std::shared_ptr<ClientState> state = client->state;
    int replay = 0;
    unsigned long long generation = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->statusCallback = callback;
        state->statusUserData = userData;
        generation = state->generation.load();
        if (callback) {
            if (hasSubscriptionPhase(state, SubscriptionState::Phase::Armed))
                replay = LP_SUB_ARMED;
            else if (hasSubscriptionPhase(state, SubscriptionState::Phase::Held))
                replay = LP_SUB_HELD;
        }
    }
    if (replay)
        reportSubscriptionStatus(state, replay, generation,
                                 replay == LP_SUB_HELD ? "provider_unavailable" : nullptr);
    return 1;
}

unsigned long long lp_client_subscription_generation(lp_client* client)
{
    return client ? client->state->generation.load() : 0;
}

int lp_client_set_subscription_options(lp_client* client, const char* optionsJson)
try {
    if (!client) return 0;
    // As the Qt C ABI: anything but a "manual" restart means automatic, and
    // only unparseable JSON is refused.
    bool manual = false;
    if (optionsJson && *optionsJson) {
        const json options = json::parse(optionsJson, nullptr, false);
        if (options.is_discarded()) return 0;
        manual = options.is_object() && stringField(options, "restart") == "manual";
    }
    {
        std::lock_guard<std::mutex> lock(client->state->mutex);
        client->state->manualRestart = manual;
    }
    return 1;
} catch (...) {
    return 0;
}

int lp_client_rearm_subscriptions(lp_client* client)
{
    if (!client) return 0;
    bool revived = false;
    {
        std::lock_guard<std::mutex> lock(client->state->mutex);
        for (auto it = client->state->subscriptions.begin();
             it != client->state->subscriptions.end();) {
            if (auto subscription = it->lock()) {
                if (subscription->active
                    && subscription->phase == SubscriptionState::Phase::Held) {
                    subscription->phase = SubscriptionState::Phase::Pending;
                    revived = true;
                }
                ++it;
            } else {
                it = client->state->subscriptions.erase(it);
            }
        }
    }
    if (revived) client->state->subscriptionChanged.notify_all();
    return revived ? 1 : 0;
}

void lp_unsubscribe(lp_subscription* subscription)
{
    if (!subscription) return;
    {
        std::lock_guard<std::recursive_mutex> lock(subscription->state->callbackMutex);
        subscription->state->active = false;
    }
    subscription->owner->subscriptionChanged.notify_all();
    delete subscription;
}

char* lp_pending_subscriptions(lp_client* client)
try {
    if (!client) return nullptr;
    json pending = json::array();
    std::lock_guard<std::mutex> lock(client->state->mutex);
    for (auto it = client->state->subscriptions.begin();
         it != client->state->subscriptions.end();) {
        if (auto subscription = it->lock()) {
            if (subscription->active
                && subscription->phase == SubscriptionState::Phase::Pending)
                pending.push_back(client->state->target + "::" + subscription->event);
            ++it;
        } else {
            it = client->state->subscriptions.erase(it);
        }
    }
    return duplicate(dumpJson(pending));
} catch (...) {
    return nullptr;
}

char* lp_get_methods(lp_client* client)
try {
    if (!client) return nullptr;
    const auto& state = client->state;
    std::string error;
    if (state->targetConfig.protocol == LogosProtocol::Tcp
        || state->targetConfig.protocol == LogosProtocol::TcpSsl) {
        std::string code;
        auto result = networkCall(state, state->target, "getPluginInterface", json::array(), {},
                                  kDefaultTimeoutMs, error, code);
        if (result) return duplicate(dumpJson(rpcToJson(*result)));
        if (code == "timeout" || code == "transport_error") return nullptr;
        // The provider answered: one built before logos-cpp-sdk #71 lacks it.
        const auto part = [&](const char* method) -> std::optional<json> {
            auto value = networkCall(state, state->target, method, json::array(), {},
                                     kDefaultTimeoutMs, error, code);
            if (!value) return std::nullopt;
            return rpcToJson(*value);
        };
        const auto legacy = legacyInterface(part("getPluginMethods"), part("getPluginEvents"));
        return legacy ? duplicate(dumpJson(*legacy)) : nullptr;
    }
    if (!ensureConnected(state, kDefaultTimeoutMs, error)) return nullptr;
    Client& wire = *state->wire;
    const auto timeout = std::chrono::milliseconds(kDefaultTimeoutMs);
    if (!wire.acquire(state->target, timeout, &error)) return nullptr;
    const auto definition = wire.definition(state->target);
    const auto has = [&](const char* signature) {
        return definition && std::any_of(definition->methodDefinitions.begin(),
            definition->methodDefinitions.end(),
            [&](const auto& method) { return method.signature == signature; });
    };
    if (!has("getPluginInterface()") && has("getPluginMethods()")) {
        const auto part = [&](const char* signature) -> std::optional<json> {
            if (!has(signature)) return json::array();
            auto value = wire.call(state->target, signature, {}, timeout, &error);
            if (!value) return std::nullopt;
            return rpcToJson(value->value);
        };
        const auto legacy = legacyInterface(part("getPluginMethods()"), part("getPluginEvents()"));
        return legacy ? duplicate(dumpJson(*legacy)) : nullptr;
    }
    auto result = wire.call(state->target, "getPluginInterface()", {}, timeout, &error);
    return result ? duplicate(dumpJson(rpcToJson(result->value))) : nullptr;
} catch (...) {
    return nullptr;
}

char* lp_token_get(const char* moduleName)
{
    if (!moduleName) return nullptr;
    const std::string token = tokenGet(gSharedTokens, moduleName);
    return token.empty() ? nullptr : duplicate(token);
}

int lp_token_save(const char* moduleName, const char* token)
{
    if (!moduleName || !*moduleName || !token || !*token) return LP_ERR_INVALID_ARG;
    tokenSave(gSharedTokens, moduleName, token);
    return LP_OK;
}

int lp_token_save_inbound(const char* caller, const char* token)
{
    if (!caller || !*caller || !token || !*token
        || std::strchr(caller, '\x1f')) return LP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(gSharedTokens->mutex);
    gSharedTokens->inbound[caller] = token;
    if (gHostServices.load() & kTokenRegistry) gSharedTokens->outbound[caller] = token;
    return LP_OK;
}

int lp_token_isolate_identity(const char* identity)
{
    if (!identity || !*identity) return LP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(gTokenMutex);
    if (gIdentityTokens.count(identity)) return LP_OK;
    if (gVendedIdentities.count(identity)) return LP_ERR_UNSUPPORTED;
    gIdentityTokens[identity] = std::make_shared<TokenStore>();
    return LP_OK;
}

int lp_token_identity_is_isolated(const char* identity)
{
    if (!identity) return LP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(gTokenMutex);
    return gIdentityTokens.count(identity) ? 1 : 0;
}

char* lp_token_get_for(const char* identity, const char* moduleName)
{
    if (!identity || !moduleName) return nullptr;
    const std::string token = tokenGet(tokensFor(identity), moduleName);
    return token.empty() ? nullptr : duplicate(token);
}

int lp_token_save_for(const char* identity, const char* moduleName, const char* token)
{
    if (!identity || !moduleName || !*moduleName || !token || !*token)
        return LP_ERR_INVALID_ARG;
    tokenSave(tokensFor(identity), moduleName, token);
    return LP_OK;
}

int lp_token_reset_identity(const char* identity)
{
    if (!identity) return LP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(gTokenMutex);
    const auto found = gIdentityTokens.find(identity);
    if (found == gIdentityTokens.end()) return LP_ERR_UNSUPPORTED;
    std::lock_guard<std::mutex> storeLock(found->second->mutex);
    found->second->outbound.clear();
    found->second->inbound.clear();
    found->second->credential.clear();
    return LP_OK;
}

int lp_token_adopt_credential(const char* identity, const char* credential)
{
    if (!identity || !*identity || !credential || !*credential) return LP_ERR_INVALID_ARG;
    std::shared_ptr<TokenStore> store;
    {
        std::lock_guard<std::mutex> lock(gTokenMutex);
        const auto found = gIdentityTokens.find(identity);
        if (found == gIdentityTokens.end()) return LP_ERR_UNSUPPORTED;
        store = found->second;
    }
    {
        std::lock_guard<std::mutex> lock(gSharedTokens->mutex);
        if (gSharedTokens->credential == credential) return LP_ERR_UNSUPPORTED;
    }
    tokenSave(store, "core", credential);
    return LP_OK;
}

char* lp_token_keys(void)
try {
    if (!(gHostServices.load() & kTokenRegistry)) return nullptr;
    json keys = json::array();
    std::lock_guard<std::mutex> lock(gSharedTokens->mutex);
    for (const auto& entry : gSharedTokens->outbound) keys.push_back(entry.first);
    return duplicate(dumpJson(keys));
} catch (...) {
    return nullptr;
}

// Always capability_module, as documented: through the client's own connection
// when that is its target, otherwise over the capability transport. Pushing a
// token at any other module is lp_inform_module_token_to's, behind its grant.
int lp_inform_module_token(lp_client* client, const char* authToken,
                           const char* moduleName, const char* token)
try {
    if (!client || !authToken || !moduleName || !token) return LP_ERR_INVALID_ARG;
    const auto& state = client->state;
    const bool own = state->target == "capability_module";
    const LogosTransportConfig& config = own ? state->targetConfig : state->capabilityConfig;
    const Variant args[] = {Variant::fromRpc(RpcValue{authToken}),
                            Variant::fromRpc(RpcValue{moduleName}),
                            Variant::fromRpc(RpcValue{token})};
    std::string error;
    if (config.protocol == LogosProtocol::Tcp || config.protocol == LogosProtocol::TcpSsl) {
        if (own) {
            if (!ensureConnected(state, kDefaultTimeoutMs, error)) return LP_ERR_INTERNAL;
            std::shared_ptr<RpcConnectionBase> wire;
            {
                std::lock_guard<std::timed_mutex> lock(state->connectionMutex);
                wire = state->networkWire;
            }
            wire->sendToken({authToken, moduleName, token});
            return LP_OK;
        }
        auto wire = logos::plain::abi::connect(
            config, std::chrono::milliseconds(kDefaultTimeoutMs), error, &state->alive);
        if (!wire) return LP_ERR_INTERNAL;
        wire->sendToken({authToken, moduleName, token});
        // A reply to a later frame means the token frame was read before the close.
        auto barrier = wire->sendMethods({wire->nextId(), authToken, "capability_module"});
        const bool read = barrier.wait_for(std::chrono::milliseconds(kDefaultTimeoutMs))
            == std::future_status::ready;
        wire->stop("capability token delivered");
        return read ? LP_OK : LP_ERR_INTERNAL;
    }
    std::optional<Variant> result;
    if (own) {
        std::string code;
        result = directCall(state, state->target, "informModuleToken(QString,QString,QString)",
                            {args[0], args[1], args[2]}, kDefaultTimeoutMs, error, code);
    } else {
        Client capability;
        if (!capability.connect(endpoint("capability_module"),
                                std::chrono::milliseconds(kDefaultTimeoutMs), &error))
            return LP_ERR_INTERNAL;
        result = capability.call("capability_module", "informModuleToken(QString,QString,QString)",
                                 {args[0], args[1], args[2]},
                                 std::chrono::milliseconds(kDefaultTimeoutMs), &error);
    }
    return result && result->value.isBool() && result->value.asBool() ? LP_OK : LP_ERR_INTERNAL;
} catch (...) {
    return LP_ERR_INTERNAL;
}

int lp_inform_module_token_to(lp_client*, const char* authToken,
                              const char* originModule, const char* moduleName,
                              const char* token, int timeout)
try {
    if (!(gHostServices.load() & kTokenDelivery)) return LP_ERR_UNSUPPORTED;
    if (!authToken || !originModule || !*originModule || !moduleName || !token)
        return LP_ERR_INVALID_ARG;
    std::string error;
    Client target;
    const int wait = timeoutMs(timeout);
    if (!target.connect(endpoint(originModule), std::chrono::milliseconds(wait), &error))
        return LP_ERR_INTERNAL;
    auto result = target.call(std::string(originModule) + "__handshake",
        "informModuleToken(QString,QString,QString)",
        {Variant::fromRpc(RpcValue{authToken}), Variant::fromRpc(RpcValue{moduleName}),
         Variant::fromRpc(RpcValue{token})}, std::chrono::milliseconds(wait), &error);
    if (!result) {
        result = target.call(originModule, "informModuleToken(QString,QString,QString)",
            {Variant::fromRpc(RpcValue{authToken}), Variant::fromRpc(RpcValue{moduleName}),
             Variant::fromRpc(RpcValue{token})}, std::chrono::milliseconds(wait), &error);
    }
    return result && result->value.isBool() && result->value.asBool() ? LP_OK : LP_ERR_INTERNAL;
} catch (...) {
    return LP_ERR_INTERNAL;
}

int lp_grant_host_services(const char* servicesJson)
try {
    unsigned services = 0;
    if (servicesJson && *servicesJson) {
        const json value = json::parse(servicesJson, nullptr, false);
        if (value.is_discarded() || !value.is_array()) return LP_ERR_INVALID_ARG;
        for (const auto& entry : value) {
            if (!entry.is_string()) return LP_ERR_INVALID_ARG;
            if (entry == "token_registry") services |= kTokenRegistry;
            else if (entry == "token_delivery") services |= kTokenDelivery;
            else return LP_ERR_INVALID_ARG;
        }
    }
    gHostServices = services;
    return LP_OK;
} catch (...) {
    return LP_ERR_INTERNAL;
}

lp_provider* lp_provider_create(const char* moduleName, const char* transportSetJson)
try {
    if (!moduleName || !*moduleName) return nullptr;
    // Refused, not served as local only: that hid a module's TCP listeners.
    if (transportSetJson && !logos::parseTransportSet(transportSetJson, nullptr)) return nullptr;
    auto* provider = new lp_provider();
    provider->moduleName = moduleName;
    provider->transportSetJson = transportSetJson ? transportSetJson : "[]";
    return provider;
} catch (...) {
    return nullptr;
}

void lp_provider_destroy(lp_provider* provider)
{
    if (!provider) return;
    // Stop waits for every outstanding connection handler before the callback
    // pointers and module-owned user data in `provider` can be destroyed.
    for (auto& endpoint : provider->networkEndpoints) endpoint->stop();
    provider->networkEndpoints.clear();
    provider->server.stop();
    delete provider;
}

int lp_provider_prepare(lp_provider* provider, lp_dispatch_cb dispatch,
                        lp_getmethods_cb getMethods, lp_token_cb onToken,
                        void* userData)
try {
    if (!provider || !dispatch || provider->prepared || provider->registered)
        return LP_ERR_INVALID_ARG;
    provider->dispatch = dispatch;
    provider->getMethods = getMethods;
    provider->onToken = onToken;
    provider->userData = userData;
    std::string error;
    auto transports = logos::transportSetFromJsonString(provider->transportSetJson);
    if (transports.empty()) {
        LogosTransportConfig local;
        local.protocol = LogosProtocol::QtRemotePlain;
        transports.push_back(local);
    }
    for (const auto& config : transports) {
        if (config.protocol == LogosProtocol::QtRemotePlain
            || config.protocol == LogosProtocol::LocalSocket) {
            if (provider->qtroStarted) continue;
            if (!provider->server.start(endpoint(provider->moduleName), &error)
                || !provider->server.publish({provider->moduleName + "__handshake",
                    logos::qt_remote_plain::moduleHandshakeProxyDefinition(),
                    [provider](std::int32_t index, const std::vector<Variant>& args) {
                        return providerInvoke(provider, true, index, args);
                    }}, &error)) {
                provider->server.stop();
                return LP_ERR_INTERNAL;
            }
            provider->qtroStarted = true;
            continue;
        }
        if (config.protocol != LogosProtocol::Tcp
            && config.protocol != LogosProtocol::TcpSsl) return LP_ERR_UNSUPPORTED;
        const char* label = config.protocol == LogosProtocol::Tcp ? "tcp" : "tcp_ssl";
        auto network = std::make_unique<logos::plain::abi::ServerEndpoint>(config,
            [provider, label](const CallMessage& request) {
                return providerNetworkCall(provider, request, label);
            },
            [provider](const logos::plain::MethodsMessage& request) {
                return providerNetworkMethods(provider, request);
            },
            [provider](const logos::plain::TokenMessage& request) {
                providerAcceptToken(provider, request.authToken,
                                    request.moduleName, request.token);
            });
        if (!network->start()) {
            for (auto& active : provider->networkEndpoints) active->stop();
            provider->networkEndpoints.clear();
            provider->server.stop();
            return LP_ERR_INTERNAL;
        }
        provider->networkEndpoints.push_back(std::move(network));
    }
    provider->prepared = true;
    return LP_OK;
} catch (...) {
    return LP_ERR_INTERNAL;
}

int lp_provider_register(lp_provider* provider, lp_dispatch_cb dispatch,
                         lp_getmethods_cb getMethods, lp_token_cb onToken,
                         void* userData)
try {
    if (!provider || !dispatch || provider->registered) return LP_ERR_INVALID_ARG;
    if (!provider->prepared) {
        const int prepared = lp_provider_prepare(
            provider, dispatch, getMethods, onToken, userData);
        if (prepared != LP_OK) return prepared;
    } else if (provider->dispatch != dispatch || provider->getMethods != getMethods
               || provider->onToken != onToken || provider->userData != userData) {
        return LP_ERR_INVALID_ARG;
    }
    cacheReturnTypes(provider);
    std::string error;
    if (provider->qtroStarted) {
        if (!provider->server.publish({provider->moduleName,
                logos::qt_remote_plain::moduleProxyDefinition(),
                [provider](std::int32_t index, const std::vector<Variant>& args) {
                    return providerInvoke(provider, false, index, args);
                }}, &error)) {
            return LP_ERR_INTERNAL;
        }
    }
    provider->registered = true;
    return LP_OK;
} catch (...) {
    return LP_ERR_INTERNAL;
}

int lp_provider_emit_event(lp_provider* provider, const char* eventName,
                           const char* dataJson)
try {
    if (!provider || !provider->registered || !eventName || !*eventName)
        return LP_ERR_INVALID_ARG;
    const json data = json::parse(dataJson && *dataJson ? dataJson : "[]", nullptr, false);
    if (data.is_discarded() || !data.is_array()) return LP_ERR_INVALID_ARG;
    std::string resultType;
    if (std::strcmp(eventName, kCompletionEvent) == 0 && data.size() == 2 && data[0].is_string()) {
        std::lock_guard<std::mutex> lock(provider->mutex);
        const auto pending = provider->pendingResultTypes.find(data[0].get<std::string>());
        if (pending != provider->pendingResultTypes.end()) {
            resultType = pending->second;
            provider->pendingResultTypes.erase(pending);
        }
    }
    std::vector<RpcValue> arguments;
    for (const auto& item : data) arguments.push_back(jsonToRpc(item));
    for (auto& endpoint : provider->networkEndpoints)
        endpoint->emit(provider->moduleName, eventName, arguments);
    if (!provider->qtroStarted) return LP_OK;
    Variant payload = jsonToVariant(data);
    if (!resultType.empty()) payload.nestedValues[1] = resultVariant(data[1], resultType);
    std::string error;
    return provider->server.emitSignal(provider->moduleName, 0,
        {Variant::fromRpc(RpcValue{std::string(eventName)}), std::move(payload)}, &error)
        ? LP_OK : LP_ERR_INTERNAL;
} catch (...) {
    return LP_ERR_INTERNAL;
}

int lp_provider_save_token(lp_provider* provider, const char* moduleName,
                           const char* token)
{
    if (!provider || !moduleName || !*moduleName || !token || !*token)
        return LP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(provider->mutex);
    provider->inbound[moduleName] = token;
    if (std::strcmp(moduleName, "core") == 0
        || std::strcmp(moduleName, "capability_module") == 0)
        provider->credential = token;
    return LP_OK;
}

int lp_provider_set_token_validator(lp_provider* provider,
                                    lp_validate_token_cb validate,
                                    void* userData)
{
    if (!provider) return LP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(provider->mutex);
    provider->validateToken = validate;
    provider->validatorUserData = userData;
    return LP_OK;
}

int lp_provider_set_max_concurrent_calls(lp_provider* provider, unsigned maxCalls)
{
    if (!provider) return LP_ERR_INVALID_ARG;
    provider->server.setMaxConcurrentCalls(maxCalls);
    return LP_OK;
}

} // extern "C"
