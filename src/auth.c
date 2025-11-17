#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <strings.h>
#include <unistd.h>

#include "auth.h"
#include "log.h"

#define FP_SHA256_LEN 32

typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} fp_sha256_ctx;

static const uint32_t FP_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTRIGHT(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x, 2) ^ ROTRIGHT(x, 13) ^ ROTRIGHT(x, 22))
#define EP1(x) (ROTRIGHT(x, 6) ^ ROTRIGHT(x, 11) ^ ROTRIGHT(x, 25))
#define SIG0(x) (ROTRIGHT(x, 7) ^ ROTRIGHT(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x, 17) ^ ROTRIGHT(x, 19) ^ ((x) >> 10))

static void fp_sha256_transform(fp_sha256_ctx *ctx, const uint8_t data[]) {
    uint32_t m[64];
    for (uint32_t i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) | ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    }
    for (uint32_t i = 16; i < 64; ++i) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t t1 = h + EP1(e) + CH(e, f, g) + FP_K[i] + m[i];
        uint32_t t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void fp_sha256_init(fp_sha256_ctx *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

static void fp_sha256_update(fp_sha256_ctx *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            fp_sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void fp_sha256_final(fp_sha256_ctx *ctx, uint8_t hash[FP_SHA256_LEN]) {
    uint32_t i = ctx->datalen;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) {
            ctx->data[i++] = 0x00;
        }
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) {
            ctx->data[i++] = 0x00;
        }
        fp_sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    ctx->data[63] = (uint8_t)ctx->bitlen;
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    fp_sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; ++i) {
        hash[i] = (uint8_t)((ctx->state[0] >> (24 - i * 8)) & 0x000000ff);
        hash[i + 4] = (uint8_t)((ctx->state[1] >> (24 - i * 8)) & 0x000000ff);
        hash[i + 8] = (uint8_t)((ctx->state[2] >> (24 - i * 8)) & 0x000000ff);
        hash[i + 12] = (uint8_t)((ctx->state[3] >> (24 - i * 8)) & 0x000000ff);
        hash[i + 16] = (uint8_t)((ctx->state[4] >> (24 - i * 8)) & 0x000000ff);
        hash[i + 20] = (uint8_t)((ctx->state[5] >> (24 - i * 8)) & 0x000000ff);
        hash[i + 24] = (uint8_t)((ctx->state[6] >> (24 - i * 8)) & 0x000000ff);
        hash[i + 28] = (uint8_t)((ctx->state[7] >> (24 - i * 8)) & 0x000000ff);
    }
}

static void fp_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t out[FP_SHA256_LEN]) {
    uint8_t key_block[64] = {0};
    if (key_len > sizeof(key_block)) {
        fp_sha256_ctx key_ctx;
        fp_sha256_init(&key_ctx);
        fp_sha256_update(&key_ctx, key, key_len);
        fp_sha256_final(&key_ctx, key_block);
        key_len = FP_SHA256_LEN;
    } else {
        memcpy(key_block, key, key_len);
    }

    uint8_t o_key_pad[64];
    uint8_t i_key_pad[64];
    for (size_t i = 0; i < sizeof(key_block); ++i) {
        o_key_pad[i] = (uint8_t)(key_block[i] ^ 0x5c);
        i_key_pad[i] = (uint8_t)(key_block[i] ^ 0x36);
    }

    uint8_t inner_hash[FP_SHA256_LEN];
    fp_sha256_ctx ctx;
    fp_sha256_init(&ctx);
    fp_sha256_update(&ctx, i_key_pad, sizeof(i_key_pad));
    fp_sha256_update(&ctx, data, data_len);
    fp_sha256_final(&ctx, inner_hash);

    fp_sha256_init(&ctx);
    fp_sha256_update(&ctx, o_key_pad, sizeof(o_key_pad));
    fp_sha256_update(&ctx, inner_hash, sizeof(inner_hash));
    fp_sha256_final(&ctx, out);
}

static int fp_consttime_equal(const uint8_t *a, const uint8_t *b, size_t len) {
    unsigned int diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

static int fp_read_int_env(const char *name, int fallback) {
    const char *value = getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value) {
        return fallback;
    }
    return (int)parsed;
}

static void fp_trim_inplace(char *s) {
    if (!s) {
        return;
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    size_t start = 0;
    while (s[start] && isspace((unsigned char)s[start])) {
        ++start;
    }
    if (start > 0) {
        memmove(s, s + start, len - start + 1);
    }
}

static int fp_random_bytes(uint8_t *out, size_t len) {
    if (!out || len == 0) {
        return -1;
    }
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    size_t offset = 0;
    while (offset < len) {
        ssize_t got = read(fd, out + offset, len - offset);
        if (got <= 0) {
            close(fd);
            return -1;
        }
        offset += (size_t)got;
    }
    close(fd);
    return 0;
}

static int fp_sha256_hex(const char *input, char *out_hex, size_t out_hex_len) {
    if (!input || !out_hex || out_hex_len < FP_SHA256_LEN * 2 + 1) {
        return -1;
    }
    uint8_t digest[FP_SHA256_LEN];
    fp_sha256_ctx ctx;
    fp_sha256_init(&ctx);
    fp_sha256_update(&ctx, (const uint8_t *)input, strlen(input));
    fp_sha256_final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < FP_SHA256_LEN; ++i) {
        out_hex[i * 2] = hex[(digest[i] >> 4) & 0x0F];
        out_hex[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out_hex[FP_SHA256_LEN * 2] = '\0';
    return 0;
}

static char *fp_base64url_encode(const uint8_t *data, size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t encoded_len = len ? (4 * ((len + 2) / 3)) : 0;
    char *tmp = malloc(encoded_len + 1);
    if (!tmp) {
        return NULL;
    }
    size_t i = 0;
    size_t j = 0;
    while (i + 2 < len) {
        uint32_t triple = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        tmp[j++] = table[(triple >> 18) & 0x3F];
        tmp[j++] = table[(triple >> 12) & 0x3F];
        tmp[j++] = table[(triple >> 6) & 0x3F];
        tmp[j++] = table[triple & 0x3F];
        i += 3;
    }
    size_t remaining = len - i;
    if (remaining == 1) {
        uint32_t triple = (uint32_t)data[i] << 16;
        tmp[j++] = table[(triple >> 18) & 0x3F];
        tmp[j++] = table[(triple >> 12) & 0x3F];
        tmp[j++] = '=';
        tmp[j++] = '=';
    } else if (remaining == 2) {
        uint32_t triple = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        tmp[j++] = table[(triple >> 18) & 0x3F];
        tmp[j++] = table[(triple >> 12) & 0x3F];
        tmp[j++] = table[(triple >> 6) & 0x3F];
        tmp[j++] = '=';
    }
    tmp[encoded_len] = '\0';

    for (size_t k = 0; k < encoded_len; ++k) {
        if (tmp[k] == '+') {
            tmp[k] = '-';
        } else if (tmp[k] == '/') {
            tmp[k] = '_';
        }
    }
    while (encoded_len > 0 && tmp[encoded_len - 1] == '=') {
        tmp[--encoded_len] = '\0';
    }
    return tmp;
}

static int fp_base64url_decode(const char *input, uint8_t **out, size_t *out_len) {
    if (!input || !out || !out_len) {
        return -1;
    }
    size_t len = strlen(input);
    size_t padded_len = len;
    size_t mod = len % 4;
    if (mod != 0) {
        padded_len += 4 - mod;
    }
    char *buf = malloc(padded_len + 1);
    if (!buf) {
        return -1;
    }
    for (size_t i = 0; i < len; ++i) {
        char c = input[i];
        if (c == '-') {
            buf[i] = '+';
        } else if (c == '_') {
            buf[i] = '/';
        } else {
            buf[i] = c;
        }
    }
    for (size_t i = len; i < padded_len; ++i) {
        buf[i] = '=';
    }
    buf[padded_len] = '\0';

    static const uint8_t table[256] = {
        ['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,  ['E'] = 4,  ['F'] = 5,  ['G'] = 6,  ['H'] = 7,
        ['I'] = 8,  ['J'] = 9,  ['K'] = 10, ['L'] = 11, ['M'] = 12, ['N'] = 13, ['O'] = 14, ['P'] = 15,
        ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19, ['U'] = 20, ['V'] = 21, ['W'] = 22, ['X'] = 23,
        ['Y'] = 24, ['Z'] = 25,
        ['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29, ['e'] = 30, ['f'] = 31, ['g'] = 32, ['h'] = 33,
        ['i'] = 34, ['j'] = 35, ['k'] = 36, ['l'] = 37, ['m'] = 38, ['n'] = 39, ['o'] = 40, ['p'] = 41,
        ['q'] = 42, ['r'] = 43, ['s'] = 44, ['t'] = 45, ['u'] = 46, ['v'] = 47, ['w'] = 48, ['x'] = 49,
        ['y'] = 50, ['z'] = 51,
        ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55, ['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59,
        ['8'] = 60, ['9'] = 61, ['+'] = 62, ['/'] = 63
    };

    size_t alloc = (padded_len / 4) * 3;
    uint8_t *decoded = malloc(alloc);
    if (!decoded) {
        free(buf);
        return -1;
    }
    size_t out_idx = 0;
    for (size_t i = 0; i < padded_len; i += 4) {
        uint8_t a = table[(unsigned char)buf[i]];
        uint8_t b = table[(unsigned char)buf[i + 1]];
        uint8_t c = buf[i + 2] == '=' ? 0 : table[(unsigned char)buf[i + 2]];
        uint8_t d = buf[i + 3] == '=' ? 0 : table[(unsigned char)buf[i + 3]];
        decoded[out_idx++] = (uint8_t)((a << 2) | (b >> 4));
        if (buf[i + 2] != '=') {
            decoded[out_idx++] = (uint8_t)(((b & 0x0F) << 4) | (c >> 2));
        }
        if (buf[i + 3] != '=') {
            decoded[out_idx++] = (uint8_t)(((c & 0x03) << 6) | d);
        }
    }
    free(buf);
    *out = decoded;
    *out_len = out_idx;
    return 0;
}

static void fp_auth_generate_secret(fp_auth_store *store) {
    if (!store) {
        return;
    }
    unsigned char buf[32];
    if (fp_random_bytes(buf, sizeof(buf)) != 0) {
        snprintf(store->jwt_secret, sizeof(store->jwt_secret), "fallback-secret");
        store->jwt_secret_len = strlen(store->jwt_secret);
        return;
    }
    size_t idx = 0;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(buf) && idx + 1 < sizeof(store->jwt_secret); ++i) {
        store->jwt_secret[idx++] = hex[(buf[i] >> 4) & 0x0F];
        if (idx + 1 >= sizeof(store->jwt_secret)) {
            break;
        }
        store->jwt_secret[idx++] = hex[buf[i] & 0x0F];
    }
    store->jwt_secret[idx] = '\0';
    store->jwt_secret_len = idx;
}

static int fp_exec_sql(sqlite3 *db, const char *sql) {
    if (!db || !sql) {
        return -1;
    }
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err) {
            fp_log_error("SQL error: %s", err);
            sqlite3_free(err);
        } else {
            fp_log_error("SQL error: rc=%d", rc);
        }
        return -1;
    }
    return 0;
}

int fp_auth_store_init(fp_auth_store *store) {
    if (!store) {
        return -1;
    }
    memset(store, 0, sizeof(*store));

    const char *dsn = getenv("FP_DB_DSN");
    if (!dsn || !*dsn) {
        dsn = "expert_auth.db";
    }
    int rc = sqlite3_open_v2(dsn, &store->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        fp_log_error("Unable to open auth DB %s: %s", dsn, sqlite3_errmsg(store->db));
        if (store->db) {
            sqlite3_close(store->db);
            store->db = NULL;
        }
        return -1;
    }
    sqlite3_busy_timeout(store->db, 5000);
    fp_exec_sql(store->db, "PRAGMA journal_mode=WAL;");
    fp_exec_sql(store->db, "PRAGMA foreign_keys=ON;");

    const char *schema[] = {
        "CREATE TABLE IF NOT EXISTS users ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "provider TEXT NOT NULL,"
        "provider_user_id TEXT NOT NULL,"
        "email TEXT,"
        "name TEXT,"
        "picture TEXT,"
        "profile_json TEXT,"
        "created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
        "updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
        "UNIQUE(provider, provider_user_id)"
        ");",
        "CREATE TABLE IF NOT EXISTS sessions ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "user_id INTEGER NOT NULL,"
        "refresh_token_hash TEXT NOT NULL,"
        "expires_at INTEGER NOT NULL,"
        "created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
        "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
        ");",
        "CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);",
        "CREATE TABLE IF NOT EXISTS api_keys ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "user_id INTEGER NOT NULL,"
        "key_hash TEXT NOT NULL,"
        "scope TEXT NOT NULL DEFAULT 'expert',"
        "label TEXT,"
        "status TEXT NOT NULL DEFAULT 'active',"
        "created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
        "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,"
        "UNIQUE(key_hash)"
        ");",
        "CREATE INDEX IF NOT EXISTS idx_api_keys_user ON api_keys(user_id);",
        "CREATE TABLE IF NOT EXISTS subscriptions ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "user_id INTEGER NOT NULL,"
        "status TEXT NOT NULL DEFAULT 'unknown',"
        "stripe_customer_id TEXT,"
        "stripe_subscription_id TEXT,"
        "current_period_end INTEGER,"
        "updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
        "created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
        "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
        ");",
        "CREATE UNIQUE INDEX IF NOT EXISTS uq_subscriptions_user ON subscriptions(user_id);",
        "CREATE INDEX IF NOT EXISTS idx_subscriptions_user ON subscriptions(user_id);",
        "CREATE TABLE IF NOT EXISTS audit ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "user_id INTEGER,"
        "event TEXT NOT NULL,"
        "metadata_json TEXT,"
        "created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))"
        ");",
    };

    for (size_t i = 0; i < sizeof(schema) / sizeof(schema[0]); ++i) {
        if (fp_exec_sql(store->db, schema[i]) != 0) {
            sqlite3_close(store->db);
            store->db = NULL;
            return -1;
        }
    }

    const char *secret_env = getenv("FP_JWT_SECRET");
    if (secret_env && *secret_env) {
        snprintf(store->jwt_secret, sizeof(store->jwt_secret), "%s", secret_env);
        fp_trim_inplace(store->jwt_secret);
        store->jwt_secret_len = strlen(store->jwt_secret);
    } else {
        fp_auth_generate_secret(store);
        fp_log_warn("FP_JWT_SECRET missing; generated ephemeral secret for this process");
    }

    store->access_ttl_seconds = fp_read_int_env("FP_JWT_TTL", 900);
    store->refresh_ttl_seconds = fp_read_int_env("FP_REFRESH_TTL", 60 * 60 * 24 * 30);

    const char *stripe_key = getenv("FP_STRIPE_SECRET_KEY");
    if (!stripe_key || !*stripe_key) {
        fp_log_warn("Stripe secret key (FP_STRIPE_SECRET_KEY) not configured; billing handlers will be inert");
    }

    fp_log_info("🔐 Auth DB ready at %s (access TTL %ds, refresh TTL %ds)", dsn, store->access_ttl_seconds, store->refresh_ttl_seconds);
    return 0;
}

void fp_auth_store_close(fp_auth_store *store) {
    if (!store) {
        return;
    }
    if (store->db) {
        sqlite3_close(store->db);
        store->db = NULL;
    }
    memset(store->jwt_secret, 0, sizeof(store->jwt_secret));
    store->jwt_secret_len = 0;
}

static int fp_auth_load_user(fp_auth_store *store, uint64_t user_id, fp_auth_user *out_user) {
    if (!store || !store->db) {
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id,email,name,provider,picture FROM users WHERE id = ?";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)user_id);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW && out_user) {
        out_user->id = (uint64_t)sqlite3_column_int64(stmt, 0);
        const unsigned char *email = sqlite3_column_text(stmt, 1);
        const unsigned char *name = sqlite3_column_text(stmt, 2);
        const unsigned char *provider = sqlite3_column_text(stmt, 3);
        const unsigned char *picture = sqlite3_column_text(stmt, 4);
        snprintf(out_user->email, sizeof(out_user->email), "%s", email ? (const char *)email : "");
        snprintf(out_user->name, sizeof(out_user->name), "%s", name ? (const char *)name : "");
        snprintf(out_user->provider, sizeof(out_user->provider), "%s", provider ? (const char *)provider : "");
        snprintf(out_user->picture, sizeof(out_user->picture), "%s", picture ? (const char *)picture : "");
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW ? 0 : -1;
}

int fp_auth_upsert_user(fp_auth_store *store, const char *provider, const char *provider_user_id,
                        const char *email, const char *name, const char *picture, const char *profile_json,
                        fp_auth_user *out_user) {
    if (!store || !provider || !provider_user_id) {
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO users (provider, provider_user_id, email, name, picture, profile_json, created_at, updated_at)"
                      " VALUES (?, ?, ?, ?, ?, ?, strftime('%s','now'), strftime('%s','now'))"
                      " ON CONFLICT(provider, provider_user_id) DO UPDATE SET"
                      " email=excluded.email, name=excluded.name, picture=excluded.picture, profile_json=excluded.profile_json, updated_at=strftime('%s','now');";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fp_log_error("Failed to prepare user upsert: %s", sqlite3_errmsg(store->db));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, provider, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, provider_user_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, email ? email : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, name ? name : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, picture ? picture : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, profile_json ? profile_json : "", -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fp_log_error("Failed to upsert user: %s", sqlite3_errmsg(store->db));
        return -1;
    }

    sqlite3_int64 user_id = sqlite3_last_insert_rowid(store->db);
    if (user_id == 0) {
        const char *lookup_sql = "SELECT id FROM users WHERE provider = ? AND provider_user_id = ?";
        if (sqlite3_prepare_v2(store->db, lookup_sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, provider, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, provider_user_id, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                user_id = sqlite3_column_int64(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }
    if (user_id == 0) {
        return -1;
    }

    if (out_user) {
        if (fp_auth_load_user(store, (uint64_t)user_id, out_user) != 0) {
            out_user->id = (uint64_t)user_id;
            snprintf(out_user->provider, sizeof(out_user->provider), "%s", provider);
            snprintf(out_user->email, sizeof(out_user->email), "%s", email ? email : "");
            snprintf(out_user->name, sizeof(out_user->name), "%s", name ? name : "");
            snprintf(out_user->picture, sizeof(out_user->picture), "%s", picture ? picture : "");
        }
    }
    return 0;
}

static int fp_auth_store_refresh(fp_auth_store *store, uint64_t user_id, const char *refresh_token, time_t expires_at) {
    if (!store || !refresh_token) {
        return -1;
    }
    char hash[FP_SHA256_LEN * 2 + 1];
    if (fp_sha256_hex(refresh_token, hash, sizeof(hash)) != 0) {
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO sessions (user_id, refresh_token_hash, expires_at, created_at)"
                      " VALUES (?, ?, ?, strftime('%s','now'));";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)user_id);
    sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)expires_at);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static void fp_auth_escape_json(const char *in, char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }
    size_t idx = 0;
    if (!in) {
        out[0] = '\0';
        return;
    }
    for (const unsigned char *p = (const unsigned char *)in; *p && idx + 2 < out_len; ++p) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            if (idx + 2 < out_len) {
                out[idx++] = '\\';
                out[idx++] = (char)c;
            }
        } else if (c == '\n') {
            if (idx + 2 < out_len) {
                out[idx++] = '\\';
                out[idx++] = 'n';
            }
        } else if (c == '\r') {
            if (idx + 2 < out_len) {
                out[idx++] = '\\';
                out[idx++] = 'r';
            }
        } else if (c == '\t') {
            if (idx + 2 < out_len) {
                out[idx++] = '\\';
                out[idx++] = 't';
            }
        } else {
            out[idx++] = (char)c;
        }
        if (idx + 1 >= out_len) {
            break;
        }
    }
    out[idx] = '\0';
}

static int fp_auth_build_access_token(fp_auth_store *store, const fp_auth_user *user, time_t now, fp_auth_tokens *out_tokens) {
    if (!store || !user || !store->jwt_secret_len || !out_tokens) {
        return -1;
    }
    char escaped_email[256];
    char escaped_name[256];
    char escaped_provider[64];
    fp_auth_escape_json(user->email, escaped_email, sizeof(escaped_email));
    fp_auth_escape_json(user->name, escaped_name, sizeof(escaped_name));
    fp_auth_escape_json(user->provider, escaped_provider, sizeof(escaped_provider));

    time_t exp = now + store->access_ttl_seconds;
    char payload[512];
    int payload_len = snprintf(payload,
                               sizeof(payload),
                               "{\"sub\":%llu,\"provider\":\"%s\",\"email\":\"%s\",\"name\":\"%s\",\"type\":\"access\",\"exp\":%llu}",
                               (unsigned long long)user->id,
                               escaped_provider,
                               escaped_email,
                               escaped_name,
                               (unsigned long long)exp);
    if (payload_len <= 0 || (size_t)payload_len >= sizeof(payload)) {
        return -1;
    }

    const char *header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    char *b64_header = fp_base64url_encode((const uint8_t *)header, strlen(header));
    char *b64_payload = fp_base64url_encode((const uint8_t *)payload, (size_t)payload_len);
    if (!b64_header || !b64_payload) {
        free(b64_header);
        free(b64_payload);
        return -1;
    }

    size_t signing_len = strlen(b64_header) + 1 + strlen(b64_payload);
    char *signing_input = malloc(signing_len + 1);
    if (!signing_input) {
        free(b64_header);
        free(b64_payload);
        return -1;
    }
    snprintf(signing_input, signing_len + 1, "%s.%s", b64_header, b64_payload);

    uint8_t digest[FP_SHA256_LEN];
    fp_hmac_sha256((const uint8_t *)store->jwt_secret,
                   store->jwt_secret_len,
                   (const uint8_t *)signing_input,
                   strlen(signing_input),
                   digest);
    size_t digest_len = FP_SHA256_LEN;
    char *b64_sig = fp_base64url_encode(digest, digest_len);
    if (!b64_sig) {
        free(signing_input);
        free(b64_header);
        free(b64_payload);
        return -1;
    }

    int token_len = snprintf(out_tokens->access_token,
                             sizeof(out_tokens->access_token),
                             "%s.%s.%s",
                             b64_header,
                             b64_payload,
                             b64_sig);
    free(signing_input);
    free(b64_header);
    free(b64_payload);
    free(b64_sig);
    if (token_len <= 0 || (size_t)token_len >= sizeof(out_tokens->access_token)) {
        return -1;
    }
    out_tokens->access_expires_at = exp;
    return 0;
}

int fp_auth_issue_tokens(fp_auth_store *store, const fp_auth_user *user, fp_auth_tokens *out_tokens) {
    if (!store || !user || !out_tokens) {
        return -1;
    }
    time_t now = time(NULL);
    if (fp_auth_build_access_token(store, user, now, out_tokens) != 0) {
        return -1;
    }

    unsigned char refresh_raw[32];
    if (fp_random_bytes(refresh_raw, sizeof(refresh_raw)) != 0) {
        return -1;
    }
    char *refresh_b64 = fp_base64url_encode(refresh_raw, sizeof(refresh_raw));
    if (!refresh_b64) {
        return -1;
    }
    snprintf(out_tokens->refresh_token, sizeof(out_tokens->refresh_token), "%s", refresh_b64);
    free(refresh_b64);
    out_tokens->refresh_expires_at = now + store->refresh_ttl_seconds;

    if (fp_auth_store_refresh(store, user->id, out_tokens->refresh_token, out_tokens->refresh_expires_at) != 0) {
        return -1;
    }
    return 0;
}

static int fp_json_parse_int64(const char *json, const char *key, int64_t *out) {
    if (!json || !key || !out) {
        return -1;
    }
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *loc = strstr(json, needle);
    if (!loc) {
        return 0;
    }
    loc = strchr(loc, ':');
    if (!loc) {
        return -1;
    }
    ++loc;
    while (isspace((unsigned char)*loc)) {
        ++loc;
    }
    char *endptr = NULL;
    long long val = strtoll(loc, &endptr, 10);
    if (endptr == loc) {
        return -1;
    }
    *out = (int64_t)val;
    return 1;
}

static int fp_json_parse_string(const char *json, const char *key, char *out, size_t out_len) {
    if (!json || !key || !out || out_len == 0) {
        return -1;
    }
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *pos = strstr(json, needle);
    if (!pos) {
        return 0;
    }
    pos = strchr(pos, ':');
    if (!pos) {
        return -1;
    }
    ++pos;
    while (*pos == ' ' || *pos == '\t' || *pos == '\n') {
        ++pos;
    }
    if (*pos != '"') {
        return -1;
    }
    ++pos;
    size_t idx = 0;
    while (*pos && *pos != '"' && idx + 1 < out_len) {
        if (*pos == '\\' && pos[1]) {
            ++pos;
        }
        out[idx++] = *pos++;
    }
    out[idx] = '\0';
    return idx > 0 ? 1 : 0;
}

int fp_auth_validate_access(fp_auth_store *store, const char *token, fp_auth_user *out_user) {
    if (!store || !token || !*token) {
        return -1;
    }
    char *copy = strdup(token);
    if (!copy) {
        return -1;
    }
    char *dot1 = strchr(copy, '.');
    if (!dot1) {
        free(copy);
        return -1;
    }
    char *dot2 = strchr(dot1 + 1, '.');
    if (!dot2) {
        free(copy);
        return -1;
    }
    *dot1 = '\0';
    *dot2 = '\0';
    const char *b64_header = copy;
    const char *b64_payload = dot1 + 1;
    const char *b64_sig = dot2 + 1;

    size_t signing_len = strlen(b64_header) + 1 + strlen(b64_payload);
    char *signing_input = malloc(signing_len + 1);
    if (!signing_input) {
        free(copy);
        return -1;
    }
    snprintf(signing_input, signing_len + 1, "%s.%s", b64_header, b64_payload);

    uint8_t expected[FP_SHA256_LEN];
    fp_hmac_sha256((const uint8_t *)store->jwt_secret,
                   store->jwt_secret_len,
                   (const uint8_t *)signing_input,
                   strlen(signing_input),
                   expected);
    size_t expected_len = FP_SHA256_LEN;

    uint8_t *sig = NULL;
    size_t sig_len = 0;
    if (fp_base64url_decode(b64_sig, &sig, &sig_len) != 0 || !sig) {
        free(copy);
        free(signing_input);
        return -1;
    }
    int rc = -1;
    if (sig_len == expected_len && fp_consttime_equal(sig, expected, expected_len)) {
        uint8_t *payload_raw = NULL;
        size_t payload_len = 0;
        if (fp_base64url_decode(b64_payload, &payload_raw, &payload_len) == 0 && payload_raw) {
            char *payload_json = strndup((const char *)payload_raw, payload_len);
            free(payload_raw);
            if (payload_json) {
                int64_t exp = 0;
                if (fp_json_parse_int64(payload_json, "exp", &exp) == 1) {
                    time_t now = time(NULL);
                    if (exp >= (int64_t)now) {
                        int64_t sub = 0;
                        char provider[32] = {0};
                        char email[256] = {0};
                        char name[256] = {0};
                        fp_json_parse_int64(payload_json, "sub", &sub);
                        fp_json_parse_string(payload_json, "provider", provider, sizeof(provider));
                        fp_json_parse_string(payload_json, "email", email, sizeof(email));
                        fp_json_parse_string(payload_json, "name", name, sizeof(name));
                        if (sub > 0) {
                            if (out_user) {
                                if (fp_auth_load_user(store, (uint64_t)sub, out_user) != 0) {
                                    out_user->id = (uint64_t)sub;
                                    snprintf(out_user->provider, sizeof(out_user->provider), "%s", provider);
                                    snprintf(out_user->email, sizeof(out_user->email), "%s", email);
                                    snprintf(out_user->name, sizeof(out_user->name), "%s", name);
                                }
                            }
                            rc = 0;
                        }
                    }
                }
                free(payload_json);
            }
        }
    }
    free(sig);
    free(copy);
    free(signing_input);
    return rc;
}

int fp_auth_generate_api_key(fp_auth_store *store, uint64_t user_id, const char *scope, const char *label,
                             char *out_token, size_t out_len) {
    if (!store || !out_token || out_len == 0) {
        return -1;
    }
    unsigned char raw[24];
    if (fp_random_bytes(raw, sizeof(raw)) != 0) {
        return -1;
    }
    char *token = fp_base64url_encode(raw, sizeof(raw));
    if (!token) {
        return -1;
    }
    char hash[FP_SHA256_LEN * 2 + 1];
    if (fp_sha256_hex(token, hash, sizeof(hash)) != 0) {
        free(token);
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO api_keys (user_id, key_hash, scope, label, status, created_at)"
                      " VALUES (?, ?, ?, ?, 'active', strftime('%s','now'));";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(token);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)user_id);
    sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, scope && *scope ? scope : "expert", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, label ? label : "", -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        free(token);
        return -1;
    }
    snprintf(out_token, out_len, "%s", token);
    free(token);
    return 0;
}

static int fp_scope_allows(const char *scope_value, const char *required_scope) {
    if (!required_scope || !*required_scope) {
        return 1;
    }
    if (!scope_value || !*scope_value) {
        return 0;
    }
    const char *cursor = scope_value;
    size_t req_len = strlen(required_scope);
    while (*cursor) {
        while (*cursor == ' ' || *cursor == ',') {
            ++cursor;
        }
        const char *start = cursor;
        while (*cursor && *cursor != ',') {
            ++cursor;
        }
        size_t len = (size_t)(cursor - start);
        while (len > 0 && isspace((unsigned char)start[len - 1])) {
            --len;
        }
        if (len == req_len && strncasecmp(start, required_scope, req_len) == 0) {
            return 1;
        }
        if (*cursor == ',') {
            ++cursor;
        }
    }
    return 0;
}

int fp_auth_api_key_allowed(fp_auth_store *store, const char *token, const char *required_scope, fp_auth_user *out_user) {
    if (!store || !token || !*token) {
        return 0;
    }
    char hash[FP_SHA256_LEN * 2 + 1];
    if (fp_sha256_hex(token, hash, sizeof(hash)) != 0) {
        return 0;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT user_id, scope FROM api_keys WHERE key_hash = ? AND status = 'active' LIMIT 1;";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    int allowed = 0;
    if (rc == SQLITE_ROW) {
        const unsigned char *scope = sqlite3_column_text(stmt, 1);
        if (fp_scope_allows(scope ? (const char *)scope : "", required_scope)) {
            uint64_t user_id = (uint64_t)sqlite3_column_int64(stmt, 0);
            if (out_user) {
                fp_auth_load_user(store, user_id, out_user);
            }
            allowed = 1;
        }
    }
    sqlite3_finalize(stmt);
    return allowed;
}

int fp_auth_record_audit(fp_auth_store *store, uint64_t user_id, const char *event, const char *metadata_json) {
    if (!store || !event) {
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO audit (user_id, event, metadata_json, created_at) VALUES (?, ?, ?, strftime('%s','now'));";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)user_id);
    sqlite3_bind_text(stmt, 2, event, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, metadata_json ? metadata_json : "", -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int fp_status_allows_entitlement(const char *status, time_t period_end) {
    if (!status || !*status) {
        return 0;
    }
    if (strcasecmp(status, "active") != 0 &&
        strcasecmp(status, "trialing") != 0 &&
        strcasecmp(status, "past_due") != 0) {
        return 0;
    }
    if (period_end > 0) {
        time_t now = time(NULL);
        if (period_end < now) {
            return 0;
        }
    }
    return 1;
}

int fp_auth_sync_subscription(fp_auth_store *store, uint64_t user_id, const char *status,
                              const char *customer_id, const char *subscription_id, time_t period_end) {
    if (!store || !store->db) {
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO subscriptions (user_id, status, stripe_customer_id, stripe_subscription_id, current_period_end, updated_at, created_at) "
        "VALUES (?, ?, ?, ?, ?, strftime('%s','now'), strftime('%s','now')) "
        "ON CONFLICT(user_id) DO UPDATE SET "
        "status=excluded.status, "
        "stripe_customer_id=CASE WHEN excluded.stripe_customer_id != '' THEN excluded.stripe_customer_id ELSE subscriptions.stripe_customer_id END, "
        "stripe_subscription_id=CASE WHEN excluded.stripe_subscription_id != '' THEN excluded.stripe_subscription_id ELSE subscriptions.stripe_subscription_id END, "
        "current_period_end=CASE WHEN excluded.current_period_end > 0 THEN excluded.current_period_end ELSE subscriptions.current_period_end END, "
        "updated_at=strftime('%s','now');";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)user_id);
    sqlite3_bind_text(stmt, 2, status && *status ? status : "unknown", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, customer_id ? customer_id : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, subscription_id ? subscription_id : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)(period_end > 0 ? period_end : 0));
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int fp_auth_get_subscription(fp_auth_store *store, uint64_t user_id, fp_auth_subscription *out) {
    if (!store || !store->db || !out) {
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT status, stripe_customer_id, stripe_subscription_id, current_period_end "
                      "FROM subscriptions WHERE user_id = ? LIMIT 1;";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)user_id);
    int rc = sqlite3_step(stmt);
    memset(out, 0, sizeof(*out));
    if (rc == SQLITE_ROW) {
        const unsigned char *status = sqlite3_column_text(stmt, 0);
        const unsigned char *customer = sqlite3_column_text(stmt, 1);
        const unsigned char *sub = sqlite3_column_text(stmt, 2);
        snprintf(out->status, sizeof(out->status), "%s", status ? (const char *)status : "");
        snprintf(out->stripe_customer_id, sizeof(out->stripe_customer_id), "%s", customer ? (const char *)customer : "");
        snprintf(out->stripe_subscription_id, sizeof(out->stripe_subscription_id), "%s", sub ? (const char *)sub : "");
        out->current_period_end = (time_t)sqlite3_column_int64(stmt, 3);
        sqlite3_finalize(stmt);
        return 0;
    }
    sqlite3_finalize(stmt);
    return -1;
}

int fp_auth_has_active_subscription(fp_auth_store *store, uint64_t user_id) {
    fp_auth_subscription sub;
    if (fp_auth_get_subscription(store, user_id, &sub) != 0) {
        return 0;
    }
    return fp_status_allows_entitlement(sub.status, sub.current_period_end);
}

int fp_auth_find_user_by_stripe(fp_auth_store *store, const char *customer_id, const char *subscription_id, uint64_t *out_user_id) {
    if (!store || !store->db || !out_user_id) {
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT user_id FROM subscriptions "
                      "WHERE ((stripe_customer_id = ? AND ? != '') OR (stripe_subscription_id = ? AND ? != '')) "
                      "ORDER BY updated_at DESC LIMIT 1;";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, customer_id ? customer_id : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, customer_id ? customer_id : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, subscription_id ? subscription_id : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, subscription_id ? subscription_id : "", -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out_user_id = (uint64_t)sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return 0;
    }
    sqlite3_finalize(stmt);
    return -1;
}

int fp_auth_revoke_api_keys(fp_auth_store *store, uint64_t user_id, const char *reason) {
    if (!store || !store->db) {
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql = "UPDATE api_keys SET status = 'revoked' WHERE user_id = ? AND status = 'active';";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)user_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    char meta[256];
    if (reason && *reason) {
        snprintf(meta, sizeof(meta), "{\"reason\":\"%.200s\"}", reason);
    } else {
        snprintf(meta, sizeof(meta), "{}");
    }
    fp_auth_record_audit(store, user_id, "api_keys_revoked", meta);
    return rc == SQLITE_DONE ? 0 : -1;
}
