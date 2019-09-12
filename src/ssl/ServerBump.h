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
#include "XactionStep.h"

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

    /// whether we are at the bumping step atStepVal
    bool at(const XactionStep atStepVal) { return step == atStepVal;}

    /// whether we are at one of the bumping steps atStepVal1 or atStepVal2
    bool at(const XactionStep atStepVal1, const XactionStep atStepVal2) { return step == atStepVal1 || step == atStepVal2;}

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
    XactionStep step; ///< The TLS bumping step

private:
    Security::SessionPointer serverSession; ///< The TLS session object on server side.
    store_client *sc; ///< dummy client to prevent entry trimming
};

} // namespace Ssl

#endif

