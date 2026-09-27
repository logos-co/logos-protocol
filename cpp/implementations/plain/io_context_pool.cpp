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
    // Never destroyed. Made on first use, it would die before statics made earlier
    // (a module's impl) that close sessions at exit; and on Windows the worker is
    // already dead when a DLL's statics run, so ~io_context would wait forever.
    static IoContextPool* const pool = new IoContextPool;
    return *pool;
}

} // namespace logos::plain
