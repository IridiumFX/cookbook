#ifndef APENNINES_T3_SQL_H
#define APENNINES_T3_SQL_H

#include "apennines/export.h"
#include "apennines/types.h"

/* ================================================================
 *  SQL Parser (Minimal) — tokenizer, AST, walker
 *
 *  Supports: SELECT, INSERT, UPDATE, DELETE, CREATE TABLE, DROP TABLE
 *  Limited to single-table queries (no JOINs).
 * ================================================================ */

/* ---- Token types ---- */
#define SQL_TOK_EOF        0
#define SQL_TOK_KEYWORD    1   /* SELECT, INSERT, etc. */
#define SQL_TOK_IDENT      2   /* table/column names */
#define SQL_TOK_NUMBER     3   /* integer or decimal literal */
#define SQL_TOK_STRING     4   /* 'quoted string' */
#define SQL_TOK_OPERATOR   5   /* =, <, >, <=, >=, <>, != */
#define SQL_TOK_COMMA      6
#define SQL_TOK_LPAREN     7
#define SQL_TOK_RPAREN     8
#define SQL_TOK_STAR       9   /* * */
#define SQL_TOK_SEMICOLON 10
#define SQL_TOK_DOT       11
#define SQL_TOK_PLACEHOLDER 12  /* ? or ?N (sqlite-style bind marker) */

typedef struct {
    int   type;         /* SQL_TOK_* */
    char *text;         /* token text (allocated) */
    u64   text_len;
    u64   offset;       /* byte offset in input */
} sql_token;

typedef struct {
    sql_token *tokens;
    u64        count;
    u64        capacity;
} sql_token_list;

/* ---- AST node types ---- */
#define SQL_AST_SELECT       1
#define SQL_AST_INSERT       2
#define SQL_AST_UPDATE       3
#define SQL_AST_DELETE       4
#define SQL_AST_CREATE_TABLE 5
#define SQL_AST_DROP_TABLE   6
#define SQL_AST_CREATE_INDEX 7   /* text: "[IF NOT EXISTS ]name ON table(col)" */
#define SQL_AST_DROP_INDEX   8   /* text: "[IF EXISTS ]name" */
#define SQL_AST_COLUMN_REF   10
#define SQL_AST_LITERAL      11
#define SQL_AST_BINARY_OP    12
#define SQL_AST_AND          13
#define SQL_AST_OR           14
#define SQL_AST_COLUMN_DEF   15
#define SQL_AST_ASSIGNMENT   16
#define SQL_AST_ORDER_BY     17   /* text = "col[ ASC|DESC]" */
#define SQL_AST_LIMIT        18   /* text = decimal count */
#define SQL_AST_OFFSET       19   /* text = decimal skip */
#define SQL_AST_GROUP_BY     20   /* text = column name */
#define SQL_AST_HAVING       21   /* a condition subtree, children of SELECT */
#define SQL_AST_AGG_REF      22   /* "FN arg" — a reference to an aggregate
                                   * value in an expression; rewritten at
                                   * prepare time into a COLUMN_REF against
                                   * the stmt's synthetic group-row table */
#define SQL_AST_INSERT_OR_REPLACE 23  /* sqlite INSERT OR REPLACE upsert.
                                       * text=table name. Semantics: on any
                                       * UNIQUE/PK collision, delete the
                                       * colliding row then insert the new. */
#define SQL_AST_FUNC_CALL    24   /* "fnname" — a function call in a value
                                   * position (e.g. datetime('now') inside
                                   * VALUES). Children are the arg literals.
                                   * exec_insert evaluates at insert time. */

typedef struct sql_ast sql_ast;

struct sql_ast {
    int          type;      /* SQL_AST_* */
    char        *text;      /* name/value text (may be NULL) */
    sql_ast     *children;  /* child nodes array */
    u64          child_count;
    sql_ast     *left;      /* for binary ops */
    sql_ast     *right;     /* for binary ops */
};

/* Visitor callback: return 0 to continue, non-zero to stop. */
typedef int (*sql_ast_visitor)(const sql_ast *node, int depth, void *ctx);

/* ---- Functions ---- */

/* sql_tokenize — tokenize SQL string.
 *   out:      receives token list
 *   sql:      null-terminated SQL string
 *
 * Hatches: 1=null out, 2=null sql, 3=alloc failure, 4=invalid token */
APENNINES_API unsigned long sql_tokenize(sql_token_list *out, const char *sql);

/* sql_parse — parse token stream to AST.
 *   out:      receives AST root node (caller frees via sql_ast_destroy)
 *   tokens:   token list from sql_tokenize
 *
 * Hatches: 1=null out, 2=null tokens, 3=syntax error, 4=alloc failure */
APENNINES_API unsigned long sql_parse(sql_ast **out, const sql_token_list *tokens);

/* sql_ast_walk — depth-first walk of AST with visitor callback.
 *   node:     root node
 *   visitor:  callback function
 *   ctx:      user context
 *
 * Hatches: 1=null node, 2=null visitor */
APENNINES_API unsigned long sql_ast_walk(const sql_ast *node,
                                          sql_ast_visitor visitor, void *ctx);

/* sql_ast_destroy — free AST and all children. */
APENNINES_API unsigned long sql_ast_destroy(sql_ast *node);

/* sql_token_list_free — free token list. */
APENNINES_API unsigned long sql_token_list_free(sql_token_list *list);

#endif /* APENNINES_T3_SQL_H */
