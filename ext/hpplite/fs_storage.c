/*
** HPPLite Filesystem Storage Implementation
**
** Provides batch storage and commitment indexing using the filesystem.
*/

#include "fs_storage.h"
#include "sqlite3.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#define PATH_SEP '\\'
#else
#include <unistd.h>
#define PATH_SEP '/'
#endif

struct HppliteStorage {
  char *zDataDir;           /* Base data directory */
  char *zBatchesDir;        /* batches/ subdirectory */
  char *zCommitmentsPath;   /* commitments.jsonl path */
  char *zDbPath;            /* db.sqlite path */

  /* Cached latest commitment */
  uint64_t latestHeight;
  unsigned char latestStateRoot[HPPLITE_HASH_SIZE];
  int hasLatest;
};

/*
** Create directory if it doesn't exist
*/
static int ensure_dir(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    return S_ISDIR(st.st_mode) ? 0 : -1;
  }
  return mkdir(path, 0755);
}

/*
** Read entire file into memory
** Caller must free returned buffer with sqlite3_free()
*/
static char *read_file(const char *path, int *pnSize) {
  FILE *f;
  long size;
  char *buf;

  f = fopen(path, "rb");
  if (!f) return NULL;

  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);

  buf = sqlite3_malloc((int)size + 1);
  if (buf) {
    size_t nRead = fread(buf, 1, (size_t)size, f);
    buf[nRead] = '\0';
    if (pnSize) *pnSize = (int)nRead;
  }

  fclose(f);
  return buf;
}

/*
** Write buffer to file
*/
static int write_file(const char *path, const char *data, int len) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;

  size_t written = fwrite(data, 1, (size_t)len, f);
  fclose(f);

  return (written == (size_t)len) ? 0 : -1;
}

/*
** Append line to file
*/
static int append_line(const char *path, const char *line) {
  FILE *f = fopen(path, "a");
  if (!f) return -1;

  fprintf(f, "%s\n", line);
  fclose(f);
  return 0;
}

/*
** Initialize storage
*/
int hpplite_storage_init(const char *zDataDir, HppliteStorage **ppStorage) {
  HppliteStorage *pStorage;

  pStorage = sqlite3_malloc(sizeof(HppliteStorage));
  if (!pStorage) return -1;
  memset(pStorage, 0, sizeof(HppliteStorage));

  /* Set paths */
  pStorage->zDataDir = sqlite3_mprintf("%s", zDataDir);
  pStorage->zBatchesDir = sqlite3_mprintf("%s%cbatches", zDataDir, PATH_SEP);
  pStorage->zCommitmentsPath = sqlite3_mprintf("%s%ccommitments.jsonl", zDataDir, PATH_SEP);
  pStorage->zDbPath = sqlite3_mprintf("%s%cdb.sqlite", zDataDir, PATH_SEP);

  /* Create directories */
  if (ensure_dir(zDataDir) != 0) {
    hpplite_storage_close(pStorage);
    return -1;
  }
  if (ensure_dir(pStorage->zBatchesDir) != 0) {
    hpplite_storage_close(pStorage);
    return -1;
  }

  /* Load latest commitment if exists */
  pStorage->latestHeight = hpplite_storage_get_latest_height(pStorage);
  if (pStorage->latestHeight > 0) {
    HppliteCommitment *pCommit = hpplite_storage_get_commitment(pStorage, pStorage->latestHeight);
    if (pCommit) {
      memcpy(pStorage->latestStateRoot, pCommit->stateRoot, HPPLITE_HASH_SIZE);
      pStorage->hasLatest = 1;
      hpplite_commitment_free(pCommit);
    }
  }

  *ppStorage = pStorage;
  return 0;
}

/*
** Close storage
*/
void hpplite_storage_close(HppliteStorage *pStorage) {
  if (pStorage) {
    sqlite3_free(pStorage->zDataDir);
    sqlite3_free(pStorage->zBatchesDir);
    sqlite3_free(pStorage->zCommitmentsPath);
    sqlite3_free(pStorage->zDbPath);
    sqlite3_free(pStorage);
  }
}

/*
** Get data directory
*/
const char *hpplite_storage_get_dir(HppliteStorage *pStorage) {
  return pStorage ? pStorage->zDataDir : NULL;
}

/*
** Get database path
*/
char *hpplite_storage_get_db_path(HppliteStorage *pStorage) {
  if (!pStorage) return NULL;
  return sqlite3_mprintf("%s", pStorage->zDbPath);
}

/*
** Store a batch
*/
char *hpplite_storage_store_batch(HppliteStorage *pStorage, const HppliteBatch *pBatch) {
  char *zRef;
  char *zPath;
  char *zJson;
  int rc;

  if (!pStorage || !pBatch) return NULL;

  /* Generate reference path */
  zRef = sqlite3_mprintf("batches%c%08llu.json", PATH_SEP, (unsigned long long)pBatch->height);

  /* Generate full path */
  zPath = sqlite3_mprintf("%s%c%08llu.json",
                          pStorage->zBatchesDir, PATH_SEP,
                          (unsigned long long)pBatch->height);

  /* Serialize to JSON */
  zJson = hpplite_batch_to_json(pBatch);
  if (!zJson) {
    sqlite3_free(zRef);
    sqlite3_free(zPath);
    return NULL;
  }

  /* Write to file */
  rc = write_file(zPath, zJson, (int)strlen(zJson));
  sqlite3_free(zJson);
  sqlite3_free(zPath);

  if (rc != 0) {
    sqlite3_free(zRef);
    return NULL;
  }

  return zRef;
}

/*
** Load a batch
*/
HppliteBatch *hpplite_storage_load_batch(HppliteStorage *pStorage, const char *zRef) {
  char *zPath;
  char *zJson;
  HppliteBatch *pBatch;

  if (!pStorage || !zRef) return NULL;

  /* Build full path */
  zPath = sqlite3_mprintf("%s%c%s", pStorage->zDataDir, PATH_SEP, zRef);

  /* Read file */
  zJson = read_file(zPath, NULL);
  sqlite3_free(zPath);

  if (!zJson) return NULL;

  /* Parse JSON */
  pBatch = hpplite_batch_from_json(zJson);
  sqlite3_free(zJson);

  return pBatch;
}

/*
** Check if batch exists
*/
int hpplite_storage_batch_exists(HppliteStorage *pStorage, const char *zRef) {
  char *zPath;
  struct stat st;
  int exists;

  if (!pStorage || !zRef) return 0;

  zPath = sqlite3_mprintf("%s%c%s", pStorage->zDataDir, PATH_SEP, zRef);
  exists = (stat(zPath, &st) == 0);
  sqlite3_free(zPath);

  return exists;
}

/*
** Post a commitment
*/
int hpplite_storage_post_commitment(HppliteStorage *pStorage, const HppliteCommitment *pCommit) {
  char *zJson;
  int rc;

  if (!pStorage || !pCommit) return -1;

  zJson = hpplite_commitment_to_json(pCommit);
  if (!zJson) return -1;

  rc = append_line(pStorage->zCommitmentsPath, zJson);
  sqlite3_free(zJson);

  if (rc == 0) {
    /* Update cache */
    pStorage->latestHeight = pCommit->height;
    memcpy(pStorage->latestStateRoot, pCommit->stateRoot, HPPLITE_HASH_SIZE);
    pStorage->hasLatest = 1;
  }

  return rc;
}

/*
** Get all commitments
*/
int hpplite_storage_get_commitments(
  HppliteStorage *pStorage,
  HppliteCommitment ***paCommits
) {
  char *zData;
  char *line, *saveptr;
  HppliteCommitment **aCommits = NULL;
  int nCommits = 0;
  int capacity = 0;

  if (!pStorage || !paCommits) return -1;
  *paCommits = NULL;

  zData = read_file(pStorage->zCommitmentsPath, NULL);
  if (!zData) return 0;  /* No commitments file = 0 commitments */

  /* Parse each line */
  line = strtok_r(zData, "\n", &saveptr);
  while (line) {
    if (strlen(line) > 0) {
      HppliteCommitment *pCommit = hpplite_commitment_from_json(line);
      if (pCommit) {
        /* Grow array if needed */
        if (nCommits >= capacity) {
          capacity = capacity ? capacity * 2 : 16;
          aCommits = sqlite3_realloc(aCommits, sizeof(HppliteCommitment*) * capacity);
        }
        aCommits[nCommits++] = pCommit;
      }
    }
    line = strtok_r(NULL, "\n", &saveptr);
  }

  sqlite3_free(zData);
  *paCommits = aCommits;
  return nCommits;
}

/*
** Get commitment by height
*/
HppliteCommitment *hpplite_storage_get_commitment(HppliteStorage *pStorage, uint64_t height) {
  HppliteCommitment **aCommits;
  HppliteCommitment *result = NULL;
  int nCommits, i;

  nCommits = hpplite_storage_get_commitments(pStorage, &aCommits);
  if (nCommits <= 0) return NULL;

  for (i = 0; i < nCommits; i++) {
    if (aCommits[i]->height == height) {
      result = aCommits[i];
      aCommits[i] = NULL;  /* Don't free this one */
    } else {
      hpplite_commitment_free(aCommits[i]);
    }
  }

  sqlite3_free(aCommits);
  return result;
}

/*
** Get latest height
*/
uint64_t hpplite_storage_get_latest_height(HppliteStorage *pStorage) {
  HppliteCommitment **aCommits;
  int nCommits, i;
  uint64_t maxHeight = 0;

  if (!pStorage) return 0;

  /* Use cache if available */
  if (pStorage->hasLatest) {
    return pStorage->latestHeight;
  }

  nCommits = hpplite_storage_get_commitments(pStorage, &aCommits);
  if (nCommits <= 0) return 0;

  for (i = 0; i < nCommits; i++) {
    if (aCommits[i]->height > maxHeight) {
      maxHeight = aCommits[i]->height;
    }
    hpplite_commitment_free(aCommits[i]);
  }

  sqlite3_free(aCommits);
  return maxHeight;
}

/*
** Get latest state root
*/
const unsigned char *hpplite_storage_get_latest_state_root(HppliteStorage *pStorage) {
  if (!pStorage || !pStorage->hasLatest) return NULL;
  return pStorage->latestStateRoot;
}
