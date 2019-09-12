/*
 * Copyright (C) 1996-2019 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#include "squid.h"

#include "acl/AtStep.h"
#include "acl/AtStepData.h"
#include "acl/FilledChecklist.h"
#include "client_side.h"
#include "http/Stream.h"
#if USE_OPENSSL
#include "ssl/ServerBump.h"
#endif

int
ACLAtStepStrategy::match (ACLData<XactionStep> * &data, ACLFilledChecklist *checklist)
{
    Must(checklist->request);
    Must(checklist->request->masterXaction);
    if (checklist->request->masterXaction->generatingConnect && data->match(xstepGeneratingConnect))
        return 1;

#if USE_OPENSSL
    Ssl::ServerBump *bump = NULL;
    if (checklist->conn() != NULL && (bump = checklist->conn()->serverBump()))
        return data->match(bump->step);
    else
        return data->match(xstepTlsBump1);
#endif
    return 0;
}
