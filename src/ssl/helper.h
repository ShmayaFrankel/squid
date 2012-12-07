#ifndef SQUID_SSL_HELPER_H
#define SQUID_SSL_HELPER_H

#include "../helper.h"
#include "ssl/crtd_message.h"

namespace Ssl
{
/**
 * Set of thread for ssl_crtd. This class is singleton. Use this class only
 * over GetIntance() static method. This class use helper structure
 * for threads management.
 */
#if USE_SSL_CRTD
class Helper
{
public:
    static Helper * GetInstance(); ///< Instance class.
    void Init(); ///< Init helper structure.
    void Shutdown(); ///< Shutdown helper structure.
    /// Submit crtd message to external crtd server.
    void sslSubmit(CrtdMessage const & message, HLPCB * callback, void *data);
private:
    Helper();
    ~Helper();

    helper * ssl_crtd; ///< helper for management of ssl_crtd.
};
#endif

#if 1 // USE_SSL_CERT_VALIDATOR
class CertValidationHelper
{
public:
    static CertValidationHelper * GetInstance(); ///< Instance class.
    void Init(); ///< Init helper structure.
    void Shutdown(); ///< Shutdown helper structure.
    /// Submit crtd message to external crtd server.
    void sslSubmit(CrtdMessage const & message, HLPCB * callback, void *data);
private:
    CertValidationHelper();
    ~CertValidationHelper();

    helper * ssl_crt_validator; ///< helper for management of ssl_crtd.
};
#endif // USE_SSL_CERT_VALIDATOR

} //namespace Ssl
#endif // SQUID_SSL_HELPER_H
