#include "squid.h"
#include "acl/Checklist.h"
#include "acl/IntRange.h"
#include "acl/AdaptationService.h"
#include "adaptation/Config.h"
#include "adaptation/History.h"
#include "HttpRequest.h"

int
ACLAdaptationServiceStrategy::match (ACLData<MatchType> * &data, ACLFilledChecklist *checklist)
{
    HttpRequest *request = checklist->request;
    if (!request)
        return 0;
    Adaptation::History::Pointer ah = request->adaptHistory();
    if (ah == NULL)
        return 0;

    Adaptation::History::AdaptationServices::iterator it;
    for (it = ah->theAdaptationServices.begin(); it != ah->theAdaptationServices.end(); ++it) {
        if (data->match(it->termedBuf()))
            return 1;
    }

    return 0;
}

ACLAdaptationServiceStrategy *
ACLAdaptationServiceStrategy::Instance()
{
    return &Instance_;
}

ACLAdaptationServiceStrategy ACLAdaptationServiceStrategy::Instance_;
