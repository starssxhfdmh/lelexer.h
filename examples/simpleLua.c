#define LE_IMPLEMENTATION
#include "../lelexer.h"
#include <stdio.h>
#include <stdlib.h>

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

enum {
    N_BLOCK = LE_PNODE_USER,
    N_LOCAL, N_ASSIGN, N_IF, N_WHILE, N_REPEAT, N_FOR_NUM, N_FOR_IN,
    N_FUNC_DECL, N_LOCAL_FUNC, N_RETURN, N_BREAK, N_CALL, N_METHOD_CALL,
    N_INDEX, N_FIELD, N_TABLE, N_TABLE_FIELD, N_TABLE_INDEX_FIELD,
    N_FUNC_EXPR, N_PARAM_LIST, N_VARLIST, N_EXPRLIST, N_NAMELIST,
    N_BINOP, N_UNOP, N_BOOL, N_NIL, N_NUMBER, N_STRING, N_IDENT,
    N_DOTDOTDOT_EXPR,
};

typedef struct {
    leParser p;
} LuaParser;

static leAstNode *parseExpr(LuaParser *L);
static leAstNode *parseBlock(LuaParser *L);
static leAstNode *parseStat(LuaParser *L);
static leAstNode *parsePrefixExpr(LuaParser *L);
static leAstNode *parseExprList(LuaParser *L);
static leAstNode *parseFuncBody(LuaParser *L);

static leAstNode *node(LuaParser *L, int type, leToken tok) {
    return leAstNew(&L->p, type, tok);
}

static leAstNode *list(LuaParser *L) {
    return leAstList(&L->p);
}

static leAstNode *append(LuaParser *L, leAstNode *l, leAstNode *item) {
    return leAstListAppend(&L->p, l, item);
}

static leToken advance(LuaParser *L) {
    return leParserAdvance(&L->p);
}

static bool check(LuaParser *L, int t) {
    return leParserCheck(&L->p, t);
}

static bool match(LuaParser *L, int t) {
    return leParserMatch(&L->p, t);
}

static leToken expect(LuaParser *L, int t, const char *msg) {
    return leParserExpect(&L->p, t, msg);
}

static leToken peek(LuaParser *L) {
    return leParserPeek(&L->p);
}

static bool atEnd(LuaParser *L) {
    return leParserAtEnd(&L->p);
}

static bool isBlockEnd(LuaParser *L) {
    int t = peek(L).type;
    return t == leEOF || t == KW_END || t == KW_ELSE || t == KW_ELSEIF || t == KW_UNTIL;
}

static leAstNode *parseNameList(LuaParser *L) {
    leAstNode *names = list(L);
    leToken name = expect(L, leIdent, "expected name");
    leAstNode *n = node(L, N_IDENT, name);
    append(L, names, n);
    while (match(L, T_COMMA)) {
        name = expect(L, leIdent, "expected name");
        n = node(L, N_IDENT, name);
        append(L, names, n);
    }
    return names;
}

static int getUnaryPrec(int t) {
    if (t == KW_NOT || t == T_HASH || t == T_MINUS) return 80;
    return -1;
}

static int getBinaryPrec(int t) {
    switch (t) {
        case KW_OR:    return 10;
        case KW_AND:   return 20;
        case T_LT: case T_GT: case T_LTE: case T_GTE:
        case T_EQEQ: case T_NEQ: return 30;
        case T_DOTDOT: return 40;
        case T_PLUS: case T_MINUS: return 50;
        case T_STAR: case T_SLASH: case T_PERCENT: return 60;
        case T_CARET:  return 90;
        default: return -1;
    }
}

static int isRightAssoc(int t) {
    return t == T_CARET || t == T_DOTDOT;
}

static leAstNode *parseSimpleExpr(LuaParser *L) {
    leToken tok = peek(L);

    if (check(L, leInteger) || check(L, leFloat)) {
        advance(L);
        return node(L, N_NUMBER, tok);
    }
    if (check(L, leString)) {
        advance(L);
        return node(L, N_STRING, tok);
    }
    if (check(L, KW_TRUE)) {
        advance(L);
        return node(L, N_BOOL, tok);
    }
    if (check(L, KW_FALSE)) {
        advance(L);
        return node(L, N_BOOL, tok);
    }
    if (check(L, KW_NIL)) {
        advance(L);
        return node(L, N_NIL, tok);
    }
    if (check(L, T_DOTDOTDOT)) {
        advance(L);
        return node(L, N_DOTDOTDOT_EXPR, tok);
    }
    if (check(L, KW_FUNCTION)) {
        advance(L);
        return parseFuncBody(L);
    }
    if (check(L, T_LBRACE)) {
        leToken brace = advance(L);
        leAstNode *tbl = node(L, N_TABLE, brace);
        leAstNode *fields = list(L);
        tbl->left = fields;

        if (!check(L, T_RBRACE)) {
            for (;;) {
                leAstNode *field;
                if (check(L, T_LBRACKET)) {
                    leToken lb = advance(L);
                    leAstNode *key = parseExpr(L);
                    expect(L, T_RBRACKET, "expected ']'");
                    expect(L, T_EQ, "expected '='");
                    leAstNode *val = parseExpr(L);
                    field = node(L, N_TABLE_INDEX_FIELD, lb);
                    field->left = key;
                    field->right = val;
                } else if (check(L, leIdent)) {
                    leToken name = peek(L);
                    leToken next;
                    lePeekTokenN(L->p.lex, &next, 0);
                    if (next.type == T_EQ) {
                        advance(L);
                        advance(L);
                        leAstNode *val = parseExpr(L);
                        field = node(L, N_TABLE_FIELD, name);
                        field->left = node(L, N_IDENT, name);
                        field->right = val;
                    } else {
                        field = parseExpr(L);
                    }
                } else {
                    field = parseExpr(L);
                }
                append(L, fields, field);
                if (!match(L, T_COMMA) && !match(L, T_SEMI)) break;
                if (check(L, T_RBRACE)) break;
            }
        }
        expect(L, T_RBRACE, "expected '}'");
        return tbl;
    }

    return parsePrefixExpr(L);
}

static leAstNode *parseArgs(LuaParser *L) {
    if (check(L, T_LPAREN)) {
        advance(L);
        leAstNode *args = NULL;
        if (!check(L, T_RPAREN)) {
            args = parseExprList(L);
        } else {
            args = list(L);
        }
        expect(L, T_RPAREN, "expected ')'");
        return args;
    }
    if (check(L, T_LBRACE)) {
        leAstNode *tbl = parseSimpleExpr(L);
        leAstNode *args = list(L);
        append(L, args, tbl);
        return args;
    }
    if (check(L, leString)) {
        leToken s = advance(L);
        leAstNode *args = list(L);
        append(L, args, node(L, N_STRING, s));
        return args;
    }
    leParserError(&L->p, "expected function arguments");
    return list(L);
}

static leAstNode *parsePrefixExpr(LuaParser *L) {
    leAstNode *expr;

    if (check(L, T_LPAREN)) {
        advance(L);
        expr = parseExpr(L);
        expect(L, T_RPAREN, "expected ')'");
    } else if (check(L, leIdent)) {
        leToken name = advance(L);
        expr = node(L, N_IDENT, name);
    } else {
        leParserErrorf(&L->p, "unexpected token '%.*s'", peek(L).len, peek(L).start);
        return leAstError(&L->p, advance(L));
    }

    for (;;) {
        if (check(L, T_DOT)) {
            leToken dot = advance(L);
            leToken field = expect(L, leIdent, "expected field name");
            leAstNode *n = node(L, N_FIELD, dot);
            n->left = expr;
            n->right = node(L, N_IDENT, field);
            expr = n;
        } else if (check(L, T_LBRACKET)) {
            leToken lb = advance(L);
            leAstNode *idx = parseExpr(L);
            expect(L, T_RBRACKET, "expected ']'");
            leAstNode *n = node(L, N_INDEX, lb);
            n->left = expr;
            n->right = idx;
            expr = n;
        } else if (check(L, T_COLON)) {
            leToken colon = advance(L);
            leToken method = expect(L, leIdent, "expected method name");
            leAstNode *args = parseArgs(L);
            leAstNode *n = node(L, N_METHOD_CALL, colon);
            n->left = expr;
            n->right = node(L, N_IDENT, method);
            n->extra = args;
            expr = n;
        } else if (check(L, T_LPAREN) || check(L, T_LBRACE) || check(L, leString)) {
            leToken callTok = peek(L);
            leAstNode *args = parseArgs(L);
            leAstNode *n = node(L, N_CALL, callTok);
            n->left = expr;
            n->right = args;
            expr = n;
        } else {
            break;
        }
    }
    return expr;
}

static leAstNode *parseSubExpr(LuaParser *L, int minPrec) {
    leAstNode *left;
    int uPrec = getUnaryPrec(peek(L).type);
    if (uPrec >= 0) {
        leToken op = advance(L);
        leAstNode *operand = parseSubExpr(L, uPrec);
        left = node(L, N_UNOP, op);
        left->left = operand;
    } else {
        left = parseSimpleExpr(L);
    }

    for (;;) {
        int prec = getBinaryPrec(peek(L).type);
        if (prec < 0 || prec < minPrec) break;
        if (!isRightAssoc(peek(L).type) && prec == minPrec) break;

        leToken op = advance(L);
        int nextPrec = isRightAssoc(op.type) ? prec : prec + 1;
        leAstNode *right = parseSubExpr(L, nextPrec);
        leAstNode *n = node(L, N_BINOP, op);
        n->left = left;
        n->right = right;
        left = n;
    }
    return left;
}

static leAstNode *parseExpr(LuaParser *L) {
    return parseSubExpr(L, 0);
}

static leAstNode *parseExprList(LuaParser *L) {
    leAstNode *el = list(L);
    append(L, el, parseExpr(L));
    while (match(L, T_COMMA)) {
        append(L, el, parseExpr(L));
    }
    return el;
}

static leAstNode *parseVarList(LuaParser *L, leAstNode *first) {
    leAstNode *vl = list(L);
    append(L, vl, first);
    while (match(L, T_COMMA)) {
        append(L, vl, parsePrefixExpr(L));
    }
    return vl;
}

static leAstNode *parseFuncName(LuaParser *L) {
    leToken name = expect(L, leIdent, "expected function name");
    leAstNode *expr = node(L, N_IDENT, name);
    while (match(L, T_DOT)) {
        leToken field = expect(L, leIdent, "expected field name");
        leAstNode *n = node(L, N_FIELD, field);
        n->left = expr;
        n->right = node(L, N_IDENT, field);
        expr = n;
    }
    if (match(L, T_COLON)) {
        leToken method = expect(L, leIdent, "expected method name");
        leAstNode *n = node(L, N_FIELD, method);
        n->left = expr;
        n->right = node(L, N_IDENT, method);
        expr = n;
    }
    return expr;
}

static leAstNode *parseFuncBody(LuaParser *L) {
    leToken tok = peek(L);
    expect(L, T_LPAREN, "expected '('");
    leAstNode *params = list(L);
    if (!check(L, T_RPAREN)) {
        if (check(L, T_DOTDOTDOT)) {
            append(L, params, node(L, N_DOTDOTDOT_EXPR, advance(L)));
        } else {
            leToken name = expect(L, leIdent, "expected parameter name");
            append(L, params, node(L, N_IDENT, name));
            while (match(L, T_COMMA)) {
                if (check(L, T_DOTDOTDOT)) {
                    append(L, params, node(L, N_DOTDOTDOT_EXPR, advance(L)));
                    break;
                }
                name = expect(L, leIdent, "expected parameter name");
                append(L, params, node(L, N_IDENT, name));
            }
        }
    }
    expect(L, T_RPAREN, "expected ')'");
    leAstNode *body = parseBlock(L);
    expect(L, KW_END, "expected 'end'");
    leAstNode *fn = node(L, N_FUNC_EXPR, tok);
    fn->left = params;
    fn->right = body;
    return fn;
}

static leAstNode *parseStat(LuaParser *L) {
    match(L, T_SEMI);
    if (isBlockEnd(L)) return NULL;

    if (check(L, KW_IF)) {
        leToken tok = advance(L);
        leAstNode *ifn = node(L, N_IF, tok);
        leAstNode *clauses = list(L);
        ifn->left = clauses;

        leAstNode *cond = parseExpr(L);
        expect(L, KW_THEN, "expected 'then'");
        leAstNode *body = parseBlock(L);
        leAstNode *clause = node(L, N_IF, tok);
        clause->left = cond;
        clause->right = body;
        append(L, clauses, clause);

        while (check(L, KW_ELSEIF)) {
            leToken eitok = advance(L);
            cond = parseExpr(L);
            expect(L, KW_THEN, "expected 'then'");
            body = parseBlock(L);
            clause = node(L, N_IF, eitok);
            clause->left = cond;
            clause->right = body;
            append(L, clauses, clause);
        }

        if (check(L, KW_ELSE)) {
            leToken etok = advance(L);
            body = parseBlock(L);
            clause = node(L, N_IF, etok);
            clause->left = NULL;
            clause->right = body;
            append(L, clauses, clause);
        }
        expect(L, KW_END, "expected 'end'");
        return ifn;
    }

    if (check(L, KW_WHILE)) {
        leToken tok = advance(L);
        leAstNode *cond = parseExpr(L);
        expect(L, KW_DO, "expected 'do'");
        leAstNode *body = parseBlock(L);
        expect(L, KW_END, "expected 'end'");
        leAstNode *n = node(L, N_WHILE, tok);
        n->left = cond;
        n->right = body;
        return n;
    }

    if (check(L, KW_REPEAT)) {
        leToken tok = advance(L);
        leAstNode *body = parseBlock(L);
        expect(L, KW_UNTIL, "expected 'until'");
        leAstNode *cond = parseExpr(L);
        leAstNode *n = node(L, N_REPEAT, tok);
        n->left = body;
        n->right = cond;
        return n;
    }

    if (check(L, KW_FOR)) {
        leToken tok = advance(L);
        leToken name = expect(L, leIdent, "expected name");

        if (match(L, T_EQ)) {
            leAstNode *init = parseExpr(L);
            expect(L, T_COMMA, "expected ','");
            leAstNode *limit = parseExpr(L);
            leAstNode *step = NULL;
            if (match(L, T_COMMA)) {
                step = parseExpr(L);
            }
            expect(L, KW_DO, "expected 'do'");
            leAstNode *body = parseBlock(L);
            expect(L, KW_END, "expected 'end'");

            leAstNode *n = node(L, N_FOR_NUM, tok);
            leAstNode *params = list(L);
            append(L, params, node(L, N_IDENT, name));
            append(L, params, init);
            append(L, params, limit);
            if (step) append(L, params, step);
            n->left = params;
            n->right = body;
            return n;
        } else {
            leAstNode *names = list(L);
            append(L, names, node(L, N_IDENT, name));
            while (match(L, T_COMMA)) {
                leToken n2 = expect(L, leIdent, "expected name");
                append(L, names, node(L, N_IDENT, n2));
            }
            expect(L, KW_IN, "expected 'in'");
            leAstNode *iters = parseExprList(L);
            expect(L, KW_DO, "expected 'do'");
            leAstNode *body = parseBlock(L);
            expect(L, KW_END, "expected 'end'");

            leAstNode *n = node(L, N_FOR_IN, tok);
            n->left = names;
            n->right = iters;
            n->extra = body;
            return n;
        }
    }

    if (check(L, KW_FUNCTION)) {
        leToken tok = advance(L);
        leAstNode *name = parseFuncName(L);
        leAstNode *fn = parseFuncBody(L);
        leAstNode *n = node(L, N_FUNC_DECL, tok);
        n->left = name;
        n->right = fn;
        return n;
    }

    if (check(L, KW_LOCAL)) {
        leToken tok = advance(L);
        if (check(L, KW_FUNCTION)) {
            advance(L);
            leToken name = expect(L, leIdent, "expected function name");
            leAstNode *fn = parseFuncBody(L);
            leAstNode *n = node(L, N_LOCAL_FUNC, tok);
            n->left = node(L, N_IDENT, name);
            n->right = fn;
            return n;
        }
        leAstNode *names = parseNameList(L);
        leAstNode *values = NULL;
        if (match(L, T_EQ)) {
            values = parseExprList(L);
        }
        leAstNode *n = node(L, N_LOCAL, tok);
        n->left = names;
        n->right = values;
        return n;
    }

    if (check(L, KW_RETURN)) {
        leToken tok = advance(L);
        leAstNode *n = node(L, N_RETURN, tok);
        if (!isBlockEnd(L) && !check(L, T_SEMI)) {
            n->left = parseExprList(L);
        }
        match(L, T_SEMI);
        return n;
    }

    if (check(L, KW_BREAK)) {
        leToken tok = advance(L);
        return node(L, N_BREAK, tok);
    }

    if (check(L, KW_DO)) {
        advance(L);
        leAstNode *body = parseBlock(L);
        expect(L, KW_END, "expected 'end'");
        return body;
    }

    leAstNode *expr = parsePrefixExpr(L);

    if (check(L, T_COMMA) || check(L, T_EQ)) {
        leAstNode *vars = parseVarList(L, expr);
        expect(L, T_EQ, "expected '='");
        leAstNode *vals = parseExprList(L);
        leToken eqTok = leParserPrevious(&L->p);
        leAstNode *n = node(L, N_ASSIGN, eqTok);
        n->left = vars;
        n->right = vals;
        return n;
    }

    if (expr->type == N_CALL || expr->type == N_METHOD_CALL) {
        return expr;
    }

    return expr;
}

static leAstNode *parseBlock(LuaParser *L) {
    leAstNode *block = list(L);
    while (!isBlockEnd(L)) {
        leAstNode *s = parseStat(L);
        if (s) append(L, block, s);
        else break;
    }
    return block;
}

static void luaParserInit(LuaParser *L, leLexer *lex) {
    leParserInit(&L->p, lex);
}

static void luaParserFree(LuaParser *L) {
    leParserFree(&L->p);
}

static const char *nodeTypeName(int type) {
    switch (type) {
        case N_BLOCK: return "Block";
        case N_LOCAL: return "Local";
        case N_ASSIGN: return "Assign";
        case N_IF: return "If";
        case N_WHILE: return "While";
        case N_REPEAT: return "Repeat";
        case N_FOR_NUM: return "ForNum";
        case N_FOR_IN: return "ForIn";
        case N_FUNC_DECL: return "FuncDecl";
        case N_LOCAL_FUNC: return "LocalFunc";
        case N_RETURN: return "Return";
        case N_BREAK: return "Break";
        case N_CALL: return "Call";
        case N_METHOD_CALL: return "MethodCall";
        case N_INDEX: return "Index";
        case N_FIELD: return "Field";
        case N_TABLE: return "Table";
        case N_TABLE_FIELD: return "TableField";
        case N_TABLE_INDEX_FIELD: return "TableIdxField";
        case N_FUNC_EXPR: return "FuncExpr";
        case N_PARAM_LIST: return "ParamList";
        case N_VARLIST: return "VarList";
        case N_EXPRLIST: return "ExprList";
        case N_NAMELIST: return "NameList";
        case N_BINOP: return "BinOp";
        case N_UNOP: return "UnOp";
        case N_BOOL: return "Bool";
        case N_NIL: return "Nil";
        case N_NUMBER: return "Number";
        case N_STRING: return "String";
        case N_IDENT: return "Ident";
        case N_DOTDOTDOT_EXPR: return "Vararg";
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
        leAstNode *c = n->left;
        while (c) {
            printAST(c, indent + 1);
            c = c->next;
        }
    } else {
        if (n->left) printAST(n->left, indent + 1);
        if (n->right) printAST(n->right, indent + 1);
        if (n->extra) printAST(n->extra, indent + 1);
    }
}

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

    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc(len + 1);
        fread(buf, 1, len, f);
        buf[len] = '\0';
        fclose(f);
        source = buf;
    }

    leLexer lex;
    leInit(&lex, source);
    leLineComment(&lex, "--");
    leBlockComment(&lex, "--[[", "]]");

    leOperators(&lex,
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

    leKeywords(&lex,
        "and", KW_AND, "break", KW_BREAK, "do", KW_DO,
        "else", KW_ELSE, "elseif", KW_ELSEIF, "end", KW_END,
        "false", KW_FALSE, "for", KW_FOR, "function", KW_FUNCTION,
        "if", KW_IF, "in", KW_IN, "local", KW_LOCAL,
        "nil", KW_NIL, "not", KW_NOT, "or", KW_OR,
        "repeat", KW_REPEAT, "return", KW_RETURN, "then", KW_THEN,
        "true", KW_TRUE, "until", KW_UNTIL, "while", KW_WHILE,
        NULL);

    LuaParser L;
    luaParserInit(&L, &lex);

    leAstNode *program = parseBlock(&L);

    if (leParserHadError(&L.p)) {
        printf("--- ERRORS ---\n");
        leErrorNode *err = leParserFirstError(&L.p);
        while (err) {
            printf("  %s\n", leParserFormatError(&L.p, err));
            err = leNextErrorNode(err);
        }
        printf("\n");
    }

    printf("--- Lua AST ---\n");
    printAST(program, 0);

    luaParserFree(&L);
    leFree(&lex);
    if (source != test_program) free((void *)source);
    return 0;
}