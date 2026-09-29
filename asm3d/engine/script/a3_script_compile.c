/*
 * ASM3D - a3_script_compile.c
 * A3Script lexer, parser (AST) and bytecode compiler.
 */
#include "a3_script_internal.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_log.h"

/* ======================================================================== */
/* Lexer                                                                    */
/* ======================================================================== */

typedef enum TokKind {
    T_EOF = 0, T_IDENT, T_NUMBER, T_STRING,
    T_FN, T_LET, T_IF, T_ELSE, T_WHILE, T_FOR, T_IN, T_RETURN, T_BREAK, T_CONTINUE, T_TRUE, T_FALSE, T_NIL, T_AND, T_OR, T_NOT, T_SELF,
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET, T_COMMA, T_DOT, T_DOTDOT, T_SEMI, T_COLON,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_ASSIGN, T_PLUS_EQ, T_MINUS_EQ, T_STAR_EQ, T_SLASH_EQ,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE, T_BANG,
    T_COUNT
} TokKind;

static const char *const g_tok_text[T_COUNT] = {
    "end of file", "name", "number", "text", "fn", "let", "if", "else", "while", "for", "in", "return", "break", "continue", "true", "false",
    "nil", "and", "or", "not", "self", "(", ")", "{", "}", "[", "]", ",", ".", "..", ";", ":", "+", "-", "*", "/", "%", "=", "+=", "-=", "*=", "/=",
    "==", "!=", "<", "<=", ">", ">=", "!",
};

typedef struct Tok { TokKind k; i32 line, col; const char *s; u32 len; f64 num; } Tok;
typedef A3_ARRAY_TYPE(Tok) TokArray;

typedef struct Lexer { const char *src; usize len, pos; i32 line, col; A3SError *err; b32 failed; } Lexer;

static void lex_fail(Lexer *lx, const char *msg, const char *hint) {
    if (lx->failed) return;
    lx->failed = 1;
    lx->err->line = lx->line;
    lx->err->col = lx->col;
    a3_strcpy(lx->err->message, sizeof(lx->err->message), msg);
    a3_strcpy(lx->err->hint, sizeof(lx->err->hint), hint ? hint : "");
}

static char lpeek(Lexer *lx, usize o) { return lx->pos + o < lx->len ? lx->src[lx->pos + o] : 0; }
static char ladv(Lexer *lx) { char c = lx->src[lx->pos++]; if (c == '\n') { lx->line++; lx->col = 1; } else lx->col++; return c; }

static TokKind keyword(const char *s, u32 n) {
    static const struct { const char *w; TokKind k; } kw[] = {
        { "fn", T_FN }, { "let", T_LET }, { "var", T_LET }, { "if", T_IF }, { "else", T_ELSE }, { "while", T_WHILE }, { "for", T_FOR },
        { "in", T_IN }, { "return", T_RETURN }, { "break", T_BREAK }, { "continue", T_CONTINUE }, { "true", T_TRUE }, { "false", T_FALSE },
        { "nil", T_NIL }, { "null", T_NIL }, { "and", T_AND }, { "or", T_OR }, { "not", T_NOT }, { "self", T_SELF },
    };
    for (u32 i = 0; i < A3_ARRAY_COUNT(kw); ++i) if (a3_strlen(kw[i].w) == n && !a3_strncmp(kw[i].w, s, n)) return kw[i].k;
    return T_IDENT;
}

static b32 lex_all(Lexer *lx, TokArray *out) {
    for (;;) {
        /* whitespace and comments */
        for (;;) {
            char c = lpeek(lx, 0);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ladv(lx); continue; }
            if (c == '/' && lpeek(lx, 1) == '/') { while (lx->pos < lx->len && lpeek(lx, 0) != '\n') ladv(lx); continue; }
            if (c == '#') { while (lx->pos < lx->len && lpeek(lx, 0) != '\n') ladv(lx); continue; }
            if (c == '/' && lpeek(lx, 1) == '*') {
                ladv(lx); ladv(lx);
                while (lx->pos < lx->len && !(lpeek(lx, 0) == '*' && lpeek(lx, 1) == '/')) ladv(lx);
                if (lx->pos >= lx->len) { lex_fail(lx, "a /* comment is never closed", "Add */ where the comment should end."); return 0; }
                ladv(lx); ladv(lx);
                continue;
            }
            break;
        }
        Tok t;
        a3_zero_struct(&t);
        t.line = lx->line;
        t.col = lx->col;
        t.s = lx->src + lx->pos;
        if (lx->pos >= lx->len) { t.k = T_EOF; a3_array_push(*out, t, A3_MEM_SCRIPT); return 1; }
        char c = ladv(lx);
        if (a3_is_alpha(c) || c == '_') {
            while (a3_is_ident(lpeek(lx, 0))) ladv(lx);
            t.len = (u32)(lx->src + lx->pos - t.s);
            t.k = keyword(t.s, t.len);
        } else if (a3_is_digit(c) || (c == '.' && a3_is_digit(lpeek(lx, 0)))) {
            while (a3_is_digit(lpeek(lx, 0)) || lpeek(lx, 0) == '_') ladv(lx);
            if (lpeek(lx, 0) == '.' && a3_is_digit(lpeek(lx, 1))) { ladv(lx); while (a3_is_digit(lpeek(lx, 0))) ladv(lx); }
            if (lpeek(lx, 0) == 'e' || lpeek(lx, 0) == 'E') {
                ladv(lx);
                if (lpeek(lx, 0) == '+' || lpeek(lx, 0) == '-') ladv(lx);
                while (a3_is_digit(lpeek(lx, 0))) ladv(lx);
            }
            t.len = (u32)(lx->src + lx->pos - t.s);
            char buf[64];
            u32 n = 0;
            for (u32 i = 0; i < t.len && n < sizeof(buf) - 1; ++i) if (t.s[i] != '_') buf[n++] = t.s[i];
            buf[n] = 0;
            if (!a3_parse_f64(buf, n, &t.num)) { lex_fail(lx, "this number is not written correctly", "Numbers look like 3, 2.5 or 1e6."); return 0; }
            t.k = T_NUMBER;
        } else if (c == '"' || c == '\'') {
            char q = c;
            t.s = lx->src + lx->pos;
            while (lx->pos < lx->len && lpeek(lx, 0) != q && lpeek(lx, 0) != '\n') { if (lpeek(lx, 0) == '\\') ladv(lx); ladv(lx); }
            if (lpeek(lx, 0) != q) { lex_fail(lx, "a text in quotes is never closed", "Add the closing quote at the end of the text."); return 0; }
            t.len = (u32)(lx->src + lx->pos - t.s);
            ladv(lx);
            t.k = T_STRING;
        } else {
            char n = lpeek(lx, 0);
            switch (c) {
            case '(': t.k = T_LPAREN; break; case ')': t.k = T_RPAREN; break;
            case '{': t.k = T_LBRACE; break; case '}': t.k = T_RBRACE; break;
            case '[': t.k = T_LBRACKET; break; case ']': t.k = T_RBRACKET; break;
            case ',': t.k = T_COMMA; break; case ';': t.k = T_SEMI; break; case ':': t.k = T_COLON; break;
            case '.': if (n == '.') { ladv(lx); t.k = T_DOTDOT; } else t.k = T_DOT; break;
            case '+': if (n == '=') { ladv(lx); t.k = T_PLUS_EQ; } else t.k = T_PLUS; break;
            case '-': if (n == '=') { ladv(lx); t.k = T_MINUS_EQ; } else t.k = T_MINUS; break;
            case '*': if (n == '=') { ladv(lx); t.k = T_STAR_EQ; } else t.k = T_STAR; break;
            case '/': if (n == '=') { ladv(lx); t.k = T_SLASH_EQ; } else t.k = T_SLASH; break;
            case '%': t.k = T_PERCENT; break;
            case '=': if (n == '=') { ladv(lx); t.k = T_EQ; } else t.k = T_ASSIGN; break;
            case '!': if (n == '=') { ladv(lx); t.k = T_NE; } else t.k = T_BANG; break;
            case '<': if (n == '=') { ladv(lx); t.k = T_LE; } else t.k = T_LT; break;
            case '>': if (n == '=') { ladv(lx); t.k = T_GE; } else t.k = T_GT; break;
            case '&': if (n == '&') { ladv(lx); t.k = T_AND; break; } /* fallthrough */
            case '|': if (c == '|' && n == '|') { ladv(lx); t.k = T_OR; break; } /* fallthrough */
            default: {
                char msg[96];
                a3_snprintf(msg, sizeof(msg), "unexpected character '%c'", (c >= 32 && c < 127) ? c : '?');
                lx->col = t.col;
                lex_fail(lx, msg, "Remove it, or put text inside quotes.");
                return 0;
            }
            }
            t.len = (u32)(lx->src + lx->pos - t.s);
        }
        if (!a3_array_push(*out, t, A3_MEM_SCRIPT)) { lex_fail(lx, "out of memory", 0); return 0; }
    }
}

/* ======================================================================== */
/* AST                                                                      */
/* ======================================================================== */

typedef enum ExprKind { E_NUM = 0, E_STR, E_TRUE, E_FALSE, E_NIL, E_SELF, E_IDENT, E_LIST, E_UNARY, E_BINARY, E_AND, E_OR, E_CALL, E_MEMBER, E_INDEX } ExprKind;
typedef struct Expr {
    ExprKind k;
    i32 line, col;
    TokKind op;
    f64 num;
    const char *s;
    u32 slen;
    struct Expr *a, *b;
    struct Expr **items;
    u32 n;
} Expr;

typedef enum StmtKind { S_LET = 0, S_EXPR, S_ASSIGN, S_IF, S_WHILE, S_FOR_RANGE, S_FOR_IN, S_BLOCK, S_RETURN, S_BREAK, S_CONTINUE, S_FN } StmtKind;
typedef struct Stmt {
    StmtKind k;
    i32 line, col;
    const char *name;
    u32 nlen;
    TokKind op;
    Expr *e, *e2;
    struct Stmt *body, *els;
    struct Stmt **list;
    u32 n;
    Tok *params;
    u32 nparams;
} Stmt;

typedef struct Parser {
    Tok *toks;
    u32 count, pos;
    A3Arena *ar;
    A3SError *err;
    b32 failed;
    u32 depth;
} Parser;

static Tok *cur(Parser *p) { return &p->toks[p->pos]; }
static b32 check(Parser *p, TokKind k) { return cur(p)->k == k; }
static b32 match(Parser *p, TokKind k) { if (check(p, k)) { if (p->pos < p->count - 1) p->pos++; return 1; } return 0; }

static void perr(Parser *p, Tok *at, const char *msg, const char *hint) {
    if (p->failed) return;
    p->failed = 1;
    p->err->line = at->line;
    p->err->col = at->col;
    a3_strcpy(p->err->message, sizeof(p->err->message), msg);
    a3_strcpy(p->err->hint, sizeof(p->err->hint), hint ? hint : "");
}

static void tok_desc(Tok *t, char *out, usize cap) {
    if (t->k == T_IDENT || t->k == T_NUMBER) a3_snprintf(out, cap, "'%.*s'", (int)a3_mini((i32)t->len, 40), t->s);
    else if (t->k == T_STRING) a3_snprintf(out, cap, "a text");
    else a3_snprintf(out, cap, "'%s'", g_tok_text[t->k]);
}

static b32 expect(Parser *p, TokKind k, const char *what) {
    if (match(p, k)) return 1;
    char got[64], msg[200];
    tok_desc(cur(p), got, sizeof(got));
    a3_snprintf(msg, sizeof(msg), "expected '%s' %s, but found %s", g_tok_text[k], what, got);
    const char *hint = k == T_RPAREN ? "Every ( needs a matching )." : k == T_RBRACE ? "Every { needs a matching }." : k == T_LBRACE ? "Blocks of code go inside { and }." : 0;
    perr(p, cur(p), msg, hint);
    return 0;
}

static void *node(Parser *p, usize size) {
    void *n = a3_arena_push(p->ar, size, 8);
    if (n) a3_memset(n, 0, size);
    else perr(p, cur(p), "script too large", 0);
    return n;
}

static Expr *mk_expr(Parser *p, ExprKind k, Tok *t) {
    Expr *e = (Expr *)node(p, sizeof(Expr));
    if (e) { e->k = k; e->line = t->line; e->col = t->col; }
    return e;
}

static Expr *expression(Parser *p);

static Expr *primary(Parser *p) {
    Tok *t = cur(p);
    Expr *e = 0;
    if (match(p, T_NUMBER)) { e = mk_expr(p, E_NUM, t); if (e) e->num = t->num; }
    else if (match(p, T_STRING)) { e = mk_expr(p, E_STR, t); if (e) { e->s = t->s; e->slen = t->len; } }
    else if (match(p, T_TRUE)) e = mk_expr(p, E_TRUE, t);
    else if (match(p, T_FALSE)) e = mk_expr(p, E_FALSE, t);
    else if (match(p, T_NIL)) e = mk_expr(p, E_NIL, t);
    else if (match(p, T_SELF)) e = mk_expr(p, E_SELF, t);
    else if (match(p, T_IDENT)) { e = mk_expr(p, E_IDENT, t); if (e) { e->s = t->s; e->slen = t->len; } }
    else if (match(p, T_LPAREN)) { e = expression(p); expect(p, T_RPAREN, "to close the ( "); }
    else if (match(p, T_LBRACKET)) {
        e = mk_expr(p, E_LIST, t);
        Expr *tmp[256];
        u32 n = 0;
        if (!check(p, T_RBRACKET)) {
            do {
                if (check(p, T_RBRACKET)) break; /* trailing comma */
                if (n >= 256) { perr(p, cur(p), "a list written in code can have at most 256 items", "Build longer lists with push()."); return 0; }
                tmp[n++] = expression(p);
                if (p->failed) return 0;
            } while (match(p, T_COMMA));
        }
        expect(p, T_RBRACKET, "to close the list");
        if (e && n) { e->items = (Expr **)node(p, sizeof(Expr *) * n); if (e->items) a3_memcpy(e->items, tmp, sizeof(Expr *) * n); e->n = n; }
    } else {
        char got[64], msg[160];
        tok_desc(t, got, sizeof(got));
        a3_snprintf(msg, sizeof(msg), "expected a value, but found %s", got);
        perr(p, t, msg, t->k == T_RBRACE || t->k == T_RPAREN ? "Something is missing before this bracket." : "A value is a number, text, name, list or a call like f(x).");
        return 0;
    }
    /* postfix: calls, members, indexing */
    while (e && !p->failed) {
        Tok *pt = cur(p);
        if (match(p, T_LPAREN)) {
            Expr *c = mk_expr(p, E_CALL, pt);
            if (!c) return 0;
            c->a = e;
            Expr *tmp[64];
            u32 n = 0;
            if (!check(p, T_RPAREN)) {
                do {
                    if (n >= 64) { perr(p, cur(p), "too many arguments (max 64)", 0); return 0; }
                    tmp[n++] = expression(p);
                    if (p->failed) return 0;
                } while (match(p, T_COMMA));
            }
            expect(p, T_RPAREN, "after the arguments");
            if (n) { c->items = (Expr **)node(p, sizeof(Expr *) * n); if (c->items) a3_memcpy(c->items, tmp, sizeof(Expr *) * n); c->n = n; }
            e = c;
        } else if (match(p, T_DOT)) {
            Tok *nt = cur(p);
            if (!match(p, T_IDENT)) {
                if (nt->k >= T_FN && nt->k <= T_SELF) { match(p, nt->k); } /* allow keywords as field names (e.g. .in) */
                else { perr(p, nt, "expected a name after '.'", "For example: self.Transform or position.x"); return 0; }
            }
            Expr *m = mk_expr(p, E_MEMBER, nt);
            if (!m) return 0;
            m->a = e;
            m->s = nt->s;
            m->slen = nt->len;
            e = m;
        } else if (match(p, T_LBRACKET)) {
            Expr *ix = mk_expr(p, E_INDEX, pt);
            if (!ix) return 0;
            ix->a = e;
            ix->b = expression(p);
            expect(p, T_RBRACKET, "after the index");
            e = ix;
        } else break;
    }
    return e;
}

static Expr *unary(Parser *p) {
    Tok *t = cur(p);
    if (match(p, T_MINUS) || match(p, T_NOT) || match(p, T_BANG)) {
        if (++p->depth > 200) { perr(p, t, "expression nested too deeply", 0); return 0; }
        Expr *e = mk_expr(p, E_UNARY, t);
        if (e) { e->op = t->k == T_MINUS ? T_MINUS : T_NOT; e->a = unary(p); }
        p->depth--;
        return e;
    }
    return primary(p);
}

static int prec(TokKind k) {
    switch (k) {
    case T_OR: return 1;
    case T_AND: return 2;
    case T_EQ: case T_NE: return 3;
    case T_LT: case T_LE: case T_GT: case T_GE: return 4;
    case T_PLUS: case T_MINUS: return 5;
    case T_STAR: case T_SLASH: case T_PERCENT: return 6;
    default: return 0;
    }
}

static Expr *binary(Parser *p, int min_prec) {
    if (++p->depth > 200) { perr(p, cur(p), "expression nested too deeply", 0); return 0; }
    Expr *left = unary(p);
    while (left && !p->failed) {
        TokKind k = cur(p)->k;
        int pr = prec(k);
        if (!pr || pr < min_prec) break;
        Tok *t = cur(p);
        match(p, k);
        Expr *right = binary(p, pr + 1);
        Expr *e = mk_expr(p, k == T_AND ? E_AND : k == T_OR ? E_OR : E_BINARY, t);
        if (!e) break;
        e->op = k;
        e->a = left;
        e->b = right;
        left = e;
    }
    p->depth--;
    return left;
}

static Expr *expression(Parser *p) { return binary(p, 1); }

static Stmt *statement(Parser *p, b32 top);

static Stmt *mk_stmt(Parser *p, StmtKind k, Tok *t) {
    Stmt *s = (Stmt *)node(p, sizeof(Stmt));
    if (s) { s->k = k; s->line = t->line; s->col = t->col; }
    return s;
}

static Stmt *block(Parser *p) {
    Tok *t = cur(p);
    if (!expect(p, T_LBRACE, "to start a block")) return 0;
    Stmt *b = mk_stmt(p, S_BLOCK, t);
    Stmt *tmp[1024];
    u32 n = 0;
    while (!check(p, T_RBRACE) && !check(p, T_EOF) && !p->failed) {
        if (n >= 1024) { perr(p, cur(p), "block too long (max 1024 statements)", "Split it into functions."); return 0; }
        tmp[n++] = statement(p, 0);
    }
    if (!check(p, T_RBRACE)) { perr(p, t, "this { is never closed", "Add a } at the end of the block."); return 0; }
    match(p, T_RBRACE);
    if (b && n) { b->list = (Stmt **)node(p, sizeof(Stmt *) * n); if (b->list) a3_memcpy(b->list, tmp, sizeof(Stmt *) * n); b->n = n; }
    return b;
}

static Stmt *statement(Parser *p, b32 top) {
    if (p->failed) return 0;
    if (++p->depth > 200) { perr(p, cur(p), "code nested too deeply", 0); return 0; }
    Tok *t = cur(p);
    Stmt *s = 0;
    if (match(p, T_LET)) {
        Tok *nt = cur(p);
        if (!match(p, T_IDENT)) { perr(p, nt, "expected a variable name after 'let'", "For example: let speed = 5"); return 0; }
        s = mk_stmt(p, S_LET, t);
        if (s) { s->name = nt->s; s->nlen = nt->len; }
        if (match(p, T_ASSIGN)) { if (s) s->e = expression(p); }
    } else if (match(p, T_FN)) {
        if (!top) { perr(p, t, "functions must be written at the top level of the script", "Move this fn outside of other code."); return 0; }
        Tok *nt = cur(p);
        if (!match(p, T_IDENT)) { perr(p, nt, "expected a function name after 'fn'", "For example: fn on_update(dt) { }"); return 0; }
        s = mk_stmt(p, S_FN, t);
        if (!s) return 0;
        s->name = nt->s;
        s->nlen = nt->len;
        expect(p, T_LPAREN, "after the function name");
        Tok tmp[32];
        u32 n = 0;
        if (!check(p, T_RPAREN)) {
            do {
                Tok *pt = cur(p);
                if (!match(p, T_IDENT)) { perr(p, pt, "expected a parameter name", 0); return 0; }
                if (n >= 32) { perr(p, pt, "too many parameters (max 32)", 0); return 0; }
                tmp[n++] = *pt;
            } while (match(p, T_COMMA));
        }
        expect(p, T_RPAREN, "after the parameters");
        if (n) { s->params = (Tok *)node(p, sizeof(Tok) * n); if (s->params) a3_memcpy(s->params, tmp, sizeof(Tok) * n); s->nparams = n; }
        s->body = block(p);
    } else if (match(p, T_IF)) {
        s = mk_stmt(p, S_IF, t);
        if (!s) return 0;
        s->e = expression(p);
        s->body = block(p);
        if (match(p, T_ELSE)) {
            if (check(p, T_IF)) s->els = statement(p, 0);
            else s->els = block(p);
        }
    } else if (match(p, T_WHILE)) {
        s = mk_stmt(p, S_WHILE, t);
        if (!s) return 0;
        s->e = expression(p);
        s->body = block(p);
    } else if (match(p, T_FOR)) {
        Tok *vt = cur(p);
        if (!match(p, T_IDENT)) { perr(p, vt, "expected a variable name after 'for'", "For example: for i in 0..10 { }"); return 0; }
        expect(p, T_IN, "after the loop variable");
        Expr *a = expression(p);
        s = mk_stmt(p, S_FOR_IN, t);
        if (!s) return 0;
        s->name = vt->s;
        s->nlen = vt->len;
        s->e = a;
        if (match(p, T_DOTDOT)) { s->k = S_FOR_RANGE; s->e2 = expression(p); }
        s->body = block(p);
    } else if (match(p, T_RETURN)) {
        s = mk_stmt(p, S_RETURN, t);
        if (s && !check(p, T_RBRACE) && !check(p, T_SEMI) && !check(p, T_EOF) && cur(p)->line == t->line) s->e = expression(p);
    } else if (match(p, T_BREAK)) s = mk_stmt(p, S_BREAK, t);
    else if (match(p, T_CONTINUE)) s = mk_stmt(p, S_CONTINUE, t);
    else if (check(p, T_LBRACE)) s = block(p);
    else {
        Expr *e = expression(p);
        if (p->failed) return 0;
        TokKind k = cur(p)->k;
        if (k == T_ASSIGN || k == T_PLUS_EQ || k == T_MINUS_EQ || k == T_STAR_EQ || k == T_SLASH_EQ) {
            Tok *at = cur(p);
            match(p, k);
            if (!e || (e->k != E_IDENT && e->k != E_MEMBER && e->k != E_INDEX)) {
                perr(p, at, "the left side of '=' must be a variable or a field", "For example: speed = 3 or self.position.y = 1");
                return 0;
            }
            s = mk_stmt(p, S_ASSIGN, at);
            if (s) { s->e = e; s->op = k; s->e2 = expression(p); }
        } else if (k == T_EQ && 0) {
        } else {
            s = mk_stmt(p, S_EXPR, t);
            if (s) s->e = e;
            if (e && e->k != E_CALL && !p->failed) {
                perr(p, t, "this line has a value but does nothing with it", "Did you mean to assign it (x = ...) or call a function (f(...))?");
            }
        }
    }
    while (match(p, T_SEMI)) {}
    p->depth--;
    return s;
}

/* ======================================================================== */
/* Suggestions                                                              */
/* ======================================================================== */

static u32 edit_distance(const char *a, const char *b) {
    u32 la = (u32)a3_strlen(a), lb = (u32)a3_strlen(b);
    if (la > 40 || lb > 40) return 99;
    u32 row[41];
    for (u32 j = 0; j <= lb; ++j) row[j] = j;
    for (u32 i = 1; i <= la; ++i) {
        u32 diag = row[0];
        row[0] = i;
        for (u32 j = 1; j <= lb; ++j) {
            u32 up = row[j];
            u32 cost = a3_to_lower(a[i - 1]) == a3_to_lower(b[j - 1]) ? 0 : 1;
            u32 v = diag + cost;
            if (row[j] + 1 < v) v = row[j] + 1;
            if (row[j - 1] + 1 < v) v = row[j - 1] + 1;
            row[j] = v;
            diag = up;
        }
    }
    return row[lb];
}

void a3s_suggest(const char *name, const char *const *cands, u32 count, char *out, usize cap) {
    out[0] = 0;
    u32 best = 99;
    const char *bs = 0;
    u32 limit = (u32)a3_strlen(name) <= 3 ? 1 : 2;
    for (u32 i = 0; i < count; ++i) {
        if (!cands[i]) continue;
        u32 d = edit_distance(name, cands[i]);
        if (d < best) { best = d; bs = cands[i]; }
    }
    if (bs && best <= limit) a3_snprintf(out, cap, "Did you mean '%s'?", bs);
}

/* ======================================================================== */
/* Compiler                                                                 */
/* ======================================================================== */

typedef struct Local { char name[A3S_NAME]; u32 depth; } Local;
typedef struct Loop { u32 continue_target; b32 continue_forward; u32 breaks[64]; u32 nbreaks; u32 continues[64]; u32 ncontinues; } Loop;

typedef struct Comp {
    A3SModule *m;
    A3SError *err;
    b32 failed;
    /* current function */
    Local locals[A3S_MAX_LOCALS];
    u32 nlocals, depth, max_locals;
    b32 in_function;
    Loop loops[32];
    u32 nloops;
    i32 line;
    /* module names */
    char (*fn_names)[A3S_NAME];
    u32 fn_count;
} Comp;

static void cerr(Comp *c, i32 line, i32 col, const char *msg, const char *hint) {
    if (c->failed) return;
    c->failed = 1;
    c->err->line = line;
    c->err->col = col;
    a3_strcpy(c->err->message, sizeof(c->err->message), msg);
    a3_strcpy(c->err->hint, sizeof(c->err->hint), hint ? hint : "");
}

static void emit(Comp *c, u8 b) {
    if (!a3_array_push(c->m->code, b, A3_MEM_SCRIPT) || !a3_array_push(c->m->lines, c->line, A3_MEM_SCRIPT)) cerr(c, c->line, 0, "out of memory", 0);
}
static void emit16(Comp *c, u32 v) { emit(c, (u8)(v & 0xFF)); emit(c, (u8)(v >> 8)); }
static void emit_op16(Comp *c, A3SOp op, u32 v) { emit(c, (u8)op); emit16(c, v); }
static u32 here(Comp *c) { return c->m->code.count; }

static u32 emit_jump(Comp *c, A3SOp op) { emit(c, (u8)op); emit16(c, 0xFFFF); return here(c) - 2; }
static void patch_jump(Comp *c, u32 at) {
    u32 off = here(c) - (at + 2);
    if (off > 0xFFFF) { cerr(c, c->line, 0, "function too long for a jump", "Split it into smaller functions."); return; }
    c->m->code.data[at] = (u8)(off & 0xFF);
    c->m->code.data[at + 1] = (u8)(off >> 8);
}
static void emit_loop(Comp *c, u32 target) {
    emit(c, OP_LOOP);
    u32 off = here(c) + 2 - target;
    if (off > 0xFFFF) { cerr(c, c->line, 0, "loop body too long", "Split it into smaller functions."); return; }
    emit16(c, off);
}

static u32 add_const(Comp *c, A3SValue v) {
    for (u32 i = 0; i < c->m->consts.count; ++i) {
        A3SValue *k = &c->m->consts.data[i];
        if (k->type == v.type && k->aux == v.aux) {
            if (v.type == A3S_NUM && k->as.num == v.as.num && !(v.as.num == 0 && (1.0 / v.as.num) != (1.0 / k->as.num))) { return i; }
            if (v.type == A3S_STR && k->as.str->len == v.as.str->len && !a3_memcmp(k->as.str->chars, v.as.str->chars, v.as.str->len)) { a3s_release(&v); return i; }
            if (v.type == A3S_FUNC || v.type == A3S_NATIVE) return i;
        }
    }
    if (c->m->consts.count >= 0xFFFF) { cerr(c, c->line, 0, "too many constants in one script", 0); a3s_release(&v); return 0; }
    if (!a3_array_push(c->m->consts, v, A3_MEM_SCRIPT)) { cerr(c, c->line, 0, "out of memory", 0); a3s_release(&v); return 0; }
    return c->m->consts.count - 1;
}

static u32 str_const(Comp *c, const char *s, u32 n) { return add_const(c, a3s_str_n(s, n)); }

/* string literal with escapes */
static u32 str_literal(Comp *c, const char *s, u32 n) {
    char buf[1024];
    u32 w = 0;
    for (u32 i = 0; i < n && w < sizeof(buf) - 1; ++i) {
        char ch = s[i];
        if (ch == '\\' && i + 1 < n) {
            char e = s[++i];
            ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == '0' ? '\0' : e;
        }
        buf[w++] = ch;
    }
    return add_const(c, a3s_str_n(buf, w));
}

static void name_of(const char *s, u32 n, char *out) { a3_str_to_buf(a3_str_n(s, n < A3S_NAME - 1 ? n : A3S_NAME - 1), out, A3S_NAME); }

static i32 find_local(Comp *c, const char *name) {
    for (i32 i = (i32)c->nlocals - 1; i >= 0; --i) if (a3_streq(c->locals[i].name, name)) return i;
    return -1;
}
static i32 find_global(Comp *c, const char *name) {
    for (u32 i = 0; i < c->m->global_count; ++i) if (a3_streq(c->m->global_names[i], name)) return (i32)i;
    return -1;
}
static i32 find_fn(Comp *c, const char *name) {
    for (u32 i = 0; i < c->fn_count; ++i) if (a3_streq(c->fn_names[i], name)) return (i32)i + 1;
    return -1;
}

static i32 add_local(Comp *c, const char *name, i32 line, i32 col) {
    if (c->nlocals >= A3S_MAX_LOCALS) { cerr(c, line, col, "too many variables in one function", "Split it into smaller functions."); return 0; }
    Local *l = &c->locals[c->nlocals];
    a3_strcpy(l->name, sizeof(l->name), name);
    l->depth = c->depth;
    c->nlocals++;
    if (c->nlocals > c->max_locals) c->max_locals = c->nlocals;
    return (i32)c->nlocals - 1;
}

static void begin_scope(Comp *c) { c->depth++; }
static void end_scope(Comp *c) {
    while (c->nlocals && c->locals[c->nlocals - 1].depth >= c->depth) c->nlocals--;
    c->depth--;
}

static void unknown_name(Comp *c, const char *name, i32 line, i32 col) {
    const char *cands[512];
    u32 n = 0;
    for (u32 i = 0; i < c->nlocals && n < 512; ++i) cands[n++] = c->locals[i].name;
    for (u32 i = 0; i < c->m->global_count && n < 512; ++i) cands[n++] = c->m->global_names[i];
    for (u32 i = 0; i < c->fn_count && n < 512; ++i) cands[n++] = c->fn_names[i];
    for (u32 i = 0; i < a3s_native_count() && n < 512; ++i) cands[n++] = a3s_native_get(i)->name;
    char msg[160], hint[160];
    a3_snprintf(msg, sizeof(msg), "unknown name '%s'", name);
    a3s_suggest(name, cands, n, hint, sizeof(hint));
    if (!hint[0]) a3_snprintf(hint, sizeof(hint), "Create it first with: let %s = 0", name);
    cerr(c, line, col, msg, hint);
}

static void compile_expr(Comp *c, Expr *e);

static void compile_ident(Comp *c, Expr *e) {
    char name[A3S_NAME];
    name_of(e->s, e->slen, name);
    i32 i;
    if ((i = find_local(c, name)) >= 0) { emit_op16(c, OP_GET_LOCAL, (u32)i); return; }
    if ((i = find_global(c, name)) >= 0) { emit_op16(c, OP_GET_GLOBAL, (u32)i); return; }
    if ((i = find_fn(c, name)) >= 0) { A3SValue v = a3s_nil(); v.type = A3S_FUNC; v.aux = (u32)i; emit_op16(c, OP_CONST, add_const(c, v)); return; }
    if ((i = a3s_native_find(name)) >= 0) { A3SValue v = a3s_nil(); v.type = A3S_NATIVE; v.aux = (u32)i; emit_op16(c, OP_CONST, add_const(c, v)); return; }
    if (a3_streq(name, "pi")) { emit_op16(c, OP_CONST, add_const(c, a3s_num(3.14159265358979323846))); return; }
    if (a3_streq(name, "tau")) { emit_op16(c, OP_CONST, add_const(c, a3s_num(6.28318530717958647692))); return; }
    unknown_name(c, name, e->line, e->col);
}

static void compile_expr(Comp *c, Expr *e) {
    if (!e || c->failed) return;
    c->line = e->line;
    switch (e->k) {
    case E_NUM: emit_op16(c, OP_CONST, add_const(c, a3s_num(e->num))); break;
    case E_STR: emit_op16(c, OP_CONST, str_literal(c, e->s, e->slen)); break;
    case E_TRUE: emit(c, OP_TRUE); break;
    case E_FALSE: emit(c, OP_FALSE); break;
    case E_NIL: emit(c, OP_NIL); break;
    case E_SELF: emit(c, OP_SELF); break;
    case E_IDENT: compile_ident(c, e); break;
    case E_LIST:
        for (u32 i = 0; i < e->n; ++i) compile_expr(c, e->items[i]);
        emit_op16(c, OP_LIST, e->n);
        break;
    case E_UNARY:
        compile_expr(c, e->a);
        c->line = e->line;
        emit(c, e->op == T_MINUS ? OP_NEG : OP_NOT);
        break;
    case E_BINARY: {
        compile_expr(c, e->a);
        compile_expr(c, e->b);
        c->line = e->line;
        static const struct { TokKind t; A3SOp op; } map[] = {
            { T_PLUS, OP_ADD }, { T_MINUS, OP_SUB }, { T_STAR, OP_MUL }, { T_SLASH, OP_DIV }, { T_PERCENT, OP_MOD },
            { T_EQ, OP_EQ }, { T_NE, OP_NE }, { T_LT, OP_LT }, { T_LE, OP_LE }, { T_GT, OP_GT }, { T_GE, OP_GE },
        };
        for (u32 i = 0; i < A3_ARRAY_COUNT(map); ++i) if (map[i].t == e->op) emit(c, (u8)map[i].op);
    } break;
    case E_AND: case E_OR: {
        compile_expr(c, e->a);
        u32 j = emit_jump(c, e->k == E_AND ? OP_JUMP_IF_FALSE_KEEP : OP_JUMP_IF_TRUE_KEEP);
        emit(c, OP_POP);
        compile_expr(c, e->b);
        patch_jump(c, j);
    } break;
    case E_CALL: {
        /* compile-time argument count check for known functions */
        if (e->a && e->a->k == E_IDENT) {
            char name[A3S_NAME];
            name_of(e->a->s, e->a->slen, name);
            if (find_local(c, name) < 0 && find_global(c, name) < 0) {
                i32 nf = a3s_native_find(name);
                if (nf >= 0) {
                    const A3SNative *nt = a3s_native_get((u32)nf);
                    if ((i32)e->n < nt->min_args || (nt->max_args >= 0 && (i32)e->n > nt->max_args)) {
                        char msg[200];
                        a3_snprintf(msg, sizeof(msg), "'%s' was given %u value%s, but it needs %s", name, e->n, e->n == 1 ? "" : "s", nt->signature ? nt->signature : "a different number");
                        cerr(c, e->line, e->col, msg, nt->doc);
                        return;
                    }
                }
            }
        }
        compile_expr(c, e->a);
        for (u32 i = 0; i < e->n; ++i) compile_expr(c, e->items[i]);
        c->line = e->line;
        emit(c, OP_CALL);
        emit(c, (u8)e->n);
    } break;
    case E_MEMBER:
        compile_expr(c, e->a);
        c->line = e->line;
        emit_op16(c, OP_GET_FIELD, str_const(c, e->s, e->slen));
        break;
    case E_INDEX:
        compile_expr(c, e->a);
        compile_expr(c, e->b);
        c->line = e->line;
        emit(c, OP_GET_INDEX);
        break;
    }
}

/* Stores the value on top of the stack into `t` (pops it). Member and index
 * targets write back through their containers so value types (vectors)
 * behave: self.position.y = 1 updates the component. */
static void compile_store(Comp *c, Expr *t, b32 strict) {
    if (!t || c->failed) return;
    c->line = t->line;
    if (t->k == E_IDENT) {
        char name[A3S_NAME];
        name_of(t->s, t->slen, name);
        i32 i;
        if ((i = find_local(c, name)) >= 0) { emit_op16(c, OP_SET_LOCAL, (u32)i); return; }
        if ((i = find_global(c, name)) >= 0) { emit_op16(c, OP_SET_GLOBAL, (u32)i); return; }
        if (!strict) { emit(c, OP_POP); return; }
        if (find_fn(c, name) >= 0 || a3s_native_find(name) >= 0) {
            char msg[160];
            a3_snprintf(msg, sizeof(msg), "'%s' is a function and cannot be changed", name);
            cerr(c, t->line, t->col, msg, "Use a different name for your variable.");
            return;
        }
        unknown_name(c, name, t->line, t->col);
    } else if (t->k == E_MEMBER) {
        compile_expr(c, t->a);          /* [v obj] */
        emit(c, OP_SWAP);               /* [obj v] */
        emit_op16(c, OP_SET_FIELD, str_const(c, t->s, t->slen)); /* [obj'] */
        compile_store(c, t->a, 0);
    } else if (t->k == E_INDEX) {
        compile_expr(c, t->a);          /* [v obj] */
        compile_expr(c, t->b);          /* [v obj idx] */
        emit(c, OP_ROT3);               /* [obj idx v] */
        emit(c, OP_SET_INDEX);          /* [obj'] */
        compile_store(c, t->a, 0);
    } else {
        emit(c, OP_POP);                /* self, calls: nothing to write back to */
    }
}

static void compile_stmt(Comp *c, Stmt *s);

static void compile_block(Comp *c, Stmt *b) {
    if (!b) return;
    begin_scope(c);
    for (u32 i = 0; i < b->n && !c->failed; ++i) compile_stmt(c, b->list[i]);
    end_scope(c);
}

static void compile_stmt(Comp *c, Stmt *s) {
    if (!s || c->failed) return;
    c->line = s->line;
    switch (s->k) {
    case S_LET: {
        char name[A3S_NAME];
        name_of(s->name, s->nlen, name);
        if (s->e) compile_expr(c, s->e); else emit(c, OP_NIL);
        if (!c->in_function && c->depth == 0) { emit_op16(c, OP_SET_GLOBAL, (u32)find_global(c, name)); break; }
        for (i32 i = (i32)c->nlocals - 1; i >= 0 && c->locals[i].depth == c->depth; --i)
            if (a3_streq(c->locals[i].name, name)) { emit_op16(c, OP_SET_LOCAL, (u32)i); return; } /* re-declared in same scope */
        i32 slot = add_local(c, name, s->line, s->col);
        emit_op16(c, OP_SET_LOCAL, (u32)slot);
    } break;
    case S_EXPR:
        compile_expr(c, s->e);
        emit(c, OP_POP);
        break;
    case S_ASSIGN:
        if (s->op == T_ASSIGN) compile_expr(c, s->e2);
        else {
            compile_expr(c, s->e);
            compile_expr(c, s->e2);
            c->line = s->line;
            emit(c, (u8)(s->op == T_PLUS_EQ ? OP_ADD : s->op == T_MINUS_EQ ? OP_SUB : s->op == T_STAR_EQ ? OP_MUL : OP_DIV));
        }
        compile_store(c, s->e, 1);
        break;
    case S_IF: {
        compile_expr(c, s->e);
        u32 jf = emit_jump(c, OP_JUMP_IF_FALSE);
        compile_block(c, s->body);
        if (s->els) {
            u32 je = emit_jump(c, OP_JUMP);
            patch_jump(c, jf);
            if (s->els->k == S_BLOCK) compile_block(c, s->els); else compile_stmt(c, s->els);
            patch_jump(c, je);
        } else patch_jump(c, jf);
    } break;
    case S_WHILE: case S_FOR_RANGE: case S_FOR_IN: {
        if (c->nloops >= A3_ARRAY_COUNT(c->loops)) { cerr(c, s->line, s->col, "loops nested too deeply", 0); return; }
        begin_scope(c);
        Loop *lp = &c->loops[c->nloops++];
        a3_zero_struct(lp);
        u32 top, exit_jump;
        i32 var = -1, hidden = -1;
        char name[A3S_NAME];
        if (s->k != S_WHILE) name_of(s->name, s->nlen, name);
        if (s->k == S_WHILE) {
            top = here(c);
            compile_expr(c, s->e);
            exit_jump = emit_jump(c, OP_JUMP_IF_FALSE);
            lp->continue_target = top;
        } else if (s->k == S_FOR_RANGE) {
            hidden = add_local(c, "(end)", s->line, s->col);
            var = add_local(c, name, s->line, s->col);
            compile_expr(c, s->e);
            emit_op16(c, OP_SET_LOCAL, (u32)var);
            compile_expr(c, s->e2);
            emit_op16(c, OP_SET_LOCAL, (u32)hidden);
            top = here(c);
            emit_op16(c, OP_GET_LOCAL, (u32)var);
            emit_op16(c, OP_GET_LOCAL, (u32)hidden);
            emit(c, OP_LT);
            exit_jump = emit_jump(c, OP_JUMP_IF_FALSE);
            lp->continue_forward = 1;
        } else {
            hidden = add_local(c, "(list)", s->line, s->col);
            add_local(c, "(index)", s->line, s->col);
            var = add_local(c, name, s->line, s->col);
            compile_expr(c, s->e);
            emit_op16(c, OP_SET_LOCAL, (u32)hidden);
            emit_op16(c, OP_CONST, add_const(c, a3s_num(0)));
            emit_op16(c, OP_SET_LOCAL, (u32)hidden + 1);
            top = here(c);
            emit_op16(c, OP_ITER, (u32)hidden);
            emit16(c, 0xFFFF);
            exit_jump = here(c) - 2;
            lp->continue_target = top;
        }
        compile_block(c, s->body);
        /* continue lands here */
        if (lp->continue_forward) {
            for (u32 i = 0; i < lp->ncontinues; ++i) patch_jump(c, lp->continues[i]);
            emit_op16(c, OP_GET_LOCAL, (u32)var);
            emit_op16(c, OP_CONST, add_const(c, a3s_num(1)));
            emit(c, OP_ADD);
            emit_op16(c, OP_SET_LOCAL, (u32)var);
        }
        emit_loop(c, top);
        patch_jump(c, exit_jump);
        for (u32 i = 0; i < lp->nbreaks; ++i) patch_jump(c, lp->breaks[i]);
        c->nloops--;
        end_scope(c);
    } break;
    case S_BLOCK: compile_block(c, s); break;
    case S_RETURN:
        if (!c->in_function) { cerr(c, s->line, s->col, "'return' can only be used inside a function", 0); return; }
        if (s->e) compile_expr(c, s->e); else emit(c, OP_NIL);
        emit(c, OP_RETURN);
        break;
    case S_BREAK: case S_CONTINUE: {
        if (!c->nloops) { cerr(c, s->line, s->col, s->k == S_BREAK ? "'break' must be inside a loop" : "'continue' must be inside a loop", 0); return; }
        Loop *lp = &c->loops[c->nloops - 1];
        if (s->k == S_BREAK) {
            if (lp->nbreaks >= 64) { cerr(c, s->line, s->col, "too many breaks in one loop", 0); return; }
            lp->breaks[lp->nbreaks++] = emit_jump(c, OP_JUMP);
        } else if (lp->continue_forward) {
            if (lp->ncontinues >= 64) { cerr(c, s->line, s->col, "too many continues in one loop", 0); return; }
            lp->continues[lp->ncontinues++] = emit_jump(c, OP_JUMP);
        } else emit_loop(c, lp->continue_target);
    } break;
    case S_FN: break; /* compiled separately */
    }
}

/* ======================================================================== */
/* Entry                                                                    */
/* ======================================================================== */

A3SModule *a3s_compile(const char *name, const char *src, usize len, A3SError *err) {
    A3SError local;
    if (!err) err = &local;
    a3_zero_struct(err);
    a3s_register_core_lib();
    TokArray toks = { 0 };
    Lexer lx = { src ? src : "", src ? len : 0, 0, 1, 1, err, 0 };
    if (!lex_all(&lx, &toks)) { a3_array_free(toks); return 0; }
    A3Arena ar;
    a3_arena_init(&ar, A3_MEM_SCRIPT, A3_KB(64));
    Parser p = { toks.data, toks.count, 0, &ar, err, 0, 0 };
    A3_ARRAY_TYPE(Stmt *) top = { 0 };
    while (!check(&p, T_EOF) && !p.failed) {
        Stmt *s = statement(&p, 1);
        if (s) a3_array_push(top, s, A3_MEM_SCRIPT);
    }
    A3SModule *m = 0;
    if (p.failed) goto done;
    m = A3_NEW(A3SModule, A3_MEM_SCRIPT);
    if (!m) goto done;
    m->refs = 1;
    a3_strcpy(m->name, sizeof(m->name), name ? name : "script");
    Comp c;
    a3_zero_struct(&c);
    c.m = m;
    c.err = err;
    /* pre-pass: function and global names (usable before their definition) */
    u32 nfn = 0, nglob = 0;
    for (u32 i = 0; i < top.count; ++i) { if (top.data[i]->k == S_FN) nfn++; else if (top.data[i]->k == S_LET) nglob++; }
    c.fn_names = (char (*)[A3S_NAME])a3_calloc(sizeof(*c.fn_names) * (nfn + 1), A3_MEM_SCRIPT);
    m->global_names = (char (*)[A3S_NAME])a3_calloc(sizeof(*m->global_names) * (nglob + 1), A3_MEM_SCRIPT);
    if (!c.fn_names || !m->global_names) { cerr(&c, 1, 1, "out of memory", 0); goto fail; }
    for (u32 i = 0; i < top.count; ++i) {
        Stmt *s = top.data[i];
        char nm[A3S_NAME];
        if (s->k == S_FN) {
            name_of(s->name, s->nlen, nm);
            if (find_fn(&c, nm) >= 0) { char msg[160]; a3_snprintf(msg, sizeof(msg), "the function '%s' is written twice", nm); cerr(&c, s->line, s->col, msg, "Rename or remove one of them."); goto fail; }
            if (a3s_native_find(nm) >= 0 && !a3_str_starts_with(nm, "on_")) {
                char msg[160]; a3_snprintf(msg, sizeof(msg), "'%s' is already a built-in function", nm);
                cerr(&c, s->line, s->col, msg, "Choose another name for your function."); goto fail;
            }
            a3_strcpy(c.fn_names[c.fn_count++], A3S_NAME, nm);
        } else if (s->k == S_LET) {
            name_of(s->name, s->nlen, nm);
            if (find_global(&c, nm) < 0) a3_strcpy(m->global_names[m->global_count++], A3S_NAME, nm);
        }
    }
    /* funcs[0]: top-level code */
    A3SFunc main_fn;
    a3_zero_struct(&main_fn);
    a3_strcpy(main_fn.name, sizeof(main_fn.name), "(top level)");
    main_fn.line = 1;
    a3_array_push(m->funcs, main_fn, A3_MEM_SCRIPT);
    for (u32 i = 0; i < c.fn_count; ++i) { A3SFunc f; a3_zero_struct(&f); a3_strcpy(f.name, sizeof(f.name), c.fn_names[i]); a3_array_push(m->funcs, f, A3_MEM_SCRIPT); }
    /* top level */
    m->funcs.data[0].start = here(&c);
    for (u32 i = 0; i < top.count && !c.failed; ++i) if (top.data[i]->k != S_FN) compile_stmt(&c, top.data[i]);
    emit(&c, OP_NIL);
    emit(&c, OP_RETURN);
    m->funcs.data[0].max_locals = c.max_locals;
    /* functions */
    u32 fi = 1;
    for (u32 i = 0; i < top.count && !c.failed; ++i) {
        Stmt *s = top.data[i];
        if (s->k != S_FN) continue;
        A3SFunc *f = &m->funcs.data[fi++];
        f->start = here(&c);
        f->arity = s->nparams;
        f->line = s->line;
        c.nlocals = c.depth = c.max_locals = 0;
        c.in_function = 1;
        c.depth = 1;
        for (u32 k = 0; k < s->nparams; ++k) {
            char pn[A3S_NAME];
            name_of(s->params[k].s, s->params[k].len, pn);
            add_local(&c, pn, s->params[k].line, s->params[k].col);
        }
        if (s->body) for (u32 k = 0; k < s->body->n && !c.failed; ++k) compile_stmt(&c, s->body->list[k]);
        c.line = s->line;
        emit(&c, OP_NIL);
        emit(&c, OP_RETURN);
        f->max_locals = c.max_locals;
    }
    if (c.failed) goto fail;
    a3_free(c.fn_names);
    goto done;
fail:
    a3_free(c.fn_names);
    a3s_module_release(m);
    m = 0;
done:
    a3_array_free(top);
    a3_arena_release(&ar);
    a3_array_free(toks);
    return m;
}

void a3s_module_retain(A3SModule *m) { if (m) m->refs++; }

void a3s_module_release(A3SModule *m) {
    if (!m || --m->refs) return;
    for (u32 i = 0; i < m->consts.count; ++i) a3s_release(&m->consts.data[i]);
    a3_array_free(m->consts);
    a3_array_free(m->code);
    a3_array_free(m->lines);
    a3_array_free(m->funcs);
    a3_free(m->global_names);
    a3_free(m);
}

const char *a3s_module_name(const A3SModule *m) { return m ? m->name : ""; }

b32 a3s_module_has_function(const A3SModule *m, const char *fn) {
    if (!m) return 0;
    for (u32 i = 1; i < m->funcs.count; ++i) if (a3_streq(m->funcs.data[i].name, fn)) return 1;
    return 0;
}

u32 a3s_module_function_arity(const A3SModule *m, const char *fn) {
    if (!m) return 0;
    for (u32 i = 1; i < m->funcs.count; ++i) if (a3_streq(m->funcs.data[i].name, fn)) return m->funcs.data[i].arity;
    return 0;
}
