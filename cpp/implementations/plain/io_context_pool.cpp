#include "io_context_pool.h"

namespace logos::plain {

IoContextPool::IoContextPool()
    : m_ioc()
    , m_guard(boost::asio::make_work_guard(m_ioc))
    , m_worker([this]{ m_ioc.run(); })
{
}

IoContextPool::~IoContextPool()
{
    // Drop the work guard so run() can return once all outstanding work
    // completes, then stop forcefully if something lingers.
    m_guard.reset();
    m_ioc.stop();
    if (m_worker.joinable())
        m_worker.join();
}

IoContextPool& IoContextPool::shared()
{
#ifdef _WIN32
    // Never destroyed on Windows: by the time a DLL's statics are destroyed the
    // worker has been killed, maybe mid-handler, and ~io_context would wait on it forever.
    static IoContextPool* pool = new IoContextPool;
    return *pool;
#else
    static IoContextPool pool;
    return pool;
#endif
}

} // namespace logos::plain
