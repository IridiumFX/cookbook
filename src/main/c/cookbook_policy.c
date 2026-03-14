#include "cookbook_policy.h"
#include "alforno.h"
#include "pasta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Policy storage                                                     */
/* ------------------------------------------------------------------ */

COOKBOOK_API int cookbook_policy_put(cookbook_db *db, const char *subject,
                                    const char *kind, const char *pastlet) {
    if (!db || !subject || !kind || !pastlet) return -1;
    cookbook_db_param params[] = {
        COOKBOOK_P_TEXT(subject),
        COOKBOOK_P_TEXT(kind),
        COOKBOOK_P_TEXT(pastlet)
    };
    cookbook_db_status st = db->exec_p(db,
        "INSERT OR REPLACE INTO policies "
        "(subject, kind, pastlet, updated_at) "
        "VALUES (?1, ?2, ?3, datetime('now'))",
        params, 3);
    return (st == COOKBOOK_DB_OK) ? 0 : -1;
}

/* callback for single-row pastlet lookup */
typedef struct {
    char *pastlet;
} policy_get_ctx;

static int policy_get_cb(const cookbook_db_row *row, void *user) {
    policy_get_ctx *ctx = (policy_get_ctx *)user;
    if (row->values[0]) {
        size_t len = strlen(row->values[0]);
        ctx->pastlet = (char *)malloc(len + 1);
        if (ctx->pastlet) memcpy(ctx->pastlet, row->values[0], len + 1);
    }
    return 0;
}

COOKBOOK_API char *cookbook_policy_get(cookbook_db *db, const char *subject) {
    if (!db || !subject) return NULL;
    cookbook_db_param params[] = { COOKBOOK_P_TEXT(subject) };
    policy_get_ctx ctx = { NULL };
    db->query_p(db,
        "SELECT pastlet FROM policies WHERE subject = ?1",
        params, 1, policy_get_cb, &ctx);
    return ctx.pastlet;
}

COOKBOOK_API int cookbook_policy_delete(cookbook_db *db, const char *subject) {
    if (!db || !subject) return -1;
    cookbook_db_param params[] = { COOKBOOK_P_TEXT(subject) };
    cookbook_db_status st = db->exec_p(db,
        "DELETE FROM policies WHERE subject = ?1",
        params, 1);
    return (st == COOKBOOK_DB_OK) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Team extraction from a parsed pastlet                              */
/* ------------------------------------------------------------------ */

/*
 * Extract team names from @identity.teams array.
 * Returns an array of malloc'd strings, sets *out_count.
 * Caller must free each string and the array.
 */
static char **extract_teams(const PastaValue *root, int *out_count) {
    *out_count = 0;
    if (!root || pasta_type(root) != PASTA_MAP) return NULL;

    const PastaValue *identity = pasta_map_get(root, "identity");
    if (!identity || pasta_type(identity) != PASTA_MAP) return NULL;

    const PastaValue *teams = pasta_map_get(identity, "teams");
    if (!teams || pasta_type(teams) != PASTA_ARRAY) return NULL;

    size_t n = pasta_count(teams);
    if (n == 0) return NULL;

    char **result = (char **)calloc(n, sizeof(char *));
    if (!result) return NULL;

    int count = 0;
    for (size_t i = 0; i < n; i++) {
        const PastaValue *item = pasta_array_get(teams, i);
        const char *name = NULL;
        if (pasta_type(item) == PASTA_LABEL)
            name = pasta_get_label(item);
        else if (pasta_type(item) == PASTA_STRING)
            name = pasta_get_string(item);
        if (name) {
            result[count] = (char *)malloc(strlen(name) + 1);
            if (result[count]) {
                strcpy(result[count], name);
                count++;
            }
        }
    }
    *out_count = count;
    return result;
}

/* ------------------------------------------------------------------ */
/*  Serialize grants/exclude to JSON                                   */
/* ------------------------------------------------------------------ */

/*
 * Serialize the @grants and @exclude sections from a resolved pastlet
 * into a JSON string for JWT embedding.
 */
static char *serialize_grants_json(const PastaValue *resolved) {
    if (!resolved || pasta_type(resolved) != PASTA_MAP) return NULL;

    const PastaValue *grants  = pasta_map_get(resolved, "grants");
    const PastaValue *exclude = pasta_map_get(resolved, "exclude");

    /* estimate size */
    size_t cap = 256;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    size_t pos = 0;

    pos += (size_t)snprintf(buf + pos, cap - pos, "{\"grants\":{");

    if (grants && pasta_type(grants) == PASTA_MAP) {
        for (size_t i = 0; i < pasta_count(grants); i++) {
            const char *key = pasta_map_key(grants, i);
            const PastaValue *val = pasta_map_value(grants, i);
            const char *sv = NULL;

            if (pasta_type(val) == PASTA_STRING)
                sv = pasta_get_string(val);
            else if (pasta_type(val) == PASTA_ARRAY) {
                /* collect mode: OR all permission strings in the array */
                char perms[16] = {0};
                for (size_t j = 0; j < pasta_count(val); j++) {
                    const char *ps = pasta_get_string(pasta_array_get(val, j));
                    if (ps) {
                        for (const char *c = ps; *c; c++) {
                            if (*c == 'c' || *c == 'r' || *c == 'w' || *c == 'd') {
                                if (!strchr(perms, *c)) {
                                    size_t pl = strlen(perms);
                                    if (pl < sizeof(perms) - 1) {
                                        perms[pl] = *c;
                                        perms[pl + 1] = '\0';
                                    }
                                }
                            }
                        }
                    }
                }
                /* grow buffer if needed */
                size_t need = strlen(key) + strlen(perms) + 16;
                if (pos + need >= cap) {
                    cap = cap * 2 + need;
                    char *tmp = (char *)realloc(buf, cap);
                    if (!tmp) { free(buf); return NULL; }
                    buf = tmp;
                }
                if (i > 0) pos += (size_t)snprintf(buf + pos, cap - pos, ",");
                pos += (size_t)snprintf(buf + pos, cap - pos,
                    "\"%s\":\"%s\"", key, perms);
                continue;
            }

            if (!sv) sv = "";

            size_t need = strlen(key) + strlen(sv) + 16;
            if (pos + need >= cap) {
                cap = cap * 2 + need;
                char *tmp = (char *)realloc(buf, cap);
                if (!tmp) { free(buf); return NULL; }
                buf = tmp;
            }
            if (i > 0) pos += (size_t)snprintf(buf + pos, cap - pos, ",");
            pos += (size_t)snprintf(buf + pos, cap - pos,
                "\"%s\":\"%s\"", key, sv);
        }
    }

    pos += (size_t)snprintf(buf + pos, cap - pos, "},\"exclude\":{");

    if (exclude && pasta_type(exclude) == PASTA_MAP) {
        for (size_t i = 0; i < pasta_count(exclude); i++) {
            const char *key = pasta_map_key(exclude, i);
            size_t need = strlen(key) + 16;
            if (pos + need >= cap) {
                cap = cap * 2 + need;
                char *tmp = (char *)realloc(buf, cap);
                if (!tmp) { free(buf); return NULL; }
                buf = tmp;
            }
            if (i > 0) pos += (size_t)snprintf(buf + pos, cap - pos, ",");
            pos += (size_t)snprintf(buf + pos, cap - pos,
                "\"%s\":true", key);
        }
    }

    if (pos + 4 >= cap) {
        cap += 16;
        char *tmp = (char *)realloc(buf, cap);
        if (!tmp) { free(buf); return NULL; }
        buf = tmp;
    }
    pos += (size_t)snprintf(buf + pos, cap - pos, "}}");

    return buf;
}

/* ------------------------------------------------------------------ */
/*  Policy resolution via alforno                                      */
/* ------------------------------------------------------------------ */

COOKBOOK_API char *cookbook_policy_resolve(cookbook_db *db, const char *subject) {
    if (!db || !subject) return NULL;

    /* load user pastlet */
    char *user_pastlet = cookbook_policy_get(db, subject);
    if (!user_pastlet) return NULL;

    /* parse to extract team memberships */
    PastaResult pr;
    PastaValue *user_tree = pasta_parse_cstr(user_pastlet, &pr);
    if (!user_tree) {
        free(user_pastlet);
        return NULL;
    }

    int nteams = 0;
    char **team_names = extract_teams(user_tree, &nteams);
    pasta_free(user_tree);

    /* load team pastlets */
    char **team_pastlets = NULL;
    if (nteams > 0) {
        team_pastlets = (char **)calloc((size_t)nteams, sizeof(char *));
        if (!team_pastlets) {
            for (int i = 0; i < nteams; i++) free(team_names[i]);
            free(team_names);
            free(user_pastlet);
            return NULL;
        }
        for (int i = 0; i < nteams; i++) {
            team_pastlets[i] = cookbook_policy_get(db, team_names[i]);
            /* missing team pastlet is OK — skip it */
        }
    }

    /* build dynamic conflate recipe from the union of all grant/exclude keys
       across user + team pastlets, so conflate doesn't drop unknown fields */
    char *all_pastlets[65]; /* user + up to 64 teams */
    int npastlets = 0;
    all_pastlets[npastlets++] = user_pastlet;
    for (int i = 0; i < nteams && npastlets < 65; i++) {
        if (team_pastlets[i]) all_pastlets[npastlets++] = team_pastlets[i];
    }

    /* collect unique keys from @grants and @exclude across all inputs */
    size_t recipe_cap = 512;
    char *recipe = (char *)malloc(recipe_cap);
    if (!recipe) {
        for (int i = 0; i < nteams; i++) {
            free(team_names[i]);
            free(team_pastlets[i]);
        }
        free(team_names);
        free(team_pastlets);
        free(user_pastlet);
        return NULL;
    }
    size_t rpos = 0;

    /* scan all inputs for @grants and @exclude keys */
    /* use a simple dedup array — typical policy has few keys */
    char *grant_keys[256];  int ngrant_keys = 0;
    char *exclude_keys[256]; int nexclude_keys = 0;

    for (int p = 0; p < npastlets; p++) {
        PastaResult pr2;
        PastaValue *tree = pasta_parse_cstr(all_pastlets[p], &pr2);
        if (!tree) continue;

        const PastaValue *gs = pasta_map_get(tree, "grants");
        if (gs && pasta_type(gs) == PASTA_MAP) {
            for (size_t k = 0; k < pasta_count(gs); k++) {
                const char *key = pasta_map_key(gs, k);
                /* dedup */
                int dup = 0;
                for (int d = 0; d < ngrant_keys; d++) {
                    if (strcmp(grant_keys[d], key) == 0) { dup = 1; break; }
                }
                if (!dup && ngrant_keys < 256) {
                    grant_keys[ngrant_keys] = (char *)malloc(strlen(key) + 1);
                    if (grant_keys[ngrant_keys]) {
                        strcpy(grant_keys[ngrant_keys], key);
                        ngrant_keys++;
                    }
                }
            }
        }

        const PastaValue *es = pasta_map_get(tree, "exclude");
        if (es && pasta_type(es) == PASTA_MAP) {
            for (size_t k = 0; k < pasta_count(es); k++) {
                const char *key = pasta_map_key(es, k);
                int dup = 0;
                for (int d = 0; d < nexclude_keys; d++) {
                    if (strcmp(exclude_keys[d], key) == 0) { dup = 1; break; }
                }
                if (!dup && nexclude_keys < 256) {
                    exclude_keys[nexclude_keys] = (char *)malloc(strlen(key) + 1);
                    if (exclude_keys[nexclude_keys]) {
                        strcpy(exclude_keys[nexclude_keys], key);
                        nexclude_keys++;
                    }
                }
            }
        }
        pasta_free(tree);
    }

    /* build recipe: @grants with merge:"collect", @exclude with merge:"replace" */
    rpos += (size_t)snprintf(recipe + rpos, recipe_cap - rpos,
        "@grants {\n  consumes: [\"grants\"],\n  merge: \"collect\"");
    for (int k = 0; k < ngrant_keys; k++) {
        size_t need = strlen(grant_keys[k]) + 32;
        if (rpos + need >= recipe_cap) {
            recipe_cap = recipe_cap * 2 + need;
            char *tmp = (char *)realloc(recipe, recipe_cap);
            if (!tmp) { free(recipe); recipe = NULL; break; }
            recipe = tmp;
        }
        rpos += (size_t)snprintf(recipe + rpos, recipe_cap - rpos,
            ",\n  \"%s\": \"perm\"", grant_keys[k]);
    }
    if (recipe)
        rpos += (size_t)snprintf(recipe + rpos, recipe_cap - rpos, "\n}\n");

    if (recipe && nexclude_keys > 0) {
        rpos += (size_t)snprintf(recipe + rpos, recipe_cap - rpos,
            "@exclude {\n  consumes: [\"exclude\"],\n  merge: \"replace\"");
        for (int k = 0; k < nexclude_keys; k++) {
            size_t need = strlen(exclude_keys[k]) + 32;
            if (rpos + need >= recipe_cap) {
                recipe_cap = recipe_cap * 2 + need;
                char *tmp = (char *)realloc(recipe, recipe_cap);
                if (!tmp) { free(recipe); recipe = NULL; break; }
                recipe = tmp;
            }
            rpos += (size_t)snprintf(recipe + rpos, recipe_cap - rpos,
                ",\n  \"%s\": \"flag\"", exclude_keys[k]);
        }
        if (recipe)
            rpos += (size_t)snprintf(recipe + rpos, recipe_cap - rpos, "\n}\n");
    }

    /* free key lists */
    for (int k = 0; k < ngrant_keys; k++) free(grant_keys[k]);
    for (int k = 0; k < nexclude_keys; k++) free(exclude_keys[k]);

    if (!recipe) {
        for (int i = 0; i < nteams; i++) {
            free(team_names[i]);
            free(team_pastlets[i]);
        }
        free(team_names);
        free(team_pastlets);
        free(user_pastlet);
        return NULL;
    }

    /* run alforno conflate with dynamic recipe */
    AlfResult ar;
    AlfContext *ctx = alf_create(ALF_CONFLATE, &ar);
    if (!ctx) {
        free(recipe);
        for (int i = 0; i < nteams; i++) {
            free(team_names[i]);
            free(team_pastlets[i]);
        }
        free(team_names);
        free(team_pastlets);
        free(user_pastlet);
        return NULL;
    }

    alf_set_recipe(ctx, recipe, strlen(recipe), &ar);
    free(recipe);

    /* add user pastlet first */
    alf_add_input(ctx, user_pastlet, strlen(user_pastlet), &ar);

    /* add team pastlets in order */
    for (int i = 0; i < nteams; i++) {
        if (team_pastlets[i]) {
            alf_add_input(ctx, team_pastlets[i], strlen(team_pastlets[i]),
                          &ar);
        }
    }

    PastaValue *resolved = alf_process(ctx, &ar);
    alf_free(ctx);

    /* cleanup inputs */
    free(user_pastlet);
    for (int i = 0; i < nteams; i++) {
        free(team_names[i]);
        free(team_pastlets[i]);
    }
    free(team_names);
    free(team_pastlets);

    if (!resolved) return NULL;

    /* serialize to JSON for JWT embedding */
    char *json = serialize_grants_json(resolved);
    pasta_free(resolved);
    return json;
}

/* ------------------------------------------------------------------ */
/*  Permission checking                                                */
/* ------------------------------------------------------------------ */

/*
 * Simple JSON map walker — finds "key":"value" pairs in a flat JSON map.
 * Not a full parser; works for our known-good output from serialize_grants_json.
 */
typedef int (*grant_walk_fn)(const char *key, size_t key_len,
                              const char *val, size_t val_len,
                              void *user);

static void walk_json_map(const char *json, size_t json_len,
                           grant_walk_fn fn, void *user) {
    if (!json) return;
    size_t i = 0;
    while (i < json_len) {
        /* find next key */
        while (i < json_len && json[i] != '"') i++;
        if (i >= json_len) break;
        i++; /* skip opening " */
        size_t ks = i;
        while (i < json_len && json[i] != '"') i++;
        if (i >= json_len) break;
        size_t ke = i;
        i++; /* skip closing " */

        /* skip : */
        while (i < json_len && json[i] != ':') i++;
        if (i >= json_len) break;
        i++;

        /* skip whitespace */
        while (i < json_len && (json[i] == ' ' || json[i] == '\t')) i++;

        /* value: "string" or true/false */
        if (i < json_len && json[i] == '"') {
            i++;
            size_t vs = i;
            while (i < json_len && json[i] != '"') i++;
            size_t ve = i;
            if (i < json_len) i++;
            if (fn(json + ks, ke - ks, json + vs, ve - vs, user)) return;
        } else if (i < json_len && json[i] == 't') {
            /* true */
            if (fn(json + ks, ke - ks, "true", 4, user)) return;
            while (i < json_len && json[i] != ',' && json[i] != '}') i++;
        } else {
            /* skip unknown value */
            while (i < json_len && json[i] != ',' && json[i] != '}') i++;
        }
    }
}

/* find the longest grant prefix matching group_id */
typedef struct {
    const char *group_id;
    size_t      group_len;
    const char *best_val;
    size_t      best_val_len;
    size_t      best_prefix_len;
} grant_match_ctx;

static int grant_match_cb(const char *key, size_t key_len,
                            const char *val, size_t val_len,
                            void *user) {
    grant_match_ctx *ctx = (grant_match_ctx *)user;

    /* check if key is a prefix of group_id */
    if (key_len > ctx->group_len) return 0;

    /* key must match the start of group_id */
    if (memcmp(key, ctx->group_id, key_len) != 0) return 0;

    /* if key is shorter, the next char in group_id must be '.' or end */
    if (key_len < ctx->group_len && ctx->group_id[key_len] != '.') return 0;

    /* longest prefix wins */
    if (key_len > ctx->best_prefix_len) {
        ctx->best_val = val;
        ctx->best_val_len = val_len;
        ctx->best_prefix_len = key_len;
    }
    return 0;
}

/* check if any exclude prefix matches group_id */
typedef struct {
    const char *group_id;
    size_t      group_len;
    int         excluded;
} exclude_match_ctx;

static int exclude_match_cb(const char *key, size_t key_len,
                              const char *val, size_t val_len,
                              void *user) {
    (void)val; (void)val_len;
    exclude_match_ctx *ctx = (exclude_match_ctx *)user;
    if (key_len > ctx->group_len) return 0;
    if (memcmp(key, ctx->group_id, key_len) != 0) return 0;
    if (key_len < ctx->group_len && ctx->group_id[key_len] != '.') return 0;
    ctx->excluded = 1;
    return 1; /* stop walking */
}

COOKBOOK_API int cookbook_auth_check(const char *grants_json,
                                    const char *exclude_json,
                                    const char *group_id,
                                    char op) {
    if (!grants_json || !group_id) return 0;

    size_t group_len = strlen(group_id);

    /* find the "grants":{...} sub-object */
    const char *gstart = strstr(grants_json, "\"grants\":{");
    if (!gstart) {
        /* maybe it's a bare grants map */
        gstart = grants_json;
    } else {
        gstart += 10; /* skip "grants":{ */
    }

    /* find matching closing brace */
    const char *gend = strchr(gstart, '}');
    if (!gend) return 0;

    grant_match_ctx gctx = { group_id, group_len, NULL, 0, 0 };
    walk_json_map(gstart, (size_t)(gend - gstart), grant_match_cb, &gctx);

    if (!gctx.best_val) return 0; /* no matching grant */

    /* check operation is in the grant value */
    int has_op = 0;
    for (size_t i = 0; i < gctx.best_val_len; i++) {
        if (gctx.best_val[i] == op) { has_op = 1; break; }
    }
    if (!has_op) return 0;

    /* check excludes — supports both wrapped {"exclude":{...}} and bare {...} */
    const char *exc_src = exclude_json ? exclude_json : grants_json;
    if (exc_src) {
        const char *estart = strstr(exc_src, "\"exclude\":{");
        if (estart) {
            estart += 11; /* skip "exclude":{ */
        } else if (exclude_json) {
            /* bare exclude map (from JWT v2 extracted claims) */
            estart = strchr(exclude_json, '{');
            if (estart) estart++; /* skip { */
        }
        if (estart) {
            const char *eend = strchr(estart, '}');
            if (eend) {
                exclude_match_ctx ectx = { group_id, group_len, 0 };
                walk_json_map(estart, (size_t)(eend - estart),
                              exclude_match_cb, &ectx);
                if (ectx.excluded) return 0;
            }
        }
    }

    return 1; /* allowed */
}
