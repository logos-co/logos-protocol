#include "logos_protocol.h"

#include "implementations/qt_remote_plain/qtro_transport.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
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

std::string errorJson(const char* code, const std::string& message,
                      const std::string& origin)
{
    return json{{"code", code}, {"message", message}, {"origin", origin}}.dump();
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
            // Canonical base64url decoder, accepting omitted padding.
            static constexpr signed char table[128] = {
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
                52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
                -1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,
                15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
                -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
                41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1};
            RpcBytes bytes;
            unsigned accumulator = 0;
            int bits = 0;
            for (unsigned char ch : found->get<std::string>()) {
                if (ch == '=') break;
                if (ch >= 128 || table[ch] < 0) return {};
                accumulator = (accumulator << 6) | static_cast<unsigned>(table[ch]);
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    bytes.data.push_back(static_cast<std::uint8_t>(accumulator >> bits));
                    accumulator &= (1u << bits) - 1u;
                }
            }
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

Variant resultVariant(const json& value)
{
    if (value.is_object() && value.size() == 3
        && value.contains("success") && value["success"].is_boolean()
        && value.contains("value") && value.contains("error")) {
        return Variant::logosResult(value["success"].get<bool>(),
                                    Variant::fromRpc(jsonToRpc(value["value"])),
                                    Variant::fromRpc(jsonToRpc(value["error"])));
    }
    return Variant::fromRpc(jsonToRpc(value));
}

std::string instanceId()
{
    if (const char* value = std::getenv("LOGOS_INSTANCE_ID"); value && *value)
        return value;
    std::random_device random;
    const auto stamp = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string value = std::to_string(stamp ^ random());
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

bool acceptsPlainConfig(const char* text)
{
    if (!text || !*text || std::strcmp(text, "null") == 0) return true;
    const json value = json::parse(text, nullptr, false);
    if (value.is_discarded() || !value.is_object()) return false;
    const std::string protocol = value.value("protocol", "qt_remote_plain");
    return protocol == "qt_remote_plain";
}

struct TokenStore {
    std::mutex mutex;
    std::map<std::string, std::string> outbound;
    std::map<std::string, std::string> inbound;
    std::string credential;
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
    std::recursive_mutex callbackMutex;
    std::mutex mutex;
    std::mutex connectionMutex;
    std::condition_variable completionChanged;
    std::condition_variable subscriptionChanged;
    std::shared_ptr<Client> wire = std::make_shared<Client>();
    std::string target;
    std::string origin;
    std::shared_ptr<TokenStore> tokens;
    std::vector<std::weak_ptr<SubscriptionState>> subscriptions;
    std::map<std::string, RpcValue> completions;
    std::atomic<bool> alive{true};
    std::atomic<unsigned long long> generation{0};
    lp_subscription_status_cb statusCallback = nullptr;
    void* statusUserData = nullptr;
    bool manualRestart = false;
    bool workerStop = false;
    std::thread subscriptionWorker;
};

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
    if (state->alive) callback(status, generation, reason, userData);
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

bool ensureConnected(const std::shared_ptr<ClientState>& state,
                     int timeout, std::string& error)
{
    std::lock_guard<std::mutex> connectionLock(state->connectionMutex);
    if (state->wire->isConnected()) return true;
    return state->wire->connect(endpoint(state->target),
                                std::chrono::milliseconds(timeout), &error);
}

void subscriptionLoop(const std::shared_ptr<ClientState>& state)
{
    auto retry = std::chrono::milliseconds(250);
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->subscriptionChanged.wait(lock, [&] {
                return state->workerStop
                    || hasSubscriptionPhase(state, SubscriptionState::Phase::Pending);
            });
            if (state->workerStop) return;
        }

        std::string error;
        bool acquired = ensureConnected(state, 250, error);
        if (acquired) {
            std::lock_guard<std::mutex> connectionLock(state->connectionMutex);
            acquired = state->wire->acquire(state->target, std::chrono::milliseconds(250),
                                             &error);
        }

        if (!acquired) {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (state->subscriptionChanged.wait_for(lock, retry, [&] {
                    return state->workerStop
                        || !hasSubscriptionPhase(state, SubscriptionState::Phase::Pending);
                })) {
                if (state->workerStop) return;
            }
            retry = std::min(retry * 2, std::chrono::milliseconds(5000));
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
            established = armedAny && !alreadyArmed;
            generation = established ? ++state->generation : state->generation.load();
        }
        retry = std::chrono::milliseconds(250);
        if (established)
            reportSubscriptionStatus(state, LP_SUB_ARMED, generation, nullptr);
    }
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
    }
    if (!lost) return;
    reportSubscriptionStatus(state, held ? LP_SUB_HELD : LP_SUB_LOST,
                             generation, "provider_unavailable");
    state->subscriptionChanged.notify_all();
}

std::optional<Variant> directCall(const std::shared_ptr<ClientState>& state,
                                  const std::string& object,
                                  const std::string& signature,
                                  std::vector<Variant> arguments,
                                  int timeout, std::string& error)
{
    if (object != state->target) {
        Client temporary;
        if (!temporary.connect(endpoint(object), std::chrono::milliseconds(timeout), &error))
            return std::nullopt;
        return temporary.call(object, signature, std::move(arguments),
                              std::chrono::milliseconds(timeout), &error);
    }
    if (!ensureConnected(state, timeout, error)) return std::nullopt;
    return state->wire->call(object, signature, std::move(arguments),
                             std::chrono::milliseconds(timeout), &error);
}

std::string mintToken(const std::shared_ptr<ClientState>& state,
                      int timeout, std::string& error)
{
    const std::string credential = tokenGet(state->tokens, "capability_module");
    Client capability;
    if (!capability.connect(endpoint("capability_module"),
                            std::chrono::milliseconds(timeout), &error)) return {};
    Variant requestArgs = Variant::fromRpc(RpcValue{RpcList{{RpcValue{state->origin},
                                                             RpcValue{state->target}}}});
    auto result = capability.call("capability_module",
        "callRemoteMethod(QString,QString,QVariantList)",
        {Variant::fromRpc(RpcValue{credential}),
         Variant::fromRpc(RpcValue{"requestModule"}), std::move(requestArgs)},
        std::chrono::milliseconds(timeout), &error);
    if (!result || !result->value.isString()) return {};
    const std::string token = result->value.asString();
    if (!token.empty()) tokenSave(state->tokens, state->target, token);
    return token;
}

std::optional<Variant> invoke(const std::shared_ptr<ClientState>& state,
                              const std::string& method, const json& args,
                              int timeout, std::string& error)
{
    if (!args.is_array()) {
        error = "arguments must be a JSON array";
        return std::nullopt;
    }
    std::string token = tokenGet(state->tokens, state->target);
    if (token.empty() && state->target != "capability_module")
        token = mintToken(state, timeout, error);
    Variant arguments = Variant::fromRpc(jsonToRpc(args));
    auto result = directCall(state, state->target,
        "callRemoteMethod(QString,QString,QVariantList)",
        {Variant::fromRpc(RpcValue{token}), Variant::fromRpc(RpcValue{method}),
         std::move(arguments)}, timeout, error);
    if (result && isUnauthorized(*result) && state->target != "capability_module") {
        {
            std::lock_guard<std::mutex> lock(state->tokens->mutex);
            state->tokens->outbound.erase(state->target);
        }
        token = mintToken(state, timeout, error);
        if (!token.empty()) {
            arguments = Variant::fromRpc(jsonToRpc(args));
            result = directCall(state, state->target,
                "callRemoteMethod(QString,QString,QVariantList)",
                {Variant::fromRpc(RpcValue{token}), Variant::fromRpc(RpcValue{method}),
                 std::move(arguments)}, timeout, error);
        }
    }
    if (result && isUnauthorized(*result)) {
        error = "token not recognized";
        return std::nullopt;
    }
    if (result) {
        std::string completion;
        if (pendingId(*result, completion)) {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (!state->completionChanged.wait_for(lock, std::chrono::milliseconds(timeout), [&] {
                    return !state->alive || state->completions.count(completion) != 0;
                })) {
                error = "deferred invocation timed out";
                return std::nullopt;
            }
            auto found = state->completions.find(completion);
            if (found == state->completions.end()) return std::nullopt;
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
std::mutex gDefaultMutex;
std::string gDefaultTransport = R"({"protocol":"qt_remote_plain"})";
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
    lp_dispatch_cb dispatch = nullptr;
    lp_getmethods_cb getMethods = nullptr;
    lp_token_cb onToken = nullptr;
    void* userData = nullptr;
    std::mutex mutex;
    std::map<std::string, std::string> inbound;
    std::string credential;
    bool registered = false;
};

namespace {

std::string providerCaller(lp_provider* provider, const std::string& token)
{
    if (token.empty()) return {};
    std::lock_guard<std::mutex> lock(provider->mutex);
    if (!provider->credential.empty() && provider->credential == token)
        return R"({"kind":"host"})";
    for (const auto& entry : provider->inbound)
        if (entry.second == token)
            return json{{"kind", "module"}, {"name", entry.first}}.dump();
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
        const bool event = entry.value("type", std::string{}) == "event";
        if ((requested == "events") == event) filtered.push_back(entry);
    }
    return filtered;
}

Variant providerInvoke(lp_provider* provider, bool handshake,
                       std::int32_t index, const std::vector<Variant>& arguments)
{
    if (handshake || index == 3) {
        if (arguments.size() != 3 || !arguments[0].value.isString()
            || !arguments[1].value.isString() || !arguments[2].value.isString())
            return Variant::fromRpc(RpcValue{false});
        const std::string auth = arguments[0].value.asString();
        const std::string module = arguments[1].value.asString();
        const std::string token = arguments[2].value.asString();
        bool trusted = false;
        {
            std::lock_guard<std::mutex> lock(provider->mutex);
            trusted = !provider->credential.empty() && provider->credential == auth;
            if (trusted && !module.empty() && !token.empty()) provider->inbound[module] = token;
        }
        if (!trusted) return Variant::fromRpc(RpcValue{false});
        const bool accepted = !provider->onToken
            || provider->onToken(module.c_str(), token.c_str(), provider->userData) == LP_OK;
        return Variant::fromRpc(RpcValue{accepted});
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
    const std::string argsText = args.dump();
    const std::string previousCaller = std::move(gCurrentCaller);
    gCurrentCaller = caller;
    char* text = provider->dispatch(method.c_str(), argsText.c_str(), provider->userData);
    gCurrentCaller = previousCaller;
    if (!text) return {};
    const json result = json::parse(text, nullptr, false);
    lp_string_free(text);
    return result.is_discarded() ? Variant{} : resultVariant(result);
}

} // namespace

extern "C" {

const char* lp_protocol_version(void) { return LOGOS_PROTOCOL_VERSION_STRING; }
int lp_protocol_abi_major(void) { return LOGOS_PROTOCOL_VERSION_MAJOR; }
void lp_string_free(char* value) { std::free(value); }
const char* lp_current_caller_json(void) { return gCurrentCaller.c_str(); }

int lp_set_mode(const char* mode)
{
    return mode && std::strcmp(mode, "remote") == 0 ? LP_OK : LP_ERR_UNSUPPORTED;
}
const char* lp_get_mode(void) { return "remote"; }

int lp_set_default_transport(const char* transportJson)
{
    if (!acceptsPlainConfig(transportJson)) return LP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(gDefaultMutex);
    gDefaultTransport = transportJson;
    return LP_OK;
}

lp_client* lp_client_create(const char* targetModule, const char* originModule,
                            const char* targetTransportJson,
                            const char* capabilityTransportJson)
{
    if (!targetModule || !*targetModule || !originModule
        || !acceptsPlainConfig(targetTransportJson)
        || !acceptsPlainConfig(capabilityTransportJson)) return nullptr;
    auto state = std::make_shared<ClientState>();
    state->target = targetModule;
    state->origin = originModule;
    state->tokens = tokensFor(originModule);
    std::weak_ptr<ClientState> weak = state;
    state->wire->setEventHandler(
        [weak](const std::string& object, std::int32_t signal,
               std::vector<Variant> arguments) {
            auto state = weak.lock();
            if (!state) return;
            std::lock_guard<std::recursive_mutex> callbackLock(state->callbackMutex);
            if (!state->alive || object != state->target
                || signal != 0 || arguments.size() != 2
                || !arguments[0].value.isString()) return;
            const std::string event = arguments[0].value.asString();
            const json data = rpcToJson(arguments[1].value);
            if (event == kCompletionEvent && data.is_array() && data.size() == 2
                && data[0].is_string()) {
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->completions[data[0].get<std::string>()] = jsonToRpc(data[1]);
                }
                state->completionChanged.notify_all();
                return;
            }
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
            const std::string text = (data.is_array() ? data : json::array()).dump();
            for (const auto& sub : subscriptions) {
                std::lock_guard<std::recursive_mutex> subLock(sub->callbackMutex);
                if (sub->active && (sub->event.empty() || sub->event == event) && sub->callback)
                    sub->callback(event.c_str(), text.c_str(), sub->userData);
                if (!state->alive) break;
            }
        });
    state->wire->setDisconnectHandler([weak](const std::string&) {
        if (auto state = weak.lock()) connectionLost(state);
    });
    state->subscriptionWorker = std::thread([state] { subscriptionLoop(state); });
    return new lp_client{std::move(state)};
}

void lp_client_destroy(lp_client* client)
{
    if (!client) return;
    {
        std::lock_guard<std::recursive_mutex> lock(client->state->callbackMutex);
        client->state->alive = false;
    }
    {
        std::lock_guard<std::mutex> lock(client->state->mutex);
        client->state->workerStop = true;
    }
    client->state->completionChanged.notify_all();
    client->state->subscriptionChanged.notify_all();
    client->state->wire->close();
    if (client->state->subscriptionWorker.joinable()) {
        if (client->state->subscriptionWorker.get_id() == std::this_thread::get_id())
            client->state->subscriptionWorker.detach();
        else
            client->state->subscriptionWorker.join();
    }
    delete client;
}

int lp_invoke(lp_client* client, const char* method, const char* argsJson,
              int timeout, char** outResultJson, char** outErrorJson)
{
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
    auto result = invoke(client->state, method, args, timeoutMs(timeout), error);
    if (!result) {
        if (outErrorJson) *outErrorJson = duplicate(errorJson(
            error == "token not recognized" ? "unauthorized" : "transport",
            error, client->state->target));
        return LP_ERR_UNAVAILABLE;
    }
    if (outResultJson) *outResultJson = duplicate(rpcToJson(result->value).dump());
    return LP_OK;
}

int lp_invoke_async(lp_client* client, const char* method, const char* argsJson,
                    int timeout, lp_result_cb callback, void* userData)
{
    if (!client || !method || !*method || !callback) return LP_ERR_INVALID_ARG;
    const json args = json::parse(argsJson && *argsJson ? argsJson : "[]", nullptr, false);
    if (args.is_discarded() || !args.is_array()) return LP_ERR_INVALID_ARG;
    auto state = client->state;
    const std::string methodName = method;
    std::thread([state, methodName, args, timeout, callback, userData] {
        std::string error;
        auto result = invoke(state, methodName, args, timeoutMs(timeout), error);
        std::lock_guard<std::recursive_mutex> lock(state->callbackMutex);
        if (!state->alive) return;
        const std::string text = result
            ? rpcToJson(result->value).dump()
            : errorJson(error == "token not recognized" ? "unauthorized" : "transport",
                        error, state->target);
        callback(result ? 1 : 0, text.c_str(), userData);
    }).detach();
    return LP_OK;
}

lp_subscription* lp_subscribe(lp_client* client, const char* eventName,
                              lp_event_cb callback, void* userData)
{
    if (!client || !eventName || !*eventName || !callback
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
}

int lp_client_set_subscription_status_cb(lp_client* client,
                                         lp_subscription_status_cb callback,
                                         void* userData)
{
    if (!client) return 0;
    int replay = 0;
    unsigned long long generation = 0;
    {
        std::lock_guard<std::mutex> lock(client->state->mutex);
        client->state->statusCallback = callback;
        client->state->statusUserData = userData;
        generation = client->state->generation.load();
        if (callback) {
            if (hasSubscriptionPhase(client->state, SubscriptionState::Phase::Armed))
                replay = LP_SUB_ARMED;
            else if (hasSubscriptionPhase(client->state, SubscriptionState::Phase::Held))
                replay = LP_SUB_HELD;
        }
    }
    if (replay)
        reportSubscriptionStatus(client->state, replay, generation,
                                 replay == LP_SUB_HELD ? "provider_unavailable" : nullptr);
    return 1;
}

unsigned long long lp_client_subscription_generation(lp_client* client)
{
    return client ? client->state->generation.load() : 0;
}

int lp_client_set_subscription_options(lp_client* client, const char* optionsJson)
{
    if (!client) return 0;
    json options = json::object();
    if (optionsJson && *optionsJson) {
        options = json::parse(optionsJson, nullptr, false);
        if (options.is_discarded() || !options.is_object()) return 0;
    }
    const std::string restart = options.value("restart", "automatic");
    if (restart != "automatic" && restart != "manual") return 0;
    {
        std::lock_guard<std::mutex> lock(client->state->mutex);
        client->state->manualRestart = restart == "manual";
    }
    return 1;
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
{
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
    return duplicate(pending.dump());
}

char* lp_get_methods(lp_client* client)
{
    if (!client) return nullptr;
    std::string error;
    if (!ensureConnected(client->state, kDefaultTimeoutMs, error)) return nullptr;
    auto result = client->state->wire->call(client->state->target, "getPluginInterface()", {},
                                             std::chrono::milliseconds(kDefaultTimeoutMs), &error);
    return result ? duplicate(rpcToJson(result->value).dump()) : nullptr;
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
{
    if (!(gHostServices.load() & kTokenRegistry)) return nullptr;
    json keys = json::array();
    std::lock_guard<std::mutex> lock(gSharedTokens->mutex);
    for (const auto& entry : gSharedTokens->outbound) keys.push_back(entry.first);
    return duplicate(keys.dump());
}

int lp_inform_module_token(lp_client* client, const char* authToken,
                           const char* moduleName, const char* token)
{
    if (!client || !authToken || !moduleName || !token) return LP_ERR_INVALID_ARG;
    std::string error;
    auto result = directCall(client->state, client->state->target,
        "informModuleToken(QString,QString,QString)",
        {Variant::fromRpc(RpcValue{authToken}), Variant::fromRpc(RpcValue{moduleName}),
         Variant::fromRpc(RpcValue{token})}, kDefaultTimeoutMs, error);
    return result && result->value.isBool() && result->value.asBool() ? LP_OK : LP_ERR_INTERNAL;
}

int lp_inform_module_token_to(lp_client*, const char* authToken,
                              const char* originModule, const char* moduleName,
                              const char* token, int timeout)
{
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
}

int lp_grant_host_services(const char* servicesJson)
{
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
}

lp_provider* lp_provider_create(const char* moduleName, const char* transportSetJson)
{
    if (!moduleName || !*moduleName) return nullptr;
    auto* provider = new lp_provider();
    provider->moduleName = moduleName;
    provider->transportSetJson = transportSetJson ? transportSetJson : "[]";
    return provider;
}

void lp_provider_destroy(lp_provider* provider) { delete provider; }

int lp_provider_register(lp_provider* provider, lp_dispatch_cb dispatch,
                         lp_getmethods_cb getMethods, lp_token_cb onToken,
                         void* userData)
{
    if (!provider || !dispatch || provider->registered) return LP_ERR_INVALID_ARG;
    provider->dispatch = dispatch;
    provider->getMethods = getMethods;
    provider->onToken = onToken;
    provider->userData = userData;
    std::string error;
    if (!provider->server.start(endpoint(provider->moduleName), &error))
        return LP_ERR_INTERNAL;
    if (!provider->server.publish({provider->moduleName,
            logos::qt_remote_plain::moduleProxyDefinition(),
            [provider](std::int32_t index, const std::vector<Variant>& args) {
                return providerInvoke(provider, false, index, args);
            }}, &error)
        || !provider->server.publish({provider->moduleName + "__handshake",
            logos::qt_remote_plain::moduleHandshakeProxyDefinition(),
            [provider](std::int32_t index, const std::vector<Variant>& args) {
                return providerInvoke(provider, true, index, args);
            }}, &error)) {
        provider->server.stop();
        return LP_ERR_INTERNAL;
    }
    provider->registered = true;
    return LP_OK;
}

int lp_provider_emit_event(lp_provider* provider, const char* eventName,
                           const char* dataJson)
{
    if (!provider || !provider->registered || !eventName || !*eventName)
        return LP_ERR_INVALID_ARG;
    const json data = json::parse(dataJson && *dataJson ? dataJson : "[]", nullptr, false);
    if (data.is_discarded() || !data.is_array()) return LP_ERR_INVALID_ARG;
    std::string error;
    return provider->server.emitSignal(provider->moduleName, 0,
        {Variant::fromRpc(RpcValue{std::string(eventName)}),
         Variant::fromRpc(jsonToRpc(data))}, &error) ? LP_OK : LP_ERR_INTERNAL;
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

} // extern "C"
