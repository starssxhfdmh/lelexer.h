# lelexer.h

Single-header C library for building lexers and parsers. No dependencies beyond libc.

You define your language's keywords, operators, comment syntax, and grammar rules. `lelexer.h` takes care of tokenization, lookahead, escape decoding, error collection, and AST construction.

## Features

- Single-header, stb-style (`#define LE_IMPLEMENTATION` in one file)
- Configurable keywords, operators, string/char delimiters, and comment syntax
- Pratt parser with user-defined prefix, infix, and statement rules
- UTF-8 identifier support
- Arena-based memory allocation
- Token lookahead and pushback via ring buffer
- Custom lexer rules via callbacks
- Structured error collection with source locations
- Escape sequence decoding (`\n`, `\t`, `\xNN`, `\uNNNN`, `\UNNNNNNNN`)
- Multi-line string literals
- Optional case-insensitive keyword matching
- Optional numeric underscore separators (`1_000_000`)

## Usage

In one C source file:

```c
#define LE_IMPLEMENTATION
#include "lelexer.h"
```

In all other files:

```c
#include "lelexer.h"
```

## Example

```c
#define LE_IMPLEMENTATION
#include "lelexer.h"
#include <stdio.h>

enum {
    T_PLUS = LE_USER_START,
    T_MINUS,
    T_EQ,
    KW_LET,
    KW_RETURN,
};

int main(void) {
    leLexer lex;
    leInit(&lex, "let x = 10 + 20");

    leLineComment(&lex, "//");
    leBlockComment(&lex, "/*", "*/");

    leOperators(&lex,
        "+", T_PLUS, "-", T_MINUS, "=", T_EQ, NULL);

    leKeywords(&lex,
        "let", KW_LET, "return", KW_RETURN, NULL);

    leToken tok;
    while (leNextToken(&lex, &tok)) {
        printf("%-12s \"%.*s\"\n",
            leTokenTypeName(tok.type), tok.len, tok.start);
    }

    leFree(&lex);
    return 0;
}
```

Output:

```
User         "let"
Ident        "x"
User         "="
Integer      "10"
User         "+"
Integer      "20"
```

## API Reference

### Initialization

```c
void leInit(leLexer *lex, const char *source);
void leInitNamed(leLexer *lex, const char *source, const char *fileName);
void leInitBuffer(leLexer *lex, const char *data, int length, const char *fileName);
void leReset(leLexer *lex);
void leFree(leLexer *lex);
```

### Language Configuration

```c
void leKeywords(leLexer *lex, ...);          // (word, type) pairs, NULL-terminated
void leOperators(leLexer *lex, ...);         // (op, type) pairs, NULL-terminated
void leLineComment(leLexer *lex, const char *prefix);
void leBlockComment(leLexer *lex, const char *start, const char *end);
void leStringDelim(leLexer *lex, char delim, char escape);
void leCharDelim(leLexer *lex, char delim);
void leMultiLineString(leLexer *lex, const char *delim);
void leSetFlags(leLexer *lex, int flags);
void leAddFlags(leLexer *lex, int flags);
void leCustomRule(leLexer *lex, leCustomRuleFn fn);
```

### Token Reading

```c
bool leNextToken(leLexer *lex, leToken *tok);        // consume next token
bool lePeekToken(leLexer *lex, leToken *tok);        // peek without consuming
bool lePeekTokenN(leLexer *lex, leToken *tok, int n); // peek N tokens ahead
void leUngetToken(leLexer *lex, const leToken *tok); // push token back
```

### Parser

```c
void      leParserInit(leParser *p, leLexer *lex);
void      leParserFree(leParser *p);
void      leParserPrefix(leParser *p, int tokenType, leParsePrefixFn fn);
void      leParserInfix(leParser *p, int tokenType, int prec, int assoc, leParseInfixFn fn);
void      leParserStmt(leParser *p, int tokenType, leParseStmtFn fn);
leAstNode *leParseExpr(leParser *p, int minPrec);
leAstNode *leParseStmt(leParser *p);
leAstNode *leParseAll(leParser *p);
```

### AST Construction

```c
leAstNode *leAstAtom(leParser *p, leToken tok);
leAstNode *leAstUnary(leParser *p, leToken op, leAstNode *operand);
leAstNode *leAstBinary(leParser *p, leToken op, leAstNode *left, leAstNode *right);
leAstNode *leAstList(leParser *p);
leAstNode *leAstListAppend(leParser *p, leAstNode *list, leAstNode *item);
void       leAstPrint(leAstNode *node, int indent);
```

## Flags

Default flags after `leInit()`: `LE_SKIP_WHITESPACE | LE_SKIP_COMMENTS | LE_PARSE_NUMBERS | LE_DECODE_ESCAPES`.

| Flag | Effect |
|---|---|
| `LE_SKIP_WHITESPACE` | Skip whitespace between tokens |
| `LE_SKIP_COMMENTS` | Skip comments between tokens |
| `LE_PARSE_NUMBERS` | Parse numeric values into `intVal`/`floatVal` |
| `LE_DECODE_ESCAPES` | Decode escape sequences in strings |
| `LE_CASE_KEYWORDS` | Case-insensitive keyword matching |
| `LE_MULTILINE_STRING` | Allow strings to span multiple lines |
| `LE_NUM_UNDERSCORE` | Allow `_` as digit separator |
| `LE_COLLECT_ERRORS` | Collect errors into a list |

## Compile-Time Options

| Macro | Default | Description |
|---|---|---|
| `LE_ARENA_DEFAULT_CAP` | `8192` | Initial arena block size (bytes) |
| `LE_HASH_SIZE` | `256` | Keyword hash table buckets |
| `LE_PARSER_HASH_SIZE` | `64` | Parser rule hash table buckets |
| `LE_STATIC` | | Make all functions `static` |
| `LEDEF` | | Override function linkage |

## Built-in Token Types

User-defined types should start at `LE_USER_START` (64).

| Type | Value |
|---|---|
| `leEOF` | End of input |
| `leError` | Lexer error |
| `leInteger` | Integer literal |
| `leFloat` | Float literal |
| `leString` | String literal |
| `leChar` | Character literal |
| `leIdent` | Identifier |
| `leSingleChar` | Unrecognized single character |

## Examples

[`examples/simpleLuaParser.c`](examples/simpleLuaParser.c) is a complete Lua 5.1 parser (~750 lines) built with `lelexer.h`. It covers keyword/operator registration, recursive-descent parsing, AST construction, and error reporting.

```bash
gcc -o simpleLuaParser examples/simpleLuaParser.c -I.
./simpleLuaParser              # run built-in test
./simpleLuaParser script.lua   # parse a file
```

## License

[MIT](LICENSE.md) © starssxhfdmh
