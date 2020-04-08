/*
 * Copyright (C) 1996-2020 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#include "squid.h"
#include "CachePeer.h"
#include "comm/Connection.h"
#include "comm/ConnOpener.h"
#include "ResolvedPeers.h"
#include "SquidConfig.h"

ResolvedPeers::ResolvedPeers()
{
    if (Config.forward_max_tries > 0)
        paths_.reserve(Config.forward_max_tries);
}

void
ResolvedPeers::retryPath(const Comm::ConnectionPointer &path)
{
    debugs(17, 4, path);
    assert(path);
    // Cannot use candidatesToSkip for a faster (reverse) search because there
    // may be unavailable candidates past candidatesToSkip. We could remember
    // the last extraction index, but, to completely avoid a linear search,
    // extract*() methods should return the candidate index.
    const auto found = std::find_if(paths_.begin(), paths_.end(),
    [path](const ResolvedPeerPath &candidate) {
        return candidate.connection == path; // (refcounted) pointer comparison
    });
    assert(found != paths_.end());
    assert(!found->available);
    found->available = true;

    // if we restored availability of a candidate that we used to skip, update
    const auto candidatesToTheLeft = static_cast<size_type>(found - paths_.begin());
    if (candidatesToTheLeft < candidatesToSkip) {
        candidatesToSkip = candidatesToTheLeft;
    } else {
        // *found was unavailable so candidatesToSkip could not end at it
        Must(candidatesToTheLeft != candidatesToSkip);
    }
}

ConnectionList::size_type
ResolvedPeers::size() const
{
    return std::count_if(start(), paths_.end(),
    [](const ResolvedPeerPath &path) {
        return path.available;
    });
}

void
ResolvedPeers::addPath(const Comm::ConnectionPointer &path)
{
    paths_.emplace_back(path);
    Must(paths_.back().available); // no candidatesToSkip updates are needed
}

/// \returns the beginning iterator for any available-path search
ConnectionList::iterator
ResolvedPeers::start()
{
    Must(candidatesToSkip <= paths_.size());
    return paths_.begin() + candidatesToSkip; // may return end()
}

/// \returns the first available same-peer same-family address iterator or end()
/// \param foundOther (optional) filled with "found other-peer or other-family address"
ConnectionList::iterator
ResolvedPeers::findPrime(const Comm::Connection &currentPeer, bool *foundOther)
{
    const auto found = start();
    const auto foundNextOrSpare = found != paths_.end() &&
        (currentPeer.getPeer() != found->connection->getPeer() || // next peer
            ConnectionFamily(currentPeer) != ConnectionFamily(*found->connection));
    if (foundOther)
        *foundOther = foundNextOrSpare;
    return foundNextOrSpare ? paths_.end() : found;
}

/// \returns the first available same-peer different-family address iterator or end()
/// \param foundOther (optional) filled with "found other-peer address"
ConnectionList::iterator
ResolvedPeers::findSpare(const Comm::Connection &currentPeer, bool *foundOther)
{
    const auto primeFamily = ConnectionFamily(currentPeer);
    const auto found = std::find_if(start(), paths_.end(),
    [&](const ResolvedPeerPath &path) {
        if (!path.available)
            return false;
        if (primeFamily == ConnectionFamily(*path.connection))
            return false;
        return true; // found either spare or next peer
    });
    const auto foundNext = found != paths_.end() &&
        currentPeer.getPeer() != found->connection->getPeer();
    if (foundOther)
        *foundOther = foundNext;
    return foundNext ? paths_.end() : found;
}

/// \returns the first available same-peer address iterator or end()
/// If not found and there is an other-peer address, the optional *foundOther becomes true
ConnectionList::iterator
ResolvedPeers::findPeer(const Comm::Connection &currentPeer, bool *foundOther)
{
    const auto peerToMatch = currentPeer.getPeer();
    bool foundNext = false;
    const auto found = std::find_if(start(), paths_.end(),
    [&](const ResolvedPeerPath &path) {
        if (!path.available) // skip unavailable
            return false;
        // an other-peer address means that there are no current peer addresses left
        if (peerToMatch != path.connection->getPeer())
            foundNext = true;
        return true;
    });
    if (foundOther)
        *foundOther = foundNext;
    return foundNext ? paths_.end() : found;
}

Comm::ConnectionPointer
ResolvedPeers::extractFront()
{
    Must(!empty());
    return extractFound("first: ", start());
}

Comm::ConnectionPointer
ResolvedPeers::extractPrime(const Comm::Connection &currentPeer)
{
    const auto found = findPrime(currentPeer);
    if (found != paths_.end())
        return extractFound("same-peer same-family match: ", found);

    debugs(17, 7, "no same-peer same-family paths");
    return nullptr;
}

Comm::ConnectionPointer
ResolvedPeers::extractSpare(const Comm::Connection &currentPeer)
{
    const auto found = findSpare(currentPeer);
    if (found != paths_.end())
        return extractFound("same-peer different-family match: ", found);

    debugs(17, 7, "no same-peer different-family paths");
    return nullptr;
}

/// convenience method to finish a successful extract*() call
Comm::ConnectionPointer
ResolvedPeers::extractFound(const char *description, const ConnectionList::iterator &found)
{
    auto &path = *found;
    debugs(17, 7, description << path.connection);
    assert(path.available);
    path.available = false;

    // if we extracted the left-most available candidate, find the next one
    if (static_cast<size_type>(found - paths_.begin()) == candidatesToSkip) {
        while (++candidatesToSkip < paths_.size() && !paths_[candidatesToSkip].available) {}
    }

    return path.connection;
}

bool
ResolvedPeers::haveSpare(const Comm::Connection &currentPeer)
{
    return findSpare(currentPeer) != paths_.end();
}

/// a common code for all ResolvedPeers::doneWith*()
bool
ResolvedPeers::doneWith(const Comm::Connection &currentPeer, findSmthFun findSmth)
{
    bool foundOther = false;
    if ((*this.*findSmth)(currentPeer, &foundOther) != paths_.end())
        return false;
    return foundOther ? true : destinationsFinalized;
}

bool
ResolvedPeers::doneWithSpares(const Comm::Connection &currentPeer)
{
    return doneWith(currentPeer, &ResolvedPeers::findSpare);
}

bool
ResolvedPeers::doneWithPrimes(const Comm::Connection &currentPeer)
{
    return doneWith(currentPeer, &ResolvedPeers::findPrime);
}

bool
ResolvedPeers::doneWithPeer(const Comm::Connection &currentPeer)
{
    return doneWith(currentPeer, &ResolvedPeers::findPeer);
}

int
ResolvedPeers::ConnectionFamily(const Comm::Connection &conn)
{
    return conn.remote.isIPv4() ? AF_INET : AF_INET6;
}

std::ostream &
operator <<(std::ostream &os, const ResolvedPeers &peers)
{
    const auto size = peers.size();
    if (!size)
        return os << "[no paths]";
    return os << size << (peers.destinationsFinalized ? "" : "+") << " paths";
}

