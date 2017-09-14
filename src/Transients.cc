/*
 * Copyright (C) 1996-2017 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/* DEBUG: section 20    Storage Manager */

#include "squid.h"
#include "base/RunnersRegistry.h"
#include "CollapsedForwarding.h"
#include "HttpReply.h"
#include "ipc/mem/Page.h"
#include "ipc/mem/Pages.h"
#include "MemObject.h"
#include "mime_header.h"
#include "SquidConfig.h"
#include "SquidMath.h"
#include "StoreStats.h"
#include "tools.h"
#include "Transients.h"

#include <limits>

/// shared memory segment path to use for Transients map
static const SBuf MapLabel("transients_map");
/// shared memory segment path to use for Transients map extras
static const char *ExtrasLabel = "transients_ex";

Transients::Transients(): map(NULL), locals(NULL)
{
}

Transients::~Transients()
{
    delete map;
    delete locals;
}

void
Transients::init()
{
    const int64_t entryLimit = EntryLimit();
    if (entryLimit <= 0)
        return; // no SMP support or a misconfiguration

    Must(!map);
    map = new TransientsMap(MapLabel);
    map->cleaner = this;

    extras = shm_old(TransientsMapExtras)(ExtrasLabel);

    locals = new Locals(entryLimit, 0);
}

void
Transients::getStats(StoreInfoStats &stats) const
{
#if TRANSIENT_STATS_SUPPORTED
    const size_t pageSize = Ipc::Mem::PageSize();

    stats.mem.shared = true;
    stats.mem.capacity =
        Ipc::Mem::PageLimit(Ipc::Mem::PageId::cachePage) * pageSize;
    stats.mem.size =
        Ipc::Mem::PageLevel(Ipc::Mem::PageId::cachePage) * pageSize;
    stats.mem.count = currentCount();
#endif
}

void
Transients::stat(StoreEntry &e) const
{
    storeAppendPrintf(&e, "\n\nTransient Objects\n");

    storeAppendPrintf(&e, "Maximum Size: %.0f KB\n", maxSize()/1024.0);
    storeAppendPrintf(&e, "Current Size: %.2f KB %.2f%%\n",
                      currentSize() / 1024.0,
                      Math::doublePercent(currentSize(), maxSize()));

    if (map) {
        const int limit = map->entryLimit();
        storeAppendPrintf(&e, "Maximum entries: %9d\n", limit);
        if (limit > 0) {
            storeAppendPrintf(&e, "Current entries: %" PRId64 " %.2f%%\n",
                              currentCount(), (100.0 * currentCount() / limit));
        }
    }
}

void
Transients::maintain()
{
    // no lazy garbage collection needed
}

uint64_t
Transients::minSize() const
{
    return 0; // XXX: irrelevant, but Store parent forces us to implement this
}

uint64_t
Transients::maxSize() const
{
    // Squid currently does not limit the total size of all transient objects
    return std::numeric_limits<uint64_t>::max();
}

uint64_t
Transients::currentSize() const
{
    // TODO: we do not get enough information to calculate this
    // StoreEntry should update associated stores when its size changes
    return 0;
}

uint64_t
Transients::currentCount() const
{
    return map ? map->entryCount() : 0;
}

int64_t
Transients::maxObjectSize() const
{
    // Squid currently does not limit the size of a transient object
    return std::numeric_limits<uint64_t>::max();
}

void
Transients::reference(StoreEntry &)
{
    // no replacement policy (but the cache(s) storing the entry may have one)
}

bool
Transients::dereference(StoreEntry &)
{
    // no need to keep e in the global store_table for us; we have our own map
    return false;
}

StoreEntry *
Transients::get(const cache_key *key)
{
    if (!map)
        return NULL;

    sfileno index;
    const Ipc::StoreMapAnchor *anchor = map->openForReading(key, index);
    if (!anchor)
        return NULL;

    // If we already have a local entry, the store_table should have found it.
    // Since it did not, the local entry key must have changed from public to
    // private. We still need to keep the private entry around for syncing as
    // its clients depend on it, but we should not allow new clients to join.
    if (StoreEntry *oldE = locals->at(index)) {
        debugs(20, 3, "not joining private " << *oldE);
        assert(EBIT_TEST(oldE->flags, KEY_PRIVATE));
    } else if (anchor->complete()) {
        debugs(20, 3, "not joining completed " << storeKeyText(key));
    } else if (StoreEntry *newE = copyFromShm(index)) {
        return newE; // keep read lock to receive updates from others
    }

    // (private or completed) entry or loading failure
    map->closeForReading(index);
    return NULL;
}

StoreEntry *
Transients::copyFromShm(const sfileno index)
{
    const TransientsMapExtras::Item &extra = extras->items[index];

    // create a brand new store entry and initialize it with stored info
    StoreEntry *e = storeCreatePureEntry(extra.url, extra.url,
                                         extra.reqFlags, extra.reqMethod);

    assert(e->mem_obj);
    e->mem_obj->method = extra.reqMethod;
    e->mem_obj->xitTable.io = MemObject::ioReading;
    e->mem_obj->xitTable.index = index;

    // TODO: Support collapsed revalidation for SMP-aware caches.
    if (!extra.reqFlags.cachable) // should not happen?
        e->setPrivateKey(false, true);
    else
        e->setPublicKey(ksDefault);

    assert(e->key);

    // How do we know its SMP- and not just locally-collapsed? A worker gets
    // locally-collapsed entries from the local store_table, not Transients.
    // TODO: Can we remove smpCollapsed by not syncing non-transient entries?
    e->mem_obj->smpCollapsed = true;

    assert(!locals->at(index));
    // We do not lock e because we do not want to prevent its destruction;
    // e is tied to us via mem_obj so we will know when it is destructed.
    locals->at(index) = e;
    return e;
}

StoreEntry *
Transients::findCollapsed(const sfileno index)
{
    if (!map)
        return NULL;

    if (StoreEntry *oldE = locals->at(index)) {
        debugs(20, 5, "found " << *oldE << " at " << index << " in " << MapLabel);
        assert(oldE->mem_obj && oldE->mem_obj->xitTable.index == index);
        return oldE;
    }

    debugs(20, 3, "no entry at " << index << " in " << MapLabel);
    return NULL;
}

void
Transients::startWriting(StoreEntry *e, const RequestFlags &reqFlags,
                         const HttpRequestMethod &reqMethod)
{
    assert(e);
    assert(e->mem_obj);
    assert(!e->hasTransients());

    if (!map) {
        debugs(20, 5, "No map to add " << *e);
        return;
    }

    sfileno index = 0;
    Ipc::StoreMapAnchor *slot = map->openForWriting(reinterpret_cast<const cache_key *>(e->key), index);
    if (!slot) {
        debugs(20, 5, "collision registering " << *e);
        return;
    }

    try {
        if (copyToShm(*e, index, reqFlags, reqMethod)) {
            slot->set(*e);
            e->mem_obj->xitTable.io = MemObject::ioWriting;
            e->mem_obj->xitTable.index = index;
            map->startAppending(index);
            assert(!locals->at(index));
            locals->at(index) = e;

            // keep write lock -- we will be supplying others with updates
            return;
        }
        // fall through to the error handling code
    } catch (const std::exception &x) { // TODO: should we catch ... as well?
        debugs(20, 2, "error keeping entry " << index <<
               ' ' << *e << ": " << x.what());
        // fall through to the error handling code
    }

    map->abortWriting(index);
}

/// copies all relevant local data to shared memory
bool
Transients::copyToShm(const StoreEntry &e, const sfileno index,
                      const RequestFlags &reqFlags,
                      const HttpRequestMethod &reqMethod)
{
    TransientsMapExtras::Item &extra = extras->items[index];

    const char *url = e.url();
    const size_t urlLen = strlen(url);
    Must(urlLen < sizeof(extra.url)); // we have space to store it all, plus 0
    strncpy(extra.url, url, sizeof(extra.url));
    extra.url[urlLen] = '\0';

    extra.reqFlags = reqFlags;

    Must(reqMethod != Http::METHOD_OTHER);
    extra.reqMethod = reqMethod.id();

    return true;
}

void
Transients::noteFreeMapSlice(const Ipc::StoreMapSliceId)
{
    // TODO: we should probably find the entry being deleted and abort it
}

void
Transients::abandon(const StoreEntry &e)
{
    assert(e.mem_obj && map);
    // avoid useless broadcasts
    if (map->freeEntry(e.mem_obj->xitTable.index)); // just marks the locked entry
        CollapsedForwarding::Broadcast(e, true);
    // We do not unlock the entry now because the problem is most likely with
    // the server resource rather than a specific cache writer, so we want to
    // prevent other readers from collapsing requests for that resource.
}

void
Transients::status(const StoreEntry &entry, bool &aborted, bool &waitingToBeFreed) const
{
    assert(map);
    assert(entry.mem_obj);
    const auto idx = entry.mem_obj->xitTable.index;
    const auto &anchor = collapsedWriter(entry) ?
        map->writeableEntry(idx) : map->readableEntry(idx);
    aborted = false;
    // Since a shared entry gets ENTRY_ABORTED only within StoreMap::abortWriting(),
    // (anchor.writing() becomes false), we should not check this value before.
    // Also note that trying to read ENTRY_ABORTED for being written shared entry
    // may cause unpredictable result, because anchor.basics.flags is not 'atomic'.
    if (!anchor.writing() && EBIT_TEST(anchor.basics.flags, ENTRY_ABORTED))
        aborted = true;
    waitingToBeFreed = anchor.waitingToBeFreed;
}

void
Transients::completeWriting(const StoreEntry &e)
{
    if (e.hasTransients()) {
        assert(collapsedWriter(e));
        // there will be no more updates from us after this, so we must prevent
        // future readers from joining. Making the entry complete() is sufficient
        // because Transients::get() does not return completed entries.
        map->closeForWriting(e.mem_obj->xitTable.index);
        e.mem_obj->xitTable.index = -1;
        e.mem_obj->xitTable.io = MemObject::ioDone;
    }
}

int
Transients::readers(const StoreEntry &e) const
{
    if (e.hasTransients()) {
        assert(map);
        return map->peekAtEntry(e.mem_obj->xitTable.index).lock.readers;
    }
    return 0;
}

void
Transients::markForUnlink(StoreEntry &e)
{
    assert(e.key);
    if (e.hasTransients())
        abandon(e);
    else
        unlinkByKeyIfFound(reinterpret_cast<const cache_key*>(e.key));
}

void
Transients::unlinkByKeyIfFound(const cache_key *key)
{
    // Controller ensures that this worker has no StoreEntry to abandon() here.
    if (map)
        map->freeEntryByKey(key);
}

void
Transients::disconnect(MemObject &mem_obj)
{
    if (mem_obj.xitTable.index >= 0) {
        assert(map);
        if (collapsedWriter(&mem_obj)) {
            map->abortWriting(mem_obj.xitTable.index);
        } else {
            assert(collapsedReader(&mem_obj));
            map->closeForReading(mem_obj.xitTable.index);
        }
        locals->at(mem_obj.xitTable.index) = NULL;
        mem_obj.xitTable.index = -1;
        mem_obj.xitTable.io = MemObject::ioDone;
    }
}

/// calculates maximum number of entries we need to store and map
int64_t
Transients::EntryLimit()
{
    // TODO: we should also check whether any SMP-aware caching is configured
    if (!UsingSmp() || !Config.onoff.collapsed_forwarding)
        return 0; // no SMP collapsed forwarding possible or needed

    return Config.collapsed_forwarding_shared_entries_limit;
}

bool
Transients::markedForDeletion(const cache_key *key) const
{
    assert(map);
    return map->markedForDeletion(key);
}

bool
Transients::collapsedReader(const StoreEntry &e) const
{
    return collapsedReader(e.mem_obj);
}

bool
Transients::collapsedReader(const MemObject *mem_obj) const
{
    assert(mem_obj);
    return mem_obj->xitTable.io == MemObject::ioReading;
}

bool
Transients::collapsedWriter(const StoreEntry &e) const
{
    return collapsedWriter(e.mem_obj);
}

bool
Transients::collapsedWriter(const MemObject *mem_obj) const
{
    assert(mem_obj);
    return mem_obj->xitTable.io == MemObject::ioWriting;
}

/// initializes shared memory segment used by Transients
class TransientsRr: public Ipc::Mem::RegisteredRunner
{
public:
    /* RegisteredRunner API */
    TransientsRr(): mapOwner(NULL), extrasOwner(NULL) {}
    virtual void useConfig();
    virtual ~TransientsRr();

protected:
    virtual void create();

private:
    TransientsMap::Owner *mapOwner;
    Ipc::Mem::Owner<TransientsMapExtras> *extrasOwner;
};

RunnerRegistrationEntry(TransientsRr);

void
TransientsRr::useConfig()
{
    assert(Config.memShared.configured());
    Ipc::Mem::RegisteredRunner::useConfig();
}

void
TransientsRr::create()
{
    if (!Config.onoff.collapsed_forwarding)
        return;

    const int64_t entryLimit = Transients::EntryLimit();
    if (entryLimit <= 0)
        return; // no SMP configured or a misconfiguration

    Must(!mapOwner);
    mapOwner = TransientsMap::Init(MapLabel, entryLimit);
    Must(!extrasOwner);
    extrasOwner = shm_new(TransientsMapExtras)(ExtrasLabel, entryLimit);
}

TransientsRr::~TransientsRr()
{
    delete extrasOwner;
    delete mapOwner;
}

