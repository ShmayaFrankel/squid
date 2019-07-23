/*
 * Copyright (C) 1996-2019 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#ifndef _SQUID_SSL_PEEKER_H
#define _SQUID_SSL_PEEKER_H

#include "base/AsyncJob.h"
#include "base/CbcPointer.h"
#include "comm/forward.h"
#include "HttpRequest.h"
#include "ip/Address.h"
#include "security/forward.h"
#include "Store.h"

class ConnStateData;
class store_client;
class ClientHttpRequest;

namespace Ssl
{

/**
 * Maintains bump-server-first related information.
 */
class ServerBump
{
    CBDATA_CLASS(ServerBump);

public:
    explicit ServerBump(ClientHttpRequest *http, StoreEntry *e = nullptr, Ssl::BumpMode mode = Ssl::bumpServerFirst);
    ~ServerBump();
    void attachServerSession(const Security::SessionPointer &); ///< Sets the server TLS session object
    const Security::CertErrors *sslErrors() const; ///< SSL [certificate validation] errors

    /// whether there was a successful connection to (and peeking at) the origin server
    bool connectedOk() const {return entry && entry->isEmpty();}

    /// faked, minimal request; required by Client API
    HttpRequest::Pointer request;
    StoreEntry *entry; ///< for receiving Squid-generated error messages
    /// HTTPS server certificate. Maybe it is different than the one
    /// it is stored in serverSession object (error SQUID_X509_V_ERR_CERT_CHANGE)
    Security::CertPointer serverCert;
    struct {
        Ssl::BumpMode step1; ///< The SSL bump mode at step1
        Ssl::BumpMode step2; ///< The SSL bump mode at step2
        Ssl::BumpMode step3; ///< The SSL bump mode at step3
    } act; ///< bumping actions at various bumping steps
    Ssl::BumpStep step; ///< The SSL bumping step

private:
    Security::SessionPointer serverSession; ///< The TLS session object on server side.
    store_client *sc; ///< dummy client to prevent entry trimming
};

enum BumpingStates
{
    bumpStateNone,
    bumpStateExpectTlsHandshake, ///< Before the client hello message received.
    bumpStateParsingTlsHandshake, ///< While parses the client hello message
    bumpStateParsingDone, ///< The client hello message parsed
    bumpStatePeekEvaluate, ///< After peeks at client, run step2 acls evaluate client handshake with openSSL
    bumpStatePeekAtServer, ///< Wait the server side peeking procedure to be finished.
    bumpStateGenerateContext, ///< While generates internal squid structures and generate certificates to establish TLS connection with client.
    bumpStateTlsNegotiate, ///< TLS Negotiation with client
    bumpStateTlsEstablish ///< The TLS connection established
};

const char *BumpingStateStr(enum BumpingStates state);

} // namespace Ssl

#endif

