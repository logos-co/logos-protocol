#ifndef LOGOS_TESTS_CONNECTED_PAIR_H
#define LOGOS_TESTS_CONNECTED_PAIR_H

#include <boost/system/error_code.hpp>

#ifdef _WIN32
#include <boost/asio/ip/tcp.hpp>
#else
#include <boost/asio/local/connect_pair.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#endif

// Two connected stream sockets for an RpcConnection pair: socketpair() where
// there is one, a loopback TCP pair on Windows, which has none.
#ifdef _WIN32
using PairSocket = boost::asio::ip::tcp::socket;

inline void connectPair(PairSocket& a, PairSocket& b, boost::system::error_code& ec)
{
    boost::asio::ip::tcp::acceptor acceptor(b.get_executor(),
        {boost::asio::ip::address_v4::loopback(), 0});
    a.connect(acceptor.local_endpoint(), ec);
    if (!ec) acceptor.accept(b, ec);
    if (!ec) a.set_option(boost::asio::ip::tcp::no_delay(true), ec);
    if (!ec) b.set_option(boost::asio::ip::tcp::no_delay(true), ec);
}
#else
using PairSocket = boost::asio::local::stream_protocol::socket;

inline void connectPair(PairSocket& a, PairSocket& b, boost::system::error_code& ec)
{
    boost::asio::local::connect_pair(a, b, ec);
}
#endif

#endif
