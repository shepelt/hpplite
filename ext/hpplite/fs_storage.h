/*
** HPPLite Filesystem Storage
**
** Emulates:
** - IPFS/Pinata for batch storage (saves to batches/ directory)
** - L1 contract for commitment index (appends to commitments.jsonl)
*/

#ifndef HPPLITE_FS_STORAGE_H
#define HPPLITE_FS_STORAGE_H

#include "batch.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
** Filesystem storage context
*/
typedef struct HppliteStorage HppliteStorage;

/*
** Initialize storage in a data directory.
** Creates the directory structure if needed.
*/
int hpplite_storage_init(const char *zDataDir, HppliteStorage **ppStorage);

/*
** Close storage and free resources.
*/
void hpplite_storage_close(HppliteStorage *pStorage);

/*
** Get the data directory path.
*/
const char *hpplite_storage_get_dir(HppliteStorage *pStorage);

/*
** Get path to the database file.
** Caller must free returned string with sqlite3_free().
*/
char *hpplite_storage_get_db_path(HppliteStorage *pStorage);

/* === Batch Storage (Fake IPFS) === */

/*
** Store a batch to the batches/ directory.
** Returns the reference path (e.g., "batches/00000001.json").
** Caller must free returned string with sqlite3_free().
*/
char *hpplite_storage_store_batch(HppliteStorage *pStorage, const HppliteBatch *pBatch);

/*
** Load a batch by reference path.
** Caller must free returned batch with hpplite_batch_free().
*/
HppliteBatch *hpplite_storage_load_batch(HppliteStorage *pStorage, const char *zRef);

/*
** Check if a batch exists.
*/
int hpplite_storage_batch_exists(HppliteStorage *pStorage, const char *zRef);

/* === Commitment Index (Fake L1 Contract) === */

/*
** Append a commitment to the index.
** Returns 0 on success, -1 on error.
*/
int hpplite_storage_post_commitment(HppliteStorage *pStorage, const HppliteCommitment *pCommit);

/*
** Get all commitments from the index.
** Returns number of commitments, or -1 on error.
** Caller must free array and each commitment.
*/
int hpplite_storage_get_commitments(
  HppliteStorage *pStorage,
  HppliteCommitment ***paCommits
);

/*
** Get commitment by height.
** Caller must free returned commitment with hpplite_commitment_free().
** Returns NULL if not found.
*/
HppliteCommitment *hpplite_storage_get_commitment(HppliteStorage *pStorage, uint64_t height);

/*
** Get the latest committed height.
** Returns 0 if no commitments exist.
*/
uint64_t hpplite_storage_get_latest_height(HppliteStorage *pStorage);

/*
** Get the latest state root.
** Returns NULL if no commitments exist.
** Caller must NOT free returned pointer.
*/
const unsigned char *hpplite_storage_get_latest_state_root(HppliteStorage *pStorage);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_FS_STORAGE_H */
