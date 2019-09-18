/*
 * Copyright (C) 1996-2019 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS code_contexts for details.
 */

#ifndef SQUID_BASE_CODE_CONTEXT_H
#define SQUID_BASE_CODE_CONTEXT_H

#include "base/RefCount.h"

#include <iosfwd>

/// Interface for reporting what Squid code is working on.
/// Such reports are usually requested outside creator's call stack.
/// They are especially useful for attributing low-level errors to transactions.
class CodeContext: public RefCountable
{
public:
    typedef RefCount<CodeContext> Pointer;

    /// \returns the known global context or, to indicate unknown context, nil
    static const Pointer &Current();

    /// forgets the current context, setting it to nil/unknown
    static void Reset();

    /// changes the current context; nil argument sets it to nil/unknown
    static void Reset(const Pointer);

    virtual ~CodeContext() {}

    // TODO: This should probably become some kind of <char*,id> tuple so that
    // we can preserve context gist across Reset()-triggered destruction _and_
    // pass (master transaction?) context gist to SMP workers (e.g., disker).
    /// writes a word or two to help identify code context in debug messages
    virtual std::ostream &briefCodeContext(std::ostream &os) const = 0;

    /// appends human-friendly context description line(s) to a cache.log record
    virtual std::ostream &detailCodeContext(std::ostream &os) const = 0;
};

/* convenience context-reporting wrappers that also reduce linking problems */
std::ostream &CurrentCodeContextBrief(std::ostream &os);
std::ostream &CurrentCodeContextDetail(std::ostream &os);

/// Convenience class that automatically restores the current/outer CodeContext
/// when leaving the scope of the new-context following/inner code. \see Run().
class CodeContextGuard
{
public:
    CodeContextGuard(const CodeContext::Pointer &newContext): savedCodeContext(CodeContext::Current()) { CodeContext::Reset(newContext); }
    ~CodeContextGuard() { CodeContext::Reset(savedCodeContext); }

    // no copying of any kind (for simplicity and to prevent accidental copies)
    CodeContextGuard(CodeContextGuard &&) = delete;

    CodeContext::Pointer savedCodeContext;
};

/// Executes service `callback` in `callbackContext`. If an exception occurs,
/// the callback context is preserved, so that the exception is associated with
/// the callback that triggered them (rather than with the service).
///
/// Service code running in its own service context should use this function.
template <typename Fun>
inline void
CallBack(const CodeContext::Pointer &callbackContext, Fun &&callback)
{
    // TODO: Consider catching exceptions and letting CodeContext handle them.
    const auto savedCodeContext(CodeContext::Current());
    CodeContext::Reset(callbackContext);
    callback();
    CodeContext::Reset(savedCodeContext);
}

/// Executes `service` in `serviceContext` but due to automatic caller context
/// restoration, service exceptions are associated with the caller that suffered
/// from (and/or caused) them (rather than with the service itself).
///
/// Service code running in caller's context should use this function to escape
/// into service context (e.g., for submitting caller-agnostic requests).
template <typename Fun>
inline void
CallService(const CodeContext::Pointer &serviceContext, Fun &&service)
{
    // TODO: Consider catching exceptions and letting CodeContext handle them.
    CodeContextGuard guard(serviceContext);
    service();
}

#endif

