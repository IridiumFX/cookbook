#include "cookbook_db.h"

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS groups ("
    "  group_id    TEXT PRIMARY KEY,"
    "  owner_sub   TEXT NOT NULL,"
    "  created_at  TEXT,"
    "  description TEXT"
    ");"

    "CREATE TABLE IF NOT EXISTS publisher_keys ("
    "  key_id      TEXT PRIMARY KEY,"
    "  group_id    TEXT REFERENCES groups(group_id),"
    "  public_key  TEXT NOT NULL,"
    "  comment     TEXT,"
    "  added_at    TEXT,"
    "  revoked_at  TEXT"
    ");"

    "CREATE TABLE IF NOT EXISTS artifacts ("
    "  coord_id          TEXT PRIMARY KEY,"
    "  group_id          TEXT REFERENCES groups(group_id),"
    "  artifact          TEXT NOT NULL,"
    "  version           TEXT NOT NULL,"
    "  triple            TEXT NOT NULL,"
    "  snapshot          INTEGER DEFAULT 0,"
    "  yanked            INTEGER DEFAULT 0,"
    "  status            TEXT DEFAULT 'pending',"
    "  sha256            TEXT NOT NULL,"
    "  descriptor_sha256 TEXT,"
    "  signed            INTEGER DEFAULT 0,"
    "  size_bytes        INTEGER,"
    "  pending_since     TEXT,"
    "  published_at      TEXT,"
    "  published_by      TEXT,"
    "  UNIQUE (group_id, artifact, version, triple)"
    ");"

    "CREATE TABLE IF NOT EXISTS artifact_semver ("
    "  coord_id    TEXT REFERENCES artifacts(coord_id),"
    "  major       INTEGER,"
    "  minor       INTEGER,"
    "  patch       INTEGER,"
    "  pre_release TEXT,"
    "  build_meta  TEXT"
    ");"

    "CREATE INDEX IF NOT EXISTS idx_artifact_semver_range "
    "  ON artifact_semver(major, minor, patch);"
;

cookbook_db_status cookbook_db_migrate(cookbook_db *db) {
    return db->exec(db, SCHEMA_SQL);
}
