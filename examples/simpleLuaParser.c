#define LE_IMPLEMENTATION
#include "../lelexer.h"
#include <stdio.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Token types
// ---------------------------------------------------------------------------
enum {
    T_PLUS = LE_USER_START, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_CARET,
    T_HASH, T_DOT, T_DOTDOT, T_DOTDOTDOT,
    T_EQ, T_EQEQ, T_NEQ, T_LT, T_GT, T_LTE, T_GTE,
    T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_LBRACE, T_RBRACE,
    T_SEMI, T_COLON, T_COMMA,
    KW_AND, KW_BREAK, KW_DO, KW_ELSE, KW_ELSEIF, KW_END,
    KW_FALSE, KW_FOR, KW_FUNCTION, KW_IF, KW_IN,
    KW_LOCAL, KW_NIL, KW_NOT, KW_OR,
    KW_REPEAT, KW_RETURN, KW_THEN, KW_TRUE, KW_UNTIL, KW_WHILE,
};

// ---------------------------------------------------------------------------
// AST node types
// ---------------------------------------------------------------------------
enum {
    N_LOCAL = LE_PNODE_USER, N_ASSIGN, N_IF, N_WHILE, N_REPEAT,
    N_FOR_NUM, N_FOR_IN, N_FUNC_DECL, N_LOCAL_FUNC, N_RETURN, N_BREAK,
    N_CALL, N_METHOD_CALL, N_INDEX, N_FIELD, N_TABLE, N_TABLE_FIELD,
    N_TABLE_INDEX_FIELD, N_FUNC_EXPR, N_BINOP, N_UNOP,
    N_BOOL, N_NIL, N_NUMBER, N_STRING, N_IDENT, N_VARARG,
};

// Precedence levels
enum {
    PREC_OR = 10, PREC_AND = 20, PREC_CMP = 30, PREC_CONCAT = 40,
    PREC_ADD = 50, PREC_MUL = 60, PREC_UNARY = 80, PREC_POW = 90,
    PREC_POSTFIX = 100,
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static leAstNode *parseBlock(leParser *p);
static leAstNode *parseExprList(leParser *p);
static leAstNode *parseFuncBody(leParser *p);
static leAstNode *parseArgs(leParser *p);

static bool isBlockEnd(leParser *p) {
    int t = leParserPeek(p).type;
    return t == leEOF || t == KW_END || t == KW_ELSE || t == KW_ELSEIF || t == KW_UNTIL;
}

// ---------------------------------------------------------------------------
// Prefix parselets (registered via leParserPrefix)
// ---------------------------------------------------------------------------
static leAstNode *prefixNumber(leParser *p, leToken tok) {
    return leAstNew(p, N_NUMBER, tok);
}
static leAstNode *prefixString(leParser *p, leToken tok) {
    return leAstNew(p, N_STRING, tok);
}
static leAstNode *prefixBool(leParser *p, leToken tok) {
    return leAstNew(p, N_BOOL, tok);
}
static leAstNode *prefixNil(leParser *p, leToken tok) {
    return leAstNew(p, N_NIL, tok);
}
static leAstNode *prefixVararg(leParser *p, leToken tok) {
    return leAstNew(p, N_VARARG, tok);
}
static leAstNode *prefixIdent(leParser *p, leToken tok) {
    return leAstNew(p, N_IDENT, tok);
}

static leAstNode *prefixGroup(leParser *p, leToken tok) {
    (void)tok;
    leAstNode *expr = leParseExpr(p, 0);
    leParserExpect(p, T_RPAREN, "expected ')'");
    return expr;
}

static leAstNode *prefixUnary(leParser *p, leToken tok) {
    leAstNode *operand = leParseExpr(p, PREC_UNARY);
    leAstNode *n = leAstNew(p, N_UNOP, tok);
    if (n) n->left = operand;
    return n;
}

static leAstNode *prefixFunction(leParser *p, leToken tok) {
    (void)tok;
    return parseFuncBody(p);
}

static leAstNode *prefixTable(leParser *p, leToken tok) {
    leAstNode *tbl = leAstNew(p, N_TABLE, tok);
    leAstNode *fields = leAstList(p);
    if (tbl) tbl->left = fields;

    while (!leParserCheck(p, T_RBRACE) && !leParserAtEnd(p)) {
        leAstNode *field;
        if (leParserCheck(p, T_LBRACKET)) {
            leToken lb = leParserAdvance(p);
            leAstNode *key = leParseExpr(p, 0);
            leParserExpect(p, T_RBRACKET, "expected ']'");
            leParserExpect(p, T_EQ, "expected '='");
            leAstNode *val = leParseExpr(p, 0);
            field = leAstNew(p, N_TABLE_INDEX_FIELD, lb);
            if (field) { field->left = key; field->right = val; }
        } else if (leParserCheck(p, leIdent)) {
            leToken name = leParserPeek(p);
            leToken next;
            lePeekTokenN(p->lex, &next, 0);
            if (next.type == T_EQ) {
                leParserAdvance(p);
                leParserAdvance(p);
                leAstNode *val = leParseExpr(p, 0);
                field = leAstNew(p, N_TABLE_FIELD, name);
                if (field) { field->left = leAstNew(p, N_IDENT, name); field->right = val; }
            } else {
                field = leParseExpr(p, 0);
            }
        } else {
            field = leParseExpr(p, 0);
        }
        leAstListAppend(p, fields, field);
        if (!leParserMatch(p, T_COMMA) && !leParserMatch(p, T_SEMI)) break;
    }
    leParserExpect(p, T_RBRACE, "expected '}'");
    return tbl;
}

// ---------------------------------------------------------------------------
// Infix parselets (registered via leParserInfix)
// ---------------------------------------------------------------------------
static leAstNode *infixBinary(leParser *p, leAstNode *left, leToken tok) {
    int prec = leParserGetPrec(p, tok.type);
    int assoc = leParserGetAssoc(p, tok.type);
    int nextPrec = (assoc == LE_ASSOC_RIGHT) ? prec : prec + 1;
    leAstNode *right = leParseExpr(p, nextPrec);
    leAstNode *n = leAstNew(p, N_BINOP, tok);
    if (n) { n->left = left; n->right = right; }
    return n;
}

static leAstNode *infixDot(leParser *p, leAstNode *left, leToken tok) {
    leToken field = leParserExpect(p, leIdent, "expected field name");
    leAstNode *n = leAstNew(p, N_FIELD, tok);
    if (n) { n->left = left; n->right = leAstNew(p, N_IDENT, field); }
    return n;
}

static leAstNode *infixIndex(leParser *p, leAstNode *left, leToken tok) {
    leAstNode *idx = leParseExpr(p, 0);
    leParserExpect(p, T_RBRACKET, "expected ']'");
    leAstNode *n = leAstNew(p, N_INDEX, tok);
    if (n) { n->left = left; n->right = idx; }
    return n;
}

static leAstNode *infixCall(leParser *p, leAstNode *left, leToken tok) {
    leAstNode *args = leParserCheck(p, T_RPAREN) ? leAstList(p) : parseExprList(p);
    leParserExpect(p, T_RPAREN, "expected ')'");
    leAstNode *n = leAstNew(p, N_CALL, tok);
    if (n) { n->left = left; n->right = args; }
    return n;
}

static leAstNode *infixTableCall(leParser *p, leAstNode *left, leToken tok) {
    leAstNode *tbl = prefixTable(p, tok);
    leAstNode *args = leAstList(p);
    leAstListAppend(p, args, tbl);
    leAstNode *n = leAstNew(p, N_CALL, tok);
    if (n) { n->left = left; n->right = args; }
    return n;
}

static leAstNode *infixStringCall(leParser *p, leAstNode *left, leToken tok) {
    leAstNode *args = leAstList(p);
    leAstListAppend(p, args, leAstNew(p, N_STRING, tok));
    leAstNode *n = leAstNew(p, N_CALL, tok);
    if (n) { n->left = left; n->right = args; }
    return n;
}

static leAstNode *infixMethod(leParser *p, leAstNode *left, leToken tok) {
    leToken method = leParserExpect(p, leIdent, "expected method name");
    leAstNode *args = parseArgs(p);
    leAstNode *n = leAstNew(p, N_METHOD_CALL, tok);
    if (n) { n->left = left; n->right = leAstNew(p, N_IDENT, method); n->extra = args; }
    return n;
}

// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------
static leAstNode *parseArgs(leParser *p) {
    if (leParserCheck(p, T_LPAREN)) {
        leParserAdvance(p);
        leAstNode *args = leParserCheck(p, T_RPAREN) ? leAstList(p) : parseExprList(p);
        leParserExpect(p, T_RPAREN, "expected ')'");
        return args;
    }
    if (leParserCheck(p, T_LBRACE)) {
        leToken t = leParserAdvance(p);
        leAstNode *tbl = prefixTable(p, t);
        leAstNode *args = leAstList(p);
        leAstListAppend(p, args, tbl);
        return args;
    }
    if (leParserCheck(p, leString)) {
        leToken s = leParserAdvance(p);
        leAstNode *args = leAstList(p);
        leAstListAppend(p, args, leAstNew(p, N_STRING, s));
        return args;
    }
    leParserError(p, "expected function arguments");
    return leAstList(p);
}

static leAstNode *parseExprList(leParser *p) {
    leAstNode *el = leAstList(p);
    leAstListAppend(p, el, leParseExpr(p, 0));
    while (leParserMatch(p, T_COMMA))
        leAstListAppend(p, el, leParseExpr(p, 0));
    return el;
}

static leAstNode *parseFuncBody(leParser *p) {
    leToken tok = leParserPeek(p);
    leParserExpect(p, T_LPAREN, "expected '('");
    leAstNode *params = leAstList(p);
    if (!leParserCheck(p, T_RPAREN)) {
        if (leParserCheck(p, T_DOTDOTDOT)) {
            leAstListAppend(p, params, leAstNew(p, N_VARARG, leParserAdvance(p)));
        } else {
            leAstListAppend(p, params, leAstNew(p, N_IDENT, leParserExpect(p, leIdent, "expected parameter")));
            while (leParserMatch(p, T_COMMA)) {
                if (leParserCheck(p, T_DOTDOTDOT)) {
                    leAstListAppend(p, params, leAstNew(p, N_VARARG, leParserAdvance(p)));
                    break;
                }
                leAstListAppend(p, params, leAstNew(p, N_IDENT, leParserExpect(p, leIdent, "expected parameter")));
            }
        }
    }
    leParserExpect(p, T_RPAREN, "expected ')'");
    leAstNode *body = parseBlock(p);
    leParserExpect(p, KW_END, "expected 'end'");
    leAstNode *fn = leAstNew(p, N_FUNC_EXPR, tok);
    if (fn) { fn->left = params; fn->right = body; }
    return fn;
}

static leAstNode *parseFuncName(leParser *p) {
    leAstNode *expr = leAstNew(p, N_IDENT, leParserExpect(p, leIdent, "expected function name"));
    while (leParserMatch(p, T_DOT)) {
        leToken f = leParserExpect(p, leIdent, "expected field name");
        leAstNode *n = leAstNew(p, N_FIELD, f);
        if (n) { n->left = expr; n->right = leAstNew(p, N_IDENT, f); }
        expr = n;
    }
    if (leParserMatch(p, T_COLON)) {
        leToken m = leParserExpect(p, leIdent, "expected method name");
        leAstNode *n = leAstNew(p, N_FIELD, m);
        if (n) { n->left = expr; n->right = leAstNew(p, N_IDENT, m); }
        expr = n;
    }
    return expr;
}

static leAstNode *parseNameList(leParser *p) {
    leAstNode *names = leAstList(p);
    leAstListAppend(p, names, leAstNew(p, N_IDENT, leParserExpect(p, leIdent, "expected name")));
    while (leParserMatch(p, T_COMMA))
        leAstListAppend(p, names, leAstNew(p, N_IDENT, leParserExpect(p, leIdent, "expected name")));
    return names;
}

// ---------------------------------------------------------------------------
// Statement parselets (registered via leParserStmt)
// ---------------------------------------------------------------------------
static leAstNode *stmtIf(leParser *p, leToken tok) {
    leAstNode *ifn = leAstNew(p, N_IF, tok);
    leAstNode *clauses = leAstList(p);
    if (ifn) ifn->left = clauses;

    leAstNode *cond = leParseExpr(p, 0);
    leParserExpect(p, KW_THEN, "expected 'then'");
    leAstNode *clause = leAstNew(p, N_IF, tok);
    if (clause) { clause->left = cond; clause->right = parseBlock(p); }
    leAstListAppend(p, clauses, clause);

    while (leParserCheck(p, KW_ELSEIF)) {
        leToken ei = leParserAdvance(p);
        cond = leParseExpr(p, 0);
        leParserExpect(p, KW_THEN, "expected 'then'");
        clause = leAstNew(p, N_IF, ei);
        if (clause) { clause->left = cond; clause->right = parseBlock(p); }
        leAstListAppend(p, clauses, clause);
    }
    if (leParserCheck(p, KW_ELSE)) {
        leToken el = leParserAdvance(p);
        clause = leAstNew(p, N_IF, el);
        if (clause) { clause->left = NULL; clause->right = parseBlock(p); }
        leAstListAppend(p, clauses, clause);
    }
    leParserExpect(p, KW_END, "expected 'end'");
    return ifn;
}

static leAstNode *stmtWhile(leParser *p, leToken tok) {
    leAstNode *cond = leParseExpr(p, 0);
    leParserExpect(p, KW_DO, "expected 'do'");
    leAstNode *body = parseBlock(p);
    leParserExpect(p, KW_END, "expected 'end'");
    leAstNode *n = leAstNew(p, N_WHILE, tok);
    if (n) { n->left = cond; n->right = body; }
    return n;
}

static leAstNode *stmtRepeat(leParser *p, leToken tok) {
    leAstNode *body = parseBlock(p);
    leParserExpect(p, KW_UNTIL, "expected 'until'");
    leAstNode *cond = leParseExpr(p, 0);
    leAstNode *n = leAstNew(p, N_REPEAT, tok);
    if (n) { n->left = body; n->right = cond; }
    return n;
}

static leAstNode *stmtFor(leParser *p, leToken tok) {
    leToken name = leParserExpect(p, leIdent, "expected name");
    if (leParserMatch(p, T_EQ)) {
        leAstNode *init = leParseExpr(p, 0);
        leParserExpect(p, T_COMMA, "expected ','");
        leAstNode *limit = leParseExpr(p, 0);
        leAstNode *step = leParserMatch(p, T_COMMA) ? leParseExpr(p, 0) : NULL;
        leParserExpect(p, KW_DO, "expected 'do'");
        leAstNode *body = parseBlock(p);
        leParserExpect(p, KW_END, "expected 'end'");
        leAstNode *params = leAstList(p);
        leAstListAppend(p, params, leAstNew(p, N_IDENT, name));
        leAstListAppend(p, params, init);
        leAstListAppend(p, params, limit);
        if (step) leAstListAppend(p, params, step);
        leAstNode *n = leAstNew(p, N_FOR_NUM, tok);
        if (n) { n->left = params; n->right = body; }
        return n;
    }
    leAstNode *names = leAstList(p);
    leAstListAppend(p, names, leAstNew(p, N_IDENT, name));
    while (leParserMatch(p, T_COMMA))
        leAstListAppend(p, names, leAstNew(p, N_IDENT, leParserExpect(p, leIdent, "expected name")));
    leParserExpect(p, KW_IN, "expected 'in'");
    leAstNode *iters = parseExprList(p);
    leParserExpect(p, KW_DO, "expected 'do'");
    leAstNode *body = parseBlock(p);
    leParserExpect(p, KW_END, "expected 'end'");
    leAstNode *n = leAstNew(p, N_FOR_IN, tok);
    if (n) { n->left = names; n->right = iters; n->extra = body; }
    return n;
}

static leAstNode *stmtFunction(leParser *p, leToken tok) {
    leAstNode *name = parseFuncName(p);
    leAstNode *fn = parseFuncBody(p);
    leAstNode *n = leAstNew(p, N_FUNC_DECL, tok);
    if (n) { n->left = name; n->right = fn; }
    return n;
}

static leAstNode *stmtLocal(leParser *p, leToken tok) {
    if (leParserCheck(p, KW_FUNCTION)) {
        leParserAdvance(p);
        leToken name = leParserExpect(p, leIdent, "expected function name");
        leAstNode *fn = parseFuncBody(p);
        leAstNode *n = leAstNew(p, N_LOCAL_FUNC, tok);
        if (n) { n->left = leAstNew(p, N_IDENT, name); n->right = fn; }
        return n;
    }
    leAstNode *names = parseNameList(p);
    leAstNode *values = leParserMatch(p, T_EQ) ? parseExprList(p) : NULL;
    leAstNode *n = leAstNew(p, N_LOCAL, tok);
    if (n) { n->left = names; n->right = values; }
    return n;
}

static leAstNode *stmtReturn(leParser *p, leToken tok) {
    leAstNode *n = leAstNew(p, N_RETURN, tok);
    if (n && !isBlockEnd(p) && !leParserCheck(p, T_SEMI))
        n->left = parseExprList(p);
    leParserMatch(p, T_SEMI);
    return n;
}

static leAstNode *stmtBreak(leParser *p, leToken tok) {
    return leAstNew(p, N_BREAK, tok);
}

static leAstNode *stmtDo(leParser *p, leToken tok) {
    (void)tok;
    leAstNode *body = parseBlock(p);
    leParserExpect(p, KW_END, "expected 'end'");
    return body;
}

// ---------------------------------------------------------------------------
// Block and statement parsing
// ---------------------------------------------------------------------------
static leAstNode *parseStat(leParser *p) {
    leParserMatch(p, T_SEMI);
    if (isBlockEnd(p)) return NULL;

    // Use registered statement parselets for keyword statements
    if (leParserHasStmt(p, leParserPeek(p).type))
        return leParseStmt(p);

    // Expression statement or assignment (uses registered prefix/infix rules)
    leAstNode *expr = leParseExpr(p, 0);
    if (p->panicMode) { leParserSynchronize(p); return expr; }

    if (leParserCheck(p, T_COMMA) || leParserCheck(p, T_EQ)) {
        leAstNode *vars = leAstList(p);
        leAstListAppend(p, vars, expr);
        while (leParserMatch(p, T_COMMA))
            leAstListAppend(p, vars, leParseExpr(p, 0));
        leParserExpect(p, T_EQ, "expected '='");
        leAstNode *vals = parseExprList(p);
        leAstNode *n = leAstNew(p, N_ASSIGN, leParserPrevious(p));
        if (n) { n->left = vars; n->right = vals; }
        return n;
    }
    return expr;
}

static leAstNode *parseBlock(leParser *p) {
    leAstNode *block = leAstList(p);
    while (!isBlockEnd(p)) {
        leAstNode *s = parseStat(p);
        if (s) leAstListAppend(p, block, s);
        else break;
    }
    return block;
}

// ---------------------------------------------------------------------------
// AST printing
// ---------------------------------------------------------------------------
static const char *nodeTypeName(int type) {
    switch (type) {
        case N_LOCAL: return "Local"; case N_ASSIGN: return "Assign";
        case N_IF: return "If"; case N_WHILE: return "While";
        case N_REPEAT: return "Repeat"; case N_FOR_NUM: return "ForNum";
        case N_FOR_IN: return "ForIn"; case N_FUNC_DECL: return "FuncDecl";
        case N_LOCAL_FUNC: return "LocalFunc"; case N_RETURN: return "Return";
        case N_BREAK: return "Break"; case N_CALL: return "Call";
        case N_METHOD_CALL: return "MethodCall"; case N_INDEX: return "Index";
        case N_FIELD: return "Field"; case N_TABLE: return "Table";
        case N_TABLE_FIELD: return "TableField";
        case N_TABLE_INDEX_FIELD: return "TableIdxField";
        case N_FUNC_EXPR: return "FuncExpr"; case N_BINOP: return "BinOp";
        case N_UNOP: return "UnOp"; case N_BOOL: return "Bool";
        case N_NIL: return "Nil"; case N_NUMBER: return "Number";
        case N_STRING: return "String"; case N_IDENT: return "Ident";
        case N_VARARG: return "Vararg";
        default: return leAstNodeTypeName(type);
    }
}

static void printAST(leAstNode *n, int indent) {
    if (!n) return;
    for (int i = 0; i < indent; i++) printf("  ");
    printf("%s", nodeTypeName(n->type));
    if (n->token.start && n->token.len > 0 && n->token.len < 80)
        printf(" \"%.*s\"", n->token.len, n->token.start);
    printf("\n");
    if (n->type == LE_PNODE_LIST) {
        for (leAstNode *c = n->left; c; c = c->next) printAST(c, indent + 1);
    } else {
        if (n->left) printAST(n->left, indent + 1);
        if (n->right) printAST(n->right, indent + 1);
        if (n->extra) printAST(n->extra, indent + 1);
    }
}

// ---------------------------------------------------------------------------
// Setup: register all parselets and language configuration
// ---------------------------------------------------------------------------
static void setupLexer(leLexer *lex) {
    leLineComment(lex, "--");
    leBlockComment(lex, "--[[", "]]");

    leOperators(lex,
        "+", T_PLUS, "-", T_MINUS, "*", T_STAR, "/", T_SLASH,
        "%", T_PERCENT, "^", T_CARET, "#", T_HASH,
        "...", T_DOTDOTDOT, "..", T_DOTDOT, ".", T_DOT,
        "==", T_EQEQ, "~=", T_NEQ, "<=", T_LTE, ">=", T_GTE,
        "<", T_LT, ">", T_GT, "=", T_EQ,
        "(", T_LPAREN, ")", T_RPAREN,
        "[", T_LBRACKET, "]", T_RBRACKET,
        "{", T_LBRACE, "}", T_RBRACE,
        ";", T_SEMI, ":", T_COLON, ",", T_COMMA,
        NULL);

    leKeywords(lex,
        "and", KW_AND, "break", KW_BREAK, "do", KW_DO,
        "else", KW_ELSE, "elseif", KW_ELSEIF, "end", KW_END,
        "false", KW_FALSE, "for", KW_FOR, "function", KW_FUNCTION,
        "if", KW_IF, "in", KW_IN, "local", KW_LOCAL,
        "nil", KW_NIL, "not", KW_NOT, "or", KW_OR,
        "repeat", KW_REPEAT, "return", KW_RETURN, "then", KW_THEN,
        "true", KW_TRUE, "until", KW_UNTIL, "while", KW_WHILE,
        NULL);
}

static void setupParser(leParser *p) {
    // Prefix rules: atoms
    leParserPrefix(p, leInteger,    prefixNumber);
    leParserPrefix(p, leFloat,      prefixNumber);
    leParserPrefix(p, leString,     prefixString);
    leParserPrefix(p, KW_TRUE,      prefixBool);
    leParserPrefix(p, KW_FALSE,     prefixBool);
    leParserPrefix(p, KW_NIL,       prefixNil);
    leParserPrefix(p, T_DOTDOTDOT,  prefixVararg);
    leParserPrefix(p, leIdent,      prefixIdent);
    leParserPrefix(p, T_LPAREN,     prefixGroup);
    leParserPrefix(p, T_LBRACE,     prefixTable);
    leParserPrefix(p, KW_FUNCTION,  prefixFunction);

    // Prefix rules: unary operators
    leParserPrefix(p, T_MINUS, prefixUnary);
    leParserPrefix(p, KW_NOT,  prefixUnary);
    leParserPrefix(p, T_HASH,  prefixUnary);

    // Infix rules: binary operators (with precedence and associativity)
    leParserInfix(p, KW_OR,    PREC_OR,     LE_ASSOC_LEFT,  infixBinary);
    leParserInfix(p, KW_AND,   PREC_AND,    LE_ASSOC_LEFT,  infixBinary);
    leParserInfix(p, T_LT,     PREC_CMP,    LE_ASSOC_LEFT,  infixBinary);
    leParserInfix(p, T_GT,     PREC_CMP,    LE_ASSOC_LEFT,  infixBinary);
    leParserInfix(p, T_LTE,    PREC_CMP,    LE_ASSOC_LEFT,  infixBinary);
    leParserInfix(p, T_GTE,    PREC_CMP,    LE_ASSOC_LEFT,  infixBinary);
    leParserInfix(p, T_EQEQ,   PREC_CMP,    LE_ASSOC_LEFT,  infixBinary);
    leParserInfix(p, T_NEQ,    PREC_CMP,    LE_ASSOC_LEFT,  infixBinary);
    leParserInfix(p, T_DOTDOT, PREC_CONCAT, LE_ASSOC_RIGHT, infixBinary);
    leParserInfix(p, T_PLUS,   PREC_ADD,    LE_ASSOC_LEFT,  infixBinary);
    leParserInfix(p, T_MINUS,  PREC_ADD,    LE_ASSOC_LEFT,  infixBinary);
    leParserInfix(p, T_STAR,   PREC_MUL,    LE_ASSOC_LEFT,  infixBinary);
    leParserInfix(p, T_SLASH,  PREC_MUL,    LE_ASSOC_LEFT,  infixBinary);
    leParserInfix(p, T_PERCENT,PREC_MUL,    LE_ASSOC_LEFT,  infixBinary);
    leParserInfix(p, T_CARET,  PREC_POW,    LE_ASSOC_RIGHT, infixBinary);

    // Infix rules: postfix operations (call, field, index, method)
    leParserInfix(p, T_DOT,      PREC_POSTFIX, LE_ASSOC_LEFT, infixDot);
    leParserInfix(p, T_LBRACKET, PREC_POSTFIX, LE_ASSOC_LEFT, infixIndex);
    leParserInfix(p, T_LPAREN,   PREC_POSTFIX, LE_ASSOC_LEFT, infixCall);
    leParserInfix(p, T_LBRACE,   PREC_POSTFIX, LE_ASSOC_LEFT, infixTableCall);
    leParserInfix(p, leString,   PREC_POSTFIX, LE_ASSOC_LEFT, infixStringCall);
    leParserInfix(p, T_COLON,    PREC_POSTFIX, LE_ASSOC_LEFT, infixMethod);

    // Statement rules
    leParserStmt(p, KW_IF,       stmtIf);
    leParserStmt(p, KW_WHILE,    stmtWhile);
    leParserStmt(p, KW_REPEAT,   stmtRepeat);
    leParserStmt(p, KW_FOR,      stmtFor);
    leParserStmt(p, KW_FUNCTION, stmtFunction);
    leParserStmt(p, KW_LOCAL,    stmtLocal);
    leParserStmt(p, KW_RETURN,   stmtReturn);
    leParserStmt(p, KW_BREAK,    stmtBreak);
    leParserStmt(p, KW_DO,       stmtDo);

    // Error recovery sync points
    leParserSyncOn(p, KW_IF);
    leParserSyncOn(p, KW_WHILE);
    leParserSyncOn(p, KW_FOR);
    leParserSyncOn(p, KW_FUNCTION);
    leParserSyncOn(p, KW_LOCAL);
    leParserSyncOn(p, KW_RETURN);
    leParserSyncOn(p, KW_END);
}

// ---------------------------------------------------------------------------
// Test program
// ---------------------------------------------------------------------------
static const char *test_program =
    "local x = 10\n"
    "local y, z = 20, 30\n"
    "\n"
    "local function add(a, b)\n"
    "    return a + b\n"
    "end\n"
    "\n"
    "function math.double(n)\n"
    "    return n * 2\n"
    "end\n"
    "\n"
    "if x > 5 then\n"
    "    x = x + 1\n"
    "elseif x == 5 then\n"
    "    x = 0\n"
    "else\n"
    "    x = -1\n"
    "end\n"
    "\n"
    "while x > 0 do\n"
    "    x = x - 1\n"
    "end\n"
    "\n"
    "repeat\n"
    "    y = y - 1\n"
    "until y == 0\n"
    "\n"
    "for i = 1, 10, 2 do\n"
    "    print(i)\n"
    "end\n"
    "\n"
    "local t = {1, 2, 3, name = \"lua\", [\"key\"] = true}\n"
    "\n"
    "for k, v in pairs(t) do\n"
    "    print(k, v)\n"
    "end\n"
    "\n"
    "local sq = function(x) return x ^ 2 end\n"
    "\n"
    "local result = add(x, y) + sq(3)\n"
    "print(result)\n"
    "\n"
    "t.name = \"new\"\n"
    "t[1] = 99\n"
    "\n"
    "local s = \"hello\" .. \" \" .. \"world\"\n"
    "print(s)\n"
    "print(#t)\n"
    "\n"
    "local ok = not false and true or false\n"
    "print(ok)\n"
;

int main(int argc, char **argv) {
    const char *source = test_program;
    char *fileBuf = NULL;

    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        fileBuf = (char *)malloc(len + 1);
        fread(fileBuf, 1, len, f);
        fileBuf[len] = '\0';
        fclose(f);
        source = fileBuf;
    }

    leLexer lex;
    leInitNamed(&lex, source, argc > 1 ? argv[1] : "<test>");
    setupLexer(&lex);

    leParser parser;
    leParserInit(&parser, &lex);
    setupParser(&parser);

    leAstNode *program = parseBlock(&parser);

    if (leParserHadError(&parser)) {
        printf("--- ERRORS ---\n");
        for (leErrorNode *e = leParserFirstError(&parser); e; e = leNextErrorNode(e))
            printf("  %s\n", leParserFormatError(&parser, e));
        printf("\n");
    }

    printf("--- Lua AST ---\n");
    printAST(program, 0);

    leParserFree(&parser);
    leFree(&lex);
    free(fileBuf);
    return 0;
}