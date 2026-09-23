#ifndef LOGOS_PROTOCOL_PLAIN_TOKENS_H
#define LOGOS_PROTOCOL_PLAIN_TOKENS_H

// Internal, not installed: lets tests see that a provider compares every
// stored token however early one matches, as module_proxy.h does for Qt.
namespace logos::plain::abi {
unsigned long long tokenComparisonCount();
}

#endif
