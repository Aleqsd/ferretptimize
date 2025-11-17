#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef long long sqlite3_int64;
typedef unsigned long long sqlite3_uint64;

#ifndef SQLITE_OK
#define SQLITE_OK 0
#define SQLITE_ROW 100
#define SQLITE_DONE 101
#define SQLITE_OPEN_READWRITE 0x00000002
#define SQLITE_OPEN_CREATE 0x00000004
#define SQLITE_OPEN_FULLMUTEX 0x00010000
#define SQLITE_TRANSIENT ((void (*)(void *)) -1)
#endif

int sqlite3_open_v2(const char *filename, sqlite3 **ppDb, int flags, const char *zVfs);
int sqlite3_close(sqlite3 *);
int sqlite3_busy_timeout(sqlite3 *, int ms);
int sqlite3_exec(sqlite3 *, const char *sql, int (*callback)(void *, int, char **, char **), void *, char **errmsg);
int sqlite3_prepare_v2(sqlite3 *, const char *zSql, int nByte, sqlite3_stmt **ppStmt, const char **pzTail);
int sqlite3_bind_text(sqlite3_stmt *, int, const char *, int, void (*)(void *));
int sqlite3_bind_int64(sqlite3_stmt *, int, long long);
int sqlite3_step(sqlite3_stmt *);
int sqlite3_finalize(sqlite3_stmt *);
long long sqlite3_last_insert_rowid(sqlite3 *);
const unsigned char *sqlite3_column_text(sqlite3_stmt *, int iCol);
long long sqlite3_column_int64(sqlite3_stmt *, int iCol);
const char *sqlite3_errmsg(sqlite3 *);
void sqlite3_free(void *);

typedef struct {
    sqlite3 *db;
    char jwt_secret[128];
    size_t jwt_secret_len;
    int access_ttl_seconds;
    int refresh_ttl_seconds;
} fp_auth_store;

typedef struct {
    uint64_t id;
    char email[256];
    char name[256];
    char provider[32];
    char picture[512];
} fp_auth_user;

typedef struct {
    char status[32];
    char stripe_customer_id[128];
    char stripe_subscription_id[128];
    time_t current_period_end;
} fp_auth_subscription;

typedef struct {
    char access_token[512];
    char refresh_token[256];
    time_t access_expires_at;
    time_t refresh_expires_at;
} fp_auth_tokens;

int fp_auth_store_init(fp_auth_store *store);
void fp_auth_store_close(fp_auth_store *store);

int fp_auth_upsert_user(fp_auth_store *store, const char *provider, const char *provider_user_id,
                        const char *email, const char *name, const char *picture, const char *profile_json,
                        fp_auth_user *out_user);

int fp_auth_issue_tokens(fp_auth_store *store, const fp_auth_user *user, fp_auth_tokens *out_tokens);
int fp_auth_validate_access(fp_auth_store *store, const char *token, fp_auth_user *out_user);

int fp_auth_generate_api_key(fp_auth_store *store, uint64_t user_id, const char *scope, const char *label,
                             char *out_token, size_t out_len);
int fp_auth_api_key_allowed(fp_auth_store *store, const char *token, const char *required_scope, fp_auth_user *out_user);

int fp_auth_record_audit(fp_auth_store *store, uint64_t user_id, const char *event, const char *metadata_json);

int fp_auth_sync_subscription(fp_auth_store *store, uint64_t user_id, const char *status,
                              const char *customer_id, const char *subscription_id, time_t period_end);
int fp_auth_get_subscription(fp_auth_store *store, uint64_t user_id, fp_auth_subscription *out);
int fp_auth_has_active_subscription(fp_auth_store *store, uint64_t user_id);
int fp_auth_find_user_by_stripe(fp_auth_store *store, const char *customer_id, const char *subscription_id, uint64_t *out_user_id);
int fp_auth_revoke_api_keys(fp_auth_store *store, uint64_t user_id, const char *reason);
