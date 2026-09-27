#ifndef LOGOS_PLAIN_IO_CONTEXT_POOL_H
#define LOGOS_PLAIN_IO_CONTEXT_POOL_H

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <memory>
#include <thread>

namespace logos::plain {

// -----------------------------------------------------------------------------
// IoContextPool — owns a single boost::asio::io_context and a worker thread
// that runs it until the pool is destroyed.
//
// One pool per SDK process is sufficient for our traffic (a handful of
// concurrent connections). If we ever need more parallelism, swap for a
// multi-thread pool (one io_context per thread + round-robin dispatch).
//
// Access the shared pool via `sharedPool()`; tests / special cases can
// construct their own.
// -----------------------------------------------------------------------------
class IoContextPool {
public:
    IoContextPool();
    ~IoContextPool();

    IoContextPool(const IoContextPool&) = delete;
    IoContextPool& operator=(const IoContextPool&) = delete;

    boost::asio::io_context& ioContext() { return m_ioc; }

    // Process-wide default pool: made on first use, never destroyed.
    static IoContextPool& shared();

private:
    boost::asio::io_context m_ioc;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> m_guard;
    std::thread m_worker;
};

} // namespace logos::plain

#endif // LOGOS_PLAIN_IO_CONTEXT_POOL_H
