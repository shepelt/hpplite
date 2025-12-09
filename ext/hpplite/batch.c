/*
** HPPLite Batch Implementation
**
** Batch creation, serialization, and parsing.
** Uses simple hand-rolled JSON for portability.
*/

#include "batch.h"
#include "sqlite3.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>

/*
** Convert bytes to hex string
*/
void hpplite_to_hex(const unsigned char *pData, int len, char *zOut) {
  static const char hexDigits[] = "0123456789abcdef";
  int i;
  for (i = 0; i < len; i++) {
    zOut[i*2] = hexDigits[(pData[i] >> 4) & 0xf];
    zOut[i*2 + 1] = hexDigits[pData[i] & 0xf];
  }
  zOut[len*2] = '\0';
}

/*
** Convert hex string to bytes
*/
int hpplite_from_hex(const char *zHex, unsigned char *pOut, int len) {
  int i;
  for (i = 0; i < len; i++) {
    int hi, lo;
    char c1 = zHex[i*2];
    char c2 = zHex[i*2 + 1];

    if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
    else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
    else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
    else return -1;

    if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
    else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
    else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
    else return -1;

    pOut[i] = (unsigned char)((hi << 4) | lo);
  }
  return 0;
}

/*
** Create a new empty batch
*/
HppliteBatch *hpplite_batch_new(uint64_t height) {
  HppliteBatch *pBatch = sqlite3_malloc(sizeof(HppliteBatch));
  if (pBatch) {
    memset(pBatch, 0, sizeof(HppliteBatch));
    pBatch->height = height;
    pBatch->timestamp = (uint64_t)time(NULL);
  }
  return pBatch;
}

/*
** Free a batch
*/
void hpplite_batch_free(HppliteBatch *pBatch) {
  if (pBatch) {
    int i;
    for (i = 0; i < pBatch->nTxns; i++) {
      sqlite3_free(pBatch->aTxns[i].zSql);
    }
    sqlite3_free(pBatch->aTxns);
    sqlite3_free(pBatch);
  }
}

/*
** Add a transaction to a batch
*/
int hpplite_batch_add_txn(HppliteBatch *pBatch, const char *zSql, int64_t nRowsAffected) {
  HppliteTxn *aNew;

  aNew = sqlite3_realloc(pBatch->aTxns, sizeof(HppliteTxn) * (pBatch->nTxns + 1));
  if (!aNew) return -1;

  pBatch->aTxns = aNew;
  pBatch->aTxns[pBatch->nTxns].zSql = sqlite3_mprintf("%s", zSql);
  pBatch->aTxns[pBatch->nTxns].nRowsAffected = nRowsAffected;
  pBatch->nTxns++;

  return 0;
}

/*
** Set pre-state root
*/
void hpplite_batch_set_pre_state(HppliteBatch *pBatch, const unsigned char *pRoot) {
  memcpy(pBatch->preStateRoot, pRoot, HPPLITE_HASH_SIZE);
}

/*
** Set post-state root
*/
void hpplite_batch_set_post_state(HppliteBatch *pBatch, const unsigned char *pRoot) {
  memcpy(pBatch->postStateRoot, pRoot, HPPLITE_HASH_SIZE);
}

/*
** Escape a string for JSON
** Caller must free returned string with sqlite3_free()
*/
static char *json_escape_string(const char *zIn) {
  sqlite3_str *pStr = sqlite3_str_new(NULL);
  const char *p;

  for (p = zIn; *p; p++) {
    switch (*p) {
      case '"':  sqlite3_str_append(pStr, "\\\"", 2); break;
      case '\\': sqlite3_str_append(pStr, "\\\\", 2); break;
      case '\b': sqlite3_str_append(pStr, "\\b", 2); break;
      case '\f': sqlite3_str_append(pStr, "\\f", 2); break;
      case '\n': sqlite3_str_append(pStr, "\\n", 2); break;
      case '\r': sqlite3_str_append(pStr, "\\r", 2); break;
      case '\t': sqlite3_str_append(pStr, "\\t", 2); break;
      default:
        if ((unsigned char)*p < 0x20) {
          sqlite3_str_appendf(pStr, "\\u%04x", (unsigned char)*p);
        } else {
          sqlite3_str_appendchar(pStr, 1, *p);
        }
    }
  }

  return sqlite3_str_finish(pStr);
}

/*
** Serialize batch to JSON
*/
char *hpplite_batch_to_json(const HppliteBatch *pBatch) {
  sqlite3_str *pStr;
  char hexBuf[HPPLITE_HASH_SIZE * 2 + 1];
  int i;

  if (!pBatch) return NULL;

  pStr = sqlite3_str_new(NULL);

  sqlite3_str_appendf(pStr, "{\n");
  sqlite3_str_appendf(pStr, "  \"height\": %llu,\n", (unsigned long long)pBatch->height);
  sqlite3_str_appendf(pStr, "  \"timestamp\": %llu,\n", (unsigned long long)pBatch->timestamp);

  hpplite_to_hex(pBatch->preStateRoot, HPPLITE_HASH_SIZE, hexBuf);
  sqlite3_str_appendf(pStr, "  \"pre_state_root\": \"%s\",\n", hexBuf);

  hpplite_to_hex(pBatch->postStateRoot, HPPLITE_HASH_SIZE, hexBuf);
  sqlite3_str_appendf(pStr, "  \"post_state_root\": \"%s\",\n", hexBuf);

  sqlite3_str_appendf(pStr, "  \"transactions\": [\n");

  for (i = 0; i < pBatch->nTxns; i++) {
    char *escaped = json_escape_string(pBatch->aTxns[i].zSql);
    sqlite3_str_appendf(pStr, "    {\"sql\": \"%s\"", escaped ? escaped : "");
    sqlite3_free(escaped);

    if (pBatch->aTxns[i].nRowsAffected >= 0) {
      sqlite3_str_appendf(pStr, ", \"rows_affected\": %lld",
                          (long long)pBatch->aTxns[i].nRowsAffected);
    }

    sqlite3_str_appendf(pStr, "}%s\n", (i < pBatch->nTxns - 1) ? "," : "");
  }

  sqlite3_str_appendf(pStr, "  ]\n");
  sqlite3_str_appendf(pStr, "}");

  return sqlite3_str_finish(pStr);
}

/*
** Simple JSON parser helper - skip whitespace
*/
static const char *json_skip_ws(const char *p) {
  while (*p && isspace((unsigned char)*p)) p++;
  return p;
}

/*
** Simple JSON parser helper - parse string (returns malloc'd string)
*/
static char *json_parse_string(const char **pp) {
  const char *p = *pp;
  sqlite3_str *pStr;
  char *result;

  p = json_skip_ws(p);
  if (*p != '"') return NULL;
  p++;

  pStr = sqlite3_str_new(NULL);

  while (*p && *p != '"') {
    if (*p == '\\' && p[1]) {
      p++;
      switch (*p) {
        case '"': sqlite3_str_appendchar(pStr, 1, '"'); break;
        case '\\': sqlite3_str_appendchar(pStr, 1, '\\'); break;
        case 'b': sqlite3_str_appendchar(pStr, 1, '\b'); break;
        case 'f': sqlite3_str_appendchar(pStr, 1, '\f'); break;
        case 'n': sqlite3_str_appendchar(pStr, 1, '\n'); break;
        case 'r': sqlite3_str_appendchar(pStr, 1, '\r'); break;
        case 't': sqlite3_str_appendchar(pStr, 1, '\t'); break;
        default: sqlite3_str_appendchar(pStr, 1, *p);
      }
    } else {
      sqlite3_str_appendchar(pStr, 1, *p);
    }
    p++;
  }

  if (*p == '"') p++;
  *pp = p;

  result = sqlite3_str_finish(pStr);
  return result;
}

/*
** Simple JSON parser helper - parse number
*/
static int64_t json_parse_number(const char **pp) {
  const char *p = json_skip_ws(*pp);
  int64_t val = 0;
  int neg = 0;

  if (*p == '-') { neg = 1; p++; }

  while (*p >= '0' && *p <= '9') {
    val = val * 10 + (*p - '0');
    p++;
  }

  *pp = p;
  return neg ? -val : val;
}

/*
** Parse batch from JSON (simplified parser)
*/
HppliteBatch *hpplite_batch_from_json(const char *zJson) {
  HppliteBatch *pBatch = NULL;
  const char *p = zJson;
  char *zKey, *zVal;

  if (!zJson) return NULL;

  pBatch = hpplite_batch_new(0);
  if (!pBatch) return NULL;

  p = json_skip_ws(p);
  if (*p != '{') goto error;
  p++;

  while (*p) {
    p = json_skip_ws(p);
    if (*p == '}') break;
    if (*p == ',') { p++; continue; }

    /* Parse key */
    zKey = json_parse_string(&p);
    if (!zKey) goto error;

    p = json_skip_ws(p);
    if (*p != ':') { sqlite3_free(zKey); goto error; }
    p++;
    p = json_skip_ws(p);

    /* Parse value based on key */
    if (strcmp(zKey, "height") == 0) {
      pBatch->height = (uint64_t)json_parse_number(&p);
    } else if (strcmp(zKey, "timestamp") == 0) {
      pBatch->timestamp = (uint64_t)json_parse_number(&p);
    } else if (strcmp(zKey, "pre_state_root") == 0) {
      zVal = json_parse_string(&p);
      if (zVal) {
        hpplite_from_hex(zVal, pBatch->preStateRoot, HPPLITE_HASH_SIZE);
        sqlite3_free(zVal);
      }
    } else if (strcmp(zKey, "post_state_root") == 0) {
      zVal = json_parse_string(&p);
      if (zVal) {
        hpplite_from_hex(zVal, pBatch->postStateRoot, HPPLITE_HASH_SIZE);
        sqlite3_free(zVal);
      }
    } else if (strcmp(zKey, "transactions") == 0) {
      /* Parse transactions array */
      p = json_skip_ws(p);
      if (*p != '[') { sqlite3_free(zKey); goto error; }
      p++;

      while (*p) {
        p = json_skip_ws(p);
        if (*p == ']') { p++; break; }
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;
        p++;

        /* Parse transaction object */
        char *zSql = NULL;
        int64_t nRows = -1;

        while (*p && *p != '}') {
          p = json_skip_ws(p);
          if (*p == ',') { p++; continue; }

          char *txKey = json_parse_string(&p);
          if (!txKey) break;

          p = json_skip_ws(p);
          if (*p == ':') p++;
          p = json_skip_ws(p);

          if (strcmp(txKey, "sql") == 0) {
            zSql = json_parse_string(&p);
          } else if (strcmp(txKey, "rows_affected") == 0) {
            nRows = json_parse_number(&p);
          } else {
            /* Skip unknown value */
            if (*p == '"') {
              char *skip = json_parse_string(&p);
              sqlite3_free(skip);
            } else {
              json_parse_number(&p);
            }
          }
          sqlite3_free(txKey);
        }

        if (*p == '}') p++;

        if (zSql) {
          hpplite_batch_add_txn(pBatch, zSql, nRows);
          sqlite3_free(zSql);
        }
      }
    } else {
      /* Skip unknown value */
      if (*p == '"') {
        zVal = json_parse_string(&p);
        sqlite3_free(zVal);
      } else if (*p == '[') {
        int depth = 1;
        p++;
        while (*p && depth > 0) {
          if (*p == '[') depth++;
          else if (*p == ']') depth--;
          p++;
        }
      } else {
        json_parse_number(&p);
      }
    }

    sqlite3_free(zKey);
  }

  return pBatch;

error:
  hpplite_batch_free(pBatch);
  return NULL;
}

/* === Checkpoint Implementation === */

/*
** Create a new checkpoint
*/
HppliteCheckpoint *hpplite_checkpoint_new(uint64_t fromHeight, uint64_t toHeight) {
  HppliteCheckpoint *pCheckpoint = sqlite3_malloc(sizeof(HppliteCheckpoint));
  if (pCheckpoint) {
    memset(pCheckpoint, 0, sizeof(HppliteCheckpoint));
    pCheckpoint->fromHeight = fromHeight;
    pCheckpoint->toHeight = toHeight;
    pCheckpoint->timestamp = (uint64_t)time(NULL);
  }
  return pCheckpoint;
}

/*
** Free a checkpoint
*/
void hpplite_checkpoint_free(HppliteCheckpoint *pCheckpoint) {
  if (pCheckpoint) {
    sqlite3_free(pCheckpoint);
  }
}

/*
** Serialize checkpoint to JSON
*/
char *hpplite_checkpoint_to_json(const HppliteCheckpoint *pCheckpoint) {
  char preHex[HPPLITE_HASH_SIZE * 2 + 1];
  char postHex[HPPLITE_HASH_SIZE * 2 + 1];
  char batchesHex[HPPLITE_HASH_SIZE * 2 + 1];

  if (!pCheckpoint) return NULL;

  hpplite_to_hex(pCheckpoint->preStateRoot, HPPLITE_HASH_SIZE, preHex);
  hpplite_to_hex(pCheckpoint->postStateRoot, HPPLITE_HASH_SIZE, postHex);
  hpplite_to_hex(pCheckpoint->batchesHash, HPPLITE_HASH_SIZE, batchesHex);

  return sqlite3_mprintf(
    "{"
    "\"from_height\":%llu,"
    "\"to_height\":%llu,"
    "\"pre_state_root\":\"%s\","
    "\"post_state_root\":\"%s\","
    "\"batches_hash\":\"%s\","
    "\"timestamp\":%llu"
    "}",
    (unsigned long long)pCheckpoint->fromHeight,
    (unsigned long long)pCheckpoint->toHeight,
    preHex,
    postHex,
    batchesHex,
    (unsigned long long)pCheckpoint->timestamp
  );
}

/*
** Parse checkpoint from JSON
*/
HppliteCheckpoint *hpplite_checkpoint_from_json(const char *zJson) {
  HppliteCheckpoint *pCheckpoint = NULL;
  const char *p = zJson;
  char *zKey, *zVal;

  if (!zJson) return NULL;

  pCheckpoint = sqlite3_malloc(sizeof(HppliteCheckpoint));
  if (!pCheckpoint) return NULL;
  memset(pCheckpoint, 0, sizeof(HppliteCheckpoint));

  p = json_skip_ws(p);
  if (*p != '{') goto error;
  p++;

  while (*p) {
    p = json_skip_ws(p);
    if (*p == '}') break;
    if (*p == ',') { p++; continue; }

    zKey = json_parse_string(&p);
    if (!zKey) goto error;

    p = json_skip_ws(p);
    if (*p != ':') { sqlite3_free(zKey); goto error; }
    p++;
    p = json_skip_ws(p);

    if (strcmp(zKey, "from_height") == 0) {
      pCheckpoint->fromHeight = (uint64_t)json_parse_number(&p);
    } else if (strcmp(zKey, "to_height") == 0) {
      pCheckpoint->toHeight = (uint64_t)json_parse_number(&p);
    } else if (strcmp(zKey, "timestamp") == 0) {
      pCheckpoint->timestamp = (uint64_t)json_parse_number(&p);
    } else if (strcmp(zKey, "pre_state_root") == 0) {
      zVal = json_parse_string(&p);
      if (zVal) {
        hpplite_from_hex(zVal, pCheckpoint->preStateRoot, HPPLITE_HASH_SIZE);
        sqlite3_free(zVal);
      }
    } else if (strcmp(zKey, "post_state_root") == 0) {
      zVal = json_parse_string(&p);
      if (zVal) {
        hpplite_from_hex(zVal, pCheckpoint->postStateRoot, HPPLITE_HASH_SIZE);
        sqlite3_free(zVal);
      }
    } else if (strcmp(zKey, "batches_hash") == 0) {
      zVal = json_parse_string(&p);
      if (zVal) {
        hpplite_from_hex(zVal, pCheckpoint->batchesHash, HPPLITE_HASH_SIZE);
        sqlite3_free(zVal);
      }
    } else {
      if (*p == '"') {
        zVal = json_parse_string(&p);
        sqlite3_free(zVal);
      } else {
        json_parse_number(&p);
      }
    }

    sqlite3_free(zKey);
  }

  return pCheckpoint;

error:
  hpplite_checkpoint_free(pCheckpoint);
  return NULL;
}

/*
** Compute hash for checkpoint signing
** Uses keccak256 from crypto module
*/
void hpplite_checkpoint_hash(
  const HppliteCheckpoint *pCheckpoint,
  unsigned char *pHashOut
) {
  /*
  ** Hash = keccak256(fromHeight || toHeight || postStateRoot || batchesHash)
  ** For now, use simple concatenation - will use keccak256 via crypto module
  */
  unsigned char data[8 + 8 + 32 + 32];
  int offset = 0;

  if (!pCheckpoint || !pHashOut) return;

  /* fromHeight (big-endian) */
  data[offset++] = (pCheckpoint->fromHeight >> 56) & 0xFF;
  data[offset++] = (pCheckpoint->fromHeight >> 48) & 0xFF;
  data[offset++] = (pCheckpoint->fromHeight >> 40) & 0xFF;
  data[offset++] = (pCheckpoint->fromHeight >> 32) & 0xFF;
  data[offset++] = (pCheckpoint->fromHeight >> 24) & 0xFF;
  data[offset++] = (pCheckpoint->fromHeight >> 16) & 0xFF;
  data[offset++] = (pCheckpoint->fromHeight >> 8) & 0xFF;
  data[offset++] = pCheckpoint->fromHeight & 0xFF;

  /* toHeight (big-endian) */
  data[offset++] = (pCheckpoint->toHeight >> 56) & 0xFF;
  data[offset++] = (pCheckpoint->toHeight >> 48) & 0xFF;
  data[offset++] = (pCheckpoint->toHeight >> 40) & 0xFF;
  data[offset++] = (pCheckpoint->toHeight >> 32) & 0xFF;
  data[offset++] = (pCheckpoint->toHeight >> 24) & 0xFF;
  data[offset++] = (pCheckpoint->toHeight >> 16) & 0xFF;
  data[offset++] = (pCheckpoint->toHeight >> 8) & 0xFF;
  data[offset++] = pCheckpoint->toHeight & 0xFF;

  /* postStateRoot */
  memcpy(data + offset, pCheckpoint->postStateRoot, 32);
  offset += 32;

  /* batchesHash */
  memcpy(data + offset, pCheckpoint->batchesHash, 32);
  offset += 32;

  /* Use keccak256 for Ethereum compatibility */
  extern void hpplite_keccak256(const unsigned char*, size_t, unsigned char*);
  hpplite_keccak256(data, (size_t)offset, pHashOut);
}
