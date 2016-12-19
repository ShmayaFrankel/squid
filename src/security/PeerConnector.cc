/*
 * Copyright (C) 1996-2016 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/* DEBUG: section 83    TLS Server/Peer negotiation */

#include "squid.h"
#include "acl/FilledChecklist.h"
#include "comm/Loops.h"
#include "Downloader.h"
#include "errorpage.h"
#include "fde.h"
#include "http/Stream.h"
#include "HttpRequest.h"
#include "security/NegotiationHistory.h"
#include "security/PeerConnector.h"
#include "SquidConfig.h"
#if USE_OPENSSL
#include "ssl/bio.h"
#include "ssl/cert_validate_message.h"
#include "ssl/Config.h"
#include "ssl/helper.h"
#endif

CBDATA_NAMESPACED_CLASS_INIT(Security, PeerConnector);

Security::PeerConnector::PeerConnector(const Comm::ConnectionPointer &aServerConn, AsyncCall::Pointer &aCallback, const AccessLogEntryPointer &alp, const time_t timeout) :
    AsyncJob("Security::PeerConnector"),
    serverConn(aServerConn),
    al(alp),
    callback(aCallback),
    negotiationTimeout(timeout),
    startTime(squid_curtime),
    useCertValidator_(true),
    certsDownloads(0)
{
    debugs(83, 5, "Security::PeerConnector constructed, this=" << (void*)this);
    // if this throws, the caller's cb dialer is not our CbDialer
    Must(dynamic_cast<CbDialer*>(callback->getDialer()));
}

Security::PeerConnector::~PeerConnector()
{
    debugs(83, 5, "Security::PeerConnector destructed, this=" << (void*)this);
}

bool Security::PeerConnector::doneAll() const
{
    return (!callback || callback->canceled()) && AsyncJob::doneAll();
}

/// Preps connection and SSL state. Calls negotiate().
void
Security::PeerConnector::start()
{
    AsyncJob::start();

    Security::SessionPointer tmp;
    if (prepareSocket() && initialize(tmp))
        negotiate();
    else
        mustStop("Security::PeerConnector TLS socket initialize failed");
}

void
Security::PeerConnector::commCloseHandler(const CommCloseCbParams &params)
{
    debugs(83, 5, "FD " << params.fd << ", Security::PeerConnector=" << params.data);
    connectionClosed("Security::PeerConnector::commCloseHandler");
}

void
Security::PeerConnector::connectionClosed(const char *reason)
{
    mustStop(reason);
    callback = NULL;
}

bool
Security::PeerConnector::prepareSocket()
{
    const int fd = serverConnection()->fd;
    if (!Comm::IsConnOpen(serverConn) || fd_table[serverConn->fd].closing()) {
        connectionClosed("Security::PeerConnector::prepareSocket");
        return false;
    }

    // watch for external connection closures
    typedef CommCbMemFunT<Security::PeerConnector, CommCloseCbParams> Dialer;
    closeHandler = JobCallback(9, 5, Dialer, this, Security::PeerConnector::commCloseHandler);
    comm_add_close_handler(fd, closeHandler);
    return true;
}

bool
Security::PeerConnector::initialize(Security::SessionPointer &serverSession)
{
#if USE_OPENSSL
    Security::ContextPtr sslContext(getSslContext());
    assert(sslContext);

    if (!Ssl::CreateClient(sslContext, serverConnection(), "server https start")) {
        ErrorState *anErr = new ErrorState(ERR_SOCKET_FAILURE, Http::scInternalServerError, request.getRaw());
        anErr->xerrno = errno;
        debugs(83, DBG_IMPORTANT, "Error allocating TLS handle: " << ERR_error_string(ERR_get_error(), NULL));
        noteNegotiationDone(anErr);
        bail(anErr);
        return false;
    }

    // A TLS/SSL session has now been created for the connection and stored in fd_table
    serverSession = fd_table[serverConnection()->fd].ssl;

    // If CertValidation Helper used do not lookup checklist for errors,
    // but keep a list of errors to send it to CertValidator
    if (!Ssl::TheConfig.ssl_crt_validator) {
        // Create the ACL check list now, while we have access to more info.
        // The list is used in ssl_verify_cb() and is freed in ssl_free().
        if (acl_access *acl = ::Config.ssl_client.cert_error) {
            ACLFilledChecklist *check = new ACLFilledChecklist(acl, request.getRaw(), dash_str);
            check->al = al;
            // check->fd(fd); XXX: need client FD here
            SSL_set_ex_data(serverSession.get(), ssl_ex_index_cert_error_check, check);
        }
    }

    return true;
#else
    return false;
#endif
}

void
Security::PeerConnector::setReadTimeout()
{
    int timeToRead;
    if (negotiationTimeout) {
        const int timeUsed = squid_curtime - startTime;
        const int timeLeft = max(0, static_cast<int>(negotiationTimeout - timeUsed));
        timeToRead = min(static_cast<int>(::Config.Timeout.read), timeLeft);
    } else
        timeToRead = ::Config.Timeout.read;
    AsyncCall::Pointer nil;
    commSetConnTimeout(serverConnection(), timeToRead, nil);
}

void
Security::PeerConnector::recordNegotiationDetails()
{
#if USE_OPENSSL
    const int fd = serverConnection()->fd;
    Security::SessionPtr ssl = fd_table[fd].ssl.get();

    // retrieve TLS server negotiated information if any
    serverConnection()->tlsNegotiations()->retrieveNegotiatedInfo(ssl);
    // retrieve TLS parsed extra info
    BIO *b = SSL_get_rbio(ssl);
    Ssl::ServerBio *bio = static_cast<Ssl::ServerBio *>(b->ptr);
    if (const Security::TlsDetails::Pointer &details = bio->receivedHelloDetails())
        serverConnection()->tlsNegotiations()->retrieveParsedInfo(details);
#endif
}

void
Security::PeerConnector::negotiate()
{
    if (!Comm::IsConnOpen(serverConnection()))
        return;

    const int fd = serverConnection()->fd;
    if (fd_table[fd].closing())
        return;

#if USE_OPENSSL
    const int result = SSL_connect(fd_table[fd].ssl.get());
#else
    const int result = -1;
#endif
    if (result <= 0) {
        handleNegotiateError(result);
        return; // we might be gone by now
    }

    recordNegotiationDetails();

    if (!sslFinalized())
        return;

    callBack();
}

bool
Security::PeerConnector::sslFinalized()
{
#if USE_OPENSSL
    if (Ssl::TheConfig.ssl_crt_validator && useCertValidator_) {
        const int fd = serverConnection()->fd;
        Security::SessionPtr ssl = fd_table[fd].ssl.get();

        Ssl::CertValidationRequest validationRequest;
        // WARNING: Currently we do not use any locking for any of the
        // members of the Ssl::CertValidationRequest class. In this code the
        // Ssl::CertValidationRequest object used only to pass data to
        // Ssl::CertValidationHelper::submit method.
        validationRequest.ssl = ssl;
        SBuf *dName = (SBuf *)SSL_get_ex_data(ssl, ssl_ex_index_server);
        validationRequest.domainName = dName->c_str();
        if (Security::CertErrors *errs = static_cast<Security::CertErrors *>(SSL_get_ex_data(ssl, ssl_ex_index_ssl_errors)))
            // validationRequest disappears on return so no need to cbdataReference
            validationRequest.errors = errs;
        else
            validationRequest.errors = NULL;
        try {
            debugs(83, 5, "Sending SSL certificate for validation to ssl_crtvd.");
            AsyncCall::Pointer call = asyncCall(83,5, "Security::PeerConnector::sslCrtvdHandleReply", Ssl::CertValidationHelper::CbDialer(this, &Security::PeerConnector::sslCrtvdHandleReply, nullptr));
            Ssl::CertValidationHelper::GetInstance()->sslSubmit(validationRequest, call);
            return false;
        } catch (const std::exception &e) {
            debugs(83, DBG_IMPORTANT, "ERROR: Failed to compose ssl_crtvd " <<
                   "request for " << validationRequest.domainName <<
                   " certificate: " << e.what() << "; will now block to " <<
                   "validate that certificate.");
            // fall through to do blocking in-process generation.
            ErrorState *anErr = new ErrorState(ERR_GATEWAY_FAILURE, Http::scInternalServerError, request.getRaw());

            noteNegotiationDone(anErr);
            bail(anErr);
            serverConn->close();
            return true;
        }
    }
#endif

    noteNegotiationDone(NULL);
    return true;
}

#if USE_OPENSSL
void
Security::PeerConnector::sslCrtvdHandleReply(Ssl::CertValidationResponse::Pointer validationResponse)
{
    Must(validationResponse != NULL);

    Ssl::ErrorDetail *errDetails = NULL;
    bool validatorFailed = false;
    if (!Comm::IsConnOpen(serverConnection())) {
        return;
    }

    if (Debug::Enabled(83, 5)) {
        Security::SessionPointer ssl(fd_table[serverConnection()->fd].ssl);
        SBuf *server = static_cast<SBuf *>(SSL_get_ex_data(ssl.get(), ssl_ex_index_server));
        debugs(83,5, *server << " cert validation result: " << validationResponse->resultCode);
    }

    if (validationResponse->resultCode == ::Helper::Error) {
        if (Security::CertErrors *errs = sslCrtvdCheckForErrors(*validationResponse, errDetails)) {
            Security::SessionPtr ssl = fd_table[serverConnection()->fd].ssl.get();
            Security::CertErrors *oldErrs = static_cast<Security::CertErrors*>(SSL_get_ex_data(ssl, ssl_ex_index_ssl_errors));
            SSL_set_ex_data(ssl, ssl_ex_index_ssl_errors,  (void *)errs);
            delete oldErrs;
        }
    } else if (validationResponse->resultCode != ::Helper::Okay)
        validatorFailed = true;

    if (!errDetails && !validatorFailed) {
        noteNegotiationDone(NULL);
        callBack();
        return;
    }

    ErrorState *anErr = NULL;
    if (validatorFailed) {
        anErr = new ErrorState(ERR_GATEWAY_FAILURE, Http::scInternalServerError, request.getRaw());
    }  else {
        anErr =  new ErrorState(ERR_SECURE_CONNECT_FAIL, Http::scServiceUnavailable, request.getRaw());
        anErr->detail = errDetails;
        /*anErr->xerrno= Should preserved*/
    }

    noteNegotiationDone(anErr);
    bail(anErr);
    serverConn->close();
    return;
}
#endif

#if USE_OPENSSL
/// Checks errors in the cert. validator response against sslproxy_cert_error.
/// The first honored error, if any, is returned via errDetails parameter.
/// The method returns all seen errors except SSL_ERROR_NONE as Security::CertErrors.
Security::CertErrors *
Security::PeerConnector::sslCrtvdCheckForErrors(Ssl::CertValidationResponse const &resp, Ssl::ErrorDetail *& errDetails)
{
    ACLFilledChecklist *check = NULL;
    if (acl_access *acl = ::Config.ssl_client.cert_error) {
        check = new ACLFilledChecklist(acl, request.getRaw(), dash_str);
        check->al = al;
    }

    Security::CertErrors *errs = nullptr;
    Security::SessionPtr ssl = fd_table[serverConnection()->fd].ssl.get();
    typedef Ssl::CertValidationResponse::RecvdErrors::const_iterator SVCRECI;
    for (SVCRECI i = resp.errors.begin(); i != resp.errors.end(); ++i) {
        debugs(83, 7, "Error item: " << i->error_no << " " << i->error_reason);

        assert(i->error_no != SSL_ERROR_NONE);

        if (!errDetails) {
            bool allowed = false;
            if (check) {
                check->sslErrors = new Security::CertErrors(Security::CertError(i->error_no, i->cert, i->error_depth));
                if (check->fastCheck() == ACCESS_ALLOWED)
                    allowed = true;
            }
            // else the Config.ssl_client.cert_error access list is not defined
            // and the first error will cause the error page

            if (allowed) {
                debugs(83, 3, "bypassing SSL error " << i->error_no << " in " << "buffer");
            } else {
                debugs(83, 5, "confirming SSL error " << i->error_no);
                X509 *brokenCert = i->cert.get();
                Security::CertPointer peerCert(SSL_get_peer_certificate(ssl));
                const char *aReason = i->error_reason.empty() ? NULL : i->error_reason.c_str();
                errDetails = new Ssl::ErrorDetail(i->error_no, peerCert.get(), brokenCert, aReason);
            }
            if (check) {
                delete check->sslErrors;
                check->sslErrors = NULL;
            }
        }

        if (!errs)
            errs = new Security::CertErrors(Security::CertError(i->error_no, i->cert, i->error_depth));
        else
            errs->push_back_unique(Security::CertError(i->error_no, i->cert, i->error_depth));
    }
    if (check)
        delete check;

    return errs;
}
#endif

/// A wrapper for Comm::SetSelect() notifications.
void
Security::PeerConnector::NegotiateSsl(int, void *data)
{
    PeerConnector *pc = static_cast<Security::PeerConnector *>(data);
    // Use job calls to add done() checks and other job logic/protections.
    CallJobHere(83, 7, pc, Security::PeerConnector, negotiate);
}

void
Security::PeerConnector::handleNegotiateError(const int ret)
{
#if USE_OPENSSL
    const int fd = serverConnection()->fd;
    unsigned long ssl_lib_error = SSL_ERROR_NONE;
    Security::SessionPtr ssl = fd_table[fd].ssl.get();
    const int ssl_error = SSL_get_error(ssl, ret);

    switch (ssl_error) {
    case SSL_ERROR_WANT_READ:
        noteWantRead();
        return;

    case SSL_ERROR_WANT_WRITE:
        noteWantWrite();
        return;

    case SSL_ERROR_SSL:
    case SSL_ERROR_SYSCALL:
        ssl_lib_error = ERR_get_error();
        // proceed to the general error handling code
        break;
    default:
        // no special error handling for all other errors
        break;
    }

    // Log connection details, if any
    recordNegotiationDetails();
    noteNegotiationError(ret, ssl_error, ssl_lib_error);
#endif
}

void
Security::PeerConnector::noteWantRead()
{
    const int fd = serverConnection()->fd;
#if USE_OPENSSL
    Security::SessionPtr ssl = fd_table[fd].ssl.get();
    BIO *b = SSL_get_rbio(ssl);
    Ssl::ServerBio *srvBio = static_cast<Ssl::ServerBio *>(b->ptr);
    if (srvBio->holdRead()) {
        if (srvBio->gotHello()) {
            if (checkForMissingCertificates())
                return; // Wait to download certificates before proceed.

            srvBio->holdRead(false);
            // schedule a negotiateSSl to allow openSSL parse received data
            Security::PeerConnector::NegotiateSsl(fd, this);
            return;
        } else if (srvBio->gotHelloFailed()) {
            srvBio->holdRead(false);
            debugs(83, DBG_IMPORTANT, "Error parsing SSL Server Hello Message on FD " << fd);
            // schedule a negotiateSSl to allow openSSL parse received data
            Security::PeerConnector::NegotiateSsl(fd, this);
            return;
        }
    }
#endif
    setReadTimeout();
    Comm::SetSelect(fd, COMM_SELECT_READ, &NegotiateSsl, this, 0);
}

void
Security::PeerConnector::noteWantWrite()
{
    const int fd = serverConnection()->fd;
    Comm::SetSelect(fd, COMM_SELECT_WRITE, &NegotiateSsl, this, 0);
    return;
}

void
Security::PeerConnector::noteNegotiationError(const int ret, const int ssl_error, const int ssl_lib_error)
{
#if USE_OPENSSL // not used unless OpenSSL enabled.
#if defined(EPROTO)
    int sysErrNo = EPROTO;
#else
    int sysErrNo = EACCES;
#endif

    // store/report errno when ssl_error is SSL_ERROR_SYSCALL, ssl_lib_error is 0, and ret is -1
    if (ssl_error == SSL_ERROR_SYSCALL && ret == -1 && ssl_lib_error == 0)
        sysErrNo = errno;

    const int fd = serverConnection()->fd;
    debugs(83, DBG_IMPORTANT, "Error negotiating SSL on FD " << fd <<
           ": " << ERR_error_string(ssl_lib_error, NULL) << " (" <<
           ssl_error << "/" << ret << "/" << errno << ")");

    ErrorState *anErr = NULL;
    if (request != NULL)
        anErr = ErrorState::NewForwarding(ERR_SECURE_CONNECT_FAIL, request.getRaw());
    else
        anErr = new ErrorState(ERR_SECURE_CONNECT_FAIL, Http::scServiceUnavailable, NULL);
    anErr->xerrno = sysErrNo;

    Security::SessionPtr ssl = fd_table[fd].ssl.get();
    Ssl::ErrorDetail *errFromFailure = static_cast<Ssl::ErrorDetail *>(SSL_get_ex_data(ssl, ssl_ex_index_ssl_error_detail));
    if (errFromFailure != NULL) {
        // The errFromFailure is attached to the ssl object
        // and will be released when ssl object destroyed.
        // Copy errFromFailure to a new Ssl::ErrorDetail object
        anErr->detail = new Ssl::ErrorDetail(*errFromFailure);
    } else {
        // server_cert can be NULL here
        X509 *server_cert = SSL_get_peer_certificate(ssl);
        anErr->detail = new Ssl::ErrorDetail(SQUID_ERR_SSL_HANDSHAKE, server_cert, NULL);
        X509_free(server_cert);
    }

    if (ssl_lib_error != SSL_ERROR_NONE)
        anErr->detail->setLibError(ssl_lib_error);

    noteNegotiationDone(anErr);
    bail(anErr);
#endif
}

void
Security::PeerConnector::bail(ErrorState *error)
{
    Must(error); // or the recepient will not know there was a problem
    Must(callback != NULL);
    CbDialer *dialer = dynamic_cast<CbDialer*>(callback->getDialer());
    Must(dialer);
    dialer->answer().error = error;

    callBack();
    // Our job is done. The callabck recepient will probably close the failed
    // peer connection and try another peer or go direct (if possible). We
    // can close the connection ourselves (our error notification would reach
    // the recepient before the fd-closure notification), but we would rather
    // minimize the number of fd-closure notifications and let the recepient
    // manage the TCP state of the connection.
}

void
Security::PeerConnector::callBack()
{
    AsyncCall::Pointer cb = callback;
    // Do this now so that if we throw below, swanSong() assert that we _tried_
    // to call back holds.
    callback = NULL; // this should make done() true

    // remove close handler
    comm_remove_close_handler(serverConnection()->fd, closeHandler);

    CbDialer *dialer = dynamic_cast<CbDialer*>(cb->getDialer());
    Must(dialer);
    dialer->answer().conn = serverConnection();
    ScheduleCallHere(cb);
}

void
Security::PeerConnector::swanSong()
{
    // XXX: unregister fd-closure monitoring and CommSetSelect interest, if any
    AsyncJob::swanSong();
    if (callback != NULL) { // paranoid: we have left the caller waiting
        debugs(83, DBG_IMPORTANT, "BUG: Unexpected state while connecting to a cache_peer or origin server");
        ErrorState *anErr = new ErrorState(ERR_GATEWAY_FAILURE, Http::scInternalServerError, request.getRaw());
        bail(anErr);
        assert(!callback);
        return;
    }
}

const char *
Security::PeerConnector::status() const
{
    static MemBuf buf;
    buf.reset();

    // TODO: redesign AsyncJob::status() API to avoid this
    // id and stop reason reporting duplication.
    buf.append(" [", 2);
    if (stopReason != NULL) {
        buf.append("Stopped, reason:", 16);
        buf.appendf("%s",stopReason);
    }
    if (serverConn != NULL)
        buf.appendf(" FD %d", serverConn->fd);
    buf.appendf(" %s%u]", id.prefix(), id.value);
    buf.terminate();

    return buf.content();
}

#if USE_OPENSSL
/// CallDialer to allow use Downloader objects within PeerConnector class.
class PeerConnectorCertDownloaderDialer: public Downloader::CbDialer
{
public:
    typedef void (Security::PeerConnector::*Method)(SBuf &object, int status);

    PeerConnectorCertDownloaderDialer(Method method, Security::PeerConnector *pc):
        method_(method),
        peerConnector_(pc) {}

    /* CallDialer API */
    virtual bool canDial(AsyncCall &call) { return peerConnector_.valid(); }
    virtual void dial(AsyncCall &call) { ((&(*peerConnector_))->*method_)(object, status); }
    Method method_; ///< The Security::PeerConnector method to dial
    CbcPointer<Security::PeerConnector> peerConnector_; ///< The Security::PeerConnector object
};

void
Security::PeerConnector::startCertDownloading(SBuf &url)
{
    AsyncCall::Pointer certCallback = asyncCall(81, 4,
                                      "Security::PeerConnector::certDownloadingDone",
                                      PeerConnectorCertDownloaderDialer(&Security::PeerConnector::certDownloadingDone, this));

    const Downloader *csd = (request ? dynamic_cast<const Downloader*>(request->downloader.valid()) : nullptr);
    Downloader *dl = new Downloader(url, certCallback, csd ? csd->nestedLevel() + 1 : 1);
    AsyncJob::Start(dl);
}

void
Security::PeerConnector::certDownloadingDone(SBuf &obj, int downloadStatus)
{
    ++certsDownloads;
    debugs(81, 5, "Certificate downloading status: " << downloadStatus << " certificate size: " << obj.length());

    // get ServerBio from SSL object
    const int fd = serverConnection()->fd;
    Security::SessionPtr ssl = fd_table[fd].ssl.get();
    BIO *b = SSL_get_rbio(ssl);
    Ssl::ServerBio *srvBio = static_cast<Ssl::ServerBio *>(b->ptr);

    // Parse Certificate. Assume that it is in DER format.
    // According to RFC 4325:
    //  The server must provide a DER encoded certificate or a collection
    // collection of certificates in a "certs-only" CMS message.
    //  The applications MUST accept DER encoded certificates and SHOULD
    // be able to accept collection of certificates.
    // TODO: support collection of certificates
    const unsigned char *raw = (const unsigned char*)obj.rawContent();
    if (X509 *cert = d2i_X509(NULL, &raw, obj.length())) {
        char buffer[1024];
        debugs(81, 5, "Retrieved certificate: " << X509_NAME_oneline(X509_get_subject_name(cert), buffer, 1024));
        const Security::CertList &certsList = srvBio->serverCertificatesIfAny();
        if (const char *issuerUri = Ssl::uriOfIssuerIfMissing(cert,  certsList)) {
            urlsOfMissingCerts.push(SBuf(issuerUri));
        }
        Ssl::SSL_add_untrusted_cert(ssl, cert);
    }

    // Check if there are URIs to download from and if yes start downloading
    // the first in queue.
    if (urlsOfMissingCerts.size() && certsDownloads <= MaxCertsDownloads) {
        startCertDownloading(urlsOfMissingCerts.front());
        urlsOfMissingCerts.pop();
        return;
    }

    srvBio->holdRead(false);
    Security::PeerConnector::NegotiateSsl(serverConnection()->fd, this);
}

bool
Security::PeerConnector::checkForMissingCertificates()
{
    // Check for nested SSL certificates downloads. For example when the
    // certificate located in an SSL site which requires to download a
    // a missing certificate (... from an SSL site which requires to ...).

    const Downloader *csd = (request ? request->downloader.get() : nullptr);
    if (csd && csd->nestedLevel() >= MaxNestedDownloads)
        return false;

    const int fd = serverConnection()->fd;
    Security::SessionPtr ssl = fd_table[fd].ssl.get();
    BIO *b = SSL_get_rbio(ssl);
    Ssl::ServerBio *srvBio = static_cast<Ssl::ServerBio *>(b->ptr);
    const Security::CertList &certs = srvBio->serverCertificatesIfAny();

    if (certs.size()) {
        debugs(83, 5, "SSL server sent " << certs.size() << " certificates");
        Ssl::missingChainCertificatesUrls(urlsOfMissingCerts, certs);
        if (urlsOfMissingCerts.size()) {
            startCertDownloading(urlsOfMissingCerts.front());
            urlsOfMissingCerts.pop();
            return true;
        }
    }

    return false;
}
#endif //USE_OPENSSL
