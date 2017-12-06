/*
 * Copyright (C) 1996-2017 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#ifndef SQUID_TRANSIENTS_H
#define SQUID_TRANSIENTS_H

#include "http/MethodType.h"
#include "ipc/mem/Page.h"
#include "ipc/mem/PageStack.h"
#include "ipc/StoreMap.h"
#include "Store.h"
#include "store/Controlled.h"
#include <vector>

// StoreEntry restoration info not already stored by Ipc::StoreMap
struct TransientsMapExtraItem {
    char url[MAX_URL+1]; ///< Request-URI; TODO: decrease MAX_URL by one
    Http::MethodType reqMethod; ///< request method; extensions are not supported
};
typedef Ipc::StoreMapItems<TransientsMapExtraItem> TransientsMapExtras;
typedef Ipc::StoreMap TransientsMap;

/// Keeps track of store entries being delivered to clients that arrived before
/// those entries were [fully] cached. This SMP-shared table is necessary to
/// * sync an entry-writing worker with entry-reading worker(s); and
/// * sync an entry-deleting worker with both entry-reading/writing workers.
class Transients: public Store::Controlled, public Ipc::StoreMapCleaner
{
public:
    Transients();
    virtual ~Transients();

    /// return a local, previously collapsed entry
    StoreEntry *findCollapsed(const sfileno xitIndex);

    /// start listening for remote DELETE requests targeting the given complete StoreEntry
    bool monitorWhileReading(StoreEntry*, const Store::CacheKey&);

    /// start listening for remote DELETE requests targeting the given miss StoreEntry
    /// and allow broadcasting of local StoreEntry updates to remote readers
    bool startWriting(StoreEntry*, const Store::CacheKey&);

    /// called when the in-transit entry has been successfully cached
    void completeWriting(const StoreEntry &e);

    /// copies current shared entry metadata into parameters
    /// \param aborted whether the entry was aborted
    /// \param waitingToBeFreed whether the entry was marked for deletion
    void status(const StoreEntry &, bool &aborted, bool &waitingToBeFreed) const;

    /// number of entry readers some time ago
    int readers(const StoreEntry &e) const;

    /// the caller is done writing or reading the given entry
    void disconnect(StoreEntry &);

    /* Store API */
    virtual StoreEntry *get(const cache_key *) override;
    virtual void create() override {}
    virtual void init() override;
    virtual uint64_t maxSize() const override;
    virtual uint64_t minSize() const override;
    virtual uint64_t currentSize() const override;
    virtual uint64_t currentCount() const override;
    virtual int64_t maxObjectSize() const override;
    virtual void getStats(StoreInfoStats &stats) const override;
    virtual void stat(StoreEntry &e) const override;
    virtual void reference(StoreEntry &e) override;
    virtual bool dereference(StoreEntry &e) override;
    virtual void evictCached(StoreEntry &) override;
    virtual void evictIfFound(const cache_key *) override;
    virtual void maintain() override;
    virtual bool smpAware() const override { return true; }

    /// Whether an entry with the given public key exists and (but) was
    /// marked for removal some time ago; get(key) returns nil in such cases.
    bool markedForDeletion(const cache_key *) const;

    /// whether the entry is in "reading from Transients" I/O state
    bool isReader(const StoreEntry &) const;
    /// whether the entry is in "writing to Transients" I/O state
    bool isWriter(const StoreEntry &) const;

    static int64_t EntryLimit();

protected:
    bool addEntry(StoreEntry*, const Store::CacheKey&);

    StoreEntry *copyFromShm(const sfileno index);
    bool copyToShm(const StoreEntry &e, const sfileno index, const Store::CacheKey &cacheKey);

    // Ipc::StoreMapCleaner API
    virtual void noteFreeMapSlice(const Ipc::StoreMapSliceId sliceId) override;

private:
    /// shared packed info indexed by Store keys, for creating new StoreEntries
    TransientsMap *map;

    /// shared packed info that standard StoreMap does not store for us
    typedef TransientsMapExtras Extras;
    Ipc::Mem::Pointer<Extras> extras;

    typedef std::vector<StoreEntry*> Locals;
    /// local collapsed reader and writer entries, indexed by transient ID,
    /// for syncing old StoreEntries
    Locals *locals;
};

// TODO: Why use Store as a base? We are not really a cache.

#endif /* SQUID_TRANSIENTS_H */

