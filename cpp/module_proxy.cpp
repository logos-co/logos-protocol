#include "module_proxy.h"
#include "logos_caller_scope.h"
#include "logos_provider_interface.h"
#include "token_manager.h"
#include "logos_rpc_status.h"
#include <QDebug>
#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <algorithm>
#include <atomic>
#include <string>

ModuleProxy::ModuleProxy(LogosProviderObject* provider, QObject* parent,
                         TokenManager* token_store)
    : QObject(parent)
    , m_provider(provider)
    , m_store(token_store ? token_store : &TokenManager::instance())
{
    if (m_provider) {
        m_provider->setEventListener([this](const QString& eventName, const QVariantList& data) {
            qDebug() << "[LogosProviderObject] ModuleProxy: forwarding event" << eventName << "as Qt signal";
            // Events may be fired from any thread (e.g. a module's worker/FFI
            // thread), but this object is the QtRemoteObjects source and must be
            // driven from its own thread. Emitting directly from a foreign
            // thread runs QtRO's source serialization there, racing the source
            // socket against a reply being sent from the source thread, which
            // can silently drop the reply.
            //
            // We *always* queue the emission to this object's own thread, never
            // emit inline — even for a same-thread caller. A module that emits an
            // event from inside an async-call-completion callback (e.g. a
            // gather/fan-out completion firing `balances_updated` from within the
            // `__logos_call_complete__` reply dispatch) is on the source thread,
            // so an AutoConnection would run QtRO's source serialization for the
            // event *re-entrantly*, while a reply is still being marshalled on the
            // same stack — corrupting the source and crashing (SIGSEGV). A queued
            // connection defers the emit to the next event-loop turn, after the
            // reply has been sent, so events and replies stay serialized on the
            // thread QtRO owns. Passing `this` as the context also cancels a
            // queued emission if this object is destroyed first.
            QMetaObject::invokeMethod(this, [this, eventName, data]() {
                emit eventResponse(eventName, data);
            }, Qt::QueuedConnection);
        });
        qDebug() << "[LogosProviderObject] ModuleProxy: created, wrapping LogosProviderObject"
                 << m_provider->providerName();
    }
}

ModuleProxy::~ModuleProxy()
{
    qDebug() << "ModuleProxy: destroyed";
}

bool ModuleProxy::saveToken(const QString& from_module_name, const QString& token)
{
    if (from_module_name.isEmpty()) {
        qWarning() << "ModuleProxy: Cannot save token with empty module name";
        return false;
    }
    if (token.isEmpty()) {
        qWarning() << "ModuleProxy: Cannot save empty token for module:" << from_module_name;
        return false;
    }

    m_tokens[from_module_name] = token;
    qDebug() << "ModuleProxy: Token saved for module:" << from_module_name;
    return true;
}

void ModuleProxy::setTokenValidator(TokenValidator validator)
{
    m_validator = std::move(validator);
}

// QtRO / local path: RemoteTransportHost only ever serves a local socket, so
// the wire is "local". Forwards to the transport-aware overload.
QVariant ModuleProxy::callRemoteMethod(const QString& authToken, const QString& methodName, const QVariantList& args)
{
    return callRemoteMethod(authToken, methodName, args, QStringLiteral("local"));
}

QVariant ModuleProxy::callRemoteMethod(const QString& authToken, const QString& methodName, const QVariantList& args, const QString& transportProtocol)
{
    if (!m_provider) {
        qWarning() << "ModuleProxy: Cannot call method on null provider:" << methodName;
        return QVariant();
    }

    if (methodName.isEmpty()) {
        qWarning() << "ModuleProxy: Method name cannot be empty";
        return QVariant();
    }

    if (methodName == "getPluginMethods" && args.isEmpty()) {
        return QVariant(getPluginMethods());
    }

    if (methodName == "getPluginEvents" && args.isEmpty()) {
        return QVariant(getPluginEvents());
    }

    if (methodName == "getPluginInterface" && args.isEmpty()) {
        return QVariant(getPluginInterface());
    }
    // NOTE: the three getPlugin* introspection calls above intentionally stay
    // ungated. They expose only the method/event signatures (no business logic
    // or state) and are needed before any token exists — a caller discovers a
    // module's interface as part of the connection handshake, ahead of the
    // capability_module token exchange. Everything past this point is a real
    // business-method dispatch and MUST be authorized.

    std::string callerJson;
    if (!authorize(authToken, transportProtocol, &callerJson)) {
        qWarning() << "ModuleProxy: rejecting unauthorized call to" << methodName
                   << "- auth token not recognized";
        // Structured rejection instead of a bare QVariant() so a NEW consumer can
        // drop its stale token and re-exchange (see logos_rpc_status.h /
        // LogosAPIClient::invokeRemoteMethod). OLD consumers convert this to the
        // same empty/default they already got from QVariant(), so it's backward
        // compatible.
        return logos::makeUnauthorizedSentinel();
    }

    // SECURITY: never log call arguments — they routinely carry secrets
    // (mnemonics, passwords, tokens, key material). Log only the method name and
    // the argument count, matching the other transport call sites.
    qDebug() << "ModuleProxy: callRemoteMethod" << methodName << "args:" << args.size();

    // WHO IS CALLING, for the duration of this dispatch and no longer.
    //
    // Opened here — after authorization, immediately before the vtable hop into
    // the provider — because authorization is the only place that ever knows the
    // answer, and the dispatch is the only frame the answer is true for. The
    // scope closes on every path out of this function, including an exception
    // thrown from a handler.
    //
    // ON THIS THREAD ONLY. The value lives in a thread-local, so a provider that
    // hands the call to a worker (concurrency:"multi") carries it across itself:
    // the generated glue pulls the document HERE, on this thread, before it
    // captures anything into the worker. There is nothing process-global to
    // clobber, which is the entire reason this is not a dynamic property on a
    // host QObject — two overlapping "multi" calls from different callers would
    // share one slot and the second would silently rename the first.
    //
    // The value does NOT reach the module image by itself. The glue pulls it
    // back out by name (LogosAPI::currentCallerJson, logos-plugin-qt) and pushes
    // it across the module-impl C ABI; see logos_caller_scope.h for why a single
    // thread-local cannot span the two images.
    logos::CallerScope callerScope(std::move(callerJson));

    const QVariant result = m_provider->callMethod(methodName, args);

    // Module identity, for a provider whose own dispatch does not answer it.
    //
    // A module built through the LIDL frontend has name()/version() generated
    // into its dispatch, so it never reaches here. A legacy module derives no
    // contract and has neither — yet every provider already knows both, via the
    // providerName()/providerVersion() vtable slots the interface has always
    // had. Answering from those makes identity uniform across every module in
    // the fleet without touching a single one of them.
    //
    // Placed AFTER dispatch, deliberately: an invalid QVariant is this slot's
    // "unknown method" answer, so a provider that DOES implement name() keeps
    // its own result and nothing existing changes behaviour. Gated on an empty
    // argument list so a same-named method taking arguments is untouched.
    if (!result.isValid() && args.isEmpty()) {
        if (methodName == QLatin1String("name"))
            return QVariant(m_provider->providerName());
        if (methodName == QLatin1String("version"))
            return QVariant(m_provider->providerVersion());
    }
    return result;
}

namespace {

// See logos::tokenComparisonCount() in module_proxy.h for what this is and is
// not. Relaxed: the tests that read it do so after the scans they measure have
// returned on the same thread, so there is nothing to order against.
std::atomic<unsigned long long> g_tokenComparisons{0};

// note: this is to ensure comparison is constant time to prevent timing attacks
// Length-independent constant-time comparison of two tokens. Returns true only
// when both byte sequences are identical. We compare over the longer of the two
// lengths (folding any length difference into the result) so the running time
// does not reveal a correct prefix or the secret's length.
bool constantTimeEquals(const QString& a, const QString& b)
{
    g_tokenComparisons.fetch_add(1, std::memory_order_relaxed);
    const QByteArray ba = a.toUtf8();
    const QByteArray bb = b.toUtf8();
    const int n = std::max(ba.size(), bb.size());
    // A different length is a mismatch, but keep scanning to stay constant-time.
    int diff = ba.size() ^ bb.size();
    for (int i = 0; i < n; ++i) {
        const unsigned char ca = i < ba.size() ? static_cast<unsigned char>(ba[i]) : 0;
        const unsigned char cb = i < bb.size() ? static_cast<unsigned char>(bb[i]) : 0;
        diff |= (ca ^ cb);
    }
    return diff == 0;
}

// ── recovering the matched key WITHOUT reintroducing a data-dependent branch ─
//
// 64 is not a limit on module names. It is the width at which recovering one
// stops being free. A key longer than this still AUTHORIZES exactly as before —
// that is isAuthorized's business and it is untouched — it simply cannot be
// NAMED, and Unknown is both the fail-closed answer and the honest one.
constexpr int kCallerKeyMax = 64;

struct CallerFold {
    unsigned char key[kCallerKeyMax] = {0};
    unsigned char keyLen = 0;

    // Merge one candidate NAME. `match` is 1 iff the presented token equalled
    // this entry's token.
    //
    // NOTHING BELOW IS CONDITIONAL ON `match`: the mask selects, so the work
    // done is a function of the STORE SIZE only — never of where the match is,
    // nor of whether there was one. An `if (match) { copy; return; }` here would
    // undo, in three lines, the property constantTimeEquals spends a full
    // length-independent scan to provide, and it would look like an
    // optimisation while doing it.
    void offer(int match, const QByteArray& candKey)
    {
        const unsigned char m = static_cast<unsigned char>(-(match & 1)); // 0x00 | 0xFF
        // candKey.size() is the length of a STORE KEY — a module name, public —
        // not of a secret, so branching on it leaks nothing. Hoisted out of the
        // loop so the fold itself stays branch-free.
        const int n = (candKey.size() <= kCallerKeyMax) ? static_cast<int>(candKey.size()) : 0;
        for (int i = 0; i < kCallerKeyMax; ++i) {
            const unsigned char c = (i < n) ? static_cast<unsigned char>(candKey[i]) : 0;
            key[i] = static_cast<unsigned char>((key[i] & ~m) | (c & m));
        }
        keyLen = static_cast<unsigned char>((keyLen & ~m) |
                                            (static_cast<unsigned char>(n) & m));
    }

    std::string name() const
    {
        return std::string(reinterpret_cast<const char*>(key), keyLen);
    }
};

// ── THE AUTHORIZATION SCAN, GIVEN ONLY WHAT IT MAY SEE ───────────────────────
//
// Free function, and the parameter list is the mechanism. It takes an
// InboundView and the credential VALUE — never the TokenManager they came from
// — so the outbound accessors are not the discouraged choice in here, they are
// unnameable. The bug this whole split removes was one line of "and also scan
// the store", written by someone who had a TokenManager* in scope; nobody in
// this function does.
//
// WHAT THE THREE SOURCES ARE, and why only one of them contributes a NAME:
//
//   (1) proxyInbound  ModuleProxy::m_tokens — caller-keyed by construction
//                     (saveToken / informModuleToken are its only writers).
//                     The naming oracle.
//   (2) storeInbound  the provider identity's own inbound record. Also
//                     caller-keyed, and it authorizes — but it offers NO name,
//                     because in the Qt stack the same (caller, token) pair is
//                     in BOTH (1) and (2): ModuleProxy::informModuleToken
//                     records one and the provider it forwards to records the
//                     other. Folding both would make moduleHits == 2 for every
//                     ordinary caller and collapse every honest answer to
//                     Unknown. Losing a name that (1) already has costs
//                     nothing; double-counting would cost all of them.
//   (3) credential    this identity's own host-issued anchor. One value, no
//                     key, so there is nothing here that could be turned into
//                     a name even by accident — which is the point of it being
//                     a scalar.
//
// CONSTANT TIME. Every entry is compared exactly once, with no early exit and
// no `break`, and the credential is compared exactly once whether or not it is
// set (the emptiness test masks the RESULT, it does not skip the comparison).
// So the number of constantTimeEquals calls is |proxyInbound| + |storeInbound|
// + 1 — a function of the store sizes alone, never of the presented token, of
// where a match sits, or of whether there was one.
// logos::tokenComparisonCount() lets InboundTokenStore.
// TheComparisonCountDependsOnStoreSizeOnly say so out loud.
struct ScanOutcome {
    bool authorized = false;
    unsigned moduleHits = 0;   // matches in the caller-keyed naming oracle
    unsigned anchorHits = 0;   // matches on this identity's own credential
    CallerFold fold;
};

ScanOutcome scanIssuedTokens(const QString& authToken,
                             const QHash<QString, QString>& proxyInbound,
                             TokenManager::InboundView storeInbound,
                             const QString& credential)
{
    ScanOutcome out;

    for (auto it = proxyInbound.constBegin(); it != proxyInbound.constEnd(); ++it) {
        const int match = constantTimeEquals(authToken, it.value()) ? 1 : 0;
        out.authorized |= (match != 0);
        out.fold.offer(match, it.key().toUtf8());
        out.moduleHits += static_cast<unsigned>(match);
    }

    // No fold.offer() below, and that absence is deliberate — see (2) above.
    for (const QString& caller : storeInbound.keys()) {
        const int match = constantTimeEquals(authToken, storeInbound.token(caller)) ? 1 : 0;
        out.authorized |= (match != 0);
    }

    // Always compared, never skipped: `present` masks the answer rather than
    // the work, so an empty credential costs the same comparison a set one
    // does. authToken is already known non-empty at the call site, so an unset
    // credential could not match anyway; the mask is there so the count does
    // not depend on the store's state either.
    {
        const int present = credential.isEmpty() ? 0 : 1;
        const int match   = constantTimeEquals(authToken, credential) ? 1 : 0;
        out.authorized |= ((match & present) != 0);
        out.anchorHits += static_cast<unsigned>(match & present);
    }

    return out;
}

} // namespace

namespace logos {
unsigned long long tokenComparisonCount()
{
    return g_tokenComparisons.load(std::memory_order_relaxed);
}
} // namespace logos

bool ModuleProxy::informModuleToken(const QString& authToken, const QString& moduleName, const QString& token)
{
    if (!m_provider) {
        qWarning() << "ModuleProxy: Cannot inform token on null provider";
        return false;
    }

    // The anchor comes from THIS PROXY'S store, not the ambient ring — the same
    // store isAuthorized scans, so the proxy has exactly one notion of who it
    // trusts. Identical objects until a host isolates the provider's identity.
    //
    // A HOST THAT PASSES AN ISOLATED STORE MUST INSTALL THAT IDENTITY'S OWN
    // CREDENTIAL IN IT (TokenManager::adoptCredentialFor / lp_token_adopt_
    // credential), because a private store is now created EMPTY. It must NOT be
    // the host's anchor: capability_module pushes to a provider identity using
    // getToken(moduleName), which is that identity's own credential, so the gate
    // below still passes on the identity's own value and no longer requires a
    // copy of the host's. logos-plugin-qt's LogosAPIProvider::
    // seedHandshakeTrustAnchor does exactly this for a module IMAGE, writing the
    // host-issued `authToken` property under both keys; logos::admitConsumer
    // does it for an in-process consumer.
    //
    // ONE GAP SURVIVES, and it is here rather than in either of those:
    // seedHandshakeTrustAnchor still writes to TokenManager::instance() BY NAME
    // (logos-plugin-qt cpp/logos_api_provider.cpp:185-190), and is the last
    // site that spells these two key strings itself. Against an isolated store
    // it therefore seeds the wrong object — invisibly, because the write
    // succeeds and only the read comes up empty. It is unreached today: that
    // function runs in a module IMAGE, whose ring is the process ring. It stops
    // being unreached the moment a provider identity is isolated in-process.
    //
    // ONE credential, not two key reads. TokenManager::credential() is the
    // value adoptCredential() installs under every bootstrapKeys() key, so this
    // asks for the anchor BY THE THING IT IS instead of by looking two module-
    // shaped names up in a map — which is what a store that also holds outbound
    // per-target tokens made dangerous. The two keys never disagree in any
    // production store: every writer in the fleet sets both from one value
    // (module_initializer.cpp:169-170, seedHandshakeTrustAnchor,
    // lidl_gen_cdylib_glue.cpp:412-413, LpBridge::syncFromApi, ui-host
    // main.cpp:212-213).
    const QString credential = m_store->credential();
    const bool callerIsTrusted =
        !credential.isEmpty() && constantTimeEquals(authToken, credential);
    if (authToken.isEmpty() || !callerIsTrusted) {
        qWarning() << "ModuleProxy: rejecting informModuleToken for" << moduleName
                   << "- caller is not the trusted core/capability_module channel";
        return false;
    }

    if (moduleName.isEmpty()) {
        qWarning() << "ModuleProxy: Cannot inform token with empty module name";
        return false;
    }
    if (token.isEmpty()) {
        qWarning() << "ModuleProxy: Cannot inform empty token for module:" << moduleName;
        return false;
    }

    // Forward FIRST, record only what the provider accepted.
    //
    // Recording before the forward was the other candidate, on the theory that a
    // module might call back into us from inside the push and be rejected with a
    // token we had already decided to accept. That window does not exist: the
    // push reaches module code only as far as a store write
    // (LogosProviderBase::informModuleToken -> TokenManager::saveInboundToken,
    // and for a cdylib logos_module_accept_token -> lp_token_save), which calls
    // nothing back. Absent a real window, mirroring the provider's
    // verdict is the smaller claim, so it is the one to make.
    if (!m_provider->informModuleToken(moduleName, token)) {
        return false;
    }

    // WHY THE PROXY KEEPS ITS OWN COPY of something the provider just stored.
    // authorize() also scans m_store's INBOUND half, where the provider's write
    // lands — so on the happy path this is redundant. It is not redundant where
    // it counts. m_tokens is the naming oracle (scanIssuedTokens takes a name
    // from it and from nothing else), it survives a store this proxy does not
    // control being emptied under it (TokenManager::resetIdentity() clears an
    // isolated store on plugin reload), and it is the record that exists even
    // when the provider writes nowhere at all.
    //
    // DELIBERATELY NOT ALSO m_store->saveInboundToken() here. One map, one
    // writer: the provider owns its store's inbound record and this proxy owns
    // m_tokens. A second writer would put the same fact in two halves of two
    // objects with different lifetimes, which is the shape that rots.
    //
    // NOT a claim that a refused push leaves the token unusable. The generated
    // Qt glue saves to the host stack BEFORE it forwards across the C ABI and
    // returns hostOk && implOk, so a cdylib-side failure returns false with the
    // host store already holding the token. All this ordering guarantees is that
    // the proxy adds no grant of its own to a push the provider rejected.
    saveToken(moduleName, token);
    return true;
}

bool ModuleProxy::isAuthorized(const QString& authToken, const QString& transportProtocol) const
{
    return authorize(authToken, transportProtocol, /*callerJson=*/nullptr);
}

bool ModuleProxy::authorize(const QString& authToken, const QString& transportProtocol,
                            std::string* callerJson) const
{
    // Unknown is SPELLED before anything else can go wrong, so every early
    // return below leaves a valid document behind rather than an empty string
    // that a reader would have to interpret.
    if (callerJson) *callerJson = logos::callerUnknownJson();

    // Fail closed: an empty token is never valid, even if some empty value
    // somehow ended up in a token store.
    if (authToken.isEmpty()) {
        return false;
    }

    // A token is valid only if THIS module actually issued it to some caller.
    // Three sources hold issued tokens, and every one of them is now
    // direction-pure:
    //   * m_tokens               — the proxy's own INBOUND record, keyed by
    //                              caller (saveToken / informModuleToken).
    //   * m_store->inbound()     — this provider identity's inbound record, the
    //                              tokens ITS provider was told to accept.
    //   * m_store->credential()  — this identity's own host-issued anchor.
    // We scan every issued token with a constant-time compare and never early
    // out, so neither a match position nor the number of issued tokens leaks
    // through timing.
    //
    // WHAT IS NO LONGER HERE, AND WHY THAT IS THE FIX. The scan used to walk
    // m_store's whole key set — which was one flat map holding OUTBOUND
    // per-target tokens as well. A token this module cached in order to CALL
    // peer_b therefore authorized peer_b to call US, so capability_module's one
    // minted value for <me -> peer_b> was silently also a grant for
    // <peer_b -> me>: no handshake, nothing logged, and the access policy at
    // capability_module_plugin.cpp:99-106 never consulted. The outbound half is
    // now a separate map, and scanIssuedTokens() is not given anything that can
    // reach it. tests/protocol/test_token_direction.cpp is the detector.
    //
    // m_store, NOT TokenManager::instance(): the store that authorizes has to be
    // the store the inbound writes go to. LogosProviderBase::informModuleToken
    // writes to LogosAPI::getTokenManager() == TokenManager::forIdentity(<own
    // name>), and hardcoding instance() here broke both ways the moment a host
    // isolated a provider identity — privately seeded tokens invisible (inbound
    // calls rejected with no diagnostic) AND every ambient token still accepted
    // (the escalation isolation exists to close). Identical objects for a name
    // nobody isolated, which is why neither half had ever been observed.
    //
    // ── WHAT THE FOLD ADDS TO THIS SCAN, AND WHAT IT DOES NOT ────────────────
    // Everything the caller-identity work adds is bookkeeping AFTER each
    // comparison has already happened — a mask-select into a fixed-width buffer
    // and two counter increments — so the comparison count, and with it the
    // property the constant-time compare exists to provide, is unchanged by
    // construction rather than by inspection. logos::tokenComparisonCount()
    // lets a test say so out loud. See scanIssuedTokens() above for the
    // count and for why only one of the three sources contributes a name.
    const ScanOutcome scan = scanIssuedTokens(authToken, m_tokens,
                                              m_store->inbound(),
                                              m_store->credential());
    bool authorized       = scan.authorized;
    const unsigned moduleHits = scan.moduleHits;   // matches in the INBOUND record
    const unsigned anchorHits = scan.anchorHits;   // matches on our own credential

    // Not one of our own issued tokens — give a host-installed validator the
    // chance to accept it for this transport. This is how operator-issued named
    // tokens (validated against the daemon's TokenStore, with expiry and
    // local_only enforced by `transportProtocol`) authorize a call without
    // being pre-registered in the in-process stores above.
    //
    // Such a call stays UNKNOWN. TokenValidator returns bool and nothing else,
    // so there is no name to be had; widening it to yield one is a
    // logos-logoscore-cli change and deliberately not part of this.
    if (!authorized && m_validator) {
        authorized = m_validator(authToken, transportProtocol);
    }
    if (!authorized) {
        return false;
    }

    // Decided AFTER the scan, on counters, in O(1).
    //
    // This part DOES branch on secret-derived values, and that is fine: it
    // reveals nothing the answer does not already carry, and the answer is
    // handed to the handler in a moment anyway.
    if (callerJson) {
        if (anchorHits > 0) {
            // The anchor wins a tie. A value that is both the host anchor and
            // some caller's inbound token is the host's; naming the module would
            // assert an identity the anchor's own ambiguity ("core" and
            // "capability_module" share one value) already forbids.
            *callerJson = logos::callerHostAnchorJson();
        } else if (moduleHits == 1 && scan.fold.keyLen > 0) {
            *callerJson = logos::callerModuleJson(scan.fold.name());
        }
        // Everything else stays Unknown, and each case is a real one:
        //   * zero name matches — a validator-accepted operator token, or a hit
        //     in the store's own inbound record that the proxy was never told
        //     about (the legacy QtProviderObject path, or a host that seeded
        //     the store directly).
        //   * two or more — two callers were issued the same token value.
        //     Impossible with UUIDs, but if it ever happens we do not get to
        //     pick one.
        //   * keyLen == 0 — the matched key is longer than kCallerKeyMax.
    }
    return true;
}

namespace {
// getMethods() returns the module's full interface — both methods and events,
// each tagged with a "type" ("method"/"event"). Split it back out. An entry
// with no "type" counts as a method, so modules built against the pre-events
// SDK (whose getMethods() contains no events) report zero events, not a crash.
QJsonArray filterInterface(const QJsonArray& interface, bool keepEvents)
{
    QJsonArray out;
    for (const QJsonValue& v : interface) {
        const bool isEvent =
            v.toObject().value(QStringLiteral("type")).toString() == QStringLiteral("event");
        if (isEvent == keepEvents) out.append(v);
    }
    return out;
}
} // namespace

QJsonArray ModuleProxy::getPluginInterface()
{
    if (!m_provider) return QJsonArray();

    qDebug() << "[LogosProviderObject] ModuleProxy: calling LogosProviderObject::getMethods()";
    QJsonArray iface = m_provider->getMethods();

    // Advertise module identity for a provider that does not list it itself.
    //
    // The dispatch fallback in callRemoteMethod answers name()/version() for
    // every module; without this, a legacy module would ANSWER them while `lm`
    // and every untyped caller reported it had no such method — present to
    // whoever already knew to ask, invisible to everyone else. The two have to
    // agree, so they are derived from the same providerName()/providerVersion().
    //
    // Additive only: an entry the provider already lists wins, so a module with
    // a generated (or hand-written) name() keeps its own description, signature
    // and parameters.
    auto lists = [&iface](QLatin1String name) {
        for (const QJsonValue& v : iface)
            if (v.isObject() && v.toObject().value("name").toString() == name)
                return true;
        return false;
    };
    // Signatures only -- this listing describes the interface, it does not
    // carry values. The VALUES come from the same two provider accessors in
    // callRemoteMethod, which is what keeps the listing and the answer in step.
    const struct { QLatin1String name; const char* desc; } identity[] = {
        { QLatin1String("name"),    "The module's name, as declared in its metadata." },
        { QLatin1String("version"), "The module's version, as declared in its metadata." },
    };
    for (const auto& id : identity) {
        if (lists(id.name)) continue;
        QJsonObject entry;
        entry["name"] = QString(id.name);
        entry["type"] = QStringLiteral("method");
        entry["signature"] = QString(id.name) + QStringLiteral("()");
        entry["returnType"] = QStringLiteral("QString");
        entry["isInvokable"] = true;
        entry["description"] = QString::fromLatin1(id.desc);
        iface.append(entry);
    }
    return iface;
}

QJsonArray ModuleProxy::getPluginMethods()
{
    return filterInterface(getPluginInterface(), /*keepEvents=*/false);
}

QJsonArray ModuleProxy::getPluginEvents()
{
    return filterInterface(getPluginInterface(), /*keepEvents=*/true);
}

#include "moc_module_proxy.cpp"

// ── ModuleHandshakeProxy ─────────────────────────────────────────────────────

ModuleHandshakeProxy::ModuleHandshakeProxy(ModuleProxy* proxy, QObject* parent)
    : QObject(parent)
    , m_proxy(proxy)
{
}

bool ModuleHandshakeProxy::informModuleToken(const QString& authToken,
                                             const QString& moduleName,
                                             const QString& token)
{
    if (!m_proxy) {
        qWarning() << "ModuleHandshakeProxy: no module proxy to deliver the token for"
                   << moduleName;
        return false;
    }
    // Same authorization and same store as the business object — this is only a
    // different door onto it, reachable earlier.
    return m_proxy->informModuleToken(authToken, moduleName, token);
}
