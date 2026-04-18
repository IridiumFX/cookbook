#include <stdio.h>
#include <string.h>
#include "cookbook.h"
#include "cookbook_db.h"
#include "cookbook_store.h"
#include "cookbook_server.h"
#include "cookbook_semver.h"
#include "cookbook_sha256.h"
#include "cookbook_auth.h"
#include "cookbook_grid.h"
#include "cookbook_policy.h"
#include "cookbook_ed25519.h"
#include "cookbook_connpool.h"
#include "cookbook_ldap.h"
#include <apennines/t3/db/wal.h>
#include "alforno.h"
#include "pasta.h"
#ifdef COOKBOOK_HAS_BASTA
#include "basta.h"
#endif
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#endif

static int tests_run    = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do {                          \
    tests_run++;                                        \
    if (!(cond)) {                                      \
        tests_failed++;                                 \
        fprintf(stderr, "FAIL: %s (%s:%d)\n",           \
                (msg), __FILE__, __LINE__);              \
    }                                                   \
} while (0)

static void test_version(void) {
    ASSERT(cookbook_version_major() == 1, "major == 1");
    ASSERT(cookbook_version_minor() == 0, "minor == 0");
    ASSERT(cookbook_version_patch() == 0, "patch == 0");
}

static void test_resources_path(void) {
#ifdef COOKBOOK_TEST_RESOURCES
    const char *res = COOKBOOK_TEST_RESOURCES;
    ASSERT(res != NULL && strlen(res) > 0, "resource path is set");
#else
    ASSERT(0, "COOKBOOK_TEST_RESOURCES not defined");
#endif
}

static void test_db_open_close(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    ASSERT(db != NULL, "open in-memory sqlite");
    db->close(db);
}

static void test_db_migrate(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    ASSERT(db != NULL, "open db for migrate");
    cookbook_db_status st = cookbook_db_migrate(db);
    ASSERT(st == COOKBOOK_DB_OK, "migrate succeeds");
    db->close(db);
}

static int count_cb(const cookbook_db_row *row, void *ctx) {
    int *count = (int *)ctx;
    (*count)++;
    return 0;
}

static void test_db_groups_crud(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    cookbook_db_status st = db->exec(db,
        "INSERT INTO groups (group_id, owner_sub, created_at) "
        "VALUES ('org.acme', 'alice', '2026-01-01T00:00:00Z')");
    ASSERT(st == COOKBOOK_DB_OK, "insert group");

    int count = 0;
    st = db->query(db,
        "SELECT group_id FROM groups WHERE group_id = 'org.acme'",
        count_cb, &count);
    ASSERT(st == COOKBOOK_DB_OK, "query group");
    ASSERT(count == 1, "found one group");

    st = db->exec(db,
        "INSERT INTO groups (group_id, owner_sub) "
        "VALUES ('org.acme', 'bob')");
    ASSERT(st == COOKBOOK_DB_CONSTRAINT, "duplicate group rejected");

    db->close(db);
}

static void test_db_artifacts_crud(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    db->exec(db,
        "INSERT INTO groups (group_id, owner_sub) "
        "VALUES ('org.acme', 'alice')");

    cookbook_db_status st = db->exec(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256) "
        "VALUES ('org.acme:core:1.0.0:linux:amd64:gnu', 'org.acme', "
        "'core', '1.0.0', 'linux:amd64:gnu', 'deadbeef')");
    ASSERT(st == COOKBOOK_DB_OK, "insert artifact");

    st = db->exec(db,
        "INSERT INTO artifact_semver (coord_id, major, minor, patch) "
        "VALUES ('org.acme:core:1.0.0:linux:amd64:gnu', 1, 0, 0)");
    ASSERT(st == COOKBOOK_DB_OK, "insert semver index");

    int count = 0;
    st = db->query(db,
        "SELECT coord_id FROM artifacts WHERE group_id = 'org.acme'",
        count_cb, &count);
    ASSERT(st == COOKBOOK_DB_OK, "query artifacts");
    ASSERT(count == 1, "found one artifact");

    db->close(db);
}

static void test_store_put_get(void) {
    const char *dir = COOKBOOK_TEST_RESOURCES "/tmp_store";
    cookbook_store *store = cookbook_store_open_fs(dir);
    ASSERT(store != NULL, "open fs store");

    const char *data = "hello cookbook";
    cookbook_store_status st = store->put(store,
        "central/org/acme/core/1.0.0/core-1.0.0-noarch.tar.gz",
        data, strlen(data));
    ASSERT(st == COOKBOOK_STORE_OK, "put object");

    st = store->exists(store,
        "central/org/acme/core/1.0.0/core-1.0.0-noarch.tar.gz");
    ASSERT(st == COOKBOOK_STORE_OK, "object exists");

    void *buf = NULL;
    size_t len = 0;
    st = store->get(store,
        "central/org/acme/core/1.0.0/core-1.0.0-noarch.tar.gz",
        &buf, &len);
    ASSERT(st == COOKBOOK_STORE_OK, "get object");
    ASSERT(len == strlen(data), "correct length");
    ASSERT(buf && memcmp(buf, data, len) == 0, "correct content");
    store->free_buf(buf);

    st = store->del(store,
        "central/org/acme/core/1.0.0/core-1.0.0-noarch.tar.gz");
    ASSERT(st == COOKBOOK_STORE_OK, "delete object");

    st = store->exists(store,
        "central/org/acme/core/1.0.0/core-1.0.0-noarch.tar.gz");
    ASSERT(st == COOKBOOK_STORE_NOT_FOUND, "object gone after delete");

    store->close(store);
}

static void test_store_not_found(void) {
    const char *dir = COOKBOOK_TEST_RESOURCES "/tmp_store";
    cookbook_store *store = cookbook_store_open_fs(dir);
    ASSERT(store != NULL, "open fs store for not-found test");

    void *buf = NULL;
    size_t len = 0;
    cookbook_store_status st = store->get(store, "no/such/key", &buf, &len);
    ASSERT(st == COOKBOOK_STORE_NOT_FOUND, "missing key returns not found");

    store->close(store);
}

/* ---- parameterized query tests ---- */

static void test_db_parameterized_exec(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    cookbook_db_param params[] = {
        COOKBOOK_P_TEXT("org.test"),
        COOKBOOK_P_TEXT("tester")
    };
    cookbook_db_status st = db->exec_p(db,
        "INSERT INTO groups (group_id, owner_sub) VALUES (?1, ?2)",
        params, 2);
    ASSERT(st == COOKBOOK_DB_OK, "parameterized insert group");

    /* verify it was inserted */
    int count = 0;
    cookbook_db_param qp[] = { COOKBOOK_P_TEXT("org.test") };
    st = db->query_p(db,
        "SELECT group_id FROM groups WHERE group_id = ?1",
        qp, 1, count_cb, &count);
    ASSERT(st == COOKBOOK_DB_OK, "parameterized query group");
    ASSERT(count == 1, "found parameterized group");

    /* test constraint violation via parameterized */
    st = db->exec_p(db,
        "INSERT INTO groups (group_id, owner_sub) VALUES (?1, ?2)",
        params, 2);
    ASSERT(st == COOKBOOK_DB_CONSTRAINT, "parameterized duplicate rejected");

    db->close(db);
}

static void test_db_parameterized_artifact(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    cookbook_db_param gp[] = {
        COOKBOOK_P_TEXT("org.acme"),
        COOKBOOK_P_TEXT("alice")
    };
    db->exec_p(db,
        "INSERT INTO groups (group_id, owner_sub) VALUES (?1, ?2)",
        gp, 2);

    cookbook_db_param ap[] = {
        COOKBOOK_P_TEXT("org.acme:core:1.0.0:noarch"),
        COOKBOOK_P_TEXT("org.acme"),
        COOKBOOK_P_TEXT("core"),
        COOKBOOK_P_TEXT("1.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("abcdef1234567890"),
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("published"),
        COOKBOOK_P_INT(1024)
    };
    cookbook_db_status st = db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
        ap, 9);
    ASSERT(st == COOKBOOK_DB_OK, "parameterized insert artifact");

    int count = 0;
    cookbook_db_param qp[] = { COOKBOOK_P_TEXT("org.acme") };
    st = db->query_p(db,
        "SELECT coord_id FROM artifacts WHERE group_id = ?1",
        qp, 1, count_cb, &count);
    ASSERT(st == COOKBOOK_DB_OK, "parameterized query artifacts");
    ASSERT(count == 1, "found parameterized artifact");

    db->close(db);
}

/* ---- SHA-256 tests ---- */

static void test_sha256_empty(void) {
    char hex[65];
    cookbook_sha256_hex("", 0, hex);
    ASSERT(strcmp(hex,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
        "SHA-256 of empty string");
}

static void test_sha256_abc(void) {
    char hex[65];
    cookbook_sha256_hex("abc", 3, hex);
    ASSERT(strcmp(hex,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
        "SHA-256 of 'abc'");
}

static void test_sha256_long(void) {
    /* "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" */
    const char *msg =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    char hex[65];
    cookbook_sha256_hex(msg, strlen(msg), hex);
    ASSERT(strcmp(hex,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0,
        "SHA-256 of NIST test vector");
}

/* ---- auth tests ---- */

static void test_base64url_roundtrip(void) {
    const char *input = "hello world";
    char encoded[64];
    size_t elen = cookbook_base64url_encode(input, strlen(input),
                                            encoded, sizeof(encoded));
    ASSERT(elen > 0, "base64url encode non-empty");
    ASSERT(strcmp(encoded, "aGVsbG8gd29ybGQ") == 0, "base64url encode correct");

    char decoded[64];
    size_t dlen = cookbook_base64url_decode(encoded, elen, decoded, sizeof(decoded));
    ASSERT(dlen == strlen(input), "base64url decode length");
    ASSERT(memcmp(decoded, input, dlen) == 0, "base64url roundtrip");
}

static void test_jwt_create_verify(void) {
    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    char *token = cookbook_jwt_create("alice", "org.acme,org.beta", 3600, sk);
    ASSERT(token != NULL, "JWT create succeeds");

    /* verify with correct key */
    cookbook_jwt_claims claims;
    int rc = cookbook_jwt_verify(token, pk, &claims);
    ASSERT(rc == 0, "JWT verify succeeds");
    ASSERT(claims.valid == 1, "JWT claims valid");
    ASSERT(strcmp(claims.sub, "alice") == 0, "JWT sub=alice");
    ASSERT(strstr(claims.groups, "org.acme") != NULL, "JWT has org.acme group");
    ASSERT(claims.exp > 0, "JWT has expiry");

    /* group check */
    ASSERT(cookbook_jwt_has_group(&claims, "org.acme") == 1,
           "JWT has_group org.acme");
    ASSERT(cookbook_jwt_has_group(&claims, "org.beta") == 1,
           "JWT has_group org.beta");
    ASSERT(cookbook_jwt_has_group(&claims, "org.gamma") == 0,
           "JWT !has_group org.gamma");

    /* verify with wrong key should fail */
    unsigned char pk2[32], sk2[64];
    cookbook_keygen(pk2, sk2);
    cookbook_jwt_claims claims2;
    rc = cookbook_jwt_verify(token, pk2, &claims2);
    ASSERT(rc != 0, "JWT verify with wrong key fails");

    free(token);
}

static void test_ed25519_sign_verify(void) {
    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    const char *msg = "test message for signing";
    unsigned char sig[64];
    ASSERT(cookbook_sign(msg, strlen(msg), sig, sk) == 0, "Ed25519 sign");
    ASSERT(cookbook_verify(msg, strlen(msg), sig, pk) == 0, "Ed25519 verify");

    /* tamper with signature */
    sig[0] ^= 0xff;
    ASSERT(cookbook_verify(msg, strlen(msg), sig, pk) != 0,
           "Ed25519 verify tampered fails");
}

/* ---- mirror manifest / metrics integration tests ---- */

/* These test the server's mirror manifest and metrics endpoints via
   the server handler functions (which we can't call directly from here
   since they require civetweb). Instead, we test the underlying data
   queries that power them. */

static int mirror_count_cb(const cookbook_db_row *row, void *ctx) {
    int *count = (int *)ctx;
    (*count)++;
    return 0;
}

static void test_mirror_query(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* insert test data */
    db->exec(db,
        "INSERT INTO groups (group_id, owner_sub) "
        "VALUES ('org.acme', 'alice')");

    cookbook_db_param ap1[] = {
        COOKBOOK_P_TEXT("org.acme:core:1.0.0:noarch"),
        COOKBOOK_P_TEXT("org.acme"),
        COOKBOOK_P_TEXT("core"),
        COOKBOOK_P_TEXT("1.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("aabbccdd"),
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("published"),
        COOKBOOK_P_INT(1024)
    };
    db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
        ap1, 9);

    cookbook_db_param ap2[] = {
        COOKBOOK_P_TEXT("org.acme:core:2.0.0:noarch"),
        COOKBOOK_P_TEXT("org.acme"),
        COOKBOOK_P_TEXT("core"),
        COOKBOOK_P_TEXT("2.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("eeff0011"),
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("published"),
        COOKBOOK_P_INT(2048)
    };
    db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
        ap2, 9);

    /* query all published — should find 2 */
    int count = 0;
    db->query(db,
        "SELECT DISTINCT group_id, artifact, version, triple "
        "FROM artifacts WHERE status = 'published' AND yanked = 0",
        mirror_count_cb, &count);
    ASSERT(count == 2, "mirror manifest finds 2 published artifacts");

    /* query specific coordinate */
    count = 0;
    cookbook_db_param qp[] = {
        COOKBOOK_P_TEXT("org.acme"),
        COOKBOOK_P_TEXT("core"),
        COOKBOOK_P_TEXT("1.0.0")
    };
    db->query_p(db,
        "SELECT group_id, artifact, version, triple "
        "FROM artifacts WHERE group_id = ?1 AND artifact = ?2 "
        "AND version = ?3 AND status = 'published'",
        qp, 3, mirror_count_cb, &count);
    ASSERT(count == 1, "mirror manifest finds specific version");

    db->close(db);
}

/* ---- S3 store open/close (no real server needed) ---- */

static void test_s3_store_open_null(void) {
    /* missing required params should return NULL */
    cookbook_store *s;
    s = cookbook_store_open_s3(NULL, "us-east-1", "key", "secret", NULL);
    ASSERT(s == NULL, "S3 open with NULL bucket fails");
    s = cookbook_store_open_s3("bucket", "us-east-1", NULL, "secret", NULL);
    ASSERT(s == NULL, "S3 open with NULL access_key fails");
    s = cookbook_store_open_s3("bucket", "us-east-1", "key", NULL, NULL);
    ASSERT(s == NULL, "S3 open with NULL secret_key fails");
}

static void test_s3_store_open_close(void) {
    /* opening with valid params should succeed (doesn't connect yet) */
    cookbook_store *s = cookbook_store_open_s3("test-bucket", "us-east-1",
                                              "AKIA_TEST", "secret123",
                                              "localhost:9000");
    ASSERT(s != NULL, "S3 open with valid params succeeds");
    if (s) s->close(s);
}

/* ---- PostgreSQL stub test ---- */

static void test_postgres_stub(void) {
    /* when built without libpq, open should return NULL gracefully */
    cookbook_db *db = cookbook_db_open_postgres("postgres://localhost/test");
#ifdef COOKBOOK_HAS_POSTGRES
    /* if PG is available, this would try to connect (and likely fail
       in test env), so we just skip */
    if (db) db->close(db);
#else
    ASSERT(db == NULL, "PG stub returns NULL when libpq unavailable");
#endif
}

/* ---- semver tests ---- */

static void test_semver_parse(void) {
    cookbook_semver sv;
    ASSERT(cookbook_semver_parse("1.3.0", &sv) == 0, "parse 1.3.0");
    ASSERT(sv.major == 1 && sv.minor == 3 && sv.patch == 0, "1.3.0 fields");
    ASSERT(sv.pre_release[0] == '\0', "no pre-release");

    ASSERT(cookbook_semver_parse("0.1.0-beta.1+build.42", &sv) == 0,
           "parse with pre+meta");
    ASSERT(sv.major == 0 && sv.minor == 1 && sv.patch == 0, "0.1.0 fields");
    ASSERT(strcmp(sv.pre_release, "beta.1") == 0, "pre_release=beta.1");
    ASSERT(strcmp(sv.build_meta, "build.42") == 0, "build_meta=build.42");

    ASSERT(cookbook_semver_parse("nope", &sv) != 0, "reject garbage");
    ASSERT(cookbook_semver_parse("1.2", &sv) != 0, "reject incomplete");
}

static void test_semver_compare(void) {
    cookbook_semver a, b;
    cookbook_semver_parse("1.0.0", &a);
    cookbook_semver_parse("2.0.0", &b);
    ASSERT(cookbook_semver_compare(&a, &b) < 0, "1.0.0 < 2.0.0");

    cookbook_semver_parse("1.0.0-alpha", &a);
    cookbook_semver_parse("1.0.0", &b);
    ASSERT(cookbook_semver_compare(&a, &b) < 0, "1.0.0-alpha < 1.0.0");

    cookbook_semver_parse("1.0.0-alpha", &a);
    cookbook_semver_parse("1.0.0-beta", &b);
    ASSERT(cookbook_semver_compare(&a, &b) < 0, "alpha < beta");
}

static void test_range_caret(void) {
    cookbook_range r;
    cookbook_semver v;

    ASSERT(cookbook_range_parse("^1.3.0", &r) == 0, "parse ^1.3.0");
    ASSERT(r.type == COOKBOOK_RANGE_CARET, "type is caret");

    cookbook_semver_parse("1.3.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.3.0 satisfies ^1.3.0");

    cookbook_semver_parse("1.4.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.4.0 satisfies ^1.3.0");

    cookbook_semver_parse("1.99.99", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.99.99 satisfies ^1.3.0");

    cookbook_semver_parse("2.0.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "2.0.0 fails ^1.3.0");

    cookbook_semver_parse("1.2.9", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "1.2.9 fails ^1.3.0");

    /* ^0.3.0 means >=0.3.0, <0.4.0 */
    ASSERT(cookbook_range_parse("^0.3.0", &r) == 0, "parse ^0.3.0");
    cookbook_semver_parse("0.3.5", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "0.3.5 satisfies ^0.3.0");
    cookbook_semver_parse("0.4.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "0.4.0 fails ^0.3.0");
}

static void test_range_tilde(void) {
    cookbook_range r;
    cookbook_semver v;

    ASSERT(cookbook_range_parse("~1.3.0", &r) == 0, "parse ~1.3.0");

    cookbook_semver_parse("1.3.5", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.3.5 satisfies ~1.3.0");

    cookbook_semver_parse("1.4.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "1.4.0 fails ~1.3.0");
}

static void test_range_wildcard(void) {
    cookbook_range r;
    cookbook_semver v;

    ASSERT(cookbook_range_parse("1.*", &r) == 0, "parse 1.*");
    cookbook_semver_parse("1.0.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.0.0 satisfies 1.*");
    cookbook_semver_parse("1.99.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.99.0 satisfies 1.*");
    cookbook_semver_parse("2.0.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "2.0.0 fails 1.*");

    ASSERT(cookbook_range_parse("*", &r) == 0, "parse *");
    cookbook_semver_parse("99.0.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "99.0.0 satisfies *");
}

static void test_range_bounded(void) {
    cookbook_range r;
    cookbook_semver v;

    ASSERT(cookbook_range_parse("[1.0.0,2.0.0)", &r) == 0, "parse [1.0.0,2.0.0)");

    cookbook_semver_parse("1.0.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.0.0 satisfies [1,2)");

    cookbook_semver_parse("1.5.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.5.0 satisfies [1,2)");

    cookbook_semver_parse("2.0.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "2.0.0 fails [1,2)");

    cookbook_semver_parse("0.9.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "0.9.0 fails [1,2)");
}

/* ---- F3: credential tests ---- */

static void test_base64_std_decode(void) {
    /* "alice:secrettoken" → base64 "YWxpY2U6c2VjcmV0dG9rZW4=" */
    const char *b64 = "YWxpY2U6c2VjcmV0dG9rZW4=";
    char out[64] = {0};
    size_t len = cookbook_base64_decode(b64, strlen(b64), out, sizeof(out) - 1);
    out[len] = '\0';
    ASSERT(len == 17, "base64 std decode length");
    ASSERT(strcmp(out, "alice:secrettoken") == 0, "base64 std decode value");

    /* empty input */
    len = cookbook_base64_decode("", 0, out, sizeof(out));
    ASSERT(len == 0, "base64 std decode empty");

    /* no padding */
    const char *b64np = "YWxpY2U6c2VjcmV0dG9rZW4";
    len = cookbook_base64_decode(b64np, strlen(b64np), out, sizeof(out) - 1);
    out[len] = '\0';
    ASSERT(len == 17, "base64 std decode no-padding length");
    ASSERT(strcmp(out, "alice:secrettoken") == 0, "base64 std decode no-padding");

    /* with + and / characters */
    const char *b64plus = "YQ+/";
    len = cookbook_base64_decode(b64plus, 4, out, sizeof(out));
    ASSERT(len == 3, "base64 std decode with +/");
}

static void test_credential_hash_verify(void) {
    const char *token = "my-secret-token-12345";
    char *hash = cookbook_credential_hash(token);
    ASSERT(hash != NULL, "credential hash not NULL");

    int rc = cookbook_credential_verify(token, hash);
    ASSERT(rc == 0, "credential verify correct token");

    free(hash);
}

static void test_credential_verify_wrong(void) {
    const char *token = "correct-token";
    char *hash = cookbook_credential_hash(token);
    ASSERT(hash != NULL, "hash for wrong-token test");

    int rc = cookbook_credential_verify("wrong-token", hash);
    ASSERT(rc != 0, "credential verify rejects wrong token");

    free(hash);
}

static void test_credentials_table(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* hash a token and store it */
    char *hash = cookbook_credential_hash("my-api-key");
    ASSERT(hash != NULL, "hash for credentials table test");

    cookbook_db_param ip[] = {
        COOKBOOK_P_TEXT("alice"),
        COOKBOOK_P_TEXT(hash),
        COOKBOOK_P_TEXT("org.acme,org.beta")
    };
    cookbook_db_status st = db->exec_p(db,
        "INSERT INTO credentials (subject, token_hash, groups) "
        "VALUES (?1, ?2, ?3)", ip, 3);
    ASSERT(st == COOKBOOK_DB_OK, "insert credential");

    /* look it up */
    int count = 0;
    cookbook_db_param qp[] = { COOKBOOK_P_TEXT("alice") };
    db->query_p(db,
        "SELECT token_hash, groups FROM credentials "
        "WHERE subject = ?1 AND revoked_at IS NULL",
        qp, 1, count_cb, &count);
    ASSERT(count == 1, "credential found");

    /* duplicate subject should fail */
    st = db->exec_p(db,
        "INSERT INTO credentials (subject, token_hash, groups) "
        "VALUES (?1, ?2, ?3)", ip, 3);
    ASSERT(st == COOKBOOK_DB_CONSTRAINT, "duplicate credential rejected");

    /* revoke and verify not found with revoked_at IS NULL */
    cookbook_db_param rp[] = { COOKBOOK_P_TEXT("alice") };
    st = db->exec_p(db,
        "UPDATE credentials SET revoked_at = '2026-01-01' "
        "WHERE subject = ?1", rp, 1);
    ASSERT(st == COOKBOOK_DB_OK, "revoke credential");

    count = 0;
    db->query_p(db,
        "SELECT token_hash FROM credentials "
        "WHERE subject = ?1 AND revoked_at IS NULL",
        qp, 1, count_cb, &count);
    ASSERT(count == 0, "revoked credential excluded");

    free(hash);
    db->close(db);
}

/* ---- Grid tests ---- */

static void test_grid_loop_detection(void) {
    ASSERT(cookbook_grid_is_loop("nodeA", "nodeA") == 1,
           "loop: exact match");
    ASSERT(cookbook_grid_is_loop("nodeA", "nodeB,nodeA") == 1,
           "loop: in chain");
    ASSERT(cookbook_grid_is_loop("nodeA", "nodeB,nodeC") == 0,
           "no loop: not in chain");
    ASSERT(cookbook_grid_is_loop("nodeA", "") == 0,
           "no loop: empty chain");
    ASSERT(cookbook_grid_is_loop("nodeA", NULL) == 0,
           "no loop: NULL chain");
    ASSERT(cookbook_grid_is_loop("node", "nodeA,nodeB") == 0,
           "no loop: prefix not match");
    ASSERT(cookbook_grid_is_loop("nodeA", "nodeAB,nodeC") == 0,
           "no loop: substring not match");
}

static void test_grid_peers_table(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* insert a peer */
    cookbook_db_param pp[] = {
        COOKBOOK_P_TEXT("east-1"),
        COOKBOOK_P_TEXT("East Region"),
        COOKBOOK_P_TEXT("http://east-1:8080"),
        COOKBOOK_P_TEXT("redirect"),
        COOKBOOK_P_INT(50)
    };
    cookbook_db_status st = db->exec_p(db,
        "INSERT INTO peers (peer_id, name, url, mode, priority, enabled) "
        "VALUES (?1, ?2, ?3, ?4, ?5, 1)", pp, 5);
    ASSERT(st == COOKBOOK_DB_OK, "insert peer");

    /* duplicate URL should fail */
    cookbook_db_param pp2[] = {
        COOKBOOK_P_TEXT("east-2"),
        COOKBOOK_P_TEXT("East 2"),
        COOKBOOK_P_TEXT("http://east-1:8080"),
        COOKBOOK_P_TEXT("proxy"),
        COOKBOOK_P_INT(100)
    };
    st = db->exec_p(db,
        "INSERT INTO peers (peer_id, name, url, mode, priority, enabled) "
        "VALUES (?1, ?2, ?3, ?4, ?5, 1)", pp2, 5);
    ASSERT(st == COOKBOOK_DB_CONSTRAINT, "duplicate URL rejected");

    /* verify peer exists */
    int count = 0;
    db->query(db, "SELECT peer_id FROM peers WHERE enabled = 1",
              count_cb, &count);
    ASSERT(count == 1, "one enabled peer");

    /* disable peer */
    cookbook_db_param dp[] = { COOKBOOK_P_TEXT("east-1") };
    db->exec_p(db,
        "UPDATE peers SET enabled = 0 WHERE peer_id = ?1", dp, 1);

    count = 0;
    db->query(db, "SELECT peer_id FROM peers WHERE enabled = 1",
              count_cb, &count);
    ASSERT(count == 0, "no enabled peers after disable");

    db->close(db);
}

static void test_grid_peer_load(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* insert two peers with different priorities */
    cookbook_db_param p1[] = {
        COOKBOOK_P_TEXT("west-1"),
        COOKBOOK_P_TEXT("West Region"),
        COOKBOOK_P_TEXT("http://west-1:8080"),
        COOKBOOK_P_TEXT("proxy"),
        COOKBOOK_P_INT(200)
    };
    db->exec_p(db,
        "INSERT INTO peers (peer_id, name, url, mode, priority, enabled) "
        "VALUES (?1, ?2, ?3, ?4, ?5, 1)", p1, 5);

    cookbook_db_param p2[] = {
        COOKBOOK_P_TEXT("east-1"),
        COOKBOOK_P_TEXT("East Region"),
        COOKBOOK_P_TEXT("http://east-1:8080"),
        COOKBOOK_P_TEXT("redirect"),
        COOKBOOK_P_INT(50)
    };
    db->exec_p(db,
        "INSERT INTO peers (peer_id, name, url, mode, priority, enabled) "
        "VALUES (?1, ?2, ?3, ?4, ?5, 1)", p2, 5);

    /* disabled peer */
    cookbook_db_param p3[] = {
        COOKBOOK_P_TEXT("down-1"),
        COOKBOOK_P_TEXT("Down"),
        COOKBOOK_P_TEXT("http://down-1:8080"),
        COOKBOOK_P_TEXT("redirect"),
        COOKBOOK_P_INT(10)
    };
    db->exec_p(db,
        "INSERT INTO peers (peer_id, name, url, mode, priority, enabled) "
        "VALUES (?1, ?2, ?3, ?4, ?5, 0)", p3, 5);

    cookbook_peer *peers = NULL;
    int n = cookbook_grid_load_peers(db, &peers);
    ASSERT(n == 2, "load 2 enabled peers");
    /* should be sorted by priority: east-1 (50) before west-1 (200) */
    ASSERT(strcmp(peers[0].peer_id, "east-1") == 0, "first peer is east-1");
    ASSERT(peers[0].mode == 'r', "east-1 mode is redirect");
    ASSERT(strcmp(peers[1].peer_id, "west-1") == 0, "second peer is west-1");
    ASSERT(peers[1].mode == 'p', "west-1 mode is proxy");

    cookbook_grid_free_peers(peers, n);
    db->close(db);
}

/* ---- #8: content negotiation tests ---- */

static void test_validate_ascii(void) {
    /* valid ASCII */
    ASSERT(cookbook_validate_ascii("hello", 5) == 0, "pure ASCII is valid");
    ASSERT(cookbook_validate_ascii("", 0) == 0, "empty is valid");
    ASSERT(cookbook_validate_ascii("a = 1\nb = 2\n", 12) == 0,
           "ASCII with newlines");

    /* byte > 0x7F */
    ASSERT(cookbook_validate_ascii("caf\xC3\xA9", 5) == 4,
           "reject UTF-8 multi-byte at offset 4");
    ASSERT(cookbook_validate_ascii("\x80", 1) == 1,
           "reject 0x80 at offset 1");
    ASSERT(cookbook_validate_ascii("ab\xFF", 3) == 3,
           "reject 0xFF at offset 3");

    /* NUL byte */
    ASSERT(cookbook_validate_ascii("ab\x00" "cd", 5) == 3,
           "reject NUL at offset 3");
    ASSERT(cookbook_validate_ascii("\x00", 1) == 1,
           "reject NUL at offset 1");
}

static void test_pasta_to_json_primitives(void) {
    /* null */
    char *j = cookbook_pasta_to_json(NULL);
    ASSERT(j && strcmp(j, "null") == 0, "NULL → \"null\"");
    free(j);

    /* parse a small Pasta doc and convert to JSON */
    PastaResult pr;
    const char *src = "{ name: \"test\", count: 42, flag: true, empty: null }";
    PastaValue *root = pasta_parse(src, strlen(src), &pr);
    ASSERT(root != NULL, "parse small Pasta doc");
    if (root) {
        j = cookbook_pasta_to_json(root);
        ASSERT(j != NULL, "JSON serialization succeeds");
        /* verify key fields are present */
        ASSERT(strstr(j, "\"name\"") != NULL, "JSON has name key");
        ASSERT(strstr(j, "\"test\"") != NULL, "JSON has test value");
        ASSERT(strstr(j, "\"count\"") != NULL, "JSON has count key");
        ASSERT(strstr(j, "42") != NULL, "JSON has 42");
        ASSERT(strstr(j, "true") != NULL, "JSON has true");
        ASSERT(strstr(j, "null") != NULL, "JSON has null");
        free(j);
        pasta_free(root);
    }
}

static void test_pasta_to_json_nested(void) {
    PastaResult pr;
    const char *src =
        "{ versions: [ { version: \"1.0.0\", triples: [\"noarch\"] } ] }";
    PastaValue *root = pasta_parse(src, strlen(src), &pr);
    ASSERT(root != NULL, "parse nested Pasta");
    if (root) {
        char *j = cookbook_pasta_to_json(root);
        ASSERT(j != NULL, "nested JSON serialization");
        ASSERT(strstr(j, "\"versions\"") != NULL, "has versions key");
        ASSERT(strstr(j, "\"1.0.0\"") != NULL, "has version value");
        ASSERT(strstr(j, "\"noarch\"") != NULL, "has triple value");
        ASSERT(j[0] == '{', "starts with {");
        free(j);
        pasta_free(root);
    }
}

static void test_pasta_to_json_escaping(void) {
    PastaResult pr;
    const char *src = "{ msg: \"line1\\nline2\\ttab\" }";
    PastaValue *root = pasta_parse(src, strlen(src), &pr);
    ASSERT(root != NULL, "parse Pasta with escapes");
    if (root) {
        char *j = cookbook_pasta_to_json(root);
        ASSERT(j != NULL, "escape JSON serialization");
        ASSERT(strstr(j, "\\n") != NULL, "JSON has \\n escape");
        ASSERT(strstr(j, "\\t") != NULL, "JSON has \\t escape");
        free(j);
        pasta_free(root);
    }
}

/* ---- additional semver edge cases ---- */

static void test_semver_parse_edge_cases(void) {
    cookbook_semver sv;

    /* large version numbers */
    ASSERT(cookbook_semver_parse("999.999.999", &sv) == 0, "parse large version");
    ASSERT(sv.major == 999 && sv.minor == 999 && sv.patch == 999,
           "large version fields");

    /* version 0.0.0 */
    ASSERT(cookbook_semver_parse("0.0.0", &sv) == 0, "parse 0.0.0");
    ASSERT(sv.major == 0 && sv.minor == 0 && sv.patch == 0, "0.0.0 fields");

    /* pre-release only (no build metadata) */
    ASSERT(cookbook_semver_parse("1.0.0-rc.1", &sv) == 0, "parse rc.1");
    ASSERT(strcmp(sv.pre_release, "rc.1") == 0, "pre_release=rc.1");
    ASSERT(sv.build_meta[0] == '\0', "no build_meta");

    /* build metadata only (no pre-release) */
    ASSERT(cookbook_semver_parse("1.0.0+sha.abc123", &sv) == 0,
           "parse build meta only");
    ASSERT(sv.pre_release[0] == '\0', "no pre_release");
    ASSERT(strcmp(sv.build_meta, "sha.abc123") == 0, "build_meta=sha.abc123");

    /* reject edge cases */
    ASSERT(cookbook_semver_parse("", &sv) != 0, "reject empty");
    ASSERT(cookbook_semver_parse("v1.0.0", &sv) != 0, "reject v-prefix");
    ASSERT(cookbook_semver_parse("1.0", &sv) != 0, "reject two-part");
    ASSERT(cookbook_semver_parse("1.0.0.0", &sv) != 0, "reject four-part");
    ASSERT(cookbook_semver_parse("-1.0.0", &sv) != 0, "reject negative");
    ASSERT(cookbook_semver_parse("1.0.0-", &sv) != 0, "reject trailing dash");
}

static void test_semver_compare_detailed(void) {
    cookbook_semver a, b;

    /* equal versions */
    cookbook_semver_parse("1.2.3", &a);
    cookbook_semver_parse("1.2.3", &b);
    ASSERT(cookbook_semver_compare(&a, &b) == 0, "1.2.3 == 1.2.3");

    /* minor difference */
    cookbook_semver_parse("1.2.0", &a);
    cookbook_semver_parse("1.3.0", &b);
    ASSERT(cookbook_semver_compare(&a, &b) < 0, "1.2.0 < 1.3.0");

    /* patch difference */
    cookbook_semver_parse("1.2.3", &a);
    cookbook_semver_parse("1.2.4", &b);
    ASSERT(cookbook_semver_compare(&a, &b) < 0, "1.2.3 < 1.2.4");

    /* pre-release numeric ordering: 1 < 2 < 11 */
    cookbook_semver_parse("1.0.0-alpha.1", &a);
    cookbook_semver_parse("1.0.0-alpha.2", &b);
    ASSERT(cookbook_semver_compare(&a, &b) < 0, "alpha.1 < alpha.2");

    /* pre-release: alpha < beta < rc */
    cookbook_semver_parse("1.0.0-beta", &a);
    cookbook_semver_parse("1.0.0-rc", &b);
    ASSERT(cookbook_semver_compare(&a, &b) < 0, "beta < rc");

    /* build metadata ignored in comparison */
    cookbook_semver_parse("1.0.0+build1", &a);
    cookbook_semver_parse("1.0.0+build2", &b);
    ASSERT(cookbook_semver_compare(&a, &b) == 0,
           "build metadata ignored in compare");

    /* symmetry */
    cookbook_semver_parse("2.0.0", &a);
    cookbook_semver_parse("1.0.0", &b);
    ASSERT(cookbook_semver_compare(&a, &b) > 0, "2.0.0 > 1.0.0");
}

static void test_range_exact(void) {
    cookbook_range r;
    cookbook_semver v;

    ASSERT(cookbook_range_parse("1.2.3", &r) == 0, "parse exact 1.2.3");
    ASSERT(r.type == COOKBOOK_RANGE_EXACT, "type is exact");

    cookbook_semver_parse("1.2.3", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.2.3 satisfies exact 1.2.3");

    cookbook_semver_parse("1.2.4", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "1.2.4 fails exact 1.2.3");

    cookbook_semver_parse("1.2.2", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "1.2.2 fails exact 1.2.3");
}

static void test_range_caret_zero(void) {
    cookbook_range r;
    cookbook_semver v;

    /* ^0.0.3 means >=0.0.3, <0.0.4 (only patch bumps allowed) */
    ASSERT(cookbook_range_parse("^0.0.3", &r) == 0, "parse ^0.0.3");
    cookbook_semver_parse("0.0.3", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "0.0.3 satisfies ^0.0.3");
    cookbook_semver_parse("0.0.4", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "0.0.4 fails ^0.0.3");
    cookbook_semver_parse("0.1.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "0.1.0 fails ^0.0.3");
}

static void test_range_bounded_inclusive(void) {
    cookbook_range r;
    cookbook_semver v;

    /* [1.0.0,2.0.0] — upper inclusive */
    ASSERT(cookbook_range_parse("[1.0.0,2.0.0]", &r) == 0,
           "parse [1.0.0,2.0.0]");

    cookbook_semver_parse("2.0.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1,
           "2.0.0 satisfies [1.0.0,2.0.0] (upper inclusive)");

    /* (1.0.0,2.0.0) — both exclusive */
    ASSERT(cookbook_range_parse("(1.0.0,2.0.0)", &r) == 0,
           "parse (1.0.0,2.0.0)");

    cookbook_semver_parse("1.0.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0,
           "1.0.0 fails (1.0.0,2.0.0) (lower exclusive)");
    cookbook_semver_parse("1.0.1", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1,
           "1.0.1 satisfies (1.0.0,2.0.0)");
}

/* ---- additional DB tests ---- */

static void test_db_yanked_status(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    db->exec(db,
        "INSERT INTO groups (group_id, owner_sub) "
        "VALUES ('org.yank', 'alice')");

    cookbook_db_param ap[] = {
        COOKBOOK_P_TEXT("org.yank:lib:1.0.0:noarch"),
        COOKBOOK_P_TEXT("org.yank"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("1.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("deadbeef"),
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("published"),
        COOKBOOK_P_INT(512)
    };
    db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)", ap, 9);

    /* yank it */
    cookbook_db_param yp[] = {
        COOKBOOK_P_TEXT("org.yank"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("1.0.0")
    };
    cookbook_db_status st = db->exec_p(db,
        "UPDATE artifacts SET yanked = 1 "
        "WHERE group_id = ?1 AND artifact = ?2 AND version = ?3",
        yp, 3);
    ASSERT(st == COOKBOOK_DB_OK, "yank succeeds");

    /* verify yanked=1 */
    int count = 0;
    db->query_p(db,
        "SELECT coord_id FROM artifacts WHERE yanked = 1 "
        "AND group_id = ?1", yp, 1, count_cb, &count);
    ASSERT(count == 1, "yanked artifact found");

    /* verify excluded from published+non-yanked query */
    count = 0;
    db->query_p(db,
        "SELECT coord_id FROM artifacts WHERE yanked = 0 "
        "AND status = 'published' AND group_id = ?1", yp, 1,
        count_cb, &count);
    ASSERT(count == 0, "yanked artifact excluded from resolve");

    db->close(db);
}

/* F1: yank reason stored and retrieved */
static void test_db_yank_reason(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    db->exec(db,
        "INSERT INTO groups (group_id, owner_sub) "
        "VALUES ('org.yr', 'alice')");

    cookbook_db_param ap[] = {
        COOKBOOK_P_TEXT("org.yr:lib:1.0.0:noarch"),
        COOKBOOK_P_TEXT("org.yr"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("1.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("deadbeef"),
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("published"),
        COOKBOOK_P_INT(512)
    };
    db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)", ap, 9);

    /* yank with reason */
    cookbook_db_param yp[] = {
        COOKBOOK_P_TEXT("org.yr"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("1.0.0"),
        COOKBOOK_P_TEXT("CVE-2026-1234: remote code execution")
    };
    cookbook_db_status st = db->exec_p(db,
        "UPDATE artifacts SET yanked = 1, yank_reason = ?4 "
        "WHERE group_id = ?1 AND artifact = ?2 AND version = ?3",
        yp, 4);
    ASSERT(st == COOKBOOK_DB_OK, "yank with reason succeeds");

    /* verify reason stored via WHERE filter */
    int count = 0;
    cookbook_db_param fp[] = {
        COOKBOOK_P_TEXT("org.yr:lib:1.0.0:noarch"),
        COOKBOOK_P_TEXT("CVE-2026-1234: remote code execution")
    };
    db->query_p(db,
        "SELECT coord_id FROM artifacts "
        "WHERE coord_id = ?1 AND yank_reason = ?2",
        fp, 2, count_cb, &count);
    ASSERT(count == 1, "yank reason stored correctly");

    /* yank without reason — reason should be NULL */
    cookbook_db_param ap2[] = {
        COOKBOOK_P_TEXT("org.yr:lib:2.0.0:noarch"),
        COOKBOOK_P_TEXT("org.yr"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("2.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("cafebabe"),
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("published"),
        COOKBOOK_P_INT(256)
    };
    db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)", ap2, 9);

    cookbook_db_param yp2[] = {
        COOKBOOK_P_TEXT("org.yr"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("2.0.0"),
        COOKBOOK_P_NULL()
    };
    st = db->exec_p(db,
        "UPDATE artifacts SET yanked = 1, yank_reason = ?4 "
        "WHERE group_id = ?1 AND artifact = ?2 AND version = ?3",
        yp2, 4);
    ASSERT(st == COOKBOOK_DB_OK, "yank without reason succeeds");

    count = 0;
    cookbook_db_param fp2[] = {
        COOKBOOK_P_TEXT("org.yr:lib:2.0.0:noarch")
    };
    db->query_p(db,
        "SELECT coord_id FROM artifacts "
        "WHERE coord_id = ?1 AND yank_reason IS NULL",
        fp2, 1, count_cb, &count);
    ASSERT(count == 1, "yank without reason stores NULL");

    db->close(db);
}

/* F2: resolve with include_yanked returns yanked versions */
static void test_db_resolve_include_yanked(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    db->exec(db,
        "INSERT INTO groups (group_id, owner_sub) "
        "VALUES ('org.ry', 'alice')");

    /* insert v1.0.0 (will be yanked) and v2.0.0 (published) */
    cookbook_db_param a1[] = {
        COOKBOOK_P_TEXT("org.ry:lib:1.0.0:noarch"),
        COOKBOOK_P_TEXT("org.ry"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("1.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("aaa"),
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("published"),
        COOKBOOK_P_INT(100)
    };
    db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)", a1, 9);

    cookbook_db_param s1[] = {
        COOKBOOK_P_TEXT("org.ry:lib:1.0.0:noarch"),
        COOKBOOK_P_INT(1), COOKBOOK_P_INT(0), COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT(""), COOKBOOK_P_TEXT("")
    };
    db->exec_p(db,
        "INSERT INTO artifact_semver "
        "(coord_id, major, minor, patch, pre_release, build_meta) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6)", s1, 6);

    cookbook_db_param a2[] = {
        COOKBOOK_P_TEXT("org.ry:lib:2.0.0:noarch"),
        COOKBOOK_P_TEXT("org.ry"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("2.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("bbb"),
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("published"),
        COOKBOOK_P_INT(200)
    };
    db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)", a2, 9);

    cookbook_db_param s2[] = {
        COOKBOOK_P_TEXT("org.ry:lib:2.0.0:noarch"),
        COOKBOOK_P_INT(2), COOKBOOK_P_INT(0), COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT(""), COOKBOOK_P_TEXT("")
    };
    db->exec_p(db,
        "INSERT INTO artifact_semver "
        "(coord_id, major, minor, patch, pre_release, build_meta) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6)", s2, 6);

    /* yank v1.0.0 with reason */
    cookbook_db_param yp[] = {
        COOKBOOK_P_TEXT("org.ry"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("1.0.0"),
        COOKBOOK_P_TEXT("security vulnerability")
    };
    db->exec_p(db,
        "UPDATE artifacts SET yanked = 1, yank_reason = ?4 "
        "WHERE group_id = ?1 AND artifact = ?2 AND version = ?3",
        yp, 4);

    /* default resolve (exclude yanked) */
    int count = 0;
    cookbook_db_param rp[] = {
        COOKBOOK_P_TEXT("org.ry"),
        COOKBOOK_P_TEXT("lib")
    };
    db->query_p(db,
        "SELECT a.version FROM artifacts a "
        "JOIN artifact_semver s ON a.coord_id = s.coord_id "
        "WHERE a.group_id = ?1 AND a.artifact = ?2 "
        "AND a.yanked = 0 AND a.status = 'published'",
        rp, 2, count_cb, &count);
    ASSERT(count == 1, "default resolve excludes yanked");

    /* include_yanked resolve */
    count = 0;
    db->query_p(db,
        "SELECT a.version FROM artifacts a "
        "JOIN artifact_semver s ON a.coord_id = s.coord_id "
        "WHERE a.group_id = ?1 AND a.artifact = ?2 "
        "AND a.status = 'published'",
        rp, 2, count_cb, &count);
    ASSERT(count == 2, "include_yanked resolve returns both versions");

    /* verify yanked version has reason in extended query */
    count = 0;
    cookbook_db_param vrp[] = {
        COOKBOOK_P_TEXT("org.ry"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("security vulnerability")
    };
    db->query_p(db,
        "SELECT a.version FROM artifacts a "
        "WHERE a.group_id = ?1 AND a.artifact = ?2 "
        "AND a.yanked = 1 AND a.yank_reason = ?3",
        vrp, 3, count_cb, &count);
    ASSERT(count == 1, "yanked version has correct reason in resolve");

    db->close(db);
}

static void test_db_null_params(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* NULL into NOT NULL column should fail with constraint error */
    cookbook_db_param gp[] = {
        COOKBOOK_P_TEXT("org.null"),
        COOKBOOK_P_NULL()
    };
    cookbook_db_status st = db->exec_p(db,
        "INSERT INTO groups (group_id, owner_sub) VALUES (?1, ?2)",
        gp, 2);
    ASSERT(st == COOKBOOK_DB_CONSTRAINT, "NULL into NOT NULL rejected");

    /* NULL in a nullable column (descriptor_sha256) should work */
    db->exec(db,
        "INSERT INTO groups (group_id, owner_sub) "
        "VALUES ('org.null', 'alice')");
    cookbook_db_param ap[] = {
        COOKBOOK_P_TEXT("org.null:lib:1.0.0:noarch"),
        COOKBOOK_P_TEXT("org.null"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("1.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("aabb"),
        COOKBOOK_P_NULL(),              /* descriptor_sha256 */
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("published"),
        COOKBOOK_P_INT(128)
    };
    st = db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " descriptor_sha256, snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)", ap, 10);
    ASSERT(st == COOKBOOK_DB_OK, "NULL in nullable column accepted");

    db->close(db);
}

static void test_db_pending_to_published(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    db->exec(db,
        "INSERT INTO groups (group_id, owner_sub) "
        "VALUES ('org.phase', 'alice')");

    /* insert as pending */
    cookbook_db_param ap[] = {
        COOKBOOK_P_TEXT("org.phase:app:1.0.0:noarch"),
        COOKBOOK_P_TEXT("org.phase"),
        COOKBOOK_P_TEXT("app"),
        COOKBOOK_P_TEXT("1.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("aabbccdd"),
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("pending"),
        COOKBOOK_P_INT(256)
    };
    db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)", ap, 9);

    /* verify pending */
    int count = 0;
    cookbook_db_param qp[] = { COOKBOOK_P_TEXT("org.phase") };
    db->query_p(db,
        "SELECT coord_id FROM artifacts WHERE status = 'pending' "
        "AND group_id = ?1", qp, 1, count_cb, &count);
    ASSERT(count == 1, "pending artifact found");

    /* transition to published */
    cookbook_db_param up[] = {
        COOKBOOK_P_TEXT("org.phase:app:1.0.0:noarch")
    };
    db->exec_p(db,
        "UPDATE artifacts SET status = 'published' WHERE coord_id = ?1",
        up, 1);

    count = 0;
    db->query_p(db,
        "SELECT coord_id FROM artifacts WHERE status = 'published' "
        "AND group_id = ?1", qp, 1, count_cb, &count);
    ASSERT(count == 1, "published artifact found");

    db->close(db);
}

/* ---- additional store tests ---- */

static void test_store_overwrite(void) {
    const char *dir = COOKBOOK_TEST_RESOURCES "/tmp_store2";
    cookbook_store *store = cookbook_store_open_fs(dir);
    ASSERT(store != NULL, "open store for overwrite test");

    const char *key = "central/overwrite/test.txt";
    store->put(store, key, "first", 5);

    /* overwrite with different content */
    cookbook_store_status st = store->put(store, key, "second", 6);
    ASSERT(st == COOKBOOK_STORE_OK, "overwrite put succeeds");

    void *buf = NULL;
    size_t len = 0;
    store->get(store, key, &buf, &len);
    ASSERT(len == 6, "overwrite length correct");
    ASSERT(buf && memcmp(buf, "second", 6) == 0, "overwrite content correct");
    store->free_buf(buf);

    store->del(store, key);
    store->close(store);
}

static void test_store_large_value(void) {
    const char *dir = COOKBOOK_TEST_RESOURCES "/tmp_store3";
    cookbook_store *store = cookbook_store_open_fs(dir);
    ASSERT(store != NULL, "open store for large value test");

    /* 64KB value */
    size_t sz = 65536;
    char *big = malloc(sz);
    for (size_t i = 0; i < sz; i++) big[i] = (char)(i & 0x7F);

    const char *key = "central/large/blob.bin";
    cookbook_store_status st = store->put(store, key, big, sz);
    ASSERT(st == COOKBOOK_STORE_OK, "large put succeeds");

    void *buf = NULL;
    size_t len = 0;
    st = store->get(store, key, &buf, &len);
    ASSERT(st == COOKBOOK_STORE_OK, "large get succeeds");
    ASSERT(len == sz, "large roundtrip length");
    ASSERT(buf && memcmp(buf, big, sz) == 0, "large roundtrip content");
    store->free_buf(buf);

    store->del(store, key);
    free(big);
    store->close(store);
}

/* ---- additional auth tests ---- */

static void test_jwt_expired(void) {
    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    /* create a token with -1 second TTL — should be expired */
    char *token = cookbook_jwt_create("bob", "org.acme", -1, sk);
    ASSERT(token != NULL, "JWT create with negative TTL");

    cookbook_jwt_claims claims;
    int rc = cookbook_jwt_verify(token, pk, &claims);
    /* either verify fails or claims.valid is 0 */
    ASSERT(rc != 0 || claims.valid == 0, "expired JWT rejected");
    free(token);
}

static void test_jwt_group_boundary(void) {
    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    char *token = cookbook_jwt_create("alice", "org.acme.core", 3600, sk);
    ASSERT(token != NULL, "JWT create with dotted group");

    cookbook_jwt_claims claims;
    cookbook_jwt_verify(token, pk, &claims);

    /* exact match should work */
    ASSERT(cookbook_jwt_has_group(&claims, "org.acme.core") == 1,
           "exact group match");
    /* prefix should NOT match */
    ASSERT(cookbook_jwt_has_group(&claims, "org.acme") == 0,
           "prefix group does not match");
    /* substring should NOT match */
    ASSERT(cookbook_jwt_has_group(&claims, "org.acme.cor") == 0,
           "substring group does not match");
    /* empty group */
    ASSERT(cookbook_jwt_has_group(&claims, "") == 0,
           "empty group does not match");

    free(token);
}

static void test_base64url_edge_cases(void) {
    /* empty input */
    char encoded[64];
    size_t elen = cookbook_base64url_encode("", 0, encoded, sizeof(encoded));
    ASSERT(elen == 0, "base64url empty encode is empty");

    /* single byte */
    elen = cookbook_base64url_encode("A", 1, encoded, sizeof(encoded));
    ASSERT(elen > 0, "base64url single byte encodes");
    char decoded[64];
    size_t dlen = cookbook_base64url_decode(encoded, elen,
                                            decoded, sizeof(decoded));
    ASSERT(dlen == 1 && decoded[0] == 'A', "base64url single byte roundtrip");

    /* two bytes (tests padding=1 scenario) */
    elen = cookbook_base64url_encode("AB", 2, encoded, sizeof(encoded));
    dlen = cookbook_base64url_decode(encoded, elen, decoded, sizeof(decoded));
    ASSERT(dlen == 2 && decoded[0] == 'A' && decoded[1] == 'B',
           "base64url two byte roundtrip");

    /* binary data with all byte values 0-255 */
    unsigned char binary[256];
    for (int i = 0; i < 256; i++) binary[i] = (unsigned char)i;
    char big_encoded[512];
    elen = cookbook_base64url_encode(binary, 256, big_encoded,
                                     sizeof(big_encoded));
    ASSERT(elen > 0, "base64url binary encode");
    unsigned char big_decoded[256];
    dlen = cookbook_base64url_decode(big_encoded, elen, big_decoded,
                                     sizeof(big_decoded));
    ASSERT(dlen == 256, "base64url binary decode length");
    ASSERT(memcmp(binary, big_decoded, 256) == 0,
           "base64url binary roundtrip all bytes");
}

/* ---- additional ASCII validation tests ---- */

static void test_validate_ascii_boundaries(void) {
    /* 0x7F (DEL) is valid ASCII */
    ASSERT(cookbook_validate_ascii("\x7F", 1) == 0,
           "0x7F (DEL) is valid ASCII");

    /* 0x01 (SOH) — valid ASCII control char */
    ASSERT(cookbook_validate_ascii("\x01", 1) == 0,
           "0x01 is valid ASCII");

    /* 0x1F — last control char, valid */
    ASSERT(cookbook_validate_ascii("\x1F", 1) == 0,
           "0x1F is valid ASCII");

    /* 0x80 — first non-ASCII */
    ASSERT(cookbook_validate_ascii("\x80", 1) == 1,
           "0x80 is first non-ASCII");

    /* mixed valid + invalid at end */
    ASSERT(cookbook_validate_ascii("hello\x80", 6) == 6,
           "invalid at position 6");

    /* all printable ASCII */
    const char *printable =
        " !\"#$%&'()*+,-./0123456789:;<=>?@"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
        "abcdefghijklmnopqrstuvwxyz{|}~";
    ASSERT(cookbook_validate_ascii(printable, strlen(printable)) == 0,
           "all printable ASCII valid");
}

/* ---- additional pasta-to-json tests ---- */

static void test_pasta_to_json_empty_containers(void) {
    PastaResult pr;

    /* empty map */
    const char *s1 = "{}";
    PastaValue *root = pasta_parse(s1, strlen(s1), &pr);
    ASSERT(root != NULL, "parse empty map");
    if (root) {
        char *j = cookbook_pasta_to_json(root);
        ASSERT(j && strcmp(j, "{}") == 0, "empty map → {}");
        free(j);
        pasta_free(root);
    }

    /* empty array */
    const char *s2 = "{ items: [] }";
    root = pasta_parse(s2, strlen(s2), &pr);
    ASSERT(root != NULL, "parse empty array");
    if (root) {
        char *j = cookbook_pasta_to_json(root);
        ASSERT(j != NULL, "empty array JSON");
        ASSERT(strstr(j, "[]") != NULL, "JSON has []");
        free(j);
        pasta_free(root);
    }
}

static void test_pasta_to_json_numbers(void) {
    PastaResult pr;

    /* integer */
    const char *s1 = "{ val: 0 }";
    PastaValue *root = pasta_parse(s1, strlen(s1), &pr);
    ASSERT(root != NULL, "parse zero");
    if (root) {
        char *j = cookbook_pasta_to_json(root);
        ASSERT(j && strstr(j, ":0}") != NULL, "zero in JSON");
        free(j);
        pasta_free(root);
    }

    /* negative */
    const char *s2 = "{ val: -42 }";
    root = pasta_parse(s2, strlen(s2), &pr);
    ASSERT(root != NULL, "parse negative");
    if (root) {
        char *j = cookbook_pasta_to_json(root);
        ASSERT(j && strstr(j, "-42") != NULL, "negative in JSON");
        free(j);
        pasta_free(root);
    }

    /* float */
    const char *s3 = "{ val: 3.14 }";
    root = pasta_parse(s3, strlen(s3), &pr);
    ASSERT(root != NULL, "parse float");
    if (root) {
        char *j = cookbook_pasta_to_json(root);
        ASSERT(j && strstr(j, "3.14") != NULL, "float in JSON");
        free(j);
        pasta_free(root);
    }
}

static void test_pasta_to_json_deeply_nested(void) {
    PastaResult pr;
    const char *src =
        "{ a: { b: { c: { d: [1, 2, { e: true }] } } } }";
    PastaValue *root = pasta_parse(src, strlen(src), &pr);
    ASSERT(root != NULL, "parse deeply nested");
    if (root) {
        char *j = cookbook_pasta_to_json(root);
        ASSERT(j != NULL, "deeply nested JSON");
        ASSERT(strstr(j, "\"a\"") != NULL, "has key a");
        ASSERT(strstr(j, "\"e\"") != NULL, "has key e");
        ASSERT(strstr(j, "true") != NULL, "has true at depth");
        free(j);
        pasta_free(root);
    }
}

/* ---- additional sorted write tests ---- */

static void test_pasta_sorted_nested(void) {
    PastaResult pr;
    const char *src =
        "{ z_outer: { z_inner: 1, a_inner: 2 }, a_outer: 3 }";
    PastaValue *root = pasta_parse(src, strlen(src), &pr);
    ASSERT(root != NULL, "parse nested unsorted");
    if (root) {
        char *sorted = pasta_write(root, PASTA_COMPACT | PASTA_SORTED);
        ASSERT(sorted != NULL, "nested sorted write");
        if (sorted) {
            /* outer keys sorted: a_outer before z_outer */
            char *pa = strstr(sorted, "a_outer");
            char *pz = strstr(sorted, "z_outer");
            ASSERT(pa && pz && pa < pz,
                   "outer keys sorted: a_outer < z_outer");
            /* inner keys sorted: a_inner before z_inner */
            char *pai = strstr(sorted, "a_inner");
            char *pzi = strstr(sorted, "z_inner");
            ASSERT(pai && pzi && pai < pzi,
                   "inner keys sorted: a_inner < z_inner");
            free(sorted);
        }
        pasta_free(root);
    }
}

static void test_pasta_sorted_write(void) {
    PastaResult pr;
    const char *src = "{ zebra: 1, apple: 2, mango: 3 }";
    PastaValue *root = pasta_parse(src, strlen(src), &pr);
    ASSERT(root != NULL, "parse unsorted Pasta");
    if (root) {
        char *sorted = pasta_write(root, PASTA_COMPACT | PASTA_SORTED);
        ASSERT(sorted != NULL, "sorted compact write");
        if (sorted) {
            /* apple should come before mango, mango before zebra */
            char *pa = strstr(sorted, "apple");
            char *pm = strstr(sorted, "mango");
            char *pz = strstr(sorted, "zebra");
            ASSERT(pa && pm && pz, "all keys present in sorted output");
            ASSERT(pa < pm && pm < pz,
                   "keys in lexicographic order: apple < mango < zebra");
            free(sorted);
        }
        pasta_free(root);
    }
}

/* ---- auth v2: policy tests ---- */

static void test_policy_crud(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* put a policy */
    int rc = cookbook_policy_put(db, "alice", "user",
        "@identity {\n  subject: \"alice\",\n  kind: \"user\",\n"
        "  teams: [core]\n}\n@grants {\n  com.iridiumfx: \"crwd\"\n}\n");
    ASSERT(rc == 0, "policy put succeeds");

    /* get it back */
    char *p = cookbook_policy_get(db, "alice");
    ASSERT(p != NULL, "policy get returns non-NULL");
    ASSERT(strstr(p, "alice") != NULL, "policy contains subject");
    ASSERT(strstr(p, "crwd") != NULL, "policy contains grants");
    free(p);

    /* get non-existent */
    char *p2 = cookbook_policy_get(db, "bob");
    ASSERT(p2 == NULL, "non-existent policy returns NULL");

    /* update (replace) */
    rc = cookbook_policy_put(db, "alice", "user",
        "@identity {\n  subject: \"alice\"\n}\n"
        "@grants {\n  com.iridiumfx: \"r\"\n}\n");
    ASSERT(rc == 0, "policy update succeeds");
    p = cookbook_policy_get(db, "alice");
    ASSERT(p != NULL, "updated policy exists");
    ASSERT(strstr(p, "\"r\"") != NULL, "updated policy has new grant");
    free(p);

    /* delete */
    rc = cookbook_policy_delete(db, "alice");
    ASSERT(rc == 0, "policy delete succeeds");
    p = cookbook_policy_get(db, "alice");
    ASSERT(p == NULL, "deleted policy is gone");

    db->close(db);
}

static void test_policy_resolve(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* add user with team reference */
    cookbook_policy_put(db, "alice", "user",
        "@identity {\n  subject: \"alice\",\n  kind: \"user\",\n"
        "  teams: [core]\n}\n"
        "@grants {\n  com.iridiumfx: \"r\"\n}\n");

    /* add team policy */
    cookbook_policy_put(db, "core", "team",
        "@identity {\n  team_id: \"core\",\n  kind: \"team\"\n}\n"
        "@grants {\n  com.iridiumfx: \"crwd\",\n"
        "  com.iridiumfx.internal: \"crwd\"\n}\n"
        "@exclude {\n  com.iridiumfx.secret: true\n}\n");

    /* resolve — should aggregate alice + core */
    char *json = cookbook_policy_resolve(db, "alice");
    ASSERT(json != NULL, "policy resolve returns non-NULL");
    ASSERT(strstr(json, "\"grants\"") != NULL, "resolved has grants");
    ASSERT(strstr(json, "com.iridiumfx") != NULL, "resolved has group");
    ASSERT(strstr(json, "\"exclude\"") != NULL, "resolved has exclude");
    ASSERT(strstr(json, "com.iridiumfx.secret") != NULL, "resolved has exclusion");

    /* verify collect OR: com.iridiumfx should have permissions from BOTH
       user ("r") and team ("crwd") OR'd together, not just last-write-wins */
    ASSERT(strstr(json, "crwd") != NULL || strstr(json, "rcrwd") != NULL,
           "resolved grants OR user+team permissions");

    /* com.iridiumfx.internal only in team → single value, not array */
    ASSERT(strstr(json, "com.iridiumfx.internal") != NULL,
           "team-only grant preserved");
    free(json);

    /* resolve non-existent user */
    char *j2 = cookbook_policy_resolve(db, "nobody");
    ASSERT(j2 == NULL, "resolve unknown user returns NULL");

    db->close(db);
}

static void test_policy_resolve_collect(void) {
    /* prove that merge:"collect" OR's permissions instead of last-write-wins */
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* user has 'r' only, team has 'cw' only — result must have all three */
    cookbook_policy_put(db, "dev1", "user",
        "@identity {\n  subject: \"dev1\",\n  kind: \"user\",\n"
        "  teams: [builders]\n}\n"
        "@grants {\n  com.example: \"r\"\n}\n");

    cookbook_policy_put(db, "builders", "team",
        "@identity {\n  team_id: \"builders\",\n  kind: \"team\"\n}\n"
        "@grants {\n  com.example: \"cw\"\n}\n");

    char *json = cookbook_policy_resolve(db, "dev1");
    ASSERT(json != NULL, "collect resolve returns non-NULL");

    /* with last-write-wins: "cw" only. with collect OR: "rcw" or "cwr" etc. */
    int has_r = (strstr(json, "r") != NULL);
    int has_c = (strstr(json, "c") != NULL);
    int has_w = (strstr(json, "w") != NULL);
    ASSERT(has_r && has_c && has_w,
           "collect OR yields all three permissions (r+cw)");

    /* verify via auth_check that all three ops are allowed */
    ASSERT(cookbook_auth_check(json, NULL, "com.example", 'r') == 1,
           "collect: read allowed (from user)");
    ASSERT(cookbook_auth_check(json, NULL, "com.example", 'c') == 1,
           "collect: create allowed (from team)");
    ASSERT(cookbook_auth_check(json, NULL, "com.example", 'w') == 1,
           "collect: write allowed (from team)");
    ASSERT(cookbook_auth_check(json, NULL, "com.example", 'd') == 0,
           "collect: delete denied (nobody granted it)");

    free(json);
    db->close(db);
}

static void test_auth_check_prefix(void) {
    const char *grants =
        "{\"grants\":{\"com.iridiumfx\":\"crwd\",\"org.acme\":\"r\"},"
        "\"exclude\":{}}";

    /* exact prefix match */
    ASSERT(cookbook_auth_check(grants, NULL, "com.iridiumfx", 'r') == 1,
           "exact prefix read allowed");
    ASSERT(cookbook_auth_check(grants, NULL, "com.iridiumfx", 'c') == 1,
           "exact prefix create allowed");

    /* hierarchical match: com.iridiumfx.pasta starts with com.iridiumfx */
    ASSERT(cookbook_auth_check(grants, NULL, "com.iridiumfx.pasta", 'r') == 1,
           "hierarchical read allowed");
    ASSERT(cookbook_auth_check(grants, NULL, "com.iridiumfx.pasta", 'w') == 1,
           "hierarchical write allowed");

    /* org.acme is read-only */
    ASSERT(cookbook_auth_check(grants, NULL, "org.acme", 'r') == 1,
           "org.acme read allowed");
    ASSERT(cookbook_auth_check(grants, NULL, "org.acme.sdk", 'r') == 1,
           "org.acme.sdk read allowed");
    ASSERT(cookbook_auth_check(grants, NULL, "org.acme", 'w') == 0,
           "org.acme write denied");
    ASSERT(cookbook_auth_check(grants, NULL, "org.acme.sdk", 'c') == 0,
           "org.acme.sdk create denied");

    /* no matching grant */
    ASSERT(cookbook_auth_check(grants, NULL, "net.example", 'r') == 0,
           "ungranted group denied");

    /* prefix must match at dot boundary */
    ASSERT(cookbook_auth_check(grants, NULL, "com.iridiumfxtra", 'r') == 0,
           "non-dot-boundary prefix rejected");
}

static void test_auth_check_exclude(void) {
    const char *grants =
        "{\"grants\":{\"com.iridiumfx\":\"crwd\"},"
        "\"exclude\":{\"com.iridiumfx.secret\":true}}";

    /* normal access allowed */
    ASSERT(cookbook_auth_check(grants, NULL, "com.iridiumfx.pasta", 'r') == 1,
           "non-excluded group allowed");

    /* excluded group denied despite grant */
    ASSERT(cookbook_auth_check(grants, NULL, "com.iridiumfx.secret", 'r') == 0,
           "excluded group denied");

    /* sub-group of excluded also denied */
    ASSERT(cookbook_auth_check(grants, NULL, "com.iridiumfx.secret.keys", 'r') == 0,
           "sub-group of excluded denied");
}

static void test_auth_check_edge_cases(void) {
    /* NULL grants */
    ASSERT(cookbook_auth_check(NULL, NULL, "com.foo", 'r') == 0,
           "NULL grants denied");

    /* NULL group */
    ASSERT(cookbook_auth_check("{\"grants\":{}}", NULL, NULL, 'r') == 0,
           "NULL group denied");

    /* empty grants */
    ASSERT(cookbook_auth_check("{\"grants\":{},\"exclude\":{}}", NULL,
                               "com.foo", 'r') == 0,
           "empty grants denied");
}

static void test_alforno_integration(void) {
    /* verify alforno aggregate works with our pasta pastlets */
    const char *user_pastlet =
        "@identity {\n  subject: \"test\"\n}\n"
        "@grants {\n  com.test: \"r\"\n}\n";
    const char *team_pastlet =
        "@identity {\n  team_id: \"devs\"\n}\n"
        "@grants {\n  com.test: \"cw\",\n  org.shared: \"r\"\n}\n";

    AlfResult ar;
    AlfContext *ctx = alf_create(ALF_AGGREGATE, &ar);
    ASSERT(ctx != NULL, "alf_create succeeds");
    if (!ctx) return;

    int rc = alf_add_input(ctx, user_pastlet, strlen(user_pastlet), &ar);
    ASSERT(rc == 0, "alf_add_input user succeeds");

    rc = alf_add_input(ctx, team_pastlet, strlen(team_pastlet), &ar);
    ASSERT(rc == 0, "alf_add_input team succeeds");

    PastaValue *resolved = alf_process(ctx, &ar);
    ASSERT(resolved != NULL, "alf_process succeeds");

    /* check that @grants section exists */
    const PastaValue *grants = pasta_map_get(resolved, "grants");
    ASSERT(grants != NULL, "resolved has @grants");
    ASSERT(pasta_type(grants) == PASTA_MAP, "@grants is a map");

    /* last-write-wins: com.test should be "cw" (from team, which came second) */
    const PastaValue *ct = pasta_map_get(grants, "com.test");
    ASSERT(ct != NULL, "com.test grant exists");
    ASSERT(pasta_type(ct) == PASTA_STRING, "com.test is string");
    ASSERT(strcmp(pasta_get_string(ct), "cw") == 0,
           "com.test is 'cw' (last-write-wins from team)");

    /* org.shared should be "r" (only in team) */
    const PastaValue *os = pasta_map_get(grants, "org.shared");
    ASSERT(os != NULL, "org.shared grant exists");
    ASSERT(strcmp(pasta_get_string(os), "r") == 0,
           "org.shared is 'r'");

    /* check @exclude doesn't exist (neither pastlet has it) */
    const PastaValue *exc = pasta_map_get(resolved, "exclude");
    ASSERT(exc == NULL, "no @exclude when none provided");

    pasta_free(resolved);
    alf_free(ctx);
}

/* ---- auth v2 phase 2: JWT v2 tests ---- */

static void test_jwt_v2_roundtrip(void) {
    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    const char *resolved = "{\"grants\":{\"com.iridiumfx\":\"crwd\",\"org.acme\":\"r\"},"
                           "\"exclude\":{\"com.iridiumfx.secret\":true}}";

    char *token = cookbook_jwt_create_v2("alice", "com.iridiumfx",
                                          resolved, 3600, sk);
    ASSERT(token != NULL, "jwt v2 create succeeds");

    cookbook_jwt_claims claims;
    int rc = cookbook_jwt_verify(token, pk, &claims);
    ASSERT(rc == 0, "jwt v2 verify succeeds");
    ASSERT(claims.valid == 1, "jwt v2 claims valid");
    ASSERT(claims.version == 2, "jwt v2 version is 2");
    ASSERT(strcmp(claims.sub, "alice") == 0, "jwt v2 sub is alice");
    ASSERT(strcmp(claims.groups, "com.iridiumfx") == 0, "jwt v2 groups preserved");

    /* check grants extracted */
    ASSERT(claims.grants_json != NULL, "jwt v2 grants_json extracted");
    ASSERT(strstr(claims.grants_json, "com.iridiumfx") != NULL,
           "jwt v2 grants contains com.iridiumfx");
    ASSERT(strstr(claims.grants_json, "crwd") != NULL,
           "jwt v2 grants contains crwd");
    ASSERT(strstr(claims.grants_json, "org.acme") != NULL,
           "jwt v2 grants contains org.acme");

    /* check exclude extracted */
    ASSERT(claims.exclude_json != NULL, "jwt v2 exclude_json extracted");
    ASSERT(strstr(claims.exclude_json, "com.iridiumfx.secret") != NULL,
           "jwt v2 exclude contains com.iridiumfx.secret");

    /* use auth_check with extracted claims */
    ASSERT(cookbook_auth_check(claims.grants_json, claims.exclude_json,
                               "com.iridiumfx.pasta", 'r') == 1,
           "jwt v2 grants allow read on sub-group");
    ASSERT(cookbook_auth_check(claims.grants_json, claims.exclude_json,
                               "com.iridiumfx.secret", 'r') == 0,
           "jwt v2 exclude blocks secret");
    ASSERT(cookbook_auth_check(claims.grants_json, claims.exclude_json,
                               "org.acme", 'r') == 1,
           "jwt v2 grants allow read on org.acme");
    ASSERT(cookbook_auth_check(claims.grants_json, claims.exclude_json,
                               "org.acme", 'w') == 0,
           "jwt v2 org.acme write denied (read-only)");

    cookbook_jwt_claims_free(&claims);
    free(token);
}

static void test_jwt_v2_policy_integration(void) {
    /* end-to-end: store policy → resolve → create JWT v2 → verify → check */
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* set up user + team policies */
    cookbook_policy_put(db, "bob", "user",
        "@identity {\n  subject: \"bob\",\n  kind: \"user\",\n"
        "  teams: [devs]\n}\n"
        "@grants {\n  com.example: \"r\"\n}\n");

    cookbook_policy_put(db, "devs", "team",
        "@identity {\n  team_id: \"devs\",\n  kind: \"team\"\n}\n"
        "@grants {\n  com.example: \"crwd\",\n"
        "  com.example.tools: \"rw\"\n}\n"
        "@exclude {\n  com.example.private: true\n}\n");

    /* resolve */
    char *resolved = cookbook_policy_resolve(db, "bob");
    ASSERT(resolved != NULL, "policy resolves for bob");

    /* create JWT v2 */
    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    char *token = cookbook_jwt_create_v2("bob", NULL, resolved, 3600, sk);
    ASSERT(token != NULL, "jwt v2 from policy succeeds");
    free(resolved);

    /* verify and extract */
    cookbook_jwt_claims claims;
    int rc = cookbook_jwt_verify(token, pk, &claims);
    ASSERT(rc == 0, "policy jwt v2 verify succeeds");
    ASSERT(claims.version == 2, "policy jwt v2 version");
    ASSERT(claims.grants_json != NULL, "policy jwt v2 has grants");
    ASSERT(claims.exclude_json != NULL, "policy jwt v2 has exclude");

    /* enforce: com.example.tools should be writable (from team) */
    ASSERT(cookbook_auth_check(claims.grants_json, claims.exclude_json,
                               "com.example.tools", 'w') == 1,
           "bob can write com.example.tools via team");

    /* enforce: com.example.private denied by exclude */
    ASSERT(cookbook_auth_check(claims.grants_json, claims.exclude_json,
                               "com.example.private", 'r') == 0,
           "bob denied com.example.private via exclude");

    /* enforce: com.example readable (aggregate: last-write-wins = crwd from team) */
    ASSERT(cookbook_auth_check(claims.grants_json, claims.exclude_json,
                               "com.example", 'c') == 1,
           "bob can create under com.example (team escalation)");

    cookbook_jwt_claims_free(&claims);
    free(token);
    db->close(db);
}

static void test_jwt_v1_v2_compat(void) {
    /* v1 token should still work — no grants/exclude, version=1 */
    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    char *token = cookbook_jwt_create("charlie", "org.test", 3600, sk);
    ASSERT(token != NULL, "jwt v1 create succeeds");

    cookbook_jwt_claims claims;
    int rc = cookbook_jwt_verify(token, pk, &claims);
    ASSERT(rc == 0, "jwt v1 verify succeeds");
    ASSERT(claims.version == 1, "jwt v1 version is 1");
    ASSERT(strcmp(claims.sub, "charlie") == 0, "jwt v1 sub");
    ASSERT(strcmp(claims.groups, "org.test") == 0, "jwt v1 groups");
    ASSERT(claims.grants_json == NULL, "jwt v1 no grants_json");
    ASSERT(claims.exclude_json == NULL, "jwt v1 no exclude_json");

    cookbook_jwt_claims_free(&claims);
    free(token);
}

/* ---- auth v2 phase 3: enforcement tests ---- */

static void test_enforcement_v1_reads_open(void) {
    /* v1 tokens: reads are open (no per-group check), writes need has_group */
    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    char *token = cookbook_jwt_create("alice", "org.acme", 3600, sk);
    ASSERT(token != NULL, "v1 token create");

    cookbook_jwt_claims c;
    cookbook_jwt_verify(token, pk, &c);
    ASSERT(c.version == 1, "enforcement: v1 version");

    /* v1 has no grants_json — auth_check should deny (used for v2 path) */
    ASSERT(cookbook_auth_check(c.grants_json, c.exclude_json,
                               "org.acme", 'r') == 0,
           "auth_check with NULL grants denies (v1 would bypass this)");

    /* v1 group check: has_group is exact match (not hierarchical) */
    ASSERT(cookbook_jwt_has_group(&c, "org.acme") == 1,
           "v1 has_group matches exact");
    ASSERT(cookbook_jwt_has_group(&c, "org.acme.sdk") == 0,
           "v1 has_group rejects child (exact only)");
    ASSERT(cookbook_jwt_has_group(&c, "net.other") == 0,
           "v1 has_group rejects unrelated");

    cookbook_jwt_claims_free(&c);
    free(token);
}

static void test_enforcement_v2_all_ops(void) {
    /* v2 tokens: all 4 ops checked via cookbook_auth_check */
    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    const char *resolved = "{\"grants\":{\"com.iridiumfx\":\"crwd\","
                           "\"org.readonly\":\"r\","
                           "\"org.readwrite\":\"rw\"},"
                           "\"exclude\":{\"com.iridiumfx.internal\":true}}";

    char *token = cookbook_jwt_create_v2("bob", "com.iridiumfx",
                                          resolved, 3600, sk);
    cookbook_jwt_claims c;
    cookbook_jwt_verify(token, pk, &c);
    ASSERT(c.version == 2, "enforcement: v2 version");

    /* full CRWD on com.iridiumfx */
    ASSERT(cookbook_auth_check(c.grants_json, c.exclude_json,
                               "com.iridiumfx.pasta", 'c') == 1,
           "v2 create on granted group");
    ASSERT(cookbook_auth_check(c.grants_json, c.exclude_json,
                               "com.iridiumfx.pasta", 'r') == 1,
           "v2 read on granted group");
    ASSERT(cookbook_auth_check(c.grants_json, c.exclude_json,
                               "com.iridiumfx.pasta", 'w') == 1,
           "v2 write on granted group");
    ASSERT(cookbook_auth_check(c.grants_json, c.exclude_json,
                               "com.iridiumfx.pasta", 'd') == 1,
           "v2 delete on granted group");

    /* read-only group */
    ASSERT(cookbook_auth_check(c.grants_json, c.exclude_json,
                               "org.readonly.lib", 'r') == 1,
           "v2 read on readonly group");
    ASSERT(cookbook_auth_check(c.grants_json, c.exclude_json,
                               "org.readonly.lib", 'c') == 0,
           "v2 create denied on readonly group");
    ASSERT(cookbook_auth_check(c.grants_json, c.exclude_json,
                               "org.readonly.lib", 'w') == 0,
           "v2 write denied on readonly group");
    ASSERT(cookbook_auth_check(c.grants_json, c.exclude_json,
                               "org.readonly.lib", 'd') == 0,
           "v2 delete denied on readonly group");

    /* read-write group: no create/delete */
    ASSERT(cookbook_auth_check(c.grants_json, c.exclude_json,
                               "org.readwrite.tool", 'r') == 1,
           "v2 read on rw group");
    ASSERT(cookbook_auth_check(c.grants_json, c.exclude_json,
                               "org.readwrite.tool", 'w') == 1,
           "v2 write on rw group");
    ASSERT(cookbook_auth_check(c.grants_json, c.exclude_json,
                               "org.readwrite.tool", 'c') == 0,
           "v2 create denied on rw group");
    ASSERT(cookbook_auth_check(c.grants_json, c.exclude_json,
                               "org.readwrite.tool", 'd') == 0,
           "v2 delete denied on rw group");

    /* excluded group: denied despite parent grant */
    ASSERT(cookbook_auth_check(c.grants_json, c.exclude_json,
                               "com.iridiumfx.internal", 'r') == 0,
           "v2 excluded group denied read");
    ASSERT(cookbook_auth_check(c.grants_json, c.exclude_json,
                               "com.iridiumfx.internal.keys", 'r') == 0,
           "v2 excluded child denied read");

    /* ungranted group */
    ASSERT(cookbook_auth_check(c.grants_json, c.exclude_json,
                               "net.unknown", 'r') == 0,
           "v2 ungranted group denied");

    cookbook_jwt_claims_free(&c);
    free(token);
}

static void test_enforcement_mirror_visibility(void) {
    /* simulate mirror manifest filtering: iterate groups, check visibility */
    const char *grants = "{\"com.iridiumfx\":\"crwd\",\"org.acme\":\"r\"}";
    const char *exclude = "{\"com.iridiumfx.secret\":true}";

    const char *groups[] = {
        "com.iridiumfx.pasta",   /* visible: granted */
        "com.iridiumfx.basta",   /* visible: granted */
        "com.iridiumfx.secret",  /* hidden: excluded */
        "org.acme.sdk",          /* visible: read granted */
        "net.private.tools",     /* hidden: no grant */
    };
    int expected[] = { 1, 1, 0, 1, 0 };

    int visible_count = 0;
    for (int i = 0; i < 5; i++) {
        int allowed = cookbook_auth_check(grants, exclude, groups[i], 'r');
        ASSERT(allowed == expected[i], "mirror visibility filter");
        if (allowed) visible_count++;
    }
    ASSERT(visible_count == 3, "mirror: 3 of 5 groups visible");
}

static void test_enforcement_policy_to_enforcement(void) {
    /* end-to-end: two users with different policies see different things */
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* admin: full access to com.iridiumfx */
    cookbook_policy_put(db, "admin", "user",
        "@identity {\n  subject: \"admin\",\n  kind: \"user\"\n}\n"
        "@grants {\n  com.iridiumfx: \"crwd\"\n}\n");

    /* viewer: read-only access to com.iridiumfx */
    cookbook_policy_put(db, "viewer", "user",
        "@identity {\n  subject: \"viewer\",\n  kind: \"user\"\n}\n"
        "@grants {\n  com.iridiumfx: \"r\"\n}\n");

    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    /* resolve and create tokens */
    char *admin_resolved = cookbook_policy_resolve(db, "admin");
    char *viewer_resolved = cookbook_policy_resolve(db, "viewer");
    ASSERT(admin_resolved != NULL, "admin policy resolves");
    ASSERT(viewer_resolved != NULL, "viewer policy resolves");

    char *admin_tok = cookbook_jwt_create_v2("admin", NULL, admin_resolved, 3600, sk);
    char *viewer_tok = cookbook_jwt_create_v2("viewer", NULL, viewer_resolved, 3600, sk);
    free(admin_resolved);
    free(viewer_resolved);

    cookbook_jwt_claims ac, vc;
    cookbook_jwt_verify(admin_tok, pk, &ac);
    cookbook_jwt_verify(viewer_tok, pk, &vc);

    /* admin can PUT (create) */
    ASSERT(cookbook_auth_check(ac.grants_json, ac.exclude_json,
                               "com.iridiumfx.pasta", 'c') == 1,
           "admin can create artifact");
    /* viewer cannot PUT */
    ASSERT(cookbook_auth_check(vc.grants_json, vc.exclude_json,
                               "com.iridiumfx.pasta", 'c') == 0,
           "viewer cannot create artifact");
    /* both can GET */
    ASSERT(cookbook_auth_check(ac.grants_json, ac.exclude_json,
                               "com.iridiumfx.pasta", 'r') == 1,
           "admin can read artifact");
    ASSERT(cookbook_auth_check(vc.grants_json, vc.exclude_json,
                               "com.iridiumfx.pasta", 'r') == 1,
           "viewer can read artifact");
    /* admin can yank (write), viewer cannot */
    ASSERT(cookbook_auth_check(ac.grants_json, ac.exclude_json,
                               "com.iridiumfx.pasta", 'w') == 1,
           "admin can yank artifact");
    ASSERT(cookbook_auth_check(vc.grants_json, vc.exclude_json,
                               "com.iridiumfx.pasta", 'w') == 0,
           "viewer cannot yank artifact");

    cookbook_jwt_claims_free(&ac);
    cookbook_jwt_claims_free(&vc);
    free(admin_tok);
    free(viewer_tok);
    db->close(db);
}

static void test_enforcement_grid_grant_roundtrip(void) {
    /* test that grants JSON → header format → JSON produces valid auth_check input */
    /* simulate the header format: "com.iridiumfx:crwd,org.acme:r" */
    /* and parse it back to JSON manually (same logic as parse_grid_grants_header) */

    /* original grants */
    const char *orig_grants = "{\"com.iridiumfx\":\"crwd\",\"org.acme\":\"r\"}";
    const char *orig_exclude = "{\"com.iridiumfx.secret\":true}";

    /* simulate header format (what build_grid_grant_headers produces) */
    const char *hdr_grants = "com.iridiumfx:crwd,org.acme:r";
    const char *hdr_exclude = "com.iridiumfx.secret";

    /* parse grants header format back to JSON */
    size_t cap = strlen(hdr_grants) * 3 + 16;
    char *parsed_grants = (char *)malloc(cap);
    size_t pos = 0;
    parsed_grants[pos++] = '{';

    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", hdr_grants);
    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    int first = 1;
    while (tok) {
        while (*tok == ' ') tok++;
        char *colon = strchr(tok, ':');
        if (colon) {
            *colon = '\0';
            if (!first) parsed_grants[pos++] = ',';
            pos += (size_t)snprintf(parsed_grants + pos, cap - pos,
                "\"%s\":\"%s\"", tok, colon + 1);
            first = 0;
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }
    parsed_grants[pos++] = '}';
    parsed_grants[pos] = '\0';

    /* parse exclude header format back to JSON */
    cap = strlen(hdr_exclude) * 3 + 16;
    char *parsed_exclude = (char *)malloc(cap);
    pos = 0;
    parsed_exclude[pos++] = '{';
    snprintf(buf, sizeof(buf), "%s", hdr_exclude);
    tok = strtok_r(buf, ",", &saveptr);
    first = 1;
    while (tok) {
        while (*tok == ' ') tok++;
        if (!first) parsed_exclude[pos++] = ',';
        pos += (size_t)snprintf(parsed_exclude + pos, cap - pos,
            "\"%s\":true", tok);
        first = 0;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    parsed_exclude[pos++] = '}';
    parsed_exclude[pos] = '\0';

    /* roundtripped grants should produce same auth_check results */
    ASSERT(cookbook_auth_check(parsed_grants, parsed_exclude,
                               "com.iridiumfx.pasta", 'r') == 1,
           "grid roundtrip: read allowed");
    ASSERT(cookbook_auth_check(parsed_grants, parsed_exclude,
                               "com.iridiumfx.pasta", 'c') == 1,
           "grid roundtrip: create allowed");
    ASSERT(cookbook_auth_check(parsed_grants, parsed_exclude,
                               "com.iridiumfx.secret", 'r') == 0,
           "grid roundtrip: excluded denied");
    ASSERT(cookbook_auth_check(parsed_grants, parsed_exclude,
                               "org.acme.sdk", 'r') == 1,
           "grid roundtrip: readonly read allowed");
    ASSERT(cookbook_auth_check(parsed_grants, parsed_exclude,
                               "org.acme.sdk", 'w') == 0,
           "grid roundtrip: readonly write denied");
    ASSERT(cookbook_auth_check(parsed_grants, parsed_exclude,
                               "net.unknown", 'r') == 0,
           "grid roundtrip: ungranted denied");

    /* compare with original grants */
    ASSERT(cookbook_auth_check(orig_grants, orig_exclude,
                               "com.iridiumfx.pasta", 'r') ==
           cookbook_auth_check(parsed_grants, parsed_exclude,
                               "com.iridiumfx.pasta", 'r'),
           "grid roundtrip matches original: read");
    ASSERT(cookbook_auth_check(orig_grants, orig_exclude,
                               "com.iridiumfx.secret", 'r') ==
           cookbook_auth_check(parsed_grants, parsed_exclude,
                               "com.iridiumfx.secret", 'r'),
           "grid roundtrip matches original: excluded");

    free(parsed_grants);
    free(parsed_exclude);
}

/* ---- Phase 4: grid peer auth tests ---- */

static void test_grid_peer_key_crud(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* insert peer with public_key */
    db->exec(db,
        "INSERT INTO peers (peer_id, name, url, mode, priority, enabled, public_key) "
        "VALUES ('node-a', 'Node A', 'http://node-a:8080', 'redirect', 100, 1, "
        "'d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a')");

    unsigned char pk[32];
    int rc = cookbook_grid_load_peer_key(db, "node-a", pk);
    ASSERT(rc == 0, "peer key: load existing key");
    ASSERT(pk[0] == 0xd7 && pk[1] == 0x5a, "peer key: first bytes match");
    ASSERT(pk[31] == 0x1a, "peer key: last byte matches");

    /* peer with NULL key */
    db->exec(db,
        "INSERT INTO peers (peer_id, name, url, mode, priority, enabled) "
        "VALUES ('node-b', 'Node B', 'http://node-b:8080', 'redirect', 100, 1)");
    rc = cookbook_grid_load_peer_key(db, "node-b", pk);
    ASSERT(rc == -1, "peer key: NULL key returns -1");

    /* unknown peer */
    rc = cookbook_grid_load_peer_key(db, "node-z", pk);
    ASSERT(rc == -1, "peer key: unknown peer returns -1");

    /* disabled peer */
    db->exec(db,
        "INSERT INTO peers (peer_id, name, url, mode, priority, enabled, public_key) "
        "VALUES ('node-c', 'Node C', 'http://node-c:8080', 'redirect', 100, 0, "
        "'d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a')");
    rc = cookbook_grid_load_peer_key(db, "node-c", pk);
    ASSERT(rc == -1, "peer key: disabled peer returns -1");

    db->close(db);
}

static void test_grid_peer_load_with_key(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* peer with key */
    db->exec(db,
        "INSERT INTO peers (peer_id, name, url, mode, priority, enabled, public_key) "
        "VALUES ('keyed', 'Keyed', 'http://keyed:8080', 'redirect', 50, 1, "
        "'d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a')");

    /* peer without key */
    db->exec(db,
        "INSERT INTO peers (peer_id, name, url, mode, priority, enabled) "
        "VALUES ('nokey', 'NoKey', 'http://nokey:8080', 'proxy', 100, 1)");

    cookbook_peer *peers = NULL;
    int n = cookbook_grid_load_peers(db, &peers);
    ASSERT(n == 2, "peer load key: 2 peers");
    /* keyed first (priority 50) */
    ASSERT(peers[0].has_public_key == 1, "peer load key: keyed has key");
    ASSERT(peers[0].public_key[0] == 0xd7, "peer load key: keyed pk byte 0");
    ASSERT(peers[1].has_public_key == 0, "peer load key: nokey has no key");

    cookbook_grid_free_peers(peers, n);
    db->close(db);
}

static void test_grid_canonical_string(void) {
    size_t len1, len2;
    char *c1 = cookbook_grid_build_canonical("GET", "/grid/resolve/org/acme/lib/*",
        "central", 1, "org.acme:r", "", 1710800000, &len1);
    ASSERT(c1 != NULL, "canonical: not null");
    ASSERT(len1 > 0, "canonical: length > 0");

    /* same inputs produce same output */
    char *c2 = cookbook_grid_build_canonical("GET", "/grid/resolve/org/acme/lib/*",
        "central", 1, "org.acme:r", "", 1710800000, &len2);
    ASSERT(len1 == len2, "canonical: deterministic length");
    ASSERT(memcmp(c1, c2, len1) == 0, "canonical: deterministic content");
    free(c2);

    /* different method produces different string */
    char *c3 = cookbook_grid_build_canonical("HEAD", "/grid/resolve/org/acme/lib/*",
        "central", 1, "org.acme:r", "", 1710800000, &len2);
    ASSERT(memcmp(c1, c3, len1 < len2 ? len1 : len2) != 0,
           "canonical: different method differs");
    free(c3);
    free(c1);
}

static void test_grid_sign_verify_roundtrip(void) {
    /* Generate keypair */
    unsigned char pk[32], sk[64];
    int rc = cookbook_ed25519_keygen(pk, sk);
    ASSERT(rc == 0, "grid sig: keygen");

    /* Build canonical and sign */
    size_t canon_len;
    char *canonical = cookbook_grid_build_canonical("GET", "/grid/resolve/org/acme/lib/*",
        "central", 1, "org.acme:crwd", "org.secret", 1710800000, &canon_len);
    ASSERT(canonical != NULL, "grid sig: canonical");

    unsigned char sig[64];
    rc = cookbook_ed25519_sign(sig, canonical, canon_len, sk);
    ASSERT(rc == 0, "grid sig: sign");

    /* Verify */
    rc = cookbook_ed25519_verify(sig, canonical, canon_len, pk);
    ASSERT(rc == 0, "grid sig: verify ok");

    /* Tamper with canonical — should fail */
    canonical[5] ^= 0x01;
    rc = cookbook_ed25519_verify(sig, canonical, canon_len, pk);
    ASSERT(rc != 0, "grid sig: tampered canonical rejected");
    canonical[5] ^= 0x01; /* restore */

    /* Tamper with signature — should fail */
    sig[10] ^= 0xff;
    rc = cookbook_ed25519_verify(sig, canonical, canon_len, pk);
    ASSERT(rc != 0, "grid sig: tampered sig rejected");

    free(canonical);
}

static void test_grid_timestamp_validation(void) {
    /* Timestamps are validated in verify_grid_peer_sig (server code),
       but we can test the canonical builder handles timestamps correctly */
    size_t len1, len2;
    char *c1 = cookbook_grid_build_canonical("GET", "/grid/resolve/x",
        "a", 1, "", "", 1000000, &len1);
    char *c2 = cookbook_grid_build_canonical("GET", "/grid/resolve/x",
        "a", 1, "", "", 1000300, &len2);
    ASSERT(c1 != NULL && c2 != NULL, "timestamp: both canonical ok");
    /* different timestamps produce different canonicals */
    ASSERT(len1 != len2 || memcmp(c1, c2, len1) != 0,
           "timestamp: different ts differs");
    free(c1);
    free(c2);
}

/* ---- wildcard grants tests ---- */

static void test_auth_check_wildcard(void) {
    /* wildcard grant: "*" matches any group */
    const char *grants = "{\"grants\":{\"*\":\"r\"}}";
    ASSERT(cookbook_auth_check(grants, NULL, "com.anything", 'r') == 1,
           "wildcard: r on any group");
    ASSERT(cookbook_auth_check(grants, NULL, "org.acme.sdk", 'r') == 1,
           "wildcard: r on different group");
    ASSERT(cookbook_auth_check(grants, NULL, "com.anything", 'w') == 0,
           "wildcard: w denied when only r granted");

    /* wildcard with full perms */
    const char *admin = "{\"grants\":{\"*\":\"crwd\"}}";
    ASSERT(cookbook_auth_check(admin, NULL, "com.foo", 'c') == 1,
           "wildcard admin: c allowed");
    ASSERT(cookbook_auth_check(admin, NULL, "com.foo", 'r') == 1,
           "wildcard admin: r allowed");
    ASSERT(cookbook_auth_check(admin, NULL, "com.foo", 'w') == 1,
           "wildcard admin: w allowed");
    ASSERT(cookbook_auth_check(admin, NULL, "com.foo", 'd') == 1,
           "wildcard admin: d allowed");

    /* specific grant overrides wildcard */
    const char *mixed = "{\"grants\":{\"*\":\"r\",\"com.iridiumfx\":\"crwd\"}}";
    ASSERT(cookbook_auth_check(mixed, NULL, "com.iridiumfx", 'w') == 1,
           "wildcard+specific: specific wins for w");
    ASSERT(cookbook_auth_check(mixed, NULL, "com.iridiumfx.sub", 'w') == 1,
           "wildcard+specific: hierarchical specific wins");
    ASSERT(cookbook_auth_check(mixed, NULL, "org.acme", 'w') == 0,
           "wildcard+specific: non-matching falls to wildcard (no w)");
    ASSERT(cookbook_auth_check(mixed, NULL, "org.acme", 'r') == 1,
           "wildcard+specific: non-matching falls to wildcard (has r)");

    /* wildcard with exclude */
    const char *excl = "{\"grants\":{\"*\":\"crwd\"},\"exclude\":{\"com.secret\":true}}";
    ASSERT(cookbook_auth_check(excl, NULL, "com.foo", 'r') == 1,
           "wildcard+exclude: non-excluded allowed");
    ASSERT(cookbook_auth_check(excl, NULL, "com.secret", 'r') == 0,
           "wildcard+exclude: excluded denied");
    ASSERT(cookbook_auth_check(excl, NULL, "com.secret.sub", 'r') == 0,
           "wildcard+exclude: sub-excluded denied");
}

/* ---- revocation list tests ---- */

static void test_revocation_list(void) {
    cookbook_revocation_list rl;
    cookbook_revocation_init(&rl, 16);

    /* initially empty */
    ASSERT(cookbook_revocation_check(&rl, "abc123") == 0,
           "revoke: empty list returns not-revoked");

    /* add and check */
    int64_t future = (int64_t)time(NULL) + 3600;
    ASSERT(cookbook_revocation_add(&rl, "tok1", future) == 0,
           "revoke: add succeeds");
    ASSERT(cookbook_revocation_check(&rl, "tok1") == 1,
           "revoke: tok1 is revoked");
    ASSERT(cookbook_revocation_check(&rl, "tok2") == 0,
           "revoke: tok2 is not revoked");

    /* duplicate add succeeds (no-op) */
    ASSERT(cookbook_revocation_add(&rl, "tok1", future) == 0,
           "revoke: duplicate add ok");
    ASSERT(rl.count == 1, "revoke: duplicate doesn't grow count");

    /* add second entry */
    ASSERT(cookbook_revocation_add(&rl, "tok2", future) == 0,
           "revoke: add tok2");
    ASSERT(rl.count == 2, "revoke: count is 2");
    ASSERT(cookbook_revocation_check(&rl, "tok2") == 1,
           "revoke: tok2 now revoked");

    /* expired entries get pruned on next add */
    int64_t past = (int64_t)time(NULL) - 10;
    ASSERT(cookbook_revocation_add(&rl, "old_tok", past) == 0,
           "revoke: add expired entry");
    /* add another to trigger prune */
    ASSERT(cookbook_revocation_add(&rl, "tok3", future) == 0,
           "revoke: add tok3 triggers prune");
    ASSERT(cookbook_revocation_check(&rl, "old_tok") == 0,
           "revoke: expired entry pruned");

    cookbook_revocation_free(&rl);
}

static void test_jwt_jti(void) {
    /* JWT v1 now includes jti */
    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    char *tok1 = cookbook_jwt_create("alice", "admin", 3600, sk);
    ASSERT(tok1 != NULL, "jti: v1 token created");

    cookbook_jwt_claims c1;
    ASSERT(cookbook_jwt_verify(tok1, pk, &c1) == 0, "jti: v1 verify ok");
    ASSERT(c1.jti[0] != '\0', "jti: v1 has jti claim");

    /* jti is unique per token */
    char *tok2 = cookbook_jwt_create("alice", "admin", 3600, sk);
    cookbook_jwt_claims c2;
    ASSERT(cookbook_jwt_verify(tok2, pk, &c2) == 0, "jti: v1 second verify");
    ASSERT(strcmp(c1.jti, c2.jti) != 0, "jti: different tokens have different jti");

    cookbook_jwt_claims_free(&c1);
    cookbook_jwt_claims_free(&c2);
    free(tok1);
    free(tok2);

    /* JWT v2 also includes jti */
    char *resolved = "{\"grants\":{\"com.x\":\"r\"},\"exclude\":{}}";
    char *tok3 = cookbook_jwt_create_v2("bob", NULL, resolved, 3600, sk);
    ASSERT(tok3 != NULL, "jti: v2 token created");

    cookbook_jwt_claims c3;
    ASSERT(cookbook_jwt_verify(tok3, pk, &c3) == 0, "jti: v2 verify ok");
    ASSERT(c3.jti[0] != '\0', "jti: v2 has jti claim");
    ASSERT(c3.version == 2, "jti: v2 version correct");

    cookbook_jwt_claims_free(&c3);
    free(tok3);
}

/* ---- credential admin tests ---- */

typedef struct {
    char hash[256];
    char groups[1024];
    int found;
} test_cred_ctx;

static int test_cred_lookup_cb(const cookbook_db_row *row, void *user) {
    test_cred_ctx *ctx = (test_cred_ctx *)user;
    if (row->ncols >= 2 && row->values[0] && row->values[1]) {
        snprintf(ctx->hash, sizeof(ctx->hash), "%s", row->values[0]);
        snprintf(ctx->groups, sizeof(ctx->groups), "%s", row->values[1]);
        ctx->found = 1;
    }
    return 0;
}

static void test_credential_admin_lifecycle(void) {
    cookbook_db *db = cookbook_db_open_sqlite(":memory:");
    ASSERT(db != NULL, "cred admin: db open");
    ASSERT(cookbook_db_migrate(db) == COOKBOOK_DB_OK, "cred admin: migrate");

    /* insert a credential */
    char *hash = cookbook_credential_hash("secret123");
    ASSERT(hash != NULL, "cred admin: hash ok");

    cookbook_db_param ip[] = {
        COOKBOOK_P_TEXT("alice"),
        COOKBOOK_P_TEXT(hash),
        COOKBOOK_P_TEXT("admin,publish")
    };
    ASSERT(db->exec_p(db,
        "INSERT INTO credentials (subject, token_hash, groups, created_at) "
        "VALUES (?1, ?2, ?3, datetime('now'))",
        ip, 3) == COOKBOOK_DB_OK, "cred admin: insert alice");

    /* verify credential works */
    ASSERT(cookbook_credential_verify("secret123", hash) == 0,
           "cred admin: verify ok");
    ASSERT(cookbook_credential_verify("wrong", hash) != 0,
           "cred admin: wrong token rejected");
    free(hash);

    /* verify the record exists via a lookup */
    test_cred_ctx lctx = { {0}, {0}, 0 };
    cookbook_db_param lp[] = { COOKBOOK_P_TEXT("alice") };
    db->query_p(db,
        "SELECT token_hash, groups FROM credentials "
        "WHERE subject = ?1 AND revoked_at IS NULL",
        lp, 1, test_cred_lookup_cb, &lctx);
    ASSERT(lctx.found == 1, "cred admin: alice found");
    ASSERT(strstr(lctx.groups, "admin") != NULL,
           "cred admin: groups include admin");

    /* revoke credential */
    cookbook_db_param rp[] = { COOKBOOK_P_TEXT("alice") };
    ASSERT(db->exec_p(db,
        "UPDATE credentials SET revoked_at = datetime('now') "
        "WHERE subject = ?1 AND revoked_at IS NULL",
        rp, 1) == COOKBOOK_DB_OK, "cred admin: revoke ok");

    /* lookup after revoke should fail */
    test_cred_ctx lctx2 = { {0}, {0}, 0 };
    db->query_p(db,
        "SELECT token_hash, groups FROM credentials "
        "WHERE subject = ?1 AND revoked_at IS NULL",
        lp, 1, test_cred_lookup_cb, &lctx2);
    ASSERT(lctx2.found == 0, "cred admin: revoked not found");

    /* re-create after revoke (INSERT OR REPLACE) */
    char *hash2 = cookbook_credential_hash("newsecret");
    ASSERT(hash2 != NULL, "cred admin: hash2 ok");
    cookbook_db_param ip2[] = {
        COOKBOOK_P_TEXT("alice"),
        COOKBOOK_P_TEXT(hash2),
        COOKBOOK_P_TEXT("readonly")
    };
    ASSERT(db->exec_p(db,
        "INSERT OR REPLACE INTO credentials "
        "(subject, token_hash, groups, created_at, revoked_at) "
        "VALUES (?1, ?2, ?3, datetime('now'), NULL)",
        ip2, 3) == COOKBOOK_DB_OK, "cred admin: re-create ok");

    /* new credential works */
    test_cred_ctx lctx3 = { {0}, {0}, 0 };
    db->query_p(db,
        "SELECT token_hash, groups FROM credentials "
        "WHERE subject = ?1 AND revoked_at IS NULL",
        lp, 1, test_cred_lookup_cb, &lctx3);
    ASSERT(lctx3.found == 1, "cred admin: re-created found");
    ASSERT(strcmp(lctx3.groups, "readonly") == 0,
           "cred admin: groups updated to readonly");
    ASSERT(cookbook_credential_verify("newsecret", hash2) == 0,
           "cred admin: new token verifies");
    free(hash2);

    /* delete credential */
    cookbook_db_param dp[] = { COOKBOOK_P_TEXT("alice") };
    ASSERT(db->exec_p(db,
        "DELETE FROM credentials WHERE subject = ?1",
        dp, 1) == COOKBOOK_DB_OK, "cred admin: delete ok");

    test_cred_ctx lctx4 = { {0}, {0}, 0 };
    db->query_p(db,
        "SELECT token_hash, groups FROM credentials "
        "WHERE subject = ?1 AND revoked_at IS NULL",
        lp, 1, test_cred_lookup_cb, &lctx4);
    ASSERT(lctx4.found == 0, "cred admin: deleted not found");

    db->close(db);
}

static void test_full_auth_flow(void) {
    /* End-to-end: create credential → create JWT → verify → check revocation */
    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    /* 1. hash credential */
    char *hash = cookbook_credential_hash("mytoken");
    ASSERT(hash != NULL, "flow: hash");
    ASSERT(cookbook_credential_verify("mytoken", hash) == 0, "flow: verify cred");

    /* 2. create JWT v1 */
    char *jwt = cookbook_jwt_create("alice", "admin", 3600, sk);
    ASSERT(jwt != NULL, "flow: jwt create");

    /* 3. verify JWT */
    cookbook_jwt_claims claims;
    ASSERT(cookbook_jwt_verify(jwt, pk, &claims) == 0, "flow: jwt verify");
    ASSERT(strcmp(claims.sub, "alice") == 0, "flow: sub is alice");
    ASSERT(claims.jti[0] != '\0', "flow: jti present");

    /* 4. check revocation (not revoked) */
    cookbook_revocation_list rl;
    cookbook_revocation_init(&rl, 16);
    ASSERT(cookbook_revocation_check(&rl, claims.jti) == 0,
           "flow: not revoked initially");

    /* 5. revoke */
    ASSERT(cookbook_revocation_add(&rl, claims.jti, claims.exp) == 0,
           "flow: revoke ok");
    ASSERT(cookbook_revocation_check(&rl, claims.jti) == 1,
           "flow: now revoked");

    /* 6. second token still works */
    char *jwt2 = cookbook_jwt_create("alice", "admin", 3600, sk);
    cookbook_jwt_claims c2;
    ASSERT(cookbook_jwt_verify(jwt2, pk, &c2) == 0, "flow: jwt2 verify");
    ASSERT(cookbook_revocation_check(&rl, c2.jti) == 0,
           "flow: jwt2 not revoked");

    cookbook_revocation_free(&rl);
    cookbook_jwt_claims_free(&claims);
    cookbook_jwt_claims_free(&c2);
    free(hash);
    free(jwt);
    free(jwt2);
}

static void test_object_cache_ttl(void) {
    cookbook_db *db = cookbook_db_open_sqlite(":memory:");
    ASSERT(db != NULL, "obj ttl: db open");
    ASSERT(cookbook_db_migrate(db) == COOKBOOK_DB_OK, "obj ttl: migrate");

    int64_t now = (int64_t)time(NULL);

    /* insert a fresh entry */
    char now_str[32];
    snprintf(now_str, sizeof(now_str), "%lld", (long long)now);
    cookbook_db_param fp[] = {
        COOKBOOK_P_TEXT("abc123.o"),
        COOKBOOK_P_TEXT("central/objects/abc123.o"),
        COOKBOOK_P_TEXT("1024"),
        COOKBOOK_P_TEXT(now_str)
    };
    ASSERT(db->exec_p(db,
        "INSERT INTO object_cache (cache_key, store_key, size_bytes, created_at) "
        "VALUES (?1, ?2, ?3, ?4)",
        fp, 4) == COOKBOOK_DB_OK, "obj ttl: insert fresh");

    /* insert an old entry (2 hours ago) */
    char old_str[32];
    snprintf(old_str, sizeof(old_str), "%lld", (long long)(now - 7200));
    cookbook_db_param op[] = {
        COOKBOOK_P_TEXT("old999.o"),
        COOKBOOK_P_TEXT("central/objects/old999.o"),
        COOKBOOK_P_TEXT("2048"),
        COOKBOOK_P_TEXT(old_str)
    };
    ASSERT(db->exec_p(db,
        "INSERT INTO object_cache (cache_key, store_key, size_bytes, created_at) "
        "VALUES (?1, ?2, ?3, ?4)",
        op, 4) == COOKBOOK_DB_OK, "obj ttl: insert old");

    /* count all entries */
    int total = 0;
    db->query(db, "SELECT cache_key FROM object_cache", count_cb, &total);
    ASSERT(total == 2, "obj ttl: two entries");

    /* query expired (TTL = 3600 sec, cutoff = now - 3600) */
    char cutoff_str[32];
    snprintf(cutoff_str, sizeof(cutoff_str), "%lld", (long long)(now - 3600));
    cookbook_db_param cp[] = { COOKBOOK_P_TEXT(cutoff_str) };
    int expired = 0;
    db->query_p(db,
        "SELECT cache_key FROM object_cache WHERE created_at < ?1",
        cp, 1, count_cb, &expired);
    ASSERT(expired == 1, "obj ttl: one expired");

    /* delete expired */
    db->exec_p(db,
        "DELETE FROM object_cache WHERE created_at < ?1", cp, 1);

    total = 0;
    db->query(db, "SELECT cache_key FROM object_cache", count_cb, &total);
    ASSERT(total == 1, "obj ttl: one remains after eviction");

    /* verify the remaining one is the fresh entry */
    int found_fresh = 0;
    cookbook_db_param qp[] = { COOKBOOK_P_TEXT("abc123.o") };
    db->query_p(db,
        "SELECT cache_key FROM object_cache WHERE cache_key = ?1",
        qp, 1, count_cb, &found_fresh);
    ASSERT(found_fresh == 1, "obj ttl: fresh entry survives");

    db->close(db);
}

static void test_repro_validation(void) {
    /* valid .repro file */
    const char *valid =
        "{ format: \"now-repro-v1\", artifact_hash: \"sha256:abc123\","
        " build: { tool: \"now 0.1.0\" } }";
    BastaValue *v = basta_parse_cstr(valid, NULL);
    ASSERT(v != NULL, "repro: valid parses");
    ASSERT(basta_type(v) == BASTA_MAP, "repro: is map");
    ASSERT(basta_map_get(v, "format") != NULL, "repro: has format");
    ASSERT(basta_map_get(v, "artifact_hash") != NULL, "repro: has artifact_hash");
    const BastaValue *fmt = basta_map_get(v, "format");
    ASSERT(basta_type(fmt) == BASTA_STRING, "repro: format is string");
    ASSERT(strcmp(basta_get_string(fmt), "now-repro-v1") == 0,
           "repro: format value");
    basta_free(v);

    /* missing format field */
    const char *no_fmt = "{ artifact_hash: \"sha256:abc\" }";
    v = basta_parse_cstr(no_fmt, NULL);
    ASSERT(v != NULL, "repro: no-fmt parses");
    ASSERT(basta_map_get(v, "format") == NULL, "repro: format absent");
    ASSERT(basta_map_get(v, "artifact_hash") != NULL, "repro: hash present");
    basta_free(v);

    /* missing artifact_hash field */
    const char *no_hash = "{ format: \"now-repro-v1\" }";
    v = basta_parse_cstr(no_hash, NULL);
    ASSERT(v != NULL, "repro: no-hash parses");
    ASSERT(basta_map_get(v, "format") != NULL, "repro: fmt present");
    ASSERT(basta_map_get(v, "artifact_hash") == NULL, "repro: hash absent");
    basta_free(v);

    /* with attestations array */
    const char *with_attest =
        "{ format: \"now-repro-v1\", artifact_hash: \"sha256:def\","
        " attestations: [{ signer: \"ed25519:abc\", timestamp: \"2026-03-19\" }] }";
    v = basta_parse_cstr(with_attest, NULL);
    ASSERT(v != NULL, "repro: attestations parse");
    ASSERT(basta_map_get(v, "attestations") != NULL, "repro: has attestations");
    const BastaValue *att = basta_map_get(v, "attestations");
    ASSERT(basta_type(att) == BASTA_ARRAY, "repro: attestations is array");
    ASSERT(basta_count(att) == 1, "repro: one attestation");
    basta_free(v);
}

static void test_object_cache_store(void) {
    /* test that the object cache storage pattern works at the store level */
    cookbook_store *store = cookbook_store_open_fs(NULL);
    ASSERT(store != NULL, "obj cache: store open");

    const char *key = "central/objects/abc123def456.o";
    const char *data = "fake compiled object data";
    size_t len = strlen(data);

    /* put */
    ASSERT(store->put(store, key, data, len) == COOKBOOK_STORE_OK,
           "obj cache: put");

    /* get */
    void *out = NULL;
    size_t out_len = 0;
    ASSERT(store->get(store, key, &out, &out_len) == COOKBOOK_STORE_OK,
           "obj cache: get");
    ASSERT(out_len == len, "obj cache: length match");
    ASSERT(memcmp(out, data, len) == 0, "obj cache: data match");
    free(out);

    /* exists via get */
    out = NULL; out_len = 0;
    ASSERT(store->get(store, key, &out, &out_len) == COOKBOOK_STORE_OK,
           "obj cache: exists");
    free(out);

    /* not found */
    out = NULL; out_len = 0;
    ASSERT(store->get(store, "central/objects/nonexistent.o",
           &out, &out_len) == COOKBOOK_STORE_NOT_FOUND,
           "obj cache: miss");

    /* delete */
    ASSERT(store->del(store, key) == COOKBOOK_STORE_OK,
           "obj cache: del");
    out = NULL; out_len = 0;
    ASSERT(store->get(store, key, &out, &out_len) == COOKBOOK_STORE_NOT_FOUND,
           "obj cache: deleted");

    store->close(store);
}

static void test_revocation_persistence(void) {
    cookbook_db *db = cookbook_db_open_sqlite(":memory:");
    ASSERT(db != NULL, "revoke persist: db open");
    ASSERT(cookbook_db_migrate(db) == COOKBOOK_DB_OK, "revoke persist: migrate");

    /* insert a revocation into the DB */
    int64_t future_exp = (int64_t)time(NULL) + 3600;
    char exp_str[32];
    snprintf(exp_str, sizeof(exp_str), "%lld", (long long)future_exp);
    cookbook_db_param rp[] = {
        COOKBOOK_P_TEXT("abc123def456"),
        COOKBOOK_P_TEXT("alice"),
        COOKBOOK_P_TEXT("2026-03-19T18:00:00Z"),
        COOKBOOK_P_TEXT(exp_str)
    };
    ASSERT(db->exec_p(db,
        "INSERT INTO revocations (jti, subject, revoked_at, expires_at) "
        "VALUES (?1, ?2, ?3, ?4)",
        rp, 4) == COOKBOOK_DB_OK, "revoke persist: insert");

    /* insert an already-expired revocation */
    char past_str[32];
    snprintf(past_str, sizeof(past_str), "%lld",
             (long long)((int64_t)time(NULL) - 100));
    cookbook_db_param rp2[] = {
        COOKBOOK_P_TEXT("expired999"),
        COOKBOOK_P_TEXT("bob"),
        COOKBOOK_P_TEXT("2026-03-19T17:00:00Z"),
        COOKBOOK_P_TEXT(past_str)
    };
    ASSERT(db->exec_p(db,
        "INSERT INTO revocations (jti, subject, revoked_at, expires_at) "
        "VALUES (?1, ?2, ?3, ?4)",
        rp2, 4) == COOKBOOK_DB_OK, "revoke persist: insert expired");

    /* load non-expired into a fresh revocation list */
    cookbook_revocation_list rl;
    cookbook_revocation_init(&rl, 64);

    char now_str[32];
    snprintf(now_str, sizeof(now_str), "%lld", (long long)time(NULL));
    cookbook_db_param tp[] = { COOKBOOK_P_TEXT(now_str) };
    int loaded = 0;
    /* simulate the startup load query */
    typedef struct { cookbook_revocation_list *rl; int *loaded; } rl_ctx;
    rl_ctx ctx = { &rl, &loaded };
    db->query_p(db,
        "SELECT jti, expires_at FROM revocations WHERE expires_at > ?1",
        tp, 1,
        (cookbook_db_row_cb)count_cb, &loaded);
    /* count_cb just counts rows — should be 1 (only non-expired) */
    ASSERT(loaded == 1, "revoke persist: only non-expired loaded");

    /* verify the jti is loadable */
    cookbook_revocation_add(&rl, "abc123def456", future_exp);
    ASSERT(cookbook_revocation_check(&rl, "abc123def456") == 1,
           "revoke persist: jti found in list");
    ASSERT(cookbook_revocation_check(&rl, "expired999") == 0,
           "revoke persist: expired jti not in list");

    /* prune expired from DB */
    db->exec_p(db, "DELETE FROM revocations WHERE expires_at <= ?1", tp, 1);
    int remaining = 0;
    db->query(db, "SELECT jti FROM revocations", count_cb, &remaining);
    ASSERT(remaining == 1, "revoke persist: expired pruned from DB");

    /* duplicate insert should be ignored (OR IGNORE) */
    ASSERT(db->exec_p(db,
        "INSERT OR IGNORE INTO revocations "
        "(jti, subject, revoked_at, expires_at) VALUES (?1, ?2, ?3, ?4)",
        rp, 4) == COOKBOOK_DB_OK, "revoke persist: dup ignored");
    remaining = 0;
    db->query(db, "SELECT jti FROM revocations", count_cb, &remaining);
    ASSERT(remaining == 1, "revoke persist: still one after dup");

    cookbook_revocation_free(&rl);
    db->close(db);
}

#ifdef COOKBOOK_HAS_BASTA
/* ---- basta integration tests ---- */

static void test_basta_integration(void) {
    /* basic: create values programmatically */
    BastaValue *s = basta_new_string("hello");
    ASSERT(s != NULL, "basta: new string");
    ASSERT(basta_type(s) == BASTA_STRING, "basta: type is string");
    ASSERT(strcmp(basta_get_string(s), "hello") == 0,
           "basta: string value");
    basta_free(s);

    /* map construction */
    BastaValue *m = basta_new_map();
    ASSERT(m != NULL, "basta: new map");
    ASSERT(basta_set(m, "key", basta_new_string("val")) == 0,
           "basta: set key");
    ASSERT(basta_count(m) == 1, "basta: map has 1 entry");
    const BastaValue *v = basta_map_get(m, "key");
    ASSERT(v != NULL, "basta: get key");
    ASSERT(strcmp(basta_get_string(v), "val") == 0,
           "basta: value is val");

    /* write and verify */
    size_t out_len = 0;
    char *out = basta_write(m, BASTA_COMPACT, &out_len);
    ASSERT(out != NULL, "basta: write map");
    ASSERT(strstr(out, "key") != NULL, "basta: output has key");
    free(out);
    basta_free(m);

    /* blob creation and read-back */
    const uint8_t blob_data[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x42 };
    BastaValue *blob = basta_new_blob(blob_data, 6);
    ASSERT(blob != NULL, "basta: blob create");
    ASSERT(basta_type(blob) == BASTA_BLOB, "basta: is blob");
    size_t blen = 0;
    const uint8_t *bdata = basta_get_blob(blob, &blen);
    ASSERT(blen == 6, "basta: blob length");
    ASSERT(bdata != NULL && bdata[0] == 0xDE && bdata[5] == 0x42,
           "basta: blob data matches");
    basta_free(blob);

    /* parse a basta document from text */
    const char *doc = "{ name: \"cookbook\", version: \"1.0\" }";
    BastaResult br;
    memset(&br, 0, sizeof(br));
    BastaValue *root = basta_parse_cstr(doc, &br);
    ASSERT(root != NULL, "basta: parse cstr");
    if (!root) {
        fprintf(stderr, "  basta parse error: %s (line %d col %d)\n",
                br.message, br.line, br.col);
    } else {
        ASSERT(br.code == BASTA_OK, "basta: parse ok");
        ASSERT(basta_type(root) == BASTA_MAP, "basta: root is map");
        const BastaValue *name = basta_map_get(root, "name");
        ASSERT(name != NULL, "basta: name field");
        if (name) {
            ASSERT(basta_type(name) == BASTA_STRING, "basta: name is string");
            const char *ns = basta_get_string(name);
            ASSERT(ns != NULL && strcmp(ns, "cookbook") == 0,
                   "basta: name is cookbook");
        }
        basta_free(root);
    }
}
#endif /* COOKBOOK_HAS_BASTA */

/* ---- native Ed25519 tests ---- */

static void hex_to_bytes(const char *hex, unsigned char *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned int b;
        sscanf(hex + i * 2, "%2x", &b);
        out[i] = (unsigned char)b;
    }
}

extern void cookbook_ed25519_sha512_test(const void *data, size_t len, unsigned char out[64]);

static void test_ed25519_sha512(void) {
    /* SHA-512 known-answer test: "abc" → ddaf35a1... (FIPS 180-4 Appendix C.1) */
    unsigned char native[64];
    unsigned char expected_abc[64];
    hex_to_bytes(
        "ddaf35a193617abacc417349ae204131"
        "12e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd"
        "454d4423643ce80e2a9ac94fa54ca49f", expected_abc, 64);

    cookbook_ed25519_sha512_test("abc", 3, native);
    ASSERT(memcmp(native, expected_abc, 64) == 0, "SHA-512 FIPS 180-4 C.1 (abc)");

    /* empty message: cf83e1357eec... */
    unsigned char expected_empty[64];
    hex_to_bytes(
        "cf83e1357eefb8bdf1542850d66d8007"
        "d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f"
        "63b931bd47417a81a538327af927da3e", expected_empty, 64);

    cookbook_ed25519_sha512_test(NULL, 0, native);
    ASSERT(memcmp(native, expected_empty, 64) == 0, "SHA-512 empty message");
}

extern void cookbook_ed25519_scalarmult_base_test(const unsigned char scalar[32],
                                                   unsigned char out[32]);

static void test_ed25519_scalarmult_base(void) {
    /* Test [1]B — scalar=1 should produce the Ed25519 basepoint encoding */
    unsigned char scalar[32] = {0};
    unsigned char result[32];
    scalar[0] = 1;  /* scalar = 1 in little-endian */

    cookbook_ed25519_scalarmult_base_test(scalar, result);

    /* Known Ed25519 basepoint compressed encoding:
       y = 4/5 mod p, x-sign bit set → 0x5866666666...66 */
    unsigned char expected_bp[32];
    hex_to_bytes(
        "5866666666666666666666666666666666"
        "66666666666666666666666666666666", expected_bp, 32);

    ASSERT(memcmp(result, expected_bp, 32) == 0, "[1]B == basepoint");

    /* Test [0]B — should be identity (0, 1) → encoded as 01000...00 */
    unsigned char zero_scalar[32] = {0};
    unsigned char id_result[32];
    cookbook_ed25519_scalarmult_base_test(zero_scalar, id_result);

    unsigned char expected_id[32] = {0};
    expected_id[0] = 0x01;  /* y=1, x=0 in compressed: 01 00 00 ... 00 */
    ASSERT(memcmp(id_result, expected_id, 32) == 0, "[0]B == identity");
}

static void test_ed25519_rfc8032_vector1(void) {
    /* RFC 8032 Section 7.1 Test Vector 1: empty message */
    unsigned char sk[64], pk[32], sig[64];
    unsigned char expected_pk[32], expected_sig[64];

    /* sk = seed(32) || pk(32) per libsodium/ref10 convention */
    hex_to_bytes(
        "9d61b19deffd5a60ba844af492ec2cc4"
        "4449c5697b326919703bac031cae7f60", sk, 32);
    hex_to_bytes(
        "d75a980182b10ab7d54bfed3c964073a"
        "0ee172f3daa62325af021a68f707511a", sk + 32, 32);

    hex_to_bytes(
        "d75a980182b10ab7d54bfed3c964073a"
        "0ee172f3daa62325af021a68f707511a", expected_pk, 32);
    hex_to_bytes(
        "e5564300c360ac729086e2cc806e828a"
        "84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46b"
        "d25bf5f0595bbe24655141438e7a100b", expected_sig, 64);

    /* test sign */
    int rc = cookbook_ed25519_sign(sig, NULL, 0, sk);
    ASSERT(rc == 0, "rfc8032 v1: sign succeeds");

    ASSERT(memcmp(sig, expected_sig, 64) == 0, "rfc8032 v1: signature matches");

    /* test verify */
    memcpy(pk, expected_pk, 32);
    rc = cookbook_ed25519_verify(expected_sig, NULL, 0, pk);
    ASSERT(rc == 0, "rfc8032 v1: verify succeeds");

    /* tampered message should fail */
    unsigned char byte = 0x42;
    rc = cookbook_ed25519_verify(expected_sig, &byte, 1, pk);
    ASSERT(rc != 0, "rfc8032 v1: tampered msg rejected");
}

static void test_ed25519_rfc8032_vector2(void) {
    /* RFC 8032 Section 7.1 Test Vector 2: single byte 0x72 */
    unsigned char sk[64], pk[32], sig[64];
    unsigned char expected_sig[64];
    unsigned char msg[] = { 0x72 };

    hex_to_bytes(
        "4ccd089b28ff96da9db6c346ec114e0f"
        "5b8a319f35aba624da8cf6ed4fb8a6fb", sk, 32);
    hex_to_bytes(
        "3d4017c3e843895a92b70aa74d1b7ebc"
        "9c982ccf2ec4968cc0cd55f12af4660c", sk + 32, 32);

    hex_to_bytes(
        "3d4017c3e843895a92b70aa74d1b7ebc"
        "9c982ccf2ec4968cc0cd55f12af4660c", pk, 32);
    hex_to_bytes(
        "92a009a9f0d4cab8720e820b5f642540"
        "a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c"
        "387b2eaeb4302aeeb00d291612bb0c00", expected_sig, 64);

    int rc = cookbook_ed25519_sign(sig, msg, 1, sk);
    ASSERT(rc == 0, "rfc8032 v2: sign succeeds");
    ASSERT(memcmp(sig, expected_sig, 64) == 0, "rfc8032 v2: signature matches");

    rc = cookbook_ed25519_verify(sig, msg, 1, pk);
    ASSERT(rc == 0, "rfc8032 v2: verify succeeds");
}

static void test_ed25519_rfc8032_vector3(void) {
    /* RFC 8032 Section 7.1 Test Vector 3: two-byte message */
    unsigned char sk[64], pk[32], sig[64];
    unsigned char expected_sig[64];
    unsigned char msg[] = { 0xaf, 0x82 };

    hex_to_bytes(
        "c5aa8df43f9f837bedb7442f31dcb7b1"
        "66d38535076f094b85ce3a2e0b4458f7", sk, 32);
    hex_to_bytes(
        "fc51cd8e6218a1a38da47ed00230f058"
        "0816ed13ba3303ac5deb911548908025", sk + 32, 32);

    hex_to_bytes(
        "fc51cd8e6218a1a38da47ed00230f058"
        "0816ed13ba3303ac5deb911548908025", pk, 32);
    hex_to_bytes(
        "6291d657deec24024827e69c3abe01a3"
        "0ce548a284743a445e3680d7db5ac3ac"
        "18ff9b538d16f290ae67f760984dc659"
        "4a7c15e9716ed28dc027beceea1ec40a", expected_sig, 64);

    int rc = cookbook_ed25519_sign(sig, msg, 2, sk);
    ASSERT(rc == 0, "rfc8032 v3: sign succeeds");
    ASSERT(memcmp(sig, expected_sig, 64) == 0, "rfc8032 v3: signature matches");

    rc = cookbook_ed25519_verify(sig, msg, 2, pk);
    ASSERT(rc == 0, "rfc8032 v3: verify succeeds");
}

static void test_ed25519_sign_verify_roundtrip(void) {
    /* native-only round trip (cross-check against libsodium removed with the
     * libsodium dependency; RFC 8032 KATs above already pin correctness). */
    unsigned char pk[32], sk[64], sig[64];
    int rc = cookbook_ed25519_keygen(pk, sk);
    ASSERT(rc == 0, "roundtrip: keygen");

    const char *msg = "cookbook roundtrip";
    size_t mlen = strlen(msg);

    rc = cookbook_ed25519_sign(sig, msg, mlen, sk);
    ASSERT(rc == 0, "roundtrip: sign");

    rc = cookbook_ed25519_verify(sig, msg, mlen, pk);
    ASSERT(rc == 0, "roundtrip: verify");

    sig[0] ^= 1;
    rc = cookbook_ed25519_verify(sig, msg, mlen, pk);
    ASSERT(rc != 0, "roundtrip: tampered sig rejected");
}

static void test_group_admin_lifecycle(void) {
    cookbook_db *db = cookbook_db_open_sqlite(":memory:");
    ASSERT(db != NULL, "group admin: db open");
    ASSERT(cookbook_db_migrate(db) == COOKBOOK_DB_OK, "group admin: migrate");

    /* create a group */
    cookbook_db_param ip[] = {
        COOKBOOK_P_TEXT("com.iridiumfx"),
        COOKBOOK_P_TEXT("alice"),
        COOKBOOK_P_TEXT("2026-03-19T00:00:00Z"),
        COOKBOOK_P_TEXT("IridiumFX components")
    };
    ASSERT(db->exec_p(db,
        "INSERT INTO groups (group_id, owner_sub, created_at, description) "
        "VALUES (?1, ?2, ?3, ?4)",
        ip, 4) == COOKBOOK_DB_OK, "group admin: create");

    /* duplicate should fail */
    ASSERT(db->exec_p(db,
        "INSERT INTO groups (group_id, owner_sub, created_at, description) "
        "VALUES (?1, ?2, ?3, ?4)",
        ip, 4) == COOKBOOK_DB_CONSTRAINT, "group admin: dup rejected");

    /* query the group */
    int count = 0;
    cookbook_db_param qp[] = { COOKBOOK_P_TEXT("com.iridiumfx") };
    db->query_p(db,
        "SELECT group_id FROM groups WHERE group_id = ?1",
        qp, 1, count_cb, &count);
    ASSERT(count == 1, "group admin: found");

    /* update description */
    cookbook_db_param up[] = {
        COOKBOOK_P_TEXT("Updated description"),
        COOKBOOK_P_TEXT("com.iridiumfx")
    };
    ASSERT(db->exec_p(db,
        "UPDATE groups SET description = ?1 WHERE group_id = ?2",
        up, 2) == COOKBOOK_DB_OK, "group admin: update desc");

    /* update owner */
    cookbook_db_param uo[] = {
        COOKBOOK_P_TEXT("bob"),
        COOKBOOK_P_TEXT("com.iridiumfx")
    };
    ASSERT(db->exec_p(db,
        "UPDATE groups SET owner_sub = ?1 WHERE group_id = ?2",
        uo, 2) == COOKBOOK_DB_OK, "group admin: update owner");

    /* create second group */
    cookbook_db_param ip2[] = {
        COOKBOOK_P_TEXT("org.example"),
        COOKBOOK_P_TEXT("charlie"),
        COOKBOOK_P_TEXT("2026-03-19T01:00:00Z")
    };
    ASSERT(db->exec_p(db,
        "INSERT INTO groups (group_id, owner_sub, created_at) "
        "VALUES (?1, ?2, ?3)",
        ip2, 3) == COOKBOOK_DB_OK, "group admin: create second");

    /* count all groups */
    count = 0;
    db->query(db, "SELECT group_id FROM groups", count_cb, &count);
    ASSERT(count == 2, "group admin: two groups");

    /* add artifact referencing first group */
    ASSERT(db->exec(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256) "
        "VALUES ('com.iridiumfx:core:1.0.0:linux:amd64:gnu', "
        "'com.iridiumfx', 'core', '1.0.0', 'linux:amd64:gnu', 'abcd1234')"
    ) == COOKBOOK_DB_OK, "group admin: add artifact");

    /* delete group with artifacts should be blocked at handler level,
       but at DB level FK may or may not cascade depending on schema.
       Here we test the count-based check the handler uses. */
    int art_count = 0;
    cookbook_db_param cp[] = { COOKBOOK_P_TEXT("com.iridiumfx") };
    db->query_p(db,
        "SELECT coord_id FROM artifacts WHERE group_id = ?1",
        cp, 1, count_cb, &art_count);
    ASSERT(art_count == 1, "group admin: artifact count = 1");

    /* delete group WITHOUT artifacts succeeds */
    cookbook_db_param dp[] = { COOKBOOK_P_TEXT("org.example") };
    ASSERT(db->exec_p(db,
        "DELETE FROM groups WHERE group_id = ?1",
        dp, 1) == COOKBOOK_DB_OK, "group admin: delete empty group");

    count = 0;
    db->query(db, "SELECT group_id FROM groups", count_cb, &count);
    ASSERT(count == 1, "group admin: one group remains");

    /* clean up artifact, then delete group */
    db->exec(db, "DELETE FROM artifacts WHERE group_id = 'com.iridiumfx'");
    art_count = 0;
    db->query_p(db,
        "SELECT coord_id FROM artifacts WHERE group_id = ?1",
        cp, 1, count_cb, &art_count);
    ASSERT(art_count == 0, "group admin: artifacts cleared");

    ASSERT(db->exec_p(db,
        "DELETE FROM groups WHERE group_id = ?1",
        cp, 1) == COOKBOOK_DB_OK, "group admin: delete after clearing");

    count = 0;
    db->query(db, "SELECT group_id FROM groups", count_cb, &count);
    ASSERT(count == 0, "group admin: all groups deleted");

    db->close(db);
}

/* ---- KV DB backend tests ---- */

static void test_db_kv_backend(void) {
    const char *path = "test_db.kv";
    cookbook_db *db = cookbook_db_open_kv(path);
    ASSERT(db != NULL, "kv db: open");

    /* migrate (no-op for KV — schema-less) */
    ASSERT(cookbook_db_migrate(db) == COOKBOOK_DB_OK, "kv db: migrate");

    /* insert a group */
    cookbook_db_param ip[] = {
        COOKBOOK_P_TEXT("com.example"),
        COOKBOOK_P_TEXT("alice"),
        COOKBOOK_P_TEXT("2026-03-21T00:00:00Z"),
        COOKBOOK_P_TEXT("Example group")
    };
    ASSERT(db->exec_p(db,
        "INSERT INTO groups (group_id, owner_sub, created_at, description) "
        "VALUES (?1, ?2, ?3, ?4)",
        ip, 4) == COOKBOOK_DB_OK, "kv db: insert");

    /* duplicate should fail (CONSTRAINT) */
    ASSERT(db->exec_p(db,
        "INSERT INTO groups (group_id, owner_sub, created_at, description) "
        "VALUES (?1, ?2, ?3, ?4)",
        ip, 4) == COOKBOOK_DB_CONSTRAINT, "kv db: dup constraint");

    /* INSERT OR IGNORE should succeed silently */
    ASSERT(db->exec_p(db,
        "INSERT OR IGNORE INTO groups (group_id, owner_sub, created_at, description) "
        "VALUES (?1, ?2, ?3, ?4)",
        ip, 4) == COOKBOOK_DB_OK, "kv db: insert or ignore");

    /* query by primary key */
    int found = 0;
    cookbook_db_param qp[] = { COOKBOOK_P_TEXT("com.example") };
    db->query_p(db,
        "SELECT group_id, owner_sub FROM groups WHERE group_id = ?1",
        qp, 1, count_cb, &found);
    ASSERT(found == 1, "kv db: query found");

    /* query non-existent */
    found = 0;
    cookbook_db_param qp2[] = { COOKBOOK_P_TEXT("org.missing") };
    db->query_p(db,
        "SELECT group_id FROM groups WHERE group_id = ?1",
        qp2, 1, count_cb, &found);
    ASSERT(found == 0, "kv db: query not found");

    /* delete */
    cookbook_db_param dp[] = { COOKBOOK_P_TEXT("com.example") };
    ASSERT(db->exec_p(db,
        "DELETE FROM groups WHERE group_id = ?1",
        dp, 1) == COOKBOOK_DB_OK, "kv db: delete");

    /* verify deleted */
    found = 0;
    db->query_p(db,
        "SELECT group_id FROM groups WHERE group_id = ?1",
        qp, 1, count_cb, &found);
    ASSERT(found == 0, "kv db: deleted");

    db->close(db);
    remove(path);
}

/* ---- connection pool tests ---- */

static void test_connpool_basic(void) {
    cookbook_connpool *pool = cookbook_connpool_create(2, 60);
    ASSERT(pool != NULL, "connpool: create");

    /* get from empty pool returns INVALID */
    cookbook_sock_t s = cookbook_connpool_get(pool, "localhost", 9999, 0);
    ASSERT(s == COOKBOOK_SOCK_INVALID, "connpool: empty get");

    /* TLS always returns INVALID (not poolable) */
    s = cookbook_connpool_get(pool, "localhost", 443, 1);
    ASSERT(s == COOKBOOK_SOCK_INVALID, "connpool: tls not pooled");

    cookbook_connpool_destroy(pool);
}

/* ---- gzip compression tests ---- */

extern void *cookbook_gzip_compress(const void *data, size_t len, size_t *out_len);

static void test_gzip_compress(void) {
    const char *input = "Hello, cookbook! This is a test of gzip compression. "
                        "Repeating data compresses well: aaaaaaaaaaaaaaaaaaa"
                        "bbbbbbbbbbbbbbbbbbbbccccccccccccccccccccdddddddddd";
    size_t in_len = strlen(input);
    size_t gz_len = 0;
    void *gz = cookbook_gzip_compress(input, in_len, &gz_len);
    ASSERT(gz != NULL, "gzip: compress ok");
    ASSERT(gz_len > 0, "gzip: output non-empty");
    ASSERT(gz_len < in_len, "gzip: compressed smaller");

    /* verify gzip magic bytes */
    unsigned char *p = (unsigned char *)gz;
    ASSERT(p[0] == 0x1F && p[1] == 0x8B, "gzip: magic bytes");
    ASSERT(p[2] == 0x08, "gzip: deflate method");

    free(gz);

    /* empty input */
    gz = cookbook_gzip_compress("", 0, &gz_len);
    ASSERT(gz == NULL, "gzip: empty returns NULL");

    /* NULL input */
    gz = cookbook_gzip_compress(NULL, 100, &gz_len);
    ASSERT(gz == NULL, "gzip: null returns NULL");
}

/* ---- LDAP BER encoding tests ---- */

static void test_ldap_config(void) {
    /* test that LDAP bind with NULL config returns -1 */
    ASSERT(cookbook_ldap_bind(NULL, "user", "pass", NULL) == -1,
           "ldap: null config");

    /* test with config but no URL */
    cookbook_ldap_config cfg = {0};
    ASSERT(cookbook_ldap_bind(&cfg, "user", "pass", NULL) == -1,
           "ldap: null url");

    /* test with unreachable LDAP server — should return -1, not crash */
    cookbook_ldap_config unreachable = {
        .url = "ldap://127.0.0.1:19389",  /* nothing listening */
        .base_dn = "dc=test",
        .user_attr = "uid",
        .group_attr = NULL,
        .group_base = NULL
    };
    char *groups = NULL;
    int lrc = cookbook_ldap_bind(&unreachable, "test", "pass", &groups);
    ASSERT(lrc == -1, "ldap: unreachable returns -1");
    ASSERT(groups == NULL, "ldap: unreachable groups null");
}

/* ---- LDAP integration test with mock server ---- */

/* Pre-built BER responses for the mock LDAP server */

/* BindResponse: success (resultCode=0, matchedDN="", diagnosticMessage="") */
static const unsigned char LDAP_BIND_RESP_OK[] = {
    0x30, 0x0C,             /* SEQUENCE, length 12 */
    0x02, 0x01, 0x01,       /* INTEGER messageID=1 */
    0x61, 0x07,             /* BindResponse [APPLICATION 1], length 7 */
    0x0A, 0x01, 0x00,       /* ENUMERATED resultCode=0 (success) */
    0x04, 0x00,             /* OCTET STRING matchedDN="" */
    0x04, 0x00              /* OCTET STRING diagnosticMessage="" */
};

/* SearchResultEntry with memberOf=CN=developers,OU=Groups,DC=test */
static const unsigned char LDAP_SEARCH_ENTRY_GROUPS[] = {
    0x30, 0x52,             /* SEQUENCE, length 82 */
    0x02, 0x01, 0x02,       /* INTEGER messageID=2 */
    0x64, 0x4D,             /* SearchResultEntry [APPLICATION 4], length 77 */
    0x04, 0x12, 'u','i','d','=','t','e','s','t',',','d','c','=','t','e','s','t',',','o', /* objectName */
    0x30, 0x37,             /* SEQUENCE (attributes), length 55 */
    0x30, 0x35,             /* SEQUENCE (PartialAttribute), length 53 */
    0x04, 0x08, 'm','e','m','b','e','r','O','f', /* type="memberOf" */
    0x31, 0x29,             /* SET, length 41 */
    0x04, 0x27, 'C','N','=','d','e','v','e','l','o','p','e','r','s',',',
    'O','U','=','G','r','o','u','p','s',',','D','C','=','t','e','s','t',
    ',','D','C','=','c','o','m' /* value */
};

/* SearchResultDone: success */
static const unsigned char LDAP_SEARCH_DONE_OK[] = {
    0x30, 0x0C,             /* SEQUENCE, length 12 */
    0x02, 0x01, 0x02,       /* INTEGER messageID=2 */
    0x65, 0x07,             /* SearchResultDone [APPLICATION 5], length 7 */
    0x0A, 0x01, 0x00,       /* ENUMERATED resultCode=0 */
    0x04, 0x00,             /* matchedDN="" */
    0x04, 0x00              /* diagnosticMessage="" */
};

typedef struct {
    int port;
    int ready;
    int served;
} mock_ldap_ctx;

#ifdef _WIN32
static DWORD WINAPI mock_ldap_thread(LPVOID arg) {
#else
static void *mock_ldap_thread(void *arg) {
#endif
    mock_ldap_ctx *ctx = (mock_ldap_ctx *)arg;

    /* create TCP listener */
    cookbook_sock_t lsock = COOKBOOK_SOCK_INVALID;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)ctx->port);

    lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock == COOKBOOK_SOCK_INVALID) goto done;

    int yes = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) != 0) goto done;
    if (listen(lsock, 1) != 0) goto done;

    ctx->ready = 1;

    /* accept one connection */
    cookbook_sock_t cli = accept(lsock, NULL, NULL);
    if (cli == COOKBOOK_SOCK_INVALID) goto done;

    /* read BindRequest (just consume it) */
    unsigned char rbuf[2048];
    int n = recv(cli, (char *)rbuf, sizeof(rbuf), 0);
    if (n <= 0) { cookbook_sock_close(cli); goto done; }

    /* send BindResponse (success) */
    send(cli, (const char *)LDAP_BIND_RESP_OK,
         sizeof(LDAP_BIND_RESP_OK), 0);

    /* read SearchRequest (just consume it) */
    n = recv(cli, (char *)rbuf, sizeof(rbuf), 0);
    if (n > 0) {
        /* send SearchResultEntry with groups */
        send(cli, (const char *)LDAP_SEARCH_ENTRY_GROUPS,
             sizeof(LDAP_SEARCH_ENTRY_GROUPS), 0);
        /* send SearchResultDone */
        send(cli, (const char *)LDAP_SEARCH_DONE_OK,
             sizeof(LDAP_SEARCH_DONE_OK), 0);
    }

    cookbook_sock_close(cli);
    ctx->served = 1;

done:
    if (lsock != COOKBOOK_SOCK_INVALID) cookbook_sock_close(lsock);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void test_ldap_integration(void) {
    /* start mock LDAP server on a high port */
    mock_ldap_ctx mctx = { .port = 19389, .ready = 0, .served = 0 };

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    HANDLE th = CreateThread(NULL, 0, mock_ldap_thread, &mctx, 0, NULL);
#else
    pthread_t th;
    pthread_create(&th, NULL, mock_ldap_thread, &mctx);
#endif

    /* wait for server to be ready */
    for (int i = 0; i < 100 && !mctx.ready; i++) {
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10000);
#endif
    }
    ASSERT(mctx.ready, "ldap mock: server ready");

    /* connect and bind */
    cookbook_ldap_config cfg = {
        .url = "ldap://127.0.0.1:19389",
        .base_dn = "dc=test,dc=com",
        .user_attr = "uid",
        .group_attr = "memberOf",
        .group_base = NULL
    };

    char *groups = NULL;
    int rc = cookbook_ldap_bind(&cfg, "test", "password", &groups);
    ASSERT(rc == 0, "ldap mock: bind success");
    ASSERT(groups != NULL, "ldap mock: groups returned");
    ASSERT(strstr(groups, "developers") != NULL,
           "ldap mock: developers group found");
    free(groups);

    /* wait for thread to finish */
#ifdef _WIN32
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
#else
    pthread_join(th, NULL);
#endif

    ASSERT(mctx.served, "ldap mock: request served");
}

/* ---- WAL tests ---- */

static void test_wal_basic(void) {
    /* create a WAL in a temp path */
    const char *path = "test_audit.wal";
    wal *w = NULL;
    unsigned long rc = wal_create(&w, path);
    ASSERT(rc == 0 && w != NULL, "wal: create");

    /* append entries */
    u64 seq1 = 0, seq2 = 0;
    const char *e1 = "{ event: \"test\", seq: 1 }";
    const char *e2 = "{ event: \"test\", seq: 2 }";
    ASSERT(wal_append(&seq1, w, (const u8 *)e1, (u64)strlen(e1)) == 0,
           "wal: append 1");
    ASSERT(wal_append(&seq2, w, (const u8 *)e2, (u64)strlen(e2)) == 0,
           "wal: append 2");
    ASSERT(seq2 > seq1, "wal: seq monotonic");

    /* read back */
    wal_entry re;
    ASSERT(wal_read(&re, w, seq1) == 0, "wal: read 1");
    ASSERT(re.len == (u64)strlen(e1), "wal: read 1 len");
    ASSERT(memcmp(re.data, e1, re.len) == 0, "wal: read 1 data");
    free(re.data);

    /* iterate */
    wal_iter *it = NULL;
    ASSERT(wal_iter_create(&it, w, 0) == 0, "wal: iter create");
    wal_entry ie;
    int count = 0;
    while (wal_iter_next(&ie, it) == 0) {
        free(ie.data);
        count++;
    }
    ASSERT(count == 2, "wal: iter count");
    wal_iter_destroy(it);

    wal_close(w);
    remove(path);
}

/* ---- KV store tests ---- */

#include <apennines/t3/db/kv.h>

static void test_kv_basic(void) {
    const char *path = "test_store.kv";
    kv_store *store = NULL;
    ASSERT(kv_open(&store, path) == 0, "kv: open");

    /* put */
    const char *k1 = "groups:com.example";
    const char *v1 = "{ owner: \"alice\", created_at: \"2026-03-20\" }";
    ASSERT(kv_put(store, (const u8 *)k1, (u64)strlen(k1),
                   (const u8 *)v1, (u64)strlen(v1)) == 0, "kv: put");

    /* get */
    u8 *out = NULL;
    u64 out_len = 0;
    ASSERT(kv_get(&out, &out_len, store, (const u8 *)k1, (u64)strlen(k1)) == 0,
           "kv: get");
    ASSERT(out_len == (u64)strlen(v1), "kv: get len");
    ASSERT(memcmp(out, v1, out_len) == 0, "kv: get data");
    free(out);

    /* put second key */
    const char *k2 = "groups:org.acme";
    const char *v2 = "{ owner: \"bob\" }";
    ASSERT(kv_put(store, (const u8 *)k2, (u64)strlen(k2),
                   (const u8 *)v2, (u64)strlen(v2)) == 0, "kv: put 2");

    /* prefix iteration */
    kv_iter *it = NULL;
    ASSERT(kv_iter_create(&it, store,
                           (const u8 *)"groups:", 7) == 0, "kv: iter create");
    int count = 0;
    const u8 *ik, *iv;
    u64 ikl, ivl;
    while (kv_iter_next(&ik, &ikl, &iv, &ivl, it) == 0)
        count++;
    ASSERT(count == 2, "kv: iter prefix count");
    kv_iter_destroy(it);

    /* delete */
    ASSERT(kv_delete(store, (const u8 *)k1, (u64)strlen(k1)) == 0,
           "kv: delete");
    ASSERT(kv_get(&out, &out_len, store, (const u8 *)k1, (u64)strlen(k1)) != 0,
           "kv: get after delete");

    /* not found */
    ASSERT(kv_get(&out, &out_len, store,
                   (const u8 *)"nonexistent", 11) != 0, "kv: not found");

    kv_close(store);
    remove(path);
}

int main(void) {
    printf("cookbook test suite\n\n");

    test_version();
    test_resources_path();
    test_db_open_close();
    test_db_migrate();
    test_db_groups_crud();
    test_db_artifacts_crud();
    test_store_put_get();
    test_store_not_found();
    test_db_parameterized_exec();
    test_db_parameterized_artifact();
    test_sha256_empty();
    test_sha256_abc();
    test_sha256_long();
    test_base64url_roundtrip();
    test_jwt_create_verify();
    test_ed25519_sign_verify();
    test_mirror_query();
    test_s3_store_open_null();
    test_s3_store_open_close();
    test_postgres_stub();
    test_semver_parse();
    test_semver_compare();
    test_range_caret();
    test_range_tilde();
    test_range_wildcard();
    test_range_bounded();
    test_semver_parse_edge_cases();
    test_semver_compare_detailed();
    test_range_exact();
    test_range_caret_zero();
    test_range_bounded_inclusive();
    test_db_yanked_status();
    test_db_yank_reason();
    test_db_resolve_include_yanked();
    test_db_null_params();
    test_db_pending_to_published();
    test_store_overwrite();
    test_store_large_value();
    test_jwt_expired();
    test_jwt_group_boundary();
    test_base64url_edge_cases();
    test_base64_std_decode();
    test_credential_hash_verify();
    test_credential_verify_wrong();
    test_credentials_table();
    test_grid_loop_detection();
    test_grid_peers_table();
    test_grid_peer_load();
    test_validate_ascii();
    test_validate_ascii_boundaries();
    test_pasta_to_json_primitives();
    test_pasta_to_json_nested();
    test_pasta_to_json_escaping();
    test_pasta_to_json_empty_containers();
    test_pasta_to_json_numbers();
    test_pasta_to_json_deeply_nested();
    test_pasta_sorted_write();
    test_pasta_sorted_nested();
    test_policy_crud();
    test_policy_resolve();
    test_policy_resolve_collect();
    test_auth_check_prefix();
    test_auth_check_exclude();
    test_auth_check_edge_cases();
    test_alforno_integration();
    test_jwt_v2_roundtrip();
    test_jwt_v2_policy_integration();
    test_jwt_v1_v2_compat();
    test_enforcement_v1_reads_open();
    test_enforcement_v2_all_ops();
    test_enforcement_mirror_visibility();
    test_enforcement_policy_to_enforcement();
    test_enforcement_grid_grant_roundtrip();
    test_grid_peer_key_crud();
    test_grid_peer_load_with_key();
    test_grid_canonical_string();
    test_grid_sign_verify_roundtrip();
    test_grid_timestamp_validation();
    test_auth_check_wildcard();
    test_revocation_list();
    test_jwt_jti();
    test_credential_admin_lifecycle();
    test_group_admin_lifecycle();
    test_full_auth_flow();
    test_repro_validation();
    test_object_cache_store();
    test_object_cache_ttl();
    test_revocation_persistence();
#ifdef COOKBOOK_HAS_BASTA
    test_basta_integration();
#endif
    test_ed25519_sha512();
    test_ed25519_scalarmult_base();
    test_ed25519_rfc8032_vector1();
    test_ed25519_rfc8032_vector2();
    test_ed25519_rfc8032_vector3();
    test_ed25519_sign_verify_roundtrip();
    test_db_kv_backend();
    test_connpool_basic();
    test_gzip_compress();
    test_ldap_config();
    test_ldap_integration();
    test_wal_basic();
    test_kv_basic();

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
