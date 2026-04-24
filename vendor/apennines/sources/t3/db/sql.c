#include "apennines/t3/db/sql.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Keyword table                                                      */
/* ------------------------------------------------------------------ */

static const char *keywords[] = {
    "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES",
    "UPDATE", "SET", "DELETE", "CREATE", "TABLE", "DROP",
    "AND", "OR", "NOT", "NULL", "ORDER", "BY", "ASC", "DESC",
    "LIMIT", "OFFSET", "AS", "DISTINCT", "IN", "IS", "LIKE", "BETWEEN",
    "GROUP", "HAVING", "INT", "INTEGER", "TEXT", "VARCHAR",
    "REAL", "BLOB", "PRIMARY", "KEY", "IF", "EXISTS",
    /* DDL constraint keywords accepted by CREATE TABLE — UNIQUE is
     * enforced; the rest (DEFAULT / REFERENCES / CHECK / FOREIGN /
     * CONSTRAINT / COLLATE) are parsed and silently skipped so that
     * SQL blobs written for sqlite (like cookbook's migration) still
     * validate. Real FK enforcement comes in a later step. */
    "UNIQUE", "DEFAULT", "REFERENCES", "FOREIGN", "CONSTRAINT",
    "CHECK", "COLLATE", "INDEX", "ON",
    NULL
};

static int is_keyword(const char *word, u64 len) {
    for (int i = 0; keywords[i]; ++i) {
        const char *kw = keywords[i];
        u64 kl = (u64)strlen(kw);
        if (kl != len) continue;
        int match = 1;
        for (u64 j = 0; j < len; ++j) {
            if (toupper((unsigned char)word[j]) != kw[j]) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Token list helpers                                                 */
/* ------------------------------------------------------------------ */

static unsigned long token_list_push(sql_token_list *list, int type,
                                     const char *text, u64 text_len, u64 offset) {
    if (list->count >= list->capacity) {
        u64 new_cap = list->capacity ? list->capacity * 2 : 32;
        sql_token *tmp = realloc(list->tokens, new_cap * sizeof(sql_token));
        if (!tmp) return 3;
        list->tokens   = tmp;
        list->capacity = new_cap;
    }
    char *copy = malloc(text_len + 1);
    if (!copy) return 3;
    memcpy(copy, text, text_len);
    copy[text_len] = '\0';
    sql_token *t = &list->tokens[list->count++];
    t->type     = type;
    t->text     = copy;
    t->text_len = text_len;
    t->offset   = offset;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sql_tokenize                                                       */
/* ------------------------------------------------------------------ */

APENNINES_API unsigned long sql_tokenize(sql_token_list *out, const char *sql) {
    if (!out)  return 1;
    if (!sql)  return 2;

    memset(out, 0, sizeof(*out));

    u64 pos = 0;
    u64 len = (u64)strlen(sql);

    while (pos < len) {
        /* skip whitespace */
        if (isspace((unsigned char)sql[pos])) { pos++; continue; }

        u64 start = pos;
        unsigned char c = (unsigned char)sql[pos];

        /* single-char symbols */
        if (c == ',') { unsigned long r = token_list_push(out, SQL_TOK_COMMA,    ",", 1, start); if (r) return r; pos++; continue; }
        if (c == '(') { unsigned long r = token_list_push(out, SQL_TOK_LPAREN,   "(", 1, start); if (r) return r; pos++; continue; }
        if (c == ')') { unsigned long r = token_list_push(out, SQL_TOK_RPAREN,   ")", 1, start); if (r) return r; pos++; continue; }
        if (c == '*') { unsigned long r = token_list_push(out, SQL_TOK_STAR,     "*", 1, start); if (r) return r; pos++; continue; }
        if (c == ';') { unsigned long r = token_list_push(out, SQL_TOK_SEMICOLON,";", 1, start); if (r) return r; pos++; continue; }
        if (c == '.') { unsigned long r = token_list_push(out, SQL_TOK_DOT,      ".", 1, start); if (r) return r; pos++; continue; }

        /* operators */
        if (c == '=' ) { unsigned long r = token_list_push(out, SQL_TOK_OPERATOR, "=",  1, start); if (r) return r; pos++; continue; }
        if (c == '<') {
            if (pos + 1 < len && sql[pos + 1] == '=') {
                unsigned long r = token_list_push(out, SQL_TOK_OPERATOR, "<=", 2, start); if (r) return r; pos += 2; continue;
            }
            if (pos + 1 < len && sql[pos + 1] == '>') {
                unsigned long r = token_list_push(out, SQL_TOK_OPERATOR, "<>", 2, start); if (r) return r; pos += 2; continue;
            }
            unsigned long r = token_list_push(out, SQL_TOK_OPERATOR, "<", 1, start); if (r) return r; pos++; continue;
        }
        if (c == '>') {
            if (pos + 1 < len && sql[pos + 1] == '=') {
                unsigned long r = token_list_push(out, SQL_TOK_OPERATOR, ">=", 2, start); if (r) return r; pos += 2; continue;
            }
            unsigned long r = token_list_push(out, SQL_TOK_OPERATOR, ">", 1, start); if (r) return r; pos++; continue;
        }
        if (c == '!' && pos + 1 < len && sql[pos + 1] == '=') {
            unsigned long r = token_list_push(out, SQL_TOK_OPERATOR, "!=", 2, start); if (r) return r; pos += 2; continue;
        }

        /* placeholder: ? or ?N (digits follow) */
        if (c == '?') {
            pos++;
            while (pos < len && isdigit((unsigned char)sql[pos])) pos++;
            unsigned long r = token_list_push(out, SQL_TOK_PLACEHOLDER,
                                                sql + start, pos - start, start);
            if (r) return r;
            continue;
        }

        /* string literal */
        if (c == '\'') {
            pos++; /* skip opening quote */
            u64 buf_cap = 64;
            char *buf = malloc(buf_cap);
            if (!buf) return 3;
            u64 buf_len = 0;
            while (pos < len) {
                if (sql[pos] == '\'') {
                    if (pos + 1 < len && sql[pos + 1] == '\'') {
                        /* escaped quote */
                        if (buf_len + 1 >= buf_cap) {
                            buf_cap *= 2;
                            char *nb = realloc(buf, buf_cap);
                            if (!nb) { free(buf); return 3; }
                            buf = nb;
                        }
                        buf[buf_len++] = '\'';
                        pos += 2;
                    } else {
                        pos++; /* skip closing quote */
                        break;
                    }
                } else {
                    if (buf_len + 1 >= buf_cap) {
                        buf_cap *= 2;
                        char *nb = realloc(buf, buf_cap);
                        if (!nb) { free(buf); return 3; }
                        buf = nb;
                    }
                    buf[buf_len++] = sql[pos++];
                }
            }
            buf[buf_len] = '\0';
            unsigned long r = token_list_push(out, SQL_TOK_STRING, buf, buf_len, start);
            free(buf);
            if (r) return r;
            continue;
        }

        /* number literal */
        if (isdigit(c)) {
            while (pos < len && isdigit((unsigned char)sql[pos])) pos++;
            if (pos < len && sql[pos] == '.') {
                pos++;
                while (pos < len && isdigit((unsigned char)sql[pos])) pos++;
            }
            unsigned long r = token_list_push(out, SQL_TOK_NUMBER, sql + start, pos - start, start);
            if (r) return r;
            continue;
        }

        /* identifier / keyword */
        if (isalpha(c) || c == '_') {
            while (pos < len && (isalnum((unsigned char)sql[pos]) || sql[pos] == '_')) pos++;
            u64 wlen = pos - start;
            int type = is_keyword(sql + start, wlen) ? SQL_TOK_KEYWORD : SQL_TOK_IDENT;
            unsigned long r = token_list_push(out, type, sql + start, wlen, start);
            if (r) return r;
            continue;
        }

        /* unrecognised character */
        return 4;
    }

    /* append EOF token */
    unsigned long r = token_list_push(out, SQL_TOK_EOF, "", 0, pos);
    if (r) return r;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Parser internals                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const sql_token_list *tokens;
    u64 pos;
} parser;

static const sql_token *peek(parser *p) {
    if (p->pos < p->tokens->count)
        return &p->tokens->tokens[p->pos];
    return NULL;
}

static const sql_token *advance(parser *p) {
    const sql_token *t = peek(p);
    if (t && t->type != SQL_TOK_EOF) p->pos++;
    return t;
}

static int tok_is_keyword(const sql_token *t, const char *kw) {
    if (!t || t->type != SQL_TOK_KEYWORD) return 0;
    u64 kl = (u64)strlen(kw);
    if (t->text_len != kl) return 0;
    for (u64 i = 0; i < kl; ++i) {
        if (toupper((unsigned char)t->text[i]) != toupper((unsigned char)kw[i])) return 0;
    }
    return 1;
}

static int tok_is(const sql_token *t, int type) {
    return t && t->type == type;
}

/* ---- AST allocation helpers ---- */

static sql_ast *ast_new(int type, const char *text) {
    sql_ast *n = calloc(1, sizeof(sql_ast));
    if (!n) return NULL;
    n->type = type;
    if (text) {
        n->text = malloc(strlen(text) + 1);
        if (!n->text) { free(n); return NULL; }
        strcpy(n->text, text);
    }
    return n;
}

static int ast_add_child(sql_ast *parent, sql_ast *child) {
    u64 new_count = parent->child_count + 1;
    sql_ast *tmp = realloc(parent->children, new_count * sizeof(sql_ast));
    if (!tmp) return -1;
    parent->children = tmp;
    parent->children[parent->child_count] = *child;
    parent->child_count = new_count;
    /* free the shell — content was copied */
    free(child);
    return 0;
}

/* ---- Forward declarations ---- */

static sql_ast *parse_condition(parser *p);

/* Skip a balanced parenthesised expression. Precondition: next token
 * is SQL_TOK_LPAREN. On return, the matching RPAREN has been consumed.
 * Used to discard DEFAULT (expr) / CHECK (expr) / REFERENCES tab(col)
 * payloads that we don't yet interpret. */
static void skip_paren_expr(parser *p) {
    if (!tok_is(peek(p), SQL_TOK_LPAREN)) return;
    int depth = 0;
    const sql_token *t = peek(p);
    while (t && t->type != SQL_TOK_EOF) {
        if (tok_is(t, SQL_TOK_LPAREN)) depth++;
        else if (tok_is(t, SQL_TOK_RPAREN)) depth--;
        advance(p);
        if (depth == 0) return;
        t = peek(p);
    }
}

/* Parse "(col1, col2, ...)" and write the names as "col1,col2,..." to
 * buf. Used for UNIQUE (a, b) and FOREIGN KEY (a) clauses. Returns 0
 * on success, -1 on any malformed input. */
static int parse_paren_col_list(parser *p, char *buf, u64 cap) {
    if (!tok_is(peek(p), SQL_TOK_LPAREN)) return -1;
    advance(p);
    u64 off = 0;
    int first = 1;
    for (;;) {
        const sql_token *t = peek(p);
        if (!t) return -1;
        if (tok_is(t, SQL_TOK_RPAREN)) break;
        if (tok_is(t, SQL_TOK_COMMA)) { advance(p); continue; }
        if (!tok_is(t, SQL_TOK_IDENT) && !tok_is(t, SQL_TOK_KEYWORD)) return -1;
        u64 nl = t->text_len;
        if (off + nl + 2 >= cap) return -1;
        if (!first) buf[off++] = ',';
        memcpy(buf + off, t->text, nl);
        off += nl;
        first = 0;
        advance(p);
    }
    if (!tok_is(peek(p), SQL_TOK_RPAREN)) return -1;
    advance(p);
    buf[off] = '\0';
    return 0;
}

/* ---- Condition parser ---- */

static sql_ast *parse_primary_condition(parser *p) {
    const sql_token *t = peek(p);
    if (!t) return NULL;

    /* parenthesised sub-expression */
    if (tok_is(t, SQL_TOK_LPAREN)) {
        advance(p);
        sql_ast *inner = parse_condition(p);
        if (!inner) return NULL;
        t = peek(p);
        if (!tok_is(t, SQL_TOK_RPAREN)) {
            sql_ast_destroy(inner);
            return NULL;
        }
        advance(p);
        return inner;
    }

    /* NOT expr */
    if (tok_is_keyword(t, "NOT")) {
        advance(p);
        sql_ast *operand = parse_primary_condition(p);
        if (!operand) return NULL;
        sql_ast *node = ast_new(SQL_AST_BINARY_OP, "NOT");
        if (!node) { sql_ast_destroy(operand); return NULL; }
        node->left = calloc(1, sizeof(sql_ast));
        if (!node->left) { sql_ast_destroy(operand); free(node->text); free(node); return NULL; }
        *node->left = *operand;
        free(operand);
        return node;
    }

    /* lhs = identifier, aggregate call, or literal */
    sql_ast *lhs = NULL;
    if (tok_is(t, SQL_TOK_IDENT) || tok_is(t, SQL_TOK_KEYWORD)) {
        /* Look ahead one token: if it's LPAREN and the name is an
         * aggregate function, consume the full call as an AGG_REF.
         * Lets HAVING / WHERE predicates like `COUNT(*) > 5` or
         * `SUM(amount) >= 100` work. */
        int is_agg_call = 0;
        if (p->pos + 1 < p->tokens->count
            && p->tokens->tokens[p->pos + 1].type == SQL_TOK_LPAREN) {
            const char *uppers[] = {"COUNT","SUM","MIN","MAX","AVG",NULL};
            u64 name_len = t->text_len;
            for (int ii = 0; uppers[ii]; ii++) {
                u64 kl = (u64)strlen(uppers[ii]);
                if (name_len != kl) continue;
                int match = 1;
                for (u64 jj = 0; jj < kl; jj++) {
                    if (toupper((unsigned char)t->text[jj]) != uppers[ii][jj]) {
                        match = 0; break;
                    }
                }
                if (match) { is_agg_call = 1; break; }
            }
        }

        if (is_agg_call) {
            /* Save canonical uppercase name. */
            char upname[16];
            u64 ul = t->text_len < sizeof(upname) - 1
                     ? t->text_len : sizeof(upname) - 1;
            for (u64 i = 0; i < ul; i++) {
                upname[i] = (char)toupper((unsigned char)t->text[i]);
            }
            upname[ul] = '\0';
            advance(p);           /* FN */
            advance(p);           /* ( */
            char arg[64]; arg[0] = '\0';
            const sql_token *a = peek(p);
            if (tok_is(a, SQL_TOK_STAR)) {
                arg[0] = '*'; arg[1] = '\0';
                advance(p);
            } else if (a && (tok_is(a, SQL_TOK_IDENT)
                              || tok_is(a, SQL_TOK_KEYWORD))) {
                u64 al = a->text_len < sizeof(arg) - 1
                          ? a->text_len : sizeof(arg) - 1;
                memcpy(arg, a->text, al); arg[al] = '\0';
                advance(p);
            }
            if (tok_is(peek(p), SQL_TOK_RPAREN)) advance(p);
            char ref_text[128];
            snprintf(ref_text, sizeof(ref_text), "%s %s",
                      upname, arg[0] ? arg : "*");
            lhs = ast_new(SQL_AST_AGG_REF, ref_text);
        } else {
            lhs = ast_new(SQL_AST_COLUMN_REF, t->text);
            advance(p);
        }
    } else if (tok_is(t, SQL_TOK_NUMBER) || tok_is(t, SQL_TOK_STRING)
               || tok_is(t, SQL_TOK_PLACEHOLDER)) {
        lhs = ast_new(SQL_AST_LITERAL, t->text);
        advance(p);
    } else {
        return NULL;
    }
    if (!lhs) return NULL;

    /* IS [NOT] NULL */
    t = peek(p);
    if (tok_is_keyword(t, "IS")) {
        advance(p);
        t = peek(p);
        const char *op_text = "IS";
        if (tok_is_keyword(t, "NOT")) {
            op_text = "IS NOT";
            advance(p);
            t = peek(p);
        }
        if (!tok_is_keyword(t, "NULL")) {
            sql_ast_destroy(lhs);
            return NULL;
        }
        advance(p);
        sql_ast *node = ast_new(SQL_AST_BINARY_OP, op_text);
        if (!node) { sql_ast_destroy(lhs); return NULL; }
        node->left = lhs;
        node->right = ast_new(SQL_AST_LITERAL, "NULL");
        if (!node->right) { sql_ast_destroy(node); return NULL; }
        return node;
    }

    /* LIKE, IN, BETWEEN */
    if (tok_is_keyword(t, "LIKE")) {
        advance(p);
        t = peek(p);
        if (!t) { sql_ast_destroy(lhs); return NULL; }
        sql_ast *rhs = ast_new(SQL_AST_LITERAL, t->text);
        advance(p);
        if (!rhs) { sql_ast_destroy(lhs); return NULL; }
        sql_ast *node = ast_new(SQL_AST_BINARY_OP, "LIKE");
        if (!node) { sql_ast_destroy(lhs); sql_ast_destroy(rhs); return NULL; }
        node->left  = lhs;
        node->right = rhs;
        return node;
    }

    if (tok_is_keyword(t, "BETWEEN")) {
        advance(p);
        t = peek(p);
        if (!t) { sql_ast_destroy(lhs); return NULL; }
        sql_ast *lo = ast_new(SQL_AST_LITERAL, t->text);
        advance(p);
        if (!lo) { sql_ast_destroy(lhs); return NULL; }
        t = peek(p);
        if (!tok_is_keyword(t, "AND")) { sql_ast_destroy(lhs); sql_ast_destroy(lo); return NULL; }
        advance(p);
        t = peek(p);
        if (!t) { sql_ast_destroy(lhs); sql_ast_destroy(lo); return NULL; }
        sql_ast *hi = ast_new(SQL_AST_LITERAL, t->text);
        advance(p);
        if (!hi) { sql_ast_destroy(lhs); sql_ast_destroy(lo); return NULL; }
        sql_ast *node = ast_new(SQL_AST_BINARY_OP, "BETWEEN");
        if (!node) { sql_ast_destroy(lhs); sql_ast_destroy(lo); sql_ast_destroy(hi); return NULL; }
        node->left  = lhs;
        /* encode lo and hi as children */
        if (ast_add_child(node, lo) < 0 || ast_add_child(node, hi) < 0) {
            sql_ast_destroy(node);
            return NULL;
        }
        return node;
    }

    if (tok_is_keyword(t, "IN")) {
        advance(p);
        t = peek(p);
        if (!tok_is(t, SQL_TOK_LPAREN)) { sql_ast_destroy(lhs); return NULL; }
        advance(p);
        sql_ast *node = ast_new(SQL_AST_BINARY_OP, "IN");
        if (!node) { sql_ast_destroy(lhs); return NULL; }
        node->left = lhs;
        /* collect list values as children */
        for (;;) {
            t = peek(p);
            if (!t || tok_is(t, SQL_TOK_RPAREN)) break;
            sql_ast *val = ast_new(SQL_AST_LITERAL, t->text);
            advance(p);
            if (!val) { sql_ast_destroy(node); return NULL; }
            if (ast_add_child(node, val) < 0) { sql_ast_destroy(node); return NULL; }
            t = peek(p);
            if (tok_is(t, SQL_TOK_COMMA)) advance(p);
        }
        if (!tok_is(peek(p), SQL_TOK_RPAREN)) { sql_ast_destroy(node); return NULL; }
        advance(p);
        return node;
    }

    /* NOT IN / NOT LIKE / NOT BETWEEN */
    if (tok_is_keyword(t, "NOT")) {
        advance(p);
        t = peek(p);
        /* Rewind: put NOT back by decrementing, parse as "col NOT LIKE ..." etc. */
        /* Simplified: treat NOT + next keyword */
        if (tok_is_keyword(t, "LIKE")) {
            advance(p);
            t = peek(p);
            if (!t) { sql_ast_destroy(lhs); return NULL; }
            sql_ast *rhs = ast_new(SQL_AST_LITERAL, t->text);
            advance(p);
            if (!rhs) { sql_ast_destroy(lhs); return NULL; }
            sql_ast *node = ast_new(SQL_AST_BINARY_OP, "NOT LIKE");
            if (!node) { sql_ast_destroy(lhs); sql_ast_destroy(rhs); return NULL; }
            node->left  = lhs;
            node->right = rhs;
            return node;
        }
        if (tok_is_keyword(t, "IN")) {
            advance(p);
            t = peek(p);
            if (!tok_is(t, SQL_TOK_LPAREN)) { sql_ast_destroy(lhs); return NULL; }
            advance(p);
            sql_ast *node = ast_new(SQL_AST_BINARY_OP, "NOT IN");
            if (!node) { sql_ast_destroy(lhs); return NULL; }
            node->left = lhs;
            for (;;) {
                t = peek(p);
                if (!t || tok_is(t, SQL_TOK_RPAREN)) break;
                sql_ast *val = ast_new(SQL_AST_LITERAL, t->text);
                advance(p);
                if (!val) { sql_ast_destroy(node); return NULL; }
                if (ast_add_child(node, val) < 0) { sql_ast_destroy(node); return NULL; }
                t = peek(p);
                if (tok_is(t, SQL_TOK_COMMA)) advance(p);
            }
            if (!tok_is(peek(p), SQL_TOK_RPAREN)) { sql_ast_destroy(node); return NULL; }
            advance(p);
            return node;
        }
        /* fallback: NOT was part of something else — error */
        sql_ast_destroy(lhs);
        return NULL;
    }

    /* comparison operator */
    if (!tok_is(t, SQL_TOK_OPERATOR)) {
        /* just a bare value, return it */
        return lhs;
    }

    const char *op = t->text;
    advance(p);

    t = peek(p);
    if (!t) { sql_ast_destroy(lhs); return NULL; }

    sql_ast *rhs = NULL;
    if (tok_is(t, SQL_TOK_IDENT) || tok_is(t, SQL_TOK_KEYWORD)) {
        /* Same aggregate-call detection as lhs — symmetric handling for
         * predicates like `5 < COUNT(*)`. */
        int is_agg_call = 0;
        if (p->pos + 1 < p->tokens->count
            && p->tokens->tokens[p->pos + 1].type == SQL_TOK_LPAREN) {
            const char *uppers[] = {"COUNT","SUM","MIN","MAX","AVG",NULL};
            u64 name_len = t->text_len;
            for (int ii = 0; uppers[ii]; ii++) {
                u64 kl = (u64)strlen(uppers[ii]);
                if (name_len != kl) continue;
                int match = 1;
                for (u64 jj = 0; jj < kl; jj++) {
                    if (toupper((unsigned char)t->text[jj]) != uppers[ii][jj]) {
                        match = 0; break;
                    }
                }
                if (match) { is_agg_call = 1; break; }
            }
        }
        if (is_agg_call) {
            char upname[16];
            u64 ul = t->text_len < sizeof(upname) - 1
                     ? t->text_len : sizeof(upname) - 1;
            for (u64 i = 0; i < ul; i++) {
                upname[i] = (char)toupper((unsigned char)t->text[i]);
            }
            upname[ul] = '\0';
            advance(p); advance(p);            /* FN ( */
            char arg[64]; arg[0] = '\0';
            const sql_token *a = peek(p);
            if (tok_is(a, SQL_TOK_STAR)) {
                arg[0] = '*'; arg[1] = '\0'; advance(p);
            } else if (a && (tok_is(a, SQL_TOK_IDENT) || tok_is(a, SQL_TOK_KEYWORD))) {
                u64 al = a->text_len < sizeof(arg) - 1 ? a->text_len : sizeof(arg) - 1;
                memcpy(arg, a->text, al); arg[al] = '\0';
                advance(p);
            }
            if (tok_is(peek(p), SQL_TOK_RPAREN)) advance(p);
            char ref_text[128];
            snprintf(ref_text, sizeof(ref_text), "%s %s",
                      upname, arg[0] ? arg : "*");
            rhs = ast_new(SQL_AST_AGG_REF, ref_text);
        } else {
            rhs = ast_new(SQL_AST_COLUMN_REF, t->text);
            advance(p);
        }
    } else if (tok_is(t, SQL_TOK_NUMBER) || tok_is(t, SQL_TOK_STRING)
               || tok_is(t, SQL_TOK_PLACEHOLDER)) {
        rhs = ast_new(SQL_AST_LITERAL, t->text);
        advance(p);
    } else {
        sql_ast_destroy(lhs);
        return NULL;
    }
    if (!rhs) { sql_ast_destroy(lhs); return NULL; }

    sql_ast *node = ast_new(SQL_AST_BINARY_OP, op);
    if (!node) { sql_ast_destroy(lhs); sql_ast_destroy(rhs); return NULL; }
    node->left  = lhs;
    node->right = rhs;
    return node;
}

static sql_ast *parse_condition(parser *p) {
    sql_ast *left = parse_primary_condition(p);
    if (!left) return NULL;

    for (;;) {
        const sql_token *t = peek(p);
        if (tok_is_keyword(t, "AND")) {
            advance(p);
            sql_ast *right = parse_primary_condition(p);
            if (!right) { sql_ast_destroy(left); return NULL; }
            sql_ast *node = ast_new(SQL_AST_AND, "AND");
            if (!node) { sql_ast_destroy(left); sql_ast_destroy(right); return NULL; }
            node->left  = left;
            node->right = right;
            left = node;
        } else if (tok_is_keyword(t, "OR")) {
            advance(p);
            sql_ast *right = parse_primary_condition(p);
            if (!right) { sql_ast_destroy(left); return NULL; }
            sql_ast *node = ast_new(SQL_AST_OR, "OR");
            if (!node) { sql_ast_destroy(left); sql_ast_destroy(right); return NULL; }
            node->left  = left;
            node->right = right;
            left = node;
        } else {
            break;
        }
    }
    return left;
}

/* ---- Statement parsers ---- */

static sql_ast *parse_select(parser *p) {
    /* SELECT already consumed */
    sql_ast *node = ast_new(SQL_AST_SELECT, NULL);
    if (!node) return NULL;

    /* DISTINCT */
    const sql_token *t = peek(p);
    if (tok_is_keyword(t, "DISTINCT")) {
        advance(p);
        /* mark in text field */
        free(node->text);
        node->text = malloc(9);
        if (node->text) strcpy(node->text, "DISTINCT");
    }

    /* columns: * or comma-separated */
    t = peek(p);
    if (tok_is(t, SQL_TOK_STAR)) {
        sql_ast *col = ast_new(SQL_AST_COLUMN_REF, "*");
        if (!col) { sql_ast_destroy(node); return NULL; }
        if (ast_add_child(node, col) < 0) { sql_ast_destroy(node); return NULL; }
        advance(p);
    } else {
        for (;;) {
            t = peek(p);
            if (!t || t->type == SQL_TOK_EOF) break;
            if (tok_is_keyword(t, "FROM")) break;
            if (tok_is(t, SQL_TOK_IDENT) || tok_is(t, SQL_TOK_KEYWORD)) {
                /* Possible aggregate: FUNC( arg ) where arg is * or col. */
                char name_buf[64]; name_buf[0] = '\0';
                u64 nl = t->text_len < sizeof(name_buf) - 1
                         ? t->text_len : sizeof(name_buf) - 1;
                memcpy(name_buf, t->text, nl); name_buf[nl] = '\0';
                /* Look one ahead WITHOUT advancing; if it's LPAREN and
                 * the name is a supported aggregate, consume the full
                 * function call. */
                int is_agg = 0;
                if (p->pos + 1 < p->tokens->count
                    && p->tokens->tokens[p->pos + 1].type == SQL_TOK_LPAREN) {
                    const char *uppers[] = {"COUNT","SUM","MIN","MAX","AVG",NULL};
                    u64 name_len = strlen(name_buf);
                    for (int ii = 0; uppers[ii]; ii++) {
                        u64 kl = (u64)strlen(uppers[ii]);
                        if (name_len != kl) continue;
                        int match = 1;
                        for (u64 jj = 0; jj < kl; jj++) {
                            if (toupper((unsigned char)name_buf[jj]) != uppers[ii][jj]) {
                                match = 0; break;
                            }
                        }
                        if (match) { is_agg = 1; break; }
                    }
                }
                if (is_agg) {
                    advance(p);              /* function name */
                    advance(p);              /* ( */
                    char arg[64]; arg[0] = '\0';
                    t = peek(p);
                    if (tok_is(t, SQL_TOK_STAR)) {
                        arg[0] = '*'; arg[1] = '\0';
                        advance(p);
                    } else if (t && (tok_is(t, SQL_TOK_IDENT)
                                      || tok_is(t, SQL_TOK_KEYWORD))) {
                        u64 al = t->text_len < sizeof(arg) - 1
                                  ? t->text_len : sizeof(arg) - 1;
                        memcpy(arg, t->text, al); arg[al] = '\0';
                        advance(p);
                    }
                    if (tok_is(peek(p), SQL_TOK_RPAREN)) advance(p);

                    /* Upper-case the function name into a canonical form. */
                    char upname[64];
                    u64 ul = strlen(name_buf);
                    for (u64 ii = 0; ii < ul && ii < sizeof(upname) - 1; ii++) {
                        upname[ii] = (char)toupper((unsigned char)name_buf[ii]);
                    }
                    upname[ul] = '\0';
                    char agg_text[256];
                    snprintf(agg_text, sizeof(agg_text),
                              "__AGG__ %s %s", upname, arg[0] ? arg : "*");
                    sql_ast *col = ast_new(SQL_AST_COLUMN_REF, agg_text);
                    if (!col) { sql_ast_destroy(node); return NULL; }
                    if (ast_add_child(node, col) < 0) {
                        sql_ast_destroy(node); return NULL;
                    }
                    /* optional alias */
                    if (tok_is_keyword(peek(p), "AS")) {
                        advance(p);
                        if (tok_is(peek(p), SQL_TOK_IDENT)
                            || tok_is(peek(p), SQL_TOK_KEYWORD)) advance(p);
                    }
                    t = peek(p);
                    if (tok_is(t, SQL_TOK_COMMA)) { advance(p); continue; }
                    else break;
                }

                sql_ast *col = ast_new(SQL_AST_COLUMN_REF, t->text);
                if (!col) { sql_ast_destroy(node); return NULL; }
                advance(p);
                /* handle alias: col AS alias */
                t = peek(p);
                if (tok_is_keyword(t, "AS")) {
                    advance(p);
                    t = peek(p);
                    /* alias ignored in AST text — append to column text */
                    if (tok_is(t, SQL_TOK_IDENT) || tok_is(t, SQL_TOK_KEYWORD)) advance(p);
                }
                if (ast_add_child(node, col) < 0) { sql_ast_destroy(node); return NULL; }
                t = peek(p);
                if (tok_is(t, SQL_TOK_COMMA)) { advance(p); continue; }
            } else {
                break;
            }
        }
    }

    /* FROM table */
    t = peek(p);
    if (!tok_is_keyword(t, "FROM")) { sql_ast_destroy(node); return NULL; }
    advance(p);
    t = peek(p);
    if (!t || (!tok_is(t, SQL_TOK_IDENT) && !tok_is(t, SQL_TOK_KEYWORD))) {
        sql_ast_destroy(node); return NULL;
    }
    /* store table name — overwrite text (DISTINCT flag) by building combined */
    {
        char *table = malloc(t->text_len + 1);
        if (!table) { sql_ast_destroy(node); return NULL; }
        memcpy(table, t->text, t->text_len);
        table[t->text_len] = '\0';
        /* If DISTINCT was set, prepend it */
        if (node->text && strcmp(node->text, "DISTINCT") == 0) {
            u64 tl = t->text_len;
            char *combined = malloc(9 + 1 + tl + 1);
            if (!combined) { free(table); sql_ast_destroy(node); return NULL; }
            memcpy(combined, "DISTINCT ", 9);
            memcpy(combined + 9, table, tl);
            combined[9 + tl] = '\0';
            free(node->text);
            node->text = combined;
        } else {
            free(node->text);
            node->text = table;
            table = NULL;
        }
        if (table) free(table);
    }
    advance(p);

    /* WHERE */
    t = peek(p);
    if (tok_is_keyword(t, "WHERE")) {
        advance(p);
        sql_ast *cond = parse_condition(p);
        if (!cond) { sql_ast_destroy(node); return NULL; }
        if (ast_add_child(node, cond) < 0) { sql_ast_destroy(node); return NULL; }
    }

    /* GROUP BY col [, col, ...] — one SQL_AST_GROUP_BY child per
     * column name. Executor iterates them in order and builds
     * composite bucket keys. */
    t = peek(p);
    if (tok_is_keyword(t, "GROUP")) {
        advance(p);
        t = peek(p);
        if (tok_is_keyword(t, "BY")) advance(p);
        for (;;) {
            t = peek(p);
            if (!t || t->type == SQL_TOK_EOF || tok_is(t, SQL_TOK_SEMICOLON)) break;
            if (tok_is_keyword(t, "HAVING") || tok_is_keyword(t, "ORDER") || tok_is_keyword(t, "LIMIT")) break;
            if (tok_is(t, SQL_TOK_IDENT) || tok_is(t, SQL_TOK_KEYWORD)) {
                sql_ast *gb = ast_new(SQL_AST_GROUP_BY, t->text);
                if (gb) {
                    if (ast_add_child(node, gb) < 0) { sql_ast_destroy(node); return NULL; }
                }
            }
            advance(p);
            t = peek(p);
            if (tok_is(t, SQL_TOK_COMMA)) advance(p); else break;
        }
    }

    /* HAVING — a condition subtree filters produced groups. Attached
     * as a SQL_AST_HAVING node whose left child is the parsed
     * condition so the executor can tell HAVING from WHERE. */
    t = peek(p);
    if (tok_is_keyword(t, "HAVING")) {
        advance(p);
        sql_ast *hcond = parse_condition(p);
        if (hcond) {
            sql_ast *hv = ast_new(SQL_AST_HAVING, NULL);
            if (!hv) { sql_ast_destroy(hcond); sql_ast_destroy(node); return NULL; }
            hv->left = hcond;
            if (ast_add_child(node, hv) < 0) { sql_ast_destroy(node); return NULL; }
        }
    }

    /* ORDER BY col[, col, ...] [ASC|DESC] — multi-column, per-column
     * direction. Each clause emits one SQL_AST_ORDER_BY child with
     * text = "col[ ASC|DESC]"; the executor processes them in order
     * (stable composite sort). */
    t = peek(p);
    if (tok_is_keyword(t, "ORDER")) {
        advance(p);
        t = peek(p);
        if (tok_is_keyword(t, "BY")) advance(p);
        for (;;) {
            t = peek(p);
            if (!t || (!tok_is(t, SQL_TOK_IDENT) && !tok_is(t, SQL_TOK_KEYWORD))) break;
            char order_text[512];
            int off = 0;
            off += snprintf(order_text + off, sizeof(order_text) - (size_t)off, "%s", t->text);
            advance(p);
            t = peek(p);
            if (tok_is_keyword(t, "ASC")) {
                off += snprintf(order_text + off, sizeof(order_text) - (size_t)off, " ASC");
                advance(p);
            } else if (tok_is_keyword(t, "DESC")) {
                off += snprintf(order_text + off, sizeof(order_text) - (size_t)off, " DESC");
                advance(p);
            }
            sql_ast *ob = ast_new(SQL_AST_ORDER_BY, order_text);
            if (!ob) { sql_ast_destroy(node); return NULL; }
            if (ast_add_child(node, ob) < 0) { sql_ast_destroy(node); return NULL; }
            t = peek(p);
            if (!tok_is(t, SQL_TOK_COMMA)) break;
            advance(p);
        }
    }

    /* LIMIT N [OFFSET M]  or  LIMIT M, N (sqlite compat form). */
    t = peek(p);
    if (tok_is_keyword(t, "LIMIT")) {
        advance(p);
        t = peek(p);
        if (t && tok_is(t, SQL_TOK_NUMBER)) {
            char first[64]; u64 fl = t->text_len;
            if (fl >= sizeof(first)) fl = sizeof(first) - 1;
            memcpy(first, t->text, fl); first[fl] = '\0';
            advance(p);
            t = peek(p);
            if (tok_is(t, SQL_TOK_COMMA)) {
                /* `LIMIT offset, count` — first is OFFSET, next is LIMIT. */
                advance(p);
                t = peek(p);
                if (t && tok_is(t, SQL_TOK_NUMBER)) {
                    sql_ast *off_n = ast_new(SQL_AST_OFFSET, first);
                    if (!off_n) { sql_ast_destroy(node); return NULL; }
                    if (ast_add_child(node, off_n) < 0) { sql_ast_destroy(node); return NULL; }
                    sql_ast *lim = ast_new(SQL_AST_LIMIT, t->text);
                    if (!lim) { sql_ast_destroy(node); return NULL; }
                    if (ast_add_child(node, lim) < 0) { sql_ast_destroy(node); return NULL; }
                    advance(p);
                }
            } else {
                sql_ast *lim = ast_new(SQL_AST_LIMIT, first);
                if (!lim) { sql_ast_destroy(node); return NULL; }
                if (ast_add_child(node, lim) < 0) { sql_ast_destroy(node); return NULL; }
                if (tok_is_keyword(peek(p), "OFFSET")) {
                    advance(p);
                    t = peek(p);
                    if (t && tok_is(t, SQL_TOK_NUMBER)) {
                        sql_ast *off_n = ast_new(SQL_AST_OFFSET, t->text);
                        if (!off_n) { sql_ast_destroy(node); return NULL; }
                        if (ast_add_child(node, off_n) < 0) { sql_ast_destroy(node); return NULL; }
                        advance(p);
                    }
                }
            }
        }
    }

    return node;
}

static sql_ast *parse_insert(parser *p) {
    /* INSERT already consumed. SQLite-style conflict clause: optional
     * "OR REPLACE|IGNORE|ABORT|ROLLBACK|FAIL". We support REPLACE
     * (upsert) and treat everything else as plain INSERT. */
    int or_replace = 0;
    if (tok_is_keyword(peek(p), "OR")) {
        advance(p);
        const sql_token *t = peek(p);
        /* REPLACE / IGNORE / ABORT / ROLLBACK / FAIL aren't in the
         * keyword set — they arrive as IDENT. Match case-insensitively. */
        if (t && (tok_is(t, SQL_TOK_IDENT) || tok_is(t, SQL_TOK_KEYWORD))) {
            int is_replace = (t->text_len == 7);
            if (is_replace) {
                for (int i = 0; i < 7; i++) {
                    if (toupper((unsigned char)t->text[i]) != "REPLACE"[i]) {
                        is_replace = 0; break;
                    }
                }
            }
            if (is_replace) or_replace = 1;
            advance(p);
        }
    }

    const sql_token *t = peek(p);
    if (!tok_is_keyword(t, "INTO")) return NULL;
    advance(p);

    t = peek(p);
    if (!t || (!tok_is(t, SQL_TOK_IDENT) && !tok_is(t, SQL_TOK_KEYWORD))) return NULL;
    sql_ast *node = ast_new(or_replace ? SQL_AST_INSERT_OR_REPLACE : SQL_AST_INSERT,
                             t->text);
    if (!node) return NULL;
    advance(p);

    /* optional column list */
    t = peek(p);
    if (tok_is(t, SQL_TOK_LPAREN)) {
        advance(p);
        for (;;) {
            t = peek(p);
            if (!t || tok_is(t, SQL_TOK_RPAREN)) break;
            if (tok_is(t, SQL_TOK_IDENT) || tok_is(t, SQL_TOK_KEYWORD)) {
                sql_ast *col = ast_new(SQL_AST_COLUMN_REF, t->text);
                if (!col) { sql_ast_destroy(node); return NULL; }
                if (ast_add_child(node, col) < 0) { sql_ast_destroy(node); return NULL; }
                advance(p);
            }
            t = peek(p);
            if (tok_is(t, SQL_TOK_COMMA)) advance(p);
        }
        if (!tok_is(peek(p), SQL_TOK_RPAREN)) { sql_ast_destroy(node); return NULL; }
        advance(p);
    }

    /* VALUES */
    t = peek(p);
    if (!tok_is_keyword(t, "VALUES")) { sql_ast_destroy(node); return NULL; }
    advance(p);

    t = peek(p);
    if (!tok_is(t, SQL_TOK_LPAREN)) { sql_ast_destroy(node); return NULL; }
    advance(p);

    for (;;) {
        t = peek(p);
        if (!t || tok_is(t, SQL_TOK_RPAREN)) break;

        /* Function call form: IDENT/KEYWORD followed by LPAREN.
         * Example: datetime('now'). Emit SQL_AST_FUNC_CALL with the
         * function name in text and each string/number arg as a
         * SQL_AST_LITERAL child. */
        if ((tok_is(t, SQL_TOK_IDENT) || tok_is(t, SQL_TOK_KEYWORD))
            && p->pos + 1 < p->tokens->count
            && tok_is(&p->tokens->tokens[p->pos + 1], SQL_TOK_LPAREN)) {
            sql_ast *call = ast_new(SQL_AST_FUNC_CALL, t->text);
            if (!call) { sql_ast_destroy(node); return NULL; }
            advance(p);   /* function name */
            advance(p);   /* ( */
            for (;;) {
                const sql_token *at = peek(p);
                if (!at || tok_is(at, SQL_TOK_RPAREN)) break;
                if (tok_is(at, SQL_TOK_STRING) || tok_is(at, SQL_TOK_NUMBER)
                    || tok_is(at, SQL_TOK_IDENT) || tok_is(at, SQL_TOK_KEYWORD)
                    || tok_is(at, SQL_TOK_PLACEHOLDER)) {
                    sql_ast *arg = ast_new(SQL_AST_LITERAL, at->text);
                    if (!arg) { sql_ast_destroy(call); sql_ast_destroy(node); return NULL; }
                    if (ast_add_child(call, arg) < 0) {
                        sql_ast_destroy(call); sql_ast_destroy(node); return NULL;
                    }
                    advance(p);
                }
                if (tok_is(peek(p), SQL_TOK_COMMA)) advance(p);
            }
            if (tok_is(peek(p), SQL_TOK_RPAREN)) advance(p);
            if (ast_add_child(node, call) < 0) { sql_ast_destroy(node); return NULL; }
        }
        else if (tok_is(t, SQL_TOK_NUMBER) || tok_is(t, SQL_TOK_STRING) ||
                 tok_is(t, SQL_TOK_IDENT) || tok_is_keyword(t, "NULL") ||
                 tok_is(t, SQL_TOK_PLACEHOLDER)) {
            sql_ast *val = ast_new(SQL_AST_LITERAL, t->text);
            if (!val) { sql_ast_destroy(node); return NULL; }
            if (ast_add_child(node, val) < 0) { sql_ast_destroy(node); return NULL; }
            advance(p);
        }
        t = peek(p);
        if (tok_is(t, SQL_TOK_COMMA)) advance(p);
    }
    if (tok_is(peek(p), SQL_TOK_RPAREN)) advance(p);

    return node;
}

static sql_ast *parse_update(parser *p) {
    /* UPDATE already consumed */
    const sql_token *t = peek(p);
    if (!t || (!tok_is(t, SQL_TOK_IDENT) && !tok_is(t, SQL_TOK_KEYWORD))) return NULL;
    sql_ast *node = ast_new(SQL_AST_UPDATE, t->text);
    if (!node) return NULL;
    advance(p);

    t = peek(p);
    if (!tok_is_keyword(t, "SET")) { sql_ast_destroy(node); return NULL; }
    advance(p);

    /* assignments: col = val [, col = val ...] */
    for (;;) {
        t = peek(p);
        if (!t || t->type == SQL_TOK_EOF) break;
        if (tok_is_keyword(t, "WHERE") || tok_is(t, SQL_TOK_SEMICOLON)) break;
        if (!tok_is(t, SQL_TOK_IDENT) && !tok_is(t, SQL_TOK_KEYWORD)) break;

        sql_ast *assign = ast_new(SQL_AST_ASSIGNMENT, t->text);
        if (!assign) { sql_ast_destroy(node); return NULL; }
        advance(p);

        t = peek(p);
        if (!tok_is(t, SQL_TOK_OPERATOR) || strcmp(t->text, "=") != 0) {
            sql_ast_destroy(assign); sql_ast_destroy(node); return NULL;
        }
        advance(p);

        t = peek(p);
        if (!t) { sql_ast_destroy(assign); sql_ast_destroy(node); return NULL; }
        sql_ast *val = ast_new(SQL_AST_LITERAL, t->text);
        if (!val) { sql_ast_destroy(assign); sql_ast_destroy(node); return NULL; }
        assign->left = val;
        advance(p);

        if (ast_add_child(node, assign) < 0) { sql_ast_destroy(node); return NULL; }

        t = peek(p);
        if (tok_is(t, SQL_TOK_COMMA)) { advance(p); continue; }
        break;
    }

    /* WHERE */
    t = peek(p);
    if (tok_is_keyword(t, "WHERE")) {
        advance(p);
        sql_ast *cond = parse_condition(p);
        if (!cond) { sql_ast_destroy(node); return NULL; }
        if (ast_add_child(node, cond) < 0) { sql_ast_destroy(node); return NULL; }
    }

    return node;
}

static sql_ast *parse_delete(parser *p) {
    /* DELETE already consumed */
    const sql_token *t = peek(p);
    if (!tok_is_keyword(t, "FROM")) return NULL;
    advance(p);

    t = peek(p);
    if (!t || (!tok_is(t, SQL_TOK_IDENT) && !tok_is(t, SQL_TOK_KEYWORD))) return NULL;
    sql_ast *node = ast_new(SQL_AST_DELETE, t->text);
    if (!node) return NULL;
    advance(p);

    /* WHERE */
    t = peek(p);
    if (tok_is_keyword(t, "WHERE")) {
        advance(p);
        sql_ast *cond = parse_condition(p);
        if (!cond) { sql_ast_destroy(node); return NULL; }
        if (ast_add_child(node, cond) < 0) { sql_ast_destroy(node); return NULL; }
    }

    return node;
}

static sql_ast *parse_create_table(parser *p) {
    /* CREATE TABLE already consumed */
    const sql_token *t = peek(p);
    int if_not_exists = 0;
    if (tok_is_keyword(t, "IF")) {
        advance(p);
        t = peek(p);
        if (tok_is_keyword(t, "NOT")) advance(p);
        t = peek(p);
        if (tok_is_keyword(t, "EXISTS")) advance(p);
        if_not_exists = 1;
    }

    t = peek(p);
    if (!t || (!tok_is(t, SQL_TOK_IDENT) && !tok_is(t, SQL_TOK_KEYWORD))) return NULL;

    char *name = NULL;
    if (if_not_exists) {
        u64 len = t->text_len + 15;
        name = malloc(len + 1);
        if (!name) return NULL;
        snprintf(name, len + 1, "IF NOT EXISTS %s", t->text);
    } else {
        name = malloc(t->text_len + 1);
        if (!name) return NULL;
        memcpy(name, t->text, t->text_len);
        name[t->text_len] = '\0';
    }
    sql_ast *node = ast_new(SQL_AST_CREATE_TABLE, name);
    free(name);
    if (!node) return NULL;
    advance(p);

    /* ( col_def [, col_def] ) */
    t = peek(p);
    if (!tok_is(t, SQL_TOK_LPAREN)) { sql_ast_destroy(node); return NULL; }
    advance(p);

    for (;;) {
        t = peek(p);
        if (!t || tok_is(t, SQL_TOK_RPAREN)) break;

        if (!tok_is(t, SQL_TOK_IDENT) && !tok_is(t, SQL_TOK_KEYWORD)) break;

        /* Optional leading "CONSTRAINT name" — skip and re-peek. */
        if (tok_is_keyword(t, "CONSTRAINT")) {
            advance(p);
            t = peek(p);
            if (tok_is(t, SQL_TOK_IDENT) || tok_is(t, SQL_TOK_KEYWORD)) advance(p);
            t = peek(p);
            if (!t) break;
        }

        /* Table-level constraints (not column defs) —
         *   UNIQUE(c1, c2, ...)      → emit "__UNIQUE__ c1,c2,..." child
         *   PRIMARY KEY(c1, ...)     → ignored (rowid-based anyway)
         *   FOREIGN KEY(c) REFERENCES ... → ignored (parse-and-skip;
         *                                             real enforcement
         *                                             comes later)
         *   CHECK (expr)             → ignored */
        if (tok_is_keyword(t, "UNIQUE")) {
            advance(p);
            if (tok_is(peek(p), SQL_TOK_LPAREN)) {
                char cols[512];
                if (parse_paren_col_list(p, cols, sizeof(cols)) != 0) {
                    sql_ast_destroy(node);
                    return NULL;
                }
                char tl_text[640];
                snprintf(tl_text, sizeof(tl_text), "__UNIQUE__ %s", cols);
                sql_ast *cd = ast_new(SQL_AST_COLUMN_DEF, tl_text);
                if (!cd) { sql_ast_destroy(node); return NULL; }
                if (ast_add_child(node, cd) < 0) { sql_ast_destroy(node); return NULL; }
            }
            t = peek(p);
            if (tok_is(t, SQL_TOK_COMMA)) { advance(p); continue; }
            if (tok_is(t, SQL_TOK_RPAREN)) break;
            continue;
        }
        if (tok_is_keyword(t, "PRIMARY")) {
            /* Could be a table-level "PRIMARY KEY (cols)" — look ahead
             * for a paren after KEY. If present, skip; otherwise fall
             * through (shouldn't happen at top of iteration, but be
             * defensive). */
            advance(p);
            if (tok_is_keyword(peek(p), "KEY")) advance(p);
            if (tok_is(peek(p), SQL_TOK_LPAREN)) {
                skip_paren_expr(p);
            }
            t = peek(p);
            if (tok_is(t, SQL_TOK_COMMA)) { advance(p); continue; }
            if (tok_is(t, SQL_TOK_RPAREN)) break;
            continue;
        }
        if (tok_is_keyword(t, "FOREIGN")) {
            advance(p);
            if (tok_is_keyword(peek(p), "KEY")) advance(p);
            /* Capture the local column(s). We currently only support
             * single-column FKs — multi-column composite FKs parse
             * cleanly but only the first column is recorded. */
            char local_cols[256]; local_cols[0] = '\0';
            if (tok_is(peek(p), SQL_TOK_LPAREN)) {
                parse_paren_col_list(p, local_cols, sizeof(local_cols));
            }
            char target_tbl[128]; target_tbl[0] = '\0';
            char target_cols[128]; target_cols[0] = '\0';
            if (tok_is_keyword(peek(p), "REFERENCES")) {
                advance(p);
                const sql_token *tt = peek(p);
                if (tt && (tok_is(tt, SQL_TOK_IDENT) || tok_is(tt, SQL_TOK_KEYWORD))) {
                    u64 tl = tt->text_len;
                    if (tl >= sizeof(target_tbl)) tl = sizeof(target_tbl) - 1;
                    memcpy(target_tbl, tt->text, tl);
                    target_tbl[tl] = '\0';
                    advance(p);
                }
                if (tok_is(peek(p), SQL_TOK_LPAREN)) {
                    parse_paren_col_list(p, target_cols, sizeof(target_cols));
                }
                /* ON DELETE <action>  /  ON UPDATE <action>
                 * Recognised actions: NO ACTION / RESTRICT / CASCADE /
                 * SET NULL / SET DEFAULT. Only ON DELETE action is
                 * captured today; ON UPDATE action is parsed and
                 * discarded. */
                char on_delete[16]; on_delete[0] = '\0';
                while (tok_is_keyword(peek(p), "ON")) {
                    advance(p);
                    int is_delete = tok_is_keyword(peek(p), "DELETE");
                    /* consume DELETE / UPDATE token */
                    if (tok_is(peek(p), SQL_TOK_IDENT) || tok_is(peek(p), SQL_TOK_KEYWORD)) advance(p);
                    /* action is 1 or 2 tokens: CASCADE / RESTRICT /
                     * "SET NULL" / "SET DEFAULT" / "NO ACTION". */
                    const sql_token *a1 = peek(p);
                    char buf[32]; buf[0] = '\0';
                    if (a1 && (tok_is(a1, SQL_TOK_IDENT) || tok_is(a1, SQL_TOK_KEYWORD))) {
                        u64 al = a1->text_len < sizeof(buf) - 1 ? a1->text_len : sizeof(buf) - 1;
                        memcpy(buf, a1->text, al); buf[al] = '\0';
                        advance(p);
                        const sql_token *a2 = peek(p);
                        if (a2 && (tok_is(a2, SQL_TOK_IDENT) || tok_is(a2, SQL_TOK_KEYWORD))) {
                            /* peek for "SET NULL" / "SET DEFAULT" / "NO ACTION" */
                            int want_second = 0;
                            u64 u = 0;
                            while (u < strlen(buf)) {
                                /* uppercase compare */
                                u++;
                            }
                            /* Use simple prefix check. */
                            if ((buf[0] == 'S' || buf[0] == 's')
                                && (buf[1] == 'E' || buf[1] == 'e')
                                && (buf[2] == 'T' || buf[2] == 't')
                                && buf[3] == '\0') want_second = 1;
                            if ((buf[0] == 'N' || buf[0] == 'n')
                                && (buf[1] == 'O' || buf[1] == 'o')
                                && buf[2] == '\0') want_second = 1;
                            if (want_second) {
                                strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
                                u64 al2 = a2->text_len < sizeof(buf) - strlen(buf) - 1
                                          ? a2->text_len : sizeof(buf) - strlen(buf) - 1;
                                strncat(buf, a2->text, al2);
                                advance(p);
                            }
                        }
                    }
                    if (is_delete && buf[0]) {
                        /* Normalise to one-word form: RESTRICT,
                         * CASCADE, SETNULL, SETDEFAULT, NOACTION. */
                        if ((buf[0]=='R'||buf[0]=='r')) snprintf(on_delete, sizeof(on_delete), "%s", "RESTRICT");
                        else if ((buf[0]=='C'||buf[0]=='c')) snprintf(on_delete, sizeof(on_delete), "%s", "CASCADE");
                        else if ((buf[0]=='S'||buf[0]=='s') && strlen(buf) >= 7
                                 && (buf[4]=='N'||buf[4]=='n')) snprintf(on_delete, sizeof(on_delete), "%s", "SETNULL");
                        else if ((buf[0]=='S'||buf[0]=='s')) snprintf(on_delete, sizeof(on_delete), "%s", "SETDEFAULT");
                        else snprintf(on_delete, sizeof(on_delete), "%s", "NOACTION");
                    }
                }
                if (!on_delete[0]) snprintf(on_delete, sizeof(on_delete), "%s", "NOACTION");
                /* Emit composite-aware FK marker. local_cols and
                 * target_cols are comma-joined (no spaces) — executor
                 * parses them back into arrays. */
                if (local_cols[0] && target_tbl[0] && target_cols[0]) {
                    char fk_text[640];
                    snprintf(fk_text, sizeof(fk_text), "__FK__ %s %s %s %s",
                              local_cols, target_tbl, target_cols, on_delete);
                    sql_ast *fk = ast_new(SQL_AST_COLUMN_DEF, fk_text);
                    if (fk) {
                        if (ast_add_child(node, fk) < 0) { sql_ast_destroy(node); return NULL; }
                    }
                }
            }
            t = peek(p);
            if (tok_is(t, SQL_TOK_COMMA)) { advance(p); continue; }
            if (tok_is(t, SQL_TOK_RPAREN)) break;
            continue;
        }
        if (tok_is_keyword(t, "CHECK")) {
            advance(p);
            if (tok_is(peek(p), SQL_TOK_LPAREN)) skip_paren_expr(p);
            t = peek(p);
            if (tok_is(t, SQL_TOK_COMMA)) { advance(p); continue; }
            if (tok_is(t, SQL_TOK_RPAREN)) break;
            continue;
        }

        /* column name */
        char col_text[512];
        char cur_col_name[128];
        int off = 0;
        /* Preserve a copy of the name for per-column REFERENCES rewrite. */
        u64 nml = t->text_len < sizeof(cur_col_name) - 1 ? t->text_len
                                                          : sizeof(cur_col_name) - 1;
        memcpy(cur_col_name, t->text, nml); cur_col_name[nml] = '\0';
        off += snprintf(col_text + off, sizeof(col_text) - (size_t)off, "%s", t->text);
        advance(p);

        /* type */
        t = peek(p);
        if (t && (tok_is(t, SQL_TOK_IDENT) || tok_is(t, SQL_TOK_KEYWORD))) {
            off += snprintf(col_text + off, sizeof(col_text) - (size_t)off, " %s", t->text);
            advance(p);
            /* optional VARCHAR(n) */
            t = peek(p);
            if (tok_is(t, SQL_TOK_LPAREN)) {
                advance(p);
                t = peek(p);
                if (t && tok_is(t, SQL_TOK_NUMBER)) {
                    off += snprintf(col_text + off, sizeof(col_text) - (size_t)off, "(%s)", t->text);
                    advance(p);
                }
                t = peek(p);
                if (tok_is(t, SQL_TOK_RPAREN)) advance(p);
            }
        }

        /* Per-column attribute loop. Order-independent; accepts any
         * combination sqlite would: PRIMARY KEY, NOT NULL, UNIQUE,
         * DEFAULT (expr) / DEFAULT lit, REFERENCES tab(col) [ON ...],
         * COLLATE name, CHECK (expr). Only PRIMARY KEY / NOT NULL /
         * UNIQUE are recorded in col_text; the rest are parsed and
         * discarded. */
        for (;;) {
            t = peek(p);
            if (!t) break;
            if (tok_is_keyword(t, "PRIMARY")) {
                advance(p);
                if (tok_is_keyword(peek(p), "KEY")) advance(p);
                off += snprintf(col_text + off, sizeof(col_text) - (size_t)off, " PRIMARY KEY");
                continue;
            }
            if (tok_is_keyword(t, "NOT")) {
                advance(p);
                if (tok_is_keyword(peek(p), "NULL")) advance(p);
                off += snprintf(col_text + off, sizeof(col_text) - (size_t)off, " NOT NULL");
                continue;
            }
            if (tok_is_keyword(t, "UNIQUE")) {
                advance(p);
                off += snprintf(col_text + off, sizeof(col_text) - (size_t)off, " UNIQUE");
                continue;
            }
            if (tok_is_keyword(t, "DEFAULT")) {
                advance(p);
                /* Skip one value — either (expr), a literal, or an
                 * unquoted keyword like CURRENT_TIMESTAMP. */
                t = peek(p);
                if (tok_is(t, SQL_TOK_LPAREN)) {
                    skip_paren_expr(p);
                } else if (t) {
                    advance(p);
                    /* Handle "function_name(args)" form like datetime('now')
                     * where DEFAULT is followed by bare function call. */
                    if (tok_is(peek(p), SQL_TOK_LPAREN)) skip_paren_expr(p);
                }
                continue;
            }
            if (tok_is_keyword(t, "REFERENCES")) {
                advance(p);
                char ref_tbl[128]; ref_tbl[0] = '\0';
                char ref_cols[128]; ref_cols[0] = '\0';
                const sql_token *tt = peek(p);
                if (tt && (tok_is(tt, SQL_TOK_IDENT) || tok_is(tt, SQL_TOK_KEYWORD))) {
                    u64 tl = tt->text_len;
                    if (tl >= sizeof(ref_tbl)) tl = sizeof(ref_tbl) - 1;
                    memcpy(ref_tbl, tt->text, tl); ref_tbl[tl] = '\0';
                    advance(p);
                }
                if (tok_is(peek(p), SQL_TOK_LPAREN)) {
                    parse_paren_col_list(p, ref_cols, sizeof(ref_cols));
                }
                char on_delete[16]; on_delete[0] = '\0';
                while (tok_is_keyword(peek(p), "ON")) {
                    advance(p);
                    int is_delete = tok_is_keyword(peek(p), "DELETE");
                    if (tok_is(peek(p), SQL_TOK_IDENT) || tok_is(peek(p), SQL_TOK_KEYWORD)) advance(p);
                    const sql_token *a1 = peek(p);
                    char buf[32]; buf[0] = '\0';
                    if (a1 && (tok_is(a1, SQL_TOK_IDENT) || tok_is(a1, SQL_TOK_KEYWORD))) {
                        u64 al = a1->text_len < sizeof(buf) - 1 ? a1->text_len : sizeof(buf) - 1;
                        memcpy(buf, a1->text, al); buf[al] = '\0';
                        advance(p);
                        const sql_token *a2 = peek(p);
                        if (a2 && (tok_is(a2, SQL_TOK_IDENT) || tok_is(a2, SQL_TOK_KEYWORD))) {
                            int want_second = 0;
                            if ((buf[0]=='S'||buf[0]=='s') && (buf[1]=='E'||buf[1]=='e')
                                && (buf[2]=='T'||buf[2]=='t') && buf[3]=='\0') want_second = 1;
                            if ((buf[0]=='N'||buf[0]=='n') && (buf[1]=='O'||buf[1]=='o')
                                && buf[2]=='\0') want_second = 1;
                            if (want_second) {
                                strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
                                u64 al2 = a2->text_len < sizeof(buf) - strlen(buf) - 1
                                          ? a2->text_len : sizeof(buf) - strlen(buf) - 1;
                                strncat(buf, a2->text, al2);
                                advance(p);
                            }
                        }
                    }
                    if (is_delete && buf[0]) {
                        if ((buf[0]=='R'||buf[0]=='r')) snprintf(on_delete, sizeof(on_delete), "%s", "RESTRICT");
                        else if ((buf[0]=='C'||buf[0]=='c')) snprintf(on_delete, sizeof(on_delete), "%s", "CASCADE");
                        else if ((buf[0]=='S'||buf[0]=='s') && strlen(buf) >= 7
                                 && (buf[4]=='N'||buf[4]=='n')) snprintf(on_delete, sizeof(on_delete), "%s", "SETNULL");
                        else if ((buf[0]=='S'||buf[0]=='s')) snprintf(on_delete, sizeof(on_delete), "%s", "SETDEFAULT");
                        else snprintf(on_delete, sizeof(on_delete), "%s", "NOACTION");
                    }
                }
                if (!on_delete[0]) snprintf(on_delete, sizeof(on_delete), "%s", "NOACTION");

                /* Emit FK marker child for this local column. Only the
                 * first comma-separated target column is used (matches
                 * cookbook's schema, composite FKs deferred). */
                if (cur_col_name[0] && ref_tbl[0] && ref_cols[0]) {
                    char lone_target[128];
                    const char *comma = strchr(ref_cols, ',');
                    u64 tll = comma ? (u64)(comma - ref_cols) : strlen(ref_cols);
                    if (tll >= sizeof(lone_target)) tll = sizeof(lone_target) - 1;
                    memcpy(lone_target, ref_cols, tll); lone_target[tll] = '\0';
                    char fk_text[512];
                    snprintf(fk_text, sizeof(fk_text), "__FK__ %s %s %s %s",
                              cur_col_name, ref_tbl, lone_target, on_delete);
                    sql_ast *fk = ast_new(SQL_AST_COLUMN_DEF, fk_text);
                    if (fk) {
                        if (ast_add_child(node, fk) < 0) {
                            sql_ast_destroy(node);
                            return NULL;
                        }
                    }
                }
                continue;
            }
            if (tok_is_keyword(t, "COLLATE")) {
                advance(p);
                if (tok_is(peek(p), SQL_TOK_IDENT) || tok_is(peek(p), SQL_TOK_KEYWORD)) advance(p);
                continue;
            }
            if (tok_is_keyword(t, "CHECK")) {
                advance(p);
                if (tok_is(peek(p), SQL_TOK_LPAREN)) skip_paren_expr(p);
                continue;
            }
            break;
        }

        sql_ast *cd = ast_new(SQL_AST_COLUMN_DEF, col_text);
        if (!cd) { sql_ast_destroy(node); return NULL; }
        if (ast_add_child(node, cd) < 0) { sql_ast_destroy(node); return NULL; }

        t = peek(p);
        if (tok_is(t, SQL_TOK_COMMA)) { advance(p); continue; }
    }

    if (tok_is(peek(p), SQL_TOK_RPAREN)) advance(p);

    return node;
}

static sql_ast *parse_drop_table(parser *p) {
    /* DROP TABLE already consumed */
    const sql_token *t = peek(p);
    int if_exists = 0;
    if (tok_is_keyword(t, "IF")) {
        advance(p);
        t = peek(p);
        if (tok_is_keyword(t, "EXISTS")) advance(p);
        if_exists = 1;
    }

    t = peek(p);
    if (!t || (!tok_is(t, SQL_TOK_IDENT) && !tok_is(t, SQL_TOK_KEYWORD))) return NULL;

    char *name = NULL;
    if (if_exists) {
        u64 len = t->text_len + 11;
        name = malloc(len + 1);
        if (!name) return NULL;
        snprintf(name, len + 1, "IF EXISTS %s", t->text);
    } else {
        name = malloc(t->text_len + 1);
        if (!name) return NULL;
        memcpy(name, t->text, t->text_len);
        name[t->text_len] = '\0';
    }
    sql_ast *node = ast_new(SQL_AST_DROP_TABLE, name);
    free(name);
    if (!node) return NULL;
    advance(p);

    return node;
}

/* ------------------------------------------------------------------ */
/*  sql_parse                                                          */
/* ------------------------------------------------------------------ */

APENNINES_API unsigned long sql_parse(sql_ast **out, const sql_token_list *tokens) {
    if (!out)    return 1;
    if (!tokens) return 2;
    *out = NULL;

    if (tokens->count == 0) return 3;

    parser p;
    p.tokens = tokens;
    p.pos    = 0;

    const sql_token *t = peek(&p);
    if (!t || t->type == SQL_TOK_EOF) return 3;

    sql_ast *result = NULL;

    if (tok_is_keyword(t, "SELECT")) {
        advance(&p);
        result = parse_select(&p);
    } else if (tok_is_keyword(t, "INSERT")) {
        advance(&p);
        result = parse_insert(&p);
    } else if (tok_is_keyword(t, "UPDATE")) {
        advance(&p);
        result = parse_update(&p);
    } else if (tok_is_keyword(t, "DELETE")) {
        advance(&p);
        result = parse_delete(&p);
    } else if (tok_is_keyword(t, "CREATE")) {
        advance(&p);
        t = peek(&p);
        /* Optional UNIQUE prefix on CREATE [UNIQUE] INDEX — recognised
         * and discarded (our index engine doesn't enforce UNIQUE-via-
         * index; table-level UNIQUE handles that separately). */
        if (tok_is_keyword(t, "UNIQUE")) { advance(&p); t = peek(&p); }
        if (tok_is_keyword(t, "INDEX")) {
            advance(&p);
            /* CREATE INDEX [IF NOT EXISTS] name ON table(col) */
            int ine = 0;
            if (tok_is_keyword(peek(&p), "IF")) {
                advance(&p);
                if (tok_is_keyword(peek(&p), "NOT")) advance(&p);
                if (tok_is_keyword(peek(&p), "EXISTS")) advance(&p);
                ine = 1;
            }
            const sql_token *nm = peek(&p);
            if (!nm || (!tok_is(nm, SQL_TOK_IDENT) && !tok_is(nm, SQL_TOK_KEYWORD))) return 3;
            char idx_name[128];
            u64 nl = nm->text_len < sizeof(idx_name) - 1 ? nm->text_len : sizeof(idx_name) - 1;
            memcpy(idx_name, nm->text, nl); idx_name[nl] = '\0';
            advance(&p);
            if (!tok_is_keyword(peek(&p), "ON")) return 3;
            advance(&p);
            const sql_token *tbn = peek(&p);
            if (!tbn || (!tok_is(tbn, SQL_TOK_IDENT) && !tok_is(tbn, SQL_TOK_KEYWORD))) return 3;
            char tbl[128];
            u64 tl = tbn->text_len < sizeof(tbl) - 1 ? tbn->text_len : sizeof(tbl) - 1;
            memcpy(tbl, tbn->text, tl); tbl[tl] = '\0';
            advance(&p);
            /* Column list — supports composite indexes "(a, b, c)". */
            char cols[256];
            if (parse_paren_col_list(&p, cols, sizeof(cols)) != 0) return 3;
            if (!cols[0]) return 3;
            char text[512];
            snprintf(text, sizeof(text), "%s%s %s %s",
                      ine ? "IF NOT EXISTS " : "", idx_name, tbl, cols);
            result = ast_new(SQL_AST_CREATE_INDEX, text);
        } else if (tok_is_keyword(t, "TABLE")) {
            advance(&p);
            result = parse_create_table(&p);
        } else {
            return 3;
        }
    } else if (tok_is_keyword(t, "DROP")) {
        advance(&p);
        t = peek(&p);
        if (tok_is_keyword(t, "TABLE")) {
            advance(&p);
            result = parse_drop_table(&p);
        } else if (tok_is_keyword(t, "INDEX")) {
            advance(&p);
            int ie = 0;
            if (tok_is_keyword(peek(&p), "IF")) {
                advance(&p);
                if (tok_is_keyword(peek(&p), "EXISTS")) advance(&p);
                ie = 1;
            }
            const sql_token *nm = peek(&p);
            if (!nm || (!tok_is(nm, SQL_TOK_IDENT) && !tok_is(nm, SQL_TOK_KEYWORD))) return 3;
            char text[192];
            snprintf(text, sizeof(text), "%s%.*s",
                      ie ? "IF EXISTS " : "",
                      (int)nm->text_len, nm->text);
            result = ast_new(SQL_AST_DROP_INDEX, text);
            advance(&p);
        } else {
            return 3;
        }
    } else {
        return 3;
    }

    if (!result) return 4;

    *out = result;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sql_ast_walk                                                       */
/* ------------------------------------------------------------------ */

static int walk_recursive(const sql_ast *node, sql_ast_visitor visitor,
                          void *ctx, int depth) {
    int rc = visitor(node, depth, ctx);
    if (rc) return rc;

    /* walk left/right (binary ops) */
    if (node->left) {
        rc = walk_recursive(node->left, visitor, ctx, depth + 1);
        if (rc) return rc;
    }
    if (node->right) {
        rc = walk_recursive(node->right, visitor, ctx, depth + 1);
        if (rc) return rc;
    }

    /* walk children array */
    for (u64 i = 0; i < node->child_count; ++i) {
        rc = walk_recursive(&node->children[i], visitor, ctx, depth + 1);
        if (rc) return rc;
    }
    return 0;
}

APENNINES_API unsigned long sql_ast_walk(const sql_ast *node,
                                          sql_ast_visitor visitor, void *ctx) {
    if (!node)    return 1;
    if (!visitor) return 2;
    walk_recursive(node, visitor, ctx, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sql_ast_destroy                                                    */
/* ------------------------------------------------------------------ */

static void destroy_recursive(sql_ast *node) {
    if (!node) return;

    if (node->left) {
        destroy_recursive(node->left);
        free(node->left);
        node->left = NULL;
    }
    if (node->right) {
        destroy_recursive(node->right);
        free(node->right);
        node->right = NULL;
    }
    for (u64 i = 0; i < node->child_count; ++i) {
        destroy_recursive(&node->children[i]);
    }
    free(node->children);
    node->children   = NULL;
    node->child_count = 0;
    free(node->text);
    node->text = NULL;
}

APENNINES_API unsigned long sql_ast_destroy(sql_ast *node) {
    if (!node) return 0;
    destroy_recursive(node);
    free(node);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sql_token_list_free                                                */
/* ------------------------------------------------------------------ */

APENNINES_API unsigned long sql_token_list_free(sql_token_list *list) {
    if (!list) return 0;
    for (u64 i = 0; i < list->count; ++i) {
        free(list->tokens[i].text);
    }
    free(list->tokens);
    list->tokens   = NULL;
    list->count    = 0;
    list->capacity = 0;
    return 0;
}
