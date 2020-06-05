/*
 * Copyright (C) 1996-2020 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#ifndef SQUID_RESOLVEDPEERS_H
#define SQUID_RESOLVEDPEERS_H

#include "base/RefCount.h"
#include "comm/forward.h"

#include <iosfwd>
#include <limits>
#include <utility>

class ResolvedPeerPath
{
public:
    explicit ResolvedPeerPath(const Comm::ConnectionPointer &conn) : connection(conn), available(true), dirty(false) {}

    Comm::ConnectionPointer connection; ///< (the address of) a path
    bool available; ///< whether this path may be used (i.e., has not been tried already)
    bool dirty; ///< whether this path was reused (i.e., retried)
};

class ResolvedPeer;

/// cache_peer and origin server addresses (a.k.a. paths)
/// selected and resolved by the peering code
class ResolvedPeers: public RefCountable
{
    MEMPROXY_CLASS(ResolvedPeers);

public:
    // ResolvedPeerPaths in addPath() call order
    typedef std::vector<ResolvedPeerPath> Paths;
    using size_type = Paths::size_type;
    typedef RefCount<ResolvedPeers> Pointer;

    static constexpr auto npos = std::numeric_limits<size_type>::max();

    ResolvedPeers();

    /// whether we lack any known candidate paths
    bool empty() const { return !availablePaths; }

    /// add a candidate path to try after all the existing paths
    void addPath(const Comm::ConnectionPointer &);

    /// re-inserts the previously extracted address into the same position
    void retryPath(const ResolvedPeer &);

    /// extracts and returns the first queued address
    ResolvedPeer extractFront();

    /// extracts and returns the first same-peer same-family address
    /// \returns nil if it cannot find the requested address
    ResolvedPeer extractPrime(const Comm::Connection &currentPeer);

    /// extracts and returns the first same-peer different-family address
    /// \returns nil if it cannot find the requested address
    ResolvedPeer extractSpare(const Comm::Connection &currentPeer);

    /// whether extractSpare() would return a non-nil path right now
    bool haveSpare(const Comm::Connection &currentPeer);

    /// whether extractPrime() returns and will continue to return nil
    bool doneWithPrimes(const Comm::Connection &currentPeer);

    /// whether extractSpare() returns and will continue to return nil
    bool doneWithSpares(const Comm::Connection &currentPeer);

    /// whether doneWithPrimes() and doneWithSpares() are true for currentPeer
    bool doneWithPeer(const Comm::Connection &currentPeer);

    /// the current number of candidate paths
    size_type size() const { return availablePaths; }

    /// whether all of the available candidate paths received from DNS
    bool destinationsFinalized = false;

    /// whether HappyConnOpener::noteCandidatesChange() is scheduled to fire
    bool notificationPending = false;

private:
    /// A find*() result: An iterator of the found path (or paths_.end()) and
    /// whether the "other" path was found instead.
    typedef std::pair<Paths::iterator, bool> Finding;

    /// The protocol family of the given path, AF_INET or AF_INET6
    static int ConnectionFamily(const Comm::Connection &conn);

    Paths::iterator start();
    Finding findSpare(const Comm::Connection &currentPeer);
    Finding findPrime(const Comm::Connection &currentPeer);
    Finding findPeer(const Comm::Connection &currentPeer);
    ResolvedPeer extractFound(const char *description, const Paths::iterator &found);
    Finding makeFinding(const Paths::iterator &found, bool foundOther);

    bool doneWith(const Finding &findings) const;

    void increaseAvailability();
    void decreaseAvailability();

    Paths paths_; ///< resolved addresses in (peer, family) order

    /// the number of leading paths_ elements that are all currently unavailable
    /// i.e. the size of the front paths_ segment comprised of unavailable items
    /// i.e. the position of the first available path (or paths_.size())
    size_type pathsToSkip = 0;

    /// the total number of currently available elements in paths_
    size_type availablePaths = 0;
};

/// An invasive reference-counting Comm::Connection pointer that also keeps an
/// (optional) ResolvedPeers position for the ResolvedPeers::retryPath() usage.
/// Reference counting mechanism is compatible with Comm::ConnectionPointer.
class ResolvedPeer
{
public:
    using size_type = ResolvedPeers::size_type;

    ResolvedPeer() = default;
    ResolvedPeer(nullptr_t): ResolvedPeer() {} ///< implicit nullptr conversion
    ResolvedPeer(const Comm::ConnectionPointer &conn, const size_type pos): connection_(conn), position_(pos) {}

    /* read-only pointer API; for Connection assignment, see finalize() */
    explicit operator bool() const { return static_cast<bool>(connection_); }
    Comm::Connection *operator->() const { assert(connection_); return connection_.getRaw(); }
    Comm::Connection &operator *() const { assert(connection_); return *connection_; }

    /// convenience conversion to Comm::ConnectionPointer
    operator const Comm::ConnectionPointer&() const { return connection_; }

    /// upgrade stored peer selection details with a matching actual connection
    void finalize(const Comm::ConnectionPointer &conn) { connection_ = conn; }

    /// whether our connection originated in ResolvedPeers
    bool returnable() const { return position_ != npos; }

private:
    static constexpr auto npos = ResolvedPeers::npos;

    /// half-baked, open, or failed Comm::Connection
    Comm::ConnectionPointer connection_;

    /// ResolvedPeers-maintained membership index (or npos)
    size_type position_ = npos;
    friend class ResolvedPeers;
};

/// summarized ResolvedPeers (for debugging)
std::ostream &operator <<(std::ostream &, const ResolvedPeers &);

#endif /* SQUID_RESOLVEDPEERS_H */

