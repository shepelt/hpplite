/*
** HPPLite Batch - Transaction batching for L2
**
** A batch packages multiple SQL transactions with their
** state transition (pre/post state roots).
*/

#ifndef HPPLITE_BATCH_H
#define HPPLITE_BATCH_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HPPLITE_HASH_SIZE 32

/*
** A single SQL transaction within a batch
*/
typedef struct HppliteTxn HppliteTxn;
struct HppliteTxn {
  char *zSql;              /* SQL statement */
  int64_t nRowsAffected;   /* Number of rows affected (-1 if unknown) */
  int64_t txn;             /* Transaction group within the batch (0, 1, ...) */
};

/*
** A batch of transactions with state transition
*/
typedef struct HppliteBatch HppliteBatch;
struct HppliteBatch {
  uint64_t height;                           /* Sequential batch number */
  uint64_t timestamp;                        /* Unix timestamp (seconds) */
  unsigned char preStateRoot[HPPLITE_HASH_SIZE];  /* State before batch */
  unsigned char postStateRoot[HPPLITE_HASH_SIZE]; /* State after batch */
  int nTxns;                                 /* Number of transactions */
  HppliteTxn *aTxns;                         /* Array of transactions */
  int partial;                               /* More pending SQL follows in the next batch */
  int hasRoot;                               /* postStateRoot is set (batch closes a checkpoint interval) */
};

/*
** Checkpoint - covers a range of batches
** Posted to L1 by the sequencer
*/
typedef struct HppliteCheckpoint HppliteCheckpoint;
struct HppliteCheckpoint {
  uint64_t fromHeight;                       /* First batch in checkpoint */
  uint64_t toHeight;                         /* Last batch in checkpoint */
  unsigned char preStateRoot[HPPLITE_HASH_SIZE];  /* State before fromHeight */
  unsigned char postStateRoot[HPPLITE_HASH_SIZE]; /* State after toHeight */
  unsigned char batchesHash[HPPLITE_HASH_SIZE];   /* Hash of all batch refs */
  uint64_t timestamp;                        /* When checkpoint created */
};

/*
** Finality levels for batches
*/
typedef enum {
  HPPLITE_FINALITY_NONE = 0,      /* Not yet produced */
  HPPLITE_FINALITY_SEQUENCED,     /* Sequencer produced */
  HPPLITE_FINALITY_L1             /* Committed to L1 */
} HppliteFinalityLevel;

/*
** Create a new empty batch
*/
HppliteBatch *hpplite_batch_new(uint64_t height);

/*
** Free a batch and all its transactions
*/
void hpplite_batch_free(HppliteBatch *pBatch);

/*
** Add a statement to a batch as its own transaction group
*/
int hpplite_batch_add_txn(HppliteBatch *pBatch, const char *zSql, int64_t nRowsAffected);

/*
** Add a statement to a batch with an explicit transaction group.
** Statements sharing a group are replayed inside one transaction.
*/
int hpplite_batch_add_stmt(HppliteBatch *pBatch, const char *zSql, int64_t nRowsAffected, int64_t txn);

/*
** Set pre-state root
*/
void hpplite_batch_set_pre_state(HppliteBatch *pBatch, const unsigned char *pRoot);

/*
** Set post-state root
*/
void hpplite_batch_set_post_state(HppliteBatch *pBatch, const unsigned char *pRoot);

/*
** Serialize batch to JSON string
** Caller must free returned string with sqlite3_free()
*/
char *hpplite_batch_to_json(const HppliteBatch *pBatch);

/*
** Parse batch from JSON string
** Caller must free returned batch with hpplite_batch_free()
*/
HppliteBatch *hpplite_batch_from_json(const char *zJson);

/* === Checkpoint Functions === */

/*
** Create a new checkpoint
*/
HppliteCheckpoint *hpplite_checkpoint_new(
  uint64_t fromHeight,
  uint64_t toHeight
);

/*
** Free a checkpoint
*/
void hpplite_checkpoint_free(HppliteCheckpoint *pCheckpoint);

/*
** Serialize checkpoint to JSON
** Caller must free returned string with sqlite3_free()
*/
char *hpplite_checkpoint_to_json(const HppliteCheckpoint *pCheckpoint);

/*
** Parse checkpoint from JSON
** Caller must free returned checkpoint with hpplite_checkpoint_free()
*/
HppliteCheckpoint *hpplite_checkpoint_from_json(const char *zJson);

/*
** Compute hash for checkpoint signing
** hash = keccak256(fromHeight || toHeight || postStateRoot || batchesHash)
*/
void hpplite_checkpoint_hash(
  const HppliteCheckpoint *pCheckpoint,
  unsigned char *pHashOut  /* 32 bytes */
);

/*
** Number of bytes a statement occupies in the batch JSON (escaped text plus
** the per-statement framing). Used to cut batches at the DA size limit
** without serializing.
*/
size_t hpplite_batch_stmt_json_size(const char *zSql, int64_t nRowsAffected, int64_t txn);

/*
** Bytes of batch JSON outside the statements (header, roots, brackets).
*/
size_t hpplite_batch_json_overhead(void);

/*
** Helper: convert bytes to hex string
** Caller must provide buffer of at least (len*2 + 1) bytes
*/
void hpplite_to_hex(const unsigned char *pData, int len, char *zOut);

/*
** Helper: convert hex string to bytes
** Returns 0 on success, -1 on error
*/
int hpplite_from_hex(const char *zHex, unsigned char *pOut, int len);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_BATCH_H */
