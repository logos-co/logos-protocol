#ifndef LOGOS_PROTOCOL_PLAIN_NETWORK_H
#define LOGOS_PROTOCOL_PLAIN_NETWORK_H

#include "logos_transport_config.h"
#include "implementations/plain/incoming_call_handler.h"
#include "implementations/plain/rpc_connection.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>

#include <functional>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace logos::plain {

// A tls_tcp session's stream, and the connection over it.
using SslStream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;
using SslConnection = RpcConnection<SslStream>;

} // namespace logos::plain

namespace logos::plain::abi {

// Token delivery, revocation and metadata: served on a network endpoint's
// control lane, never queued behind a business call.
bool isControlMethod(const std::string& method);

// A place in the provider's dispatch order, taken on the I/O thread as a
// business call arrives, so calls start in the order they arrived however many
// workers run them. The call waits for its place at the dispatch gate; a place
// dropped unused gives its turn up.
struct GatePlace {
    virtual ~GatePlace() = default;
};
using GatePlaceSource = std::function<std::shared_ptr<GatePlace>()>;

// Where a network endpoint runs what its connections ask. Control jobs run in
// arrival order on one thread; business jobs start in arrival order on up to
// capacity() threads (the provider's dispatch gate still decides how many run
// at once). Idle business threads exit after a few seconds.
class EndpointWorkers {
public:
    explicit EndpointWorkers(std::function<std::size_t()> capacity);
    ~EndpointWorkers();
    EndpointWorkers(const EndpointWorkers&) = delete;
    EndpointWorkers& operator=(const EndpointWorkers&) = delete;

    void control(std::function<void()> job);
    void business(std::function<void()> job);
    // Finishes what is queued, then joins every thread. Jobs queued afterwards
    // are dropped.
    void stop();

private:
    void controlLoop();
    void businessLoop();

    std::function<std::size_t()> capacity_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> controlJobs_;
    std::deque<std::function<void()>> businessJobs_;
    bool stopped_ = false;
    std::condition_variable workersDone_;
    std::thread controlThread_;
    std::size_t businessWorkers_ = 0;
    std::size_t businessIdle_ = 0;
};

// Event sinks keyed object -> event -> connection: one sink per connection for
// each (object, event), as IncomingCallHandler requires.
class SinkTable {
public:
    void subscribe(const std::string& object, const std::string& event,
                   const void* connection, IncomingCallHandler::EventSink sink);
    void unsubscribe(const std::string& object, const std::string& event,
                     const void* connection);
    void dropConnection(const void* connection);
    void clear();
    // A reserved event goes only to subscribers that named it.
    void emit(const std::string& object, const std::string& event,
              const std::vector<RpcValue>& data);

private:
    std::mutex mutex_;
    std::map<std::string, std::map<std::string, std::map<const void*, IncomingCallHandler::EventSink>>>
        sinks_;
};

} // namespace logos::plain::abi

#endif
