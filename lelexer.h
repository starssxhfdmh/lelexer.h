/***********************************************************************************
*
*   lelexer.h - Single-header configurable lexer/tokenizer library
*
*   USAGE:
*       #define LE_IMPLEMENTATION in exactly one source file before including this
*       header to get the implementation. In all other files, just include the
*       header normally.
*
*   FEATURES:
*       - Single-header, no external dependencies beyond libc
*       - Configurable token types: keywords, multi-char operators, strings, numbers
*       - Unicode identifier support (UTF-8)
*       - Arena-based allocation, minimal heap fragmentation
*       - Token lookahead and pushback via ring buffer
*       - Custom lexer rules via callback functions
*       - Structured error collection with source locations and callbacks
*       - Escape sequence decoding for strings (\n, \t, \xNN, \uNNNN, \UNNNNNNNN)
*       - Configurable comment syntax (line and block)
*       - Multi-line string literal support
*       - Case-insensitive keyword matching (optional)
*       - Numeric underscore separators (optional)
*
*   CONFIGURATION:
*       #define LE_ARENA_DEFAULT_CAP    - Initial arena block size (default: 8192)
*       #define LE_HASH_SIZE            - Keyword hash table bucket count (default: 256)
*       #define LE_STATIC               - Make all API functions static
*       #define LEDEF                   - Override function linkage specifier
*
************************************************************************************/

#ifndef LELEXER_H
#define LELEXER_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

// Function linkage specifier (stb-style)
#ifndef LEDEF
#ifdef LE_STATIC
#define LEDEF static
#else
#define LEDEF extern
#endif
#endif

//----------------------------------------------------------------------------------
// Configuration Defaults
//----------------------------------------------------------------------------------
#ifndef LE_ARENA_DEFAULT_CAP
#define LE_ARENA_DEFAULT_CAP (8 * 1024)         // Default arena block capacity in bytes
#endif

#ifndef LE_HASH_SIZE
#define LE_HASH_SIZE 256                         // Keyword hash table bucket count
#endif

//----------------------------------------------------------------------------------
// Built-in Token Types
//----------------------------------------------------------------------------------
// NOTE: User-defined token types should start at LE_USER_START (64) or higher
// to avoid collisions with built-in types.
enum {
    leEOF = 0,                                  // End of input
    leError,                                    // Lexer error (malformed token)
    leInteger,                                  // Integer literal (decimal, hex 0x, binary 0b)
    leFloat,                                    // Floating-point literal (with dot or exponent)
    leString,                                   // String literal (delimited by stringDelim)
    leChar,                                     // Character literal (delimited by charDelim)
    leIdent,                                    // Identifier (not matching any registered keyword)
    leSingleChar,                               // Single unrecognized character, returned as-is
    LE_USER_START = 64                          // First available ID for user-defined token types
};

//----------------------------------------------------------------------------------
// Lexer Behavior Flags
//----------------------------------------------------------------------------------
// NOTE: Combine with bitwise OR and pass to leSetFlags()/leAddFlags().
// Default flags after leInit(): LE_SKIP_WHITESPACE | LE_SKIP_COMMENTS |
//                                LE_PARSE_NUMBERS  | LE_DECODE_ESCAPES
enum {
    LE_SKIP_WHITESPACE  = 1 << 0,               // Skip whitespace between tokens automatically
    LE_SKIP_COMMENTS    = 1 << 1,               // Skip line/block comments between tokens
    LE_PARSE_NUMBERS    = 1 << 2,               // Parse numeric values into intVal/floatVal fields
    LE_CASE_KEYWORDS    = 1 << 3,               // Case-insensitive keyword matching
    LE_DECODE_ESCAPES   = 1 << 4,               // Decode escape sequences in strings into .decoded
    LE_MULTILINE_STRING = 1 << 5,               // Allow string literals to span multiple lines
    LE_NUM_UNDERSCORE   = 1 << 6,               // Allow underscores as digit separators (e.g. 1_000)
    LE_COLLECT_ERRORS   = 1 << 7                // Collect errors into error list instead of just returning them
};

//----------------------------------------------------------------------------------
// Error Severity Levels
//----------------------------------------------------------------------------------
enum {
    LE_SEVERITY_NOTE    = 0,                     // Informational, non-critical diagnostic
    LE_SEVERITY_WARNING = 1,                     // Potential problem, lexing continues normally
    LE_SEVERITY_ERROR   = 2                      // Definite error, token is malformed
};

//----------------------------------------------------------------------------------
// Error Codes
//----------------------------------------------------------------------------------
// NOTE: User-defined error codes should start at LE_ERR_USER (256) or higher.
enum {
    LE_ERR_NONE = 0,                             // Generic or unspecified error
    LE_ERR_UNTERMINATED_STRING,                  // String literal missing closing delimiter
    LE_ERR_UNTERMINATED_CHAR,                    // Character literal missing closing delimiter
    LE_ERR_UNTERMINATED_ESCAPE,                  // Escape sequence at end of input
    LE_ERR_UNTERMINATED_BLOCK_COMMENT,           // Block comment missing closing delimiter
    LE_ERR_UNTERMINATED_MULTILINE_STRING,        // Multi-line string missing closing delimiter
    LE_ERR_INVALID_HEX_LITERAL,                  // Hex literal with no valid digits after 0x
    LE_ERR_INVALID_BIN_LITERAL,                  // Binary literal with no valid digits after 0b
    LE_ERR_INVALID_FLOAT_EXPONENT,               // Float exponent with no digits after e/E
    LE_ERR_INVALID_ESCAPE,                       // Unrecognized escape sequence
    LE_ERR_INVALID_UNICODE,                      // Invalid Unicode codepoint or encoding
    LE_ERR_USER = 256                            // First available ID for user-defined error codes
};

//----------------------------------------------------------------------------------
// Structures Definition
//----------------------------------------------------------------------------------

// leSourceLoc, position within source text
typedef struct leSourceLoc {
    int line;                                   // 1-based line number
    int col;                                    // 1-based column number
    int offset;                                 // 0-based byte offset from source start
} leSourceLoc;

// leToken, single lexed token with location and value
typedef struct leToken {
    int          type;                          // Token type (built-in or user-defined)
    const char  *start;                         // Pointer into source buffer at token start
    int          len;                           // Token length in bytes (in source)
    leSourceLoc  loc;                           // Start position in source
    leSourceLoc  endLoc;                        // End position in source (exclusive)
    union {
        long long intVal;                       // Parsed integer value (when LE_PARSE_NUMBERS set)
        double    floatVal;                     // Parsed float value (when LE_PARSE_NUMBERS set)
    };
    char        *decoded;                       // Decoded string content (escape sequences resolved)
    int          decodedLen;                    // Length of decoded content in bytes
} leToken;

// leErrorNode, single entry in the error linked list
typedef struct leErrorNode {
    const char         *message;                // Human-readable error description (arena-allocated)
    int                 code;                   // Error code (LE_ERR_* or user-defined >= LE_ERR_USER)
    int                 severity;               // LE_SEVERITY_NOTE, WARNING, or ERROR
    leSourceLoc         loc;                    // Error start location in source
    leSourceLoc         endLoc;                 // Error end location (for span highlighting)
    struct leErrorNode *next;                   // Next error in the list
} leErrorNode;

// leErrorList, collected lexer errors
typedef struct leErrorList {
    leErrorNode *head;                          // First error node
    leErrorNode *tail;                          // Last error node
    int          count;                         // Total number of errors
} leErrorList;

// leArenaBlock, single block in the arena allocator chain
typedef struct leArenaBlock {
    struct leArenaBlock *next;                  // Next block in chain
    int                  cap;                   // Block capacity in bytes
    int                  used;                  // Bytes currently used
    char                 data[];                // Flexible array member for storage
} leArenaBlock;

// leArena, growing arena allocator (linked list of blocks)
typedef struct leArena {
    leArenaBlock *head;                         // First block in the chain
    leArenaBlock *current;                      // Current block for allocation
} leArena;

// leTokenBuffer, ring buffer for token lookahead and pushback
typedef struct leTokenBuffer {
    leToken *tokens;                            // Ring buffer storage (arena-allocated)
    int      head;                              // Index of first valid token in ring
    int      count;                             // Number of tokens currently buffered
    int      cap;                               // Allocated capacity
} leTokenBuffer;

// leKeywordEntry, node in the keyword hash table (separate chaining)
typedef struct leKeywordEntry {
    char                  *word;                // Keyword string (arena-allocated copy)
    int                    len;                 // Keyword length in bytes
    int                    type;                // Token type to assign on match
    struct leKeywordEntry *next;                // Next entry in hash chain
} leKeywordEntry;

// leTrieNode, node in the operator prefix trie (child/sibling linked list)
typedef struct leTrieNode {
    unsigned char      ch;                      // Character label on edge to this node
    int                type;                    // Operator token type (-1 if not a terminal)
    struct leTrieNode *child;                   // First child node (next character depth)
    struct leTrieNode *sibling;                 // Next sibling at same depth
} leTrieNode;

// Forward declaration and custom rule callback type
typedef struct leLexer leLexer;
typedef bool (*leCustomRuleFn)(leLexer *lex, leToken *tok);     // Custom lexer rule: return true if token produced
typedef bool (*leErrorHandlerFn)(leLexer *lex, const leErrorNode *err, void *userData); // Error callback: return true to collect, false to discard

// leCustomRuleNode, linked list node for registered custom rules
typedef struct leCustomRuleNode {
    leCustomRuleFn           fn;                // Rule callback function
    struct leCustomRuleNode *next;              // Next rule in the chain
} leCustomRuleNode;

// leLexer, main lexer state
struct leLexer {
    // Source tracking
    const char *source;                         // Original source buffer pointer
    const char *current;                        // Current read position in source
    const char *tokenStart;                     // Start of the current token being lexed
    const char *fileName;                       // Source file name (for diagnostics)
    int         sourceLen;                      // Total source length in bytes
    int         line;                           // Current line number (1-based)
    int         col;                            // Current column number (1-based)
    int         offset;                         // Current byte offset (0-based)
    int         startLine;                      // Token start line
    int         startCol;                       // Token start column
    int         startOffset;                    // Token start offset
    int         flags;                          // Active behavior flags (LE_SKIP_WHITESPACE, etc.)

    // Comment syntax configuration
    char       *lineComment;                    // Line comment prefix string (e.g. "//")
    int         lineCommentLen;                 // Length of line comment prefix
    char       *blockCommentStart;              // Block comment open string (e.g. "/*")
    int         blockCommentStartLen;           // Length of block comment open
    char       *blockCommentEnd;                // Block comment close string (e.g. "*/")
    int         blockCommentEndLen;             // Length of block comment close

    // String/char literal configuration
    char        stringDelim;                    // String delimiter character (default: '"')
    char        charDelim;                      // Char literal delimiter (default: '\'')
    char        escapeChar;                     // Escape character (default: '\\')
    char       *multiLineDelim;                 // Multi-line string delimiter (e.g. "\"\"\"")
    int         multiLineDelimLen;              // Length of multi-line delimiter

    // Internal subsystems
    leArena             configArena;                    // Arena for config data (keywords, operators, comments) - survives leReset()
    leArena             arena;                          // Arena for runtime data (tokens, decoded strings, errors) - reset by leReset()
    leKeywordEntry     *keywordTable[LE_HASH_SIZE];     // Keyword hash table
    leTrieNode         *operatorTrie;                   // Operator prefix trie root
    int                 maxOperatorLen;                 // Longest registered operator length
    leCustomRuleNode   *customRules;                    // Linked list of custom lexer rules
    leTokenBuffer       tokenBuffer;                    // Token pushback/lookahead buffer
    leErrorList         errors;                         // Collected error list

    // Error handling configuration
    leErrorHandlerFn    errorHandler;                   // User error callback (NULL = default behavior)
    void               *errorUserData;                  // Opaque pointer passed to errorHandler
    int                 maxErrors;                      // Max errors before lexer stops collecting (0 = unlimited)
};

//----------------------------------------------------------------------------------
// Lexer Initialization and Lifecycle
//----------------------------------------------------------------------------------
LEDEF void         leInit(leLexer *lex, const char *source);                                       // Initialize lexer with null-terminated source string
LEDEF void         leInitNamed(leLexer *lex, const char *source, const char *fileName);            // Initialize lexer with source and file name for diagnostics
LEDEF void         leInitBuffer(leLexer *lex, const char *data, int length, const char *fileName); // Initialize lexer with non-null-terminated buffer and explicit length
LEDEF void         leReset(leLexer *lex);                                                          // Reset lexer to beginning of source, keep configuration
LEDEF void         leFree(leLexer *lex);                                                           // Free all arena memory and reset internal state

//----------------------------------------------------------------------------------
// Language Configuration
//----------------------------------------------------------------------------------
// NOTE: Keywords and operators use variadic or array interfaces.
// Variadic versions expect (const char *name, int type, ..., NULL) sentinel-terminated pairs.
LEDEF void         leKeywords(leLexer *lex, ...);                                                  // Register keywords as variadic (word, type) pairs, NULL-terminated
LEDEF void         leKeywordsArray(leLexer *lex, const char **words, const int *types, int count); // Register keywords from parallel arrays
LEDEF void         leOperators(leLexer *lex, ...);                                                 // Register multi-char operators as variadic (op, type) pairs, NULL-terminated
LEDEF void         leOperatorsArray(leLexer *lex, const char **ops, const int *types, int count);  // Register operators from parallel arrays
LEDEF void         leLineComment(leLexer *lex, const char *prefix);                                // Set line comment prefix (e.g. "//", "#")
LEDEF void         leBlockComment(leLexer *lex, const char *start, const char *end);               // Set block comment delimiters (e.g. "/*", "*/")
LEDEF void         leStringDelim(leLexer *lex, char delim, char escape);                           // Set string delimiter and shared escape character
LEDEF void         leCharDelim(leLexer *lex, char delim);                                          // Set char literal delimiter (escape char shared with string, set via leStringDelim)
LEDEF void         leMultiLineString(leLexer *lex, const char *delim);                             // Enable multi-line strings with given delimiter (e.g. "\"\"\"")
LEDEF void         leSetFlags(leLexer *lex, int flags);                                            // Replace all lexer flags
LEDEF void         leAddFlags(leLexer *lex, int flags);                                            // Enable additional lexer flags (bitwise OR)
LEDEF void         leRemoveFlags(leLexer *lex, int flags);                                         // Disable specific lexer flags (bitwise AND NOT)
LEDEF void         leCustomRule(leLexer *lex, leCustomRuleFn fn);                                  // Register a custom lexer rule callback (checked before built-in rules)

//----------------------------------------------------------------------------------
// Token Reading
//----------------------------------------------------------------------------------
LEDEF bool         leNextToken(leLexer *lex, leToken *tok);                                        // Consume and return the next token, returns false on EOF
LEDEF bool         lePeekToken(leLexer *lex, leToken *tok);                                        // Peek at the next token without consuming it
LEDEF bool         lePeekTokenN(leLexer *lex, leToken *tok, int n);                                // Peek N tokens ahead (0-indexed), buffering as needed
LEDEF void         leUngetToken(leLexer *lex, const leToken *tok);                                 // Push a token back onto the front of the stream

//----------------------------------------------------------------------------------
// Token Construction (for custom rules)
//----------------------------------------------------------------------------------
LEDEF leToken      leMakeToken(leLexer *lex, int type);                                            // Create a token from tokenStart to current position
LEDEF leToken      leMakeError(leLexer *lex, const char *msg);                                     // Create error token with LE_ERR_NONE code and LE_SEVERITY_ERROR
LEDEF leToken      leMakeErrorCode(leLexer *lex, int code, const char *msg);                       // Create error token with specific error code, start points to source
LEDEF leToken      leMakeErrorf(leLexer *lex, int code, const char *fmt, ...);                     // Create error token with printf-style formatted message

//----------------------------------------------------------------------------------
// Error Configuration
//----------------------------------------------------------------------------------
LEDEF void         leSetErrorHandler(leLexer *lex, leErrorHandlerFn fn, void *userData);           // Set error callback, invoked on every error before collection
LEDEF void         leSetMaxErrors(leLexer *lex, int max);                                          // Set max error limit (0 = unlimited, default)

//----------------------------------------------------------------------------------
// Error Handling
//----------------------------------------------------------------------------------
LEDEF void         leAddError(leLexer *lex, const char *msg, leSourceLoc loc);                     // Add error with LE_ERR_NONE code to the error list
LEDEF void         leAddErrorFull(leLexer *lex, int code, int severity, const char *msg, leSourceLoc loc, leSourceLoc endLoc); // Add error with full details
LEDEF bool         leHasErrors(leLexer *lex);                                                      // Check if any errors have been collected
LEDEF int          leErrorCount(leLexer *lex);                                                     // Get total number of collected errors
LEDEF int          leErrorCountBySeverity(leLexer *lex, int severity);                             // Count errors matching a specific severity level
LEDEF leErrorNode *leGetError(leLexer *lex, int index);                                            // Get error by index (0-based), returns NULL if out of range
LEDEF leErrorNode *leFirstError(leLexer *lex);                                                     // Get first error node in the list
LEDEF leErrorNode *leNextErrorNode(leErrorNode *node);                                             // Get next error node from current node
LEDEF void         leClearErrors(leLexer *lex);                                                    // Clear all collected errors
LEDEF const char  *leFormatError(leLexer *lex, const leErrorNode *err);                            // Format as "file:line:col: severity: message" (arena-allocated)
LEDEF const char  *leErrorCodeName(int code);                                                      // Get human-readable name for a built-in error code
LEDEF const char  *leSeverityName(int severity);                                                   // Get severity label ("note", "warning", "error")

//----------------------------------------------------------------------------------
// Low-level Source Navigation (for custom rules)
//----------------------------------------------------------------------------------
LEDEF char         lePeek(leLexer *lex);                                                           // Peek at current character without advancing
LEDEF char         lePeekN(leLexer *lex, int n);                                                   // Peek N characters ahead (0 = current)
LEDEF char         leAdvance(leLexer *lex);                                                        // Consume and return current character, update line/col
LEDEF void         leAdvanceN(leLexer *lex, int n);                                                // Advance N characters
LEDEF bool         leMatch(leLexer *lex, char c);                                                  // Consume current char if it matches c, return success
LEDEF bool         leMatchStr(leLexer *lex, const char *str, int len);                             // Check if next len bytes match str (does not consume)
LEDEF void         leMarkStart(leLexer *lex);                                                      // Mark current position as the start of a new token
LEDEF leSourceLoc  leGetLoc(leLexer *lex);                                                         // Get current source location as leSourceLoc
LEDEF bool         leAtEnd(leLexer *lex);                                                          // Check if lexer has reached end of source

//----------------------------------------------------------------------------------
// Token Utilities
//----------------------------------------------------------------------------------
LEDEF const char  *leTokenTypeName(int type);                                                      // Get human-readable name for a built-in token type
LEDEF char        *leTokenString(leLexer *lex, const leToken *tok);                                // Copy token text to a null-terminated arena-allocated string

//----------------------------------------------------------------------------------
// Arena Allocator
//----------------------------------------------------------------------------------
// NOTE: Arena memory is freed all at once via leFree(). Individual allocations
// cannot be freed independently.
LEDEF void        *leArenaAlloc(leArena *a, int size);                                             // Allocate size bytes from arena (8-byte aligned)
LEDEF void         leArenaReset(leArena *a);                                                       // Reset arena usage counters (memory reused, not freed)
LEDEF char        *leArenaDupStr(leArena *a, const char *s, int len);                              // Duplicate len bytes of s as a null-terminated arena string

//----------------------------------------------------------------------------------
// Character Classification
//----------------------------------------------------------------------------------
LEDEF bool         leIsDigit(char c);                                                              // Check if c is ASCII digit [0-9]
LEDEF bool         leIsAlpha(char c);                                                              // Check if c is ASCII letter [a-zA-Z] or underscore
LEDEF bool         leIsAlphaNum(char c);                                                           // Check if c is ASCII letter, digit, or underscore
LEDEF bool         leIsHexDigit(char c);                                                           // Check if c is hexadecimal digit [0-9a-fA-F]
LEDEF bool         leIsSpace(char c);                                                              // Check if c is whitespace (space, tab, CR, LF)

//----------------------------------------------------------------------------------
// UTF-8 Utilities
//----------------------------------------------------------------------------------
LEDEF bool         leIsUtf8Start(unsigned char c);                                                 // Check if byte is a UTF-8 multi-byte sequence start
LEDEF bool         leIsUtf8Cont(unsigned char c);                                                  // Check if byte is a UTF-8 continuation byte
LEDEF int          leUtf8Decode(const char *s, int len, int *codepoint);                           // Decode one UTF-8 codepoint, return bytes consumed (0 on invalid)
LEDEF int          leUtf8Encode(int cp, char *out);                                                // Encode codepoint to UTF-8, return bytes written
LEDEF bool         leIsUnicodeIdStart(int cp);                                                     // Check if codepoint is valid identifier start (UAX #31 subset)
LEDEF bool         leIsUnicodeIdCont(int cp);                                                      // Check if codepoint is valid identifier continuation

#ifdef LE_IMPLEMENTATION

static leArenaBlock *leArenaNewBlock(int cap) {
    leArenaBlock *b = (leArenaBlock *)malloc(sizeof(leArenaBlock) + cap);
    if (!b) return NULL;
    b->next = NULL;
    b->cap = cap;
    b->used = 0;
    return b;
}

static void leArenaInit(leArena *a) {
    a->head = leArenaNewBlock(LE_ARENA_DEFAULT_CAP);
    a->current = a->head;
}

static void leArenaDestroy(leArena *a) {
    leArenaBlock *b = a->head;
    while (b) {
        leArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
    a->current = NULL;
}

LEDEF void leArenaReset(leArena *a) {
    leArenaBlock *b = a->head;
    while (b) {
        b->used = 0;
        b = b->next;
    }
    a->current = a->head;
}

LEDEF void *leArenaAlloc(leArena *a, int size) {
    int align = (size + 7) & ~7;
    if (!a->current) return NULL;
    if (a->current->used + align > a->current->cap) {
        leArenaBlock *scan = a->current->next;
        while (scan && (scan->cap - scan->used) < align) scan = scan->next;
        if (scan) {
            a->current = scan;
        } else {
            int newCap = a->current->cap * 2;
            if (newCap < align) newCap = align * 2;
            leArenaBlock *b = leArenaNewBlock(newCap);
            if (!b) return NULL;
            b->next = a->current->next;
            a->current->next = b;
            a->current = b;
        }
    }
    void *ptr = a->current->data + a->current->used;
    a->current->used += align;
    return ptr;
}

LEDEF char *leArenaDupStr(leArena *a, const char *s, int len) {
    char *dup = (char *)leArenaAlloc(a, len + 1);
    if (!dup) return NULL;
    memcpy(dup, s, len);
    dup[len] = '\0';
    return dup;
}

static void leTokenBufferInit(leTokenBuffer *buf) {
    buf->tokens = NULL;
    buf->head = 0;
    buf->count = 0;
    buf->cap = 0;
}

static void leTokenBufferFree(leTokenBuffer *buf) {
    free(buf->tokens);
    buf->tokens = NULL;
    buf->head = 0;
    buf->count = 0;
    buf->cap = 0;
}

static void leTokenBufferGrow(leTokenBuffer *buf) {
    int newCap = buf->cap == 0 ? 8 : buf->cap * 2;
    leToken *newTokens = (leToken *)malloc(newCap * sizeof(leToken));
    if (!newTokens) return;
    for (int i = 0; i < buf->count; i++) {
        newTokens[i] = buf->tokens[(buf->head + i) % buf->cap];
    }
    free(buf->tokens);
    buf->tokens = newTokens;
    buf->head = 0;
    buf->cap = newCap;
}

static void leTokenBufferPushFront(leTokenBuffer *buf, const leToken *tok) {
    if (buf->count >= buf->cap) {
        leTokenBufferGrow(buf);
    }
    if (buf->count < buf->cap) {
        buf->head = (buf->head - 1 + buf->cap) % buf->cap;
        buf->tokens[buf->head] = *tok;
        buf->count++;
    }
}

static void leTokenBufferPushBack(leTokenBuffer *buf, const leToken *tok) {
    if (buf->count >= buf->cap) {
        leTokenBufferGrow(buf);
    }
    if (buf->count < buf->cap) {
        buf->tokens[(buf->head + buf->count) % buf->cap] = *tok;
        buf->count++;
    }
}

static bool leTokenBufferPop(leTokenBuffer *buf, leToken *tok) {
    if (buf->count == 0) return false;
    *tok = buf->tokens[buf->head];
    buf->head = (buf->head + 1) % buf->cap;
    buf->count--;
    return true;
}

static leToken *leTokenBufferPeek(leTokenBuffer *buf, int index) {
    if (index < 0 || index >= buf->count) return NULL;
    return &buf->tokens[(buf->head + index) % buf->cap];
}

static void leTokenBufferClear(leTokenBuffer *buf) {
    buf->head = 0;
    buf->count = 0;
}

static void leErrorListInit(leErrorList *list) {
    list->head = NULL;
    list->tail = NULL;
    list->count = 0;
}

static void leErrorListPush(leArena *a, leErrorList *list, int code, int severity, const char *msg, leSourceLoc loc, leSourceLoc endLoc) {
    leErrorNode *node = (leErrorNode *)leArenaAlloc(a, sizeof(leErrorNode));
    if (!node) return;
    node->message = msg;
    node->code = code;
    node->severity = severity;
    node->loc = loc;
    node->endLoc = endLoc;
    node->next = NULL;
    if (list->tail) {
        list->tail->next = node;
        list->tail = node;
    } else {
        list->head = node;
        list->tail = node;
    }
    list->count++;
}

static void leErrorListClear(leErrorList *list) {
    list->head = NULL;
    list->tail = NULL;
    list->count = 0;
}

static unsigned int leHashStr(const char *s, int len) {
    unsigned int h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

static unsigned int leHashStrNoCase(const char *s, int len) {
    unsigned int h = 2166136261u;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

LEDEF bool leIsDigit(char c) { return c >= '0' && c <= '9'; }
LEDEF bool leIsAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
LEDEF bool leIsAlphaNum(char c) { return leIsAlpha(c) || leIsDigit(c); }
LEDEF bool leIsHexDigit(char c) { return leIsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
LEDEF bool leIsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
LEDEF bool leIsUtf8Start(unsigned char c) { return (c & 0xC0) == 0xC0; }
LEDEF bool leIsUtf8Cont(unsigned char c) { return (c & 0xC0) == 0x80; }

LEDEF int leUtf8Decode(const char *s, int len, int *codepoint) {
    if (len <= 0) { *codepoint = 0; return 0; }
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) {
        *codepoint = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && len >= 2) {
        if (!leIsUtf8Cont((unsigned char)s[1])) { *codepoint = 0xFFFD; return 1; }
        int cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        if (cp < 0x80) { *codepoint = 0xFFFD; return 2; }
        *codepoint = cp;
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && len >= 3) {
        if (!leIsUtf8Cont((unsigned char)s[1]) || !leIsUtf8Cont((unsigned char)s[2])) { *codepoint = 0xFFFD; return 1; }
        int cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        if (cp < 0x800) { *codepoint = 0xFFFD; return 3; }
        if (cp >= 0xD800 && cp <= 0xDFFF) { *codepoint = 0xFFFD; return 3; }
        *codepoint = cp;
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && len >= 4) {
        if (!leIsUtf8Cont((unsigned char)s[1]) || !leIsUtf8Cont((unsigned char)s[2]) || !leIsUtf8Cont((unsigned char)s[3])) { *codepoint = 0xFFFD; return 1; }
        int cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) { *codepoint = 0xFFFD; return 4; }
        *codepoint = cp;
        return 4;
    }
    *codepoint = 0xFFFD;
    return 1;
}

LEDEF int leUtf8Encode(int cp, char *out) {
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        out[0] = (char)0xEF; out[1] = (char)0xBF; out[2] = (char)0xBD;
        return 3;
    }
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

LEDEF bool leIsUnicodeIdStart(int cp) {
    if (cp == '_') return true;
    if (cp >= 'a' && cp <= 'z')         return true;
    if (cp >= 'A' && cp <= 'Z')         return true;
    if (cp >= 0x00C0 && cp <= 0x00D6)   return true;
    if (cp >= 0x00D8 && cp <= 0x00F6)   return true;
    if (cp >= 0x00F8 && cp <= 0x02FF)   return true;
    if (cp >= 0x0370 && cp <= 0x037D)   return true;
    if (cp >= 0x037F && cp <= 0x1FFF)   return true;
    if (cp >= 0x200C && cp <= 0x200D)   return true;
    if (cp >= 0x2070 && cp <= 0x218F)   return true;
    if (cp >= 0x2C00 && cp <= 0x2FEF)   return true;
    if (cp >= 0x3001 && cp <= 0xD7FF)   return true;
    if (cp >= 0xF900 && cp <= 0xFDCF)   return true;
    if (cp >= 0xFDF0 && cp <= 0xFFFD)   return true;
    if (cp >= 0x10000 && cp <= 0xEFFFF) return true;
    return false;
}

LEDEF bool leIsUnicodeIdCont(int cp) {
    if (leIsUnicodeIdStart(cp))         return true;
    if (cp >= '0' && cp <= '9')         return true;
    if (cp >= 0x0300 && cp <= 0x036F)   return true;
    if (cp >= 0x1DC0 && cp <= 0x1DFF)   return true;
    if (cp >= 0x20D0 && cp <= 0x20FF)   return true;
    if (cp >= 0xFE20 && cp <= 0xFE2F)   return true;
    return false;
}

static void leInitInternal(leLexer *lex) {
    lex->line = 1;
    lex->col = 1;
    lex->offset = 0;
    lex->startLine = 1;
    lex->startCol = 1;
    lex->startOffset = 0;
    lex->flags = LE_SKIP_WHITESPACE | LE_SKIP_COMMENTS | LE_PARSE_NUMBERS | LE_DECODE_ESCAPES;
    lex->lineComment = NULL;
    lex->lineCommentLen = 0;
    lex->blockCommentStart = NULL;
    lex->blockCommentStartLen = 0;
    lex->blockCommentEnd = NULL;
    lex->blockCommentEndLen = 0;
    lex->stringDelim = '"';
    lex->charDelim = '\'';
    lex->escapeChar = '\\';
    lex->multiLineDelim = NULL;
    lex->multiLineDelimLen = 0;
    lex->operatorTrie = NULL;
    lex->maxOperatorLen = 0;
    lex->customRules = NULL;
    memset(lex->keywordTable, 0, sizeof(lex->keywordTable));
    leArenaInit(&lex->configArena);
    leArenaInit(&lex->arena);
    leTokenBufferInit(&lex->tokenBuffer);
    leErrorListInit(&lex->errors);
    lex->errorHandler = NULL;
    lex->errorUserData = NULL;
    lex->maxErrors = 0;
}

LEDEF void leInit(leLexer *lex, const char *source) {
    if (!lex) return;
    memset(lex, 0, sizeof(leLexer));
    lex->source = source ? source : "";
    lex->current = lex->source;
    lex->tokenStart = lex->source;
    lex->sourceLen = source ? (int)strlen(source) : 0;
    lex->fileName = "<string>";
    leInitInternal(lex);
}

LEDEF void leInitNamed(leLexer *lex, const char *source, const char *fileName) {
    leInit(lex, source);
    if (lex) lex->fileName = fileName ? fileName : "<string>";
}

LEDEF void leInitBuffer(leLexer *lex, const char *data, int length, const char *fileName) {
    if (!lex) return;
    memset(lex, 0, sizeof(leLexer));
    lex->source = data ? data : "";
    lex->current = lex->source;
    lex->tokenStart = lex->source;
    lex->sourceLen = data ? length : 0;
    lex->fileName = fileName ? fileName : "<buffer>";
    leInitInternal(lex);
}

LEDEF void leReset(leLexer *lex) {
    leArenaReset(&lex->arena);  // Only reset runtime arena; configArena (keywords, operators, etc.) is preserved
    lex->current = lex->source;
    lex->tokenStart = lex->source;
    lex->line = 1;
    lex->col = 1;
    lex->offset = 0;
    lex->startLine = 1;
    lex->startCol = 1;
    lex->startOffset = 0;
    leTokenBufferClear(&lex->tokenBuffer);
    leErrorListClear(&lex->errors);
}

LEDEF void leFree(leLexer *lex) {
    leArenaDestroy(&lex->configArena);
    leArenaDestroy(&lex->arena);
    memset(lex->keywordTable, 0, sizeof(lex->keywordTable));
    lex->operatorTrie = NULL;
    lex->customRules = NULL;
    lex->lineComment = NULL;
    lex->blockCommentStart = NULL;
    lex->blockCommentEnd = NULL;
    lex->multiLineDelim = NULL;
    leTokenBufferFree(&lex->tokenBuffer);
    leErrorListInit(&lex->errors);
}

static void leAddKeyword(leLexer *lex, const char *word, int type) {
    int len = (int)strlen(word);
    char *wordCopy = leArenaDupStr(&lex->configArena, word, len);
    if (!wordCopy) return;

    unsigned int h;
    if (lex->flags & LE_CASE_KEYWORDS)
        h = leHashStrNoCase(word, len) % LE_HASH_SIZE;
    else
        h = leHashStr(word, len) % LE_HASH_SIZE;

    leKeywordEntry *e = (leKeywordEntry *)leArenaAlloc(&lex->configArena, sizeof(leKeywordEntry));
    if (!e) return;
    e->word = wordCopy;
    e->len = len;
    e->type = type;
    e->next = lex->keywordTable[h];
    lex->keywordTable[h] = e;
}

LEDEF void leKeywords(leLexer *lex, ...) {
    va_list args;
    va_start(args, lex);
    for (;;) {
        const char *word = va_arg(args, const char *);
        if (!word) break;
        int type = va_arg(args, int);
        leAddKeyword(lex, word, type);
    }
    va_end(args);
}

LEDEF void leKeywordsArray(leLexer *lex, const char **words, const int *types, int count) {
    for (int i = 0; i < count; i++) {
        leAddKeyword(lex, words[i], types[i]);
    }
}

static leTrieNode *leTrieCreate(leArena *arena) {
    leTrieNode *node = (leTrieNode *)leArenaAlloc(arena, sizeof(leTrieNode));
    if (!node) return NULL;
    node->ch = 0;
    node->type = -1;
    node->child = NULL;
    node->sibling = NULL;
    return node;
}

static leTrieNode *leTrieFindChild(leTrieNode *node, unsigned char c) {
    leTrieNode *ch = node->child;
    while (ch) {
        if (ch->ch == c) return ch;
        ch = ch->sibling;
    }
    return NULL;
}

static void leTrieInsert(leArena *arena, leTrieNode **root, const char *op, int len, int type) {
    if (!*root) *root = leTrieCreate(arena);
    if (!*root) return;

    leTrieNode *node = *root;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)op[i];
        if (c >= 128) return;
        leTrieNode *found = leTrieFindChild(node, c);
        if (!found) {
            found = leTrieCreate(arena);
            if (!found) return;
            found->ch = c;
            found->sibling = node->child;
            node->child = found;
        }
        node = found;
    }
    node->type = type;
}

static void leAddOperator(leLexer *lex, const char *op, int type) {
    int len = (int)strlen(op);
    leTrieInsert(&lex->configArena, &lex->operatorTrie, op, len, type);
    if (len > lex->maxOperatorLen)
        lex->maxOperatorLen = len;
}

LEDEF void leOperators(leLexer *lex, ...) {
    va_list args;
    va_start(args, lex);
    for (;;) {
        const char *op = va_arg(args, const char *);
        if (!op) break;
        int type = va_arg(args, int);
        leAddOperator(lex, op, type);
    }
    va_end(args);
}

LEDEF void leOperatorsArray(leLexer *lex, const char **ops, const int *types, int count) {
    for (int i = 0; i < count; i++) {
        leAddOperator(lex, ops[i], types[i]);
    }
}

LEDEF void leLineComment(leLexer *lex, const char *prefix) {
    int len = (int)strlen(prefix);
    lex->lineComment = leArenaDupStr(&lex->configArena, prefix, len);
    lex->lineCommentLen = lex->lineComment ? len : 0;
}

LEDEF void leBlockComment(leLexer *lex, const char *start, const char *end) {
    int slen = (int)strlen(start);
    int elen = (int)strlen(end);
    lex->blockCommentStart = leArenaDupStr(&lex->configArena, start, slen);
    lex->blockCommentStartLen = lex->blockCommentStart ? slen : 0;
    lex->blockCommentEnd = leArenaDupStr(&lex->configArena, end, elen);
    lex->blockCommentEndLen = lex->blockCommentEnd ? elen : 0;
}

LEDEF void leStringDelim(leLexer *lex, char delim, char escape) {
    lex->stringDelim = delim;
    lex->escapeChar = escape;
}

LEDEF void leCharDelim(leLexer *lex, char delim) {
    lex->charDelim = delim;
}

LEDEF void leMultiLineString(leLexer *lex, const char *delim) {
    int len = (int)strlen(delim);
    lex->multiLineDelim = leArenaDupStr(&lex->configArena, delim, len);
    lex->multiLineDelimLen = lex->multiLineDelim ? len : 0;
    lex->flags |= LE_MULTILINE_STRING;
}

LEDEF void leSetFlags(leLexer *lex, int flags) {
    lex->flags = flags;
}

LEDEF void leAddFlags(leLexer *lex, int flags) {
    lex->flags |= flags;
}

LEDEF void leRemoveFlags(leLexer *lex, int flags) {
    lex->flags &= ~flags;
}

LEDEF void leCustomRule(leLexer *lex, leCustomRuleFn fn) {
    leCustomRuleNode *node = (leCustomRuleNode *)leArenaAlloc(&lex->configArena, sizeof(leCustomRuleNode));
    if (!node) return;
    node->fn = fn;
    node->next = lex->customRules;
    lex->customRules = node;
}

LEDEF bool leAtEnd(leLexer *lex) {
    return lex->offset >= lex->sourceLen;
}

LEDEF char lePeek(leLexer *lex) {
    if (leAtEnd(lex)) return '\0';
    return *lex->current;
}

LEDEF char lePeekN(leLexer *lex, int n) {
    if (lex->offset + n >= lex->sourceLen) return '\0';
    return lex->current[n];
}

LEDEF char leAdvance(leLexer *lex) {
    if (leAtEnd(lex)) return '\0';
    char c = *lex->current++;
    lex->offset++;
    if (c == '\n') {
        lex->line++;
        lex->col = 1;
    } else {
        lex->col++;
    }
    return c;
}

LEDEF void leAdvanceN(leLexer *lex, int n) {
    for (int i = 0; i < n && !leAtEnd(lex); i++) {
        leAdvance(lex);
    }
}

LEDEF bool leMatch(leLexer *lex, char c) {
    if (leAtEnd(lex) || *lex->current != c) return false;
    leAdvance(lex);
    return true;
}

LEDEF bool leMatchStr(leLexer *lex, const char *str, int len) {
    if (lex->offset + len > lex->sourceLen) return false;
    return memcmp(lex->current, str, len) == 0;
}

LEDEF void leMarkStart(leLexer *lex) {
    lex->tokenStart = lex->current;
    lex->startLine = lex->line;
    lex->startCol = lex->col;
    lex->startOffset = lex->offset;
}

LEDEF leSourceLoc leGetLoc(leLexer *lex) {
    leSourceLoc loc;
    loc.line = lex->line;
    loc.col = lex->col;
    loc.offset = lex->offset;
    return loc;
}

static void leErrorDispatch(leLexer *lex, int code, int severity, const char *msg, leSourceLoc loc, leSourceLoc endLoc) {
    leErrorNode tmpNode;
    tmpNode.message = msg;
    tmpNode.code = code;
    tmpNode.severity = severity;
    tmpNode.loc = loc;
    tmpNode.endLoc = endLoc;
    tmpNode.next = NULL;

    bool shouldCollect = true;
    if (lex->errorHandler) {
        shouldCollect = lex->errorHandler(lex, &tmpNode, lex->errorUserData);
    }

    if (shouldCollect && (lex->flags & LE_COLLECT_ERRORS)) {
        if (lex->maxErrors <= 0 || lex->errors.count < lex->maxErrors) {
            leErrorListPush(&lex->arena, &lex->errors, code, severity, msg, loc, endLoc);
        }
    }
}

LEDEF void leAddError(leLexer *lex, const char *msg, leSourceLoc loc) {
    leErrorDispatch(lex, LE_ERR_NONE, LE_SEVERITY_ERROR, msg, loc, loc);
}

LEDEF void leAddErrorFull(leLexer *lex, int code, int severity, const char *msg, leSourceLoc loc, leSourceLoc endLoc) {
    leErrorDispatch(lex, code, severity, msg, loc, endLoc);
}

LEDEF void leSetErrorHandler(leLexer *lex, leErrorHandlerFn fn, void *userData) {
    lex->errorHandler = fn;
    lex->errorUserData = userData;
}

LEDEF void leSetMaxErrors(leLexer *lex, int max) {
    lex->maxErrors = max;
}

LEDEF bool leHasErrors(leLexer *lex) {
    return lex->errors.count > 0;
}

LEDEF int leErrorCount(leLexer *lex) {
    return lex->errors.count;
}

LEDEF int leErrorCountBySeverity(leLexer *lex, int severity) {
    int count = 0;
    leErrorNode *node = lex->errors.head;
    while (node) {
        if (node->severity == severity) count++;
        node = node->next;
    }
    return count;
}

LEDEF leErrorNode *leGetError(leLexer *lex, int index) {
    if (index < 0 || index >= lex->errors.count) return NULL;
    leErrorNode *node = lex->errors.head;
    for (int i = 0; i < index && node; i++) {
        node = node->next;
    }
    return node;
}

LEDEF leErrorNode *leFirstError(leLexer *lex) {
    return lex->errors.head;
}

LEDEF leErrorNode *leNextErrorNode(leErrorNode *node) {
    return node ? node->next : NULL;
}

LEDEF void leClearErrors(leLexer *lex) {
    leErrorListClear(&lex->errors);
}

LEDEF const char *leSeverityName(int severity) {
    switch (severity) {
        case LE_SEVERITY_NOTE:    return "note";
        case LE_SEVERITY_WARNING: return "warning";
        case LE_SEVERITY_ERROR:   return "error";
        default:                  return "unknown";
    }
}

LEDEF const char *leErrorCodeName(int code) {
    switch (code) {
        case LE_ERR_NONE:                        return "none";
        case LE_ERR_UNTERMINATED_STRING:          return "unterminated_string";
        case LE_ERR_UNTERMINATED_CHAR:            return "unterminated_char";
        case LE_ERR_UNTERMINATED_ESCAPE:          return "unterminated_escape";
        case LE_ERR_UNTERMINATED_BLOCK_COMMENT:   return "unterminated_block_comment";
        case LE_ERR_UNTERMINATED_MULTILINE_STRING:return "unterminated_multiline_string";
        case LE_ERR_INVALID_HEX_LITERAL:          return "invalid_hex_literal";
        case LE_ERR_INVALID_BIN_LITERAL:          return "invalid_bin_literal";
        case LE_ERR_INVALID_FLOAT_EXPONENT:       return "invalid_float_exponent";
        case LE_ERR_INVALID_ESCAPE:               return "invalid_escape";
        case LE_ERR_INVALID_UNICODE:              return "invalid_unicode";
        default:                                  return "user";
    }
}

LEDEF const char *leFormatError(leLexer *lex, const leErrorNode *err) {
    const char *sev = leSeverityName(err->severity);
    const char *fname = lex->fileName ? lex->fileName : "<input>";
    int needed = (int)strlen(fname) + (int)strlen(sev) + (int)strlen(err->message) + 64;
    char *buf = (char *)leArenaAlloc(&lex->arena, needed);
    if (!buf) return err->message;
    snprintf(buf, needed, "%s:%d:%d: %s: %s", fname, err->loc.line, err->loc.col, sev, err->message);
    return buf;
}

LEDEF const char *leTokenTypeName(int type) {
    switch (type) {
        case leEOF:        return "EOF";
        case leError:      return "Error";
        case leInteger:    return "Integer";
        case leFloat:      return "Float";
        case leString:     return "String";
        case leChar:       return "Char";
        case leIdent:      return "Ident";
        case leSingleChar: return "SingleChar";
        default:           return "User";
    }
}

LEDEF char *leTokenString(leLexer *lex, const leToken *tok) {
    return leArenaDupStr(&lex->arena, tok->start, tok->len);
}

LEDEF leToken leMakeToken(leLexer *lex, int type) {
    leToken tok;
    tok.type = type;
    tok.start = lex->tokenStart;
    tok.len = (int)(lex->current - lex->tokenStart);
    tok.loc.line = lex->startLine;
    tok.loc.col = lex->startCol;
    tok.loc.offset = lex->startOffset;
    tok.endLoc.line = lex->line;
    tok.endLoc.col = lex->col;
    tok.endLoc.offset = lex->offset;
    tok.intVal = 0;
    tok.decoded = NULL;
    tok.decodedLen = 0;
    return tok;
}

LEDEF leToken leMakeErrorCode(leLexer *lex, int code, const char *msg) {
    leToken tok;
    tok.type = leError;
    tok.start = lex->tokenStart;
    tok.len = (int)(lex->current - lex->tokenStart);
    tok.loc.line = lex->startLine;
    tok.loc.col = lex->startCol;
    tok.loc.offset = lex->startOffset;
    tok.endLoc.line = lex->line;
    tok.endLoc.col = lex->col;
    tok.endLoc.offset = lex->offset;
    tok.intVal = 0;
    
    int msgLen = (int)strlen(msg);
    tok.decoded = leArenaDupStr(&lex->arena, msg, msgLen);
    tok.decodedLen = tok.decoded ? msgLen : 0;

    leErrorDispatch(lex, code, LE_SEVERITY_ERROR, msg, tok.loc, tok.endLoc);
    return tok;
}

LEDEF leToken leMakeError(leLexer *lex, const char *msg) {
    return leMakeErrorCode(lex, LE_ERR_NONE, msg);
}

LEDEF leToken leMakeErrorf(leLexer *lex, int code, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list args2;
    va_copy(args2, args);
    int needed = vsnprintf(NULL, 0, fmt, args) + 1;
    va_end(args);
    char *buf = (char *)leArenaAlloc(&lex->arena, needed);
    if (buf) {
        vsnprintf(buf, needed, fmt, args2);
    }
    va_end(args2);
    return leMakeErrorCode(lex, code, buf ? buf : fmt);
}

static void leSkipWhitespace(leLexer *lex) {
    for (;;) {
        if (leAtEnd(lex)) return;
        char c = lePeek(lex);

        if (leIsSpace(c)) {
            leAdvance(lex);
            continue;
        }

        if ((lex->flags & LE_SKIP_COMMENTS) && lex->lineCommentLen > 0 &&
            leMatchStr(lex, lex->lineComment, lex->lineCommentLen)) {
            while (!leAtEnd(lex) && lePeek(lex) != '\n') leAdvance(lex);
            continue;
        }

        if ((lex->flags & LE_SKIP_COMMENTS) && lex->blockCommentStartLen > 0 &&
            leMatchStr(lex, lex->blockCommentStart, lex->blockCommentStartLen)) {
            leSourceLoc commentLoc = leGetLoc(lex);
            leAdvanceN(lex, lex->blockCommentStartLen);
            while (!leAtEnd(lex) && !leMatchStr(lex, lex->blockCommentEnd, lex->blockCommentEndLen)) {
                leAdvance(lex);
            }
            if (leAtEnd(lex)) {
                leSourceLoc endLoc = leGetLoc(lex);
                leErrorDispatch(lex, LE_ERR_UNTERMINATED_BLOCK_COMMENT, LE_SEVERITY_ERROR,
                                "unterminated block comment", commentLoc, endLoc);
            } else {
                leAdvanceN(lex, lex->blockCommentEndLen);
            }
            continue;
        }

        break;
    }
}

static int leMatchKeyword(leLexer *lex, const char *start, int len) {
    unsigned int h;
    if (lex->flags & LE_CASE_KEYWORDS)
        h = leHashStrNoCase(start, len) % LE_HASH_SIZE;
    else
        h = leHashStr(start, len) % LE_HASH_SIZE;

    leKeywordEntry *e = lex->keywordTable[h];
    while (e) {
        if (e->len == len) {
            if (lex->flags & LE_CASE_KEYWORDS) {
                bool match = true;
                for (int i = 0; i < len && match; i++) {
                    char a = start[i], b = e->word[i];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) match = false;
                }
                if (match) return e->type;
            } else {
                if (memcmp(start, e->word, len) == 0)
                    return e->type;
            }
        }
        e = e->next;
    }
    return -1;
}

static int leDecodeEscapeChar(char c) {
    switch (c) {
        case 'n':  return '\n';
        case 't':  return '\t';
        case 'r':  return '\r';
        case '0':  return '\0';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        case 'a':  return '\a';
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'v':  return '\v';
        default:   return -1;
    }
}

static int leDecodeHexEscape(const char *s, int maxLen, int *value) {
    *value = 0;
    int i;
    for (i = 0; i < maxLen && leIsHexDigit(s[i]); i++) {
        char c = s[i];
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else digit = c - 'A' + 10;
        *value = (*value << 4) | digit;
    }
    return i;
}

static char *leDecodeString(leLexer *lex, const char *start, int len, int *outLen) {
    char *buf = (char *)leArenaAlloc(&lex->arena, len * 4 + 1);
    if (!buf) { *outLen = 0; return NULL; }

    int j = 0;
    for (int i = 0; i < len; i++) {
        if (start[i] == lex->escapeChar && i + 1 < len) {
            char next = start[i + 1];
            int decoded = leDecodeEscapeChar(next);
            if (decoded >= 0) {
                buf[j++] = (char)decoded;
                i++;
            } else if (next == 'x' && i + 2 < len) {
                int value;
                int consumed = leDecodeHexEscape(&start[i + 2], 2, &value);
                if (consumed > 0) {
                    buf[j++] = (char)value;
                    i += 1 + consumed;
                } else {
                    buf[j++] = start[i];
                }
            } else if (next == 'u' && i + 2 < len) {
                int value;
                int consumed = leDecodeHexEscape(&start[i + 2], 4, &value);
                if (consumed > 0 && !(value >= 0xD800 && value <= 0xDFFF) && value <= 0x10FFFF) {
                    j += leUtf8Encode(value, &buf[j]);
                    i += 1 + consumed;
                } else {
                    buf[j++] = start[i];
                }
            } else if (next == 'U' && i + 2 < len) {
                int value;
                int consumed = leDecodeHexEscape(&start[i + 2], 8, &value);
                if (consumed > 0 && !(value >= 0xD800 && value <= 0xDFFF) && value <= 0x10FFFF) {
                    j += leUtf8Encode(value, &buf[j]);
                    i += 1 + consumed;
                } else {
                    buf[j++] = start[i];
                }
            } else {
                buf[j++] = next;
                i++;
            }
        } else {
            buf[j++] = start[i];
        }
    }
    buf[j] = '\0';
    *outLen = j;
    return buf;
}

static leToken leLexNumber(leLexer *lex) {
    bool isFloat = false;
    bool isHex = false;
    bool isBin = false;
    bool allowUnderscore = (lex->flags & LE_NUM_UNDERSCORE) != 0;

    if (lePeek(lex) == '0') {
        char n = lePeekN(lex, 1);
        if (n == 'x' || n == 'X') {
            leAdvance(lex);
            leAdvance(lex);
            isHex = true;
            if (!leIsHexDigit(lePeek(lex)))
                return leMakeErrorCode(lex, LE_ERR_INVALID_HEX_LITERAL, "invalid hex literal");
            while (!leAtEnd(lex)) {
                char c = lePeek(lex);
                if (leIsHexDigit(c)) {
                    leAdvance(lex);
                } else if (allowUnderscore && c == '_' && leIsHexDigit(lePeekN(lex, 1))) {
                    leAdvance(lex);
                } else {
                    break;
                }
            }
        } else if (n == 'b' || n == 'B') {
            leAdvance(lex);
            leAdvance(lex);
            isBin = true;
            if (lePeek(lex) != '0' && lePeek(lex) != '1')
                return leMakeErrorCode(lex, LE_ERR_INVALID_BIN_LITERAL, "invalid binary literal");
            while (!leAtEnd(lex)) {
                char c = lePeek(lex);
                if (c == '0' || c == '1') {
                    leAdvance(lex);
                } else if (allowUnderscore && c == '_' && (lePeekN(lex, 1) == '0' || lePeekN(lex, 1) == '1')) {
                    leAdvance(lex);
                } else {
                    break;
                }
            }
        }
    }

    if (!isHex && !isBin) {
        while (!leAtEnd(lex)) {
            char c = lePeek(lex);
            if (leIsDigit(c)) {
                leAdvance(lex);
            } else if (allowUnderscore && c == '_' && leIsDigit(lePeekN(lex, 1))) {
                leAdvance(lex);
            } else {
                break;
            }
        }
        if (!leAtEnd(lex) && lePeek(lex) == '.' && leIsDigit(lePeekN(lex, 1))) {
            isFloat = true;
            leAdvance(lex);
            while (!leAtEnd(lex)) {
                char c = lePeek(lex);
                if (leIsDigit(c)) {
                    leAdvance(lex);
                } else if (allowUnderscore && c == '_' && leIsDigit(lePeekN(lex, 1))) {
                    leAdvance(lex);
                } else {
                    break;
                }
            }
        }
        if (!leAtEnd(lex) && (lePeek(lex) == 'e' || lePeek(lex) == 'E')) {
            isFloat = true;
            leAdvance(lex);
            if (!leAtEnd(lex) && (lePeek(lex) == '+' || lePeek(lex) == '-')) leAdvance(lex);
            if (!leIsDigit(lePeek(lex)))
                return leMakeErrorCode(lex, LE_ERR_INVALID_FLOAT_EXPONENT, "invalid float exponent");
            while (!leAtEnd(lex)) {
                char c = lePeek(lex);
                if (leIsDigit(c)) {
                    leAdvance(lex);
                } else if (allowUnderscore && c == '_' && leIsDigit(lePeekN(lex, 1))) {
                    leAdvance(lex);
                } else {
                    break;
                }
            }
        }
    }

    leToken tok = leMakeToken(lex, isFloat ? leFloat : leInteger);
    if (lex->flags & LE_PARSE_NUMBERS) {
        char *parseStr = leArenaDupStr(&lex->arena, tok.start, tok.len);
        if (parseStr) {
            if (allowUnderscore) {
                int j = 0;
                for (int i = 0; parseStr[i]; i++) {
                    if (parseStr[i] != '_') parseStr[j++] = parseStr[i];
                }
                parseStr[j] = '\0';
            }
            if (isFloat) {
                tok.floatVal = strtod(parseStr, NULL);
            } else if (isHex) {
                tok.intVal = strtoll(parseStr, NULL, 16);
            } else if (isBin) {
                tok.intVal = strtoll(parseStr + 2, NULL, 2);
            } else {
                tok.intVal = strtoll(parseStr, NULL, 10);
            }
        }
    }
    return tok;
}

static leToken leLexMultiLineString(leLexer *lex) {
    leAdvanceN(lex, lex->multiLineDelimLen);
    const char *contentStart = lex->current;
    while (!leAtEnd(lex) && !leMatchStr(lex, lex->multiLineDelim, lex->multiLineDelimLen)) {
        leAdvance(lex);
    }
    const char *contentEnd = lex->current;
    if (leAtEnd(lex)) {
        return leMakeErrorCode(lex, LE_ERR_UNTERMINATED_MULTILINE_STRING, "unterminated multi-line string");
    }
    leAdvanceN(lex, lex->multiLineDelimLen);

    leToken tok = leMakeToken(lex, leString);
    tok.start = contentStart;
    tok.len = (int)(contentEnd - contentStart);

    if (lex->flags & LE_DECODE_ESCAPES) {
        tok.decoded = leDecodeString(lex, tok.start, tok.len, &tok.decodedLen);
    }
    return tok;
}

static leToken leLexString(leLexer *lex, char delim, int tokenType) {
    leAdvance(lex);  // consume opening delimiter
    const char *contentStart = lex->current;
    while (!leAtEnd(lex) && lePeek(lex) != delim) {
        if (lePeek(lex) == lex->escapeChar) {
            leAdvance(lex);
            if (leAtEnd(lex))
                return leMakeErrorCode(lex, LE_ERR_UNTERMINATED_ESCAPE, "unterminated escape sequence");
            leAdvance(lex);
            continue;
        }
        if (lePeek(lex) == '\n' && tokenType == leChar)
            return leMakeErrorCode(lex, LE_ERR_UNTERMINATED_CHAR, "unterminated character literal");
        if (lePeek(lex) == '\n' && tokenType == leString && !(lex->flags & LE_MULTILINE_STRING))
            return leMakeErrorCode(lex, LE_ERR_UNTERMINATED_STRING, "unterminated string");
        leAdvance(lex);
    }
    if (leAtEnd(lex)) {
        return leMakeErrorCode(lex, tokenType == leString ? LE_ERR_UNTERMINATED_STRING : LE_ERR_UNTERMINATED_CHAR,
                               tokenType == leString ? "unterminated string" : "unterminated character literal");
    }
    const char *contentEnd = lex->current;
    leAdvance(lex);

    leToken tok = leMakeToken(lex, tokenType);
    tok.start = contentStart;
    tok.len = (int)(contentEnd - contentStart);

    if (lex->flags & LE_DECODE_ESCAPES) {
        tok.decoded = leDecodeString(lex, tok.start, tok.len, &tok.decodedLen);
    }
    return tok;
}

static leToken leLexIdent(leLexer *lex) {
    while (!leAtEnd(lex)) {
        char c = lePeek(lex);
        if (leIsAlphaNum(c)) {
            leAdvance(lex);
        } else if (leIsUtf8Start((unsigned char)c)) {
            int remaining = lex->sourceLen - lex->offset;
            int cp;
            int cpLen = leUtf8Decode(lex->current, remaining, &cp);
            if (cpLen > 0 && leIsUnicodeIdCont(cp)) {
                leAdvanceN(lex, cpLen);
            } else {
                break;
            }
        } else {
            break;
        }
    }
    int len = (int)(lex->current - lex->tokenStart);
    int kwType = leMatchKeyword(lex, lex->tokenStart, len);
    if (kwType >= 0) return leMakeToken(lex, kwType);
    return leMakeToken(lex, leIdent);
}

static bool leTryOperator(leLexer *lex, leToken *tok) {
    if (!lex->operatorTrie) return false;

    leTrieNode *node = lex->operatorTrie;
    leTrieNode *lastMatch = NULL;
    int lastMatchLen = 0;
    int i = 0;
    int remaining = lex->sourceLen - lex->offset;

    while (i < remaining && i < lex->maxOperatorLen) {
        unsigned char c = (unsigned char)lex->current[i];
        if (c >= 128) break;
        leTrieNode *found = leTrieFindChild(node, c);
        if (!found) break;
        node = found;
        i++;
        if (node->type >= 0) {
            lastMatch = node;
            lastMatchLen = i;
        }
    }

    if (lastMatch) {
        leMarkStart(lex);
        leAdvanceN(lex, lastMatchLen);
        *tok = leMakeToken(lex, lastMatch->type);
        return true;
    }
    return false;
}

static bool leNextTokenInternal(leLexer *lex, leToken *tok) {
    if (lex->flags & LE_SKIP_WHITESPACE)
        leSkipWhitespace(lex);

    if (leAtEnd(lex)) {
        leMarkStart(lex);
        *tok = leMakeToken(lex, leEOF);
        return false;
    }

    leCustomRuleNode *rule = lex->customRules;
    while (rule) {
        const char *savedCur = lex->current;
        int savedLine = lex->line;
        int savedCol = lex->col;
        int savedOffset = lex->offset;
        const char *savedTokenStart = lex->tokenStart;
        int savedStartLine = lex->startLine;
        int savedStartCol = lex->startCol;
        int savedStartOffset = lex->startOffset;
        leMarkStart(lex);
        if (rule->fn(lex, tok)) return true;
        lex->current = savedCur;
        lex->line = savedLine;
        lex->col = savedCol;
        lex->offset = savedOffset;
        lex->tokenStart = savedTokenStart;
        lex->startLine = savedStartLine;
        lex->startCol = savedStartCol;
        lex->startOffset = savedStartOffset;
        rule = rule->next;
    }

    if (leTryOperator(lex, tok)) return true;

    leMarkStart(lex);
    char c = lePeek(lex);

    if ((lex->flags & LE_MULTILINE_STRING) && lex->multiLineDelimLen > 0 &&
        leMatchStr(lex, lex->multiLineDelim, lex->multiLineDelimLen)) {
        *tok = leLexMultiLineString(lex);
        return tok->type != leEOF;
    }

    if (c == lex->stringDelim) {
        *tok = leLexString(lex, lex->stringDelim, leString);
        return tok->type != leEOF;
    }

    if (c == lex->charDelim && lex->charDelim != lex->stringDelim) {
        *tok = leLexString(lex, lex->charDelim, leChar);
        return tok->type != leEOF;
    }

    if (leIsDigit(c)) {
        *tok = leLexNumber(lex);
        return true;
    }

    if (leIsAlpha(c)) {
        *tok = leLexIdent(lex);
        return true;
    }

    if (leIsUtf8Start((unsigned char)c)) {
        int remaining = lex->sourceLen - lex->offset;
        int cp;
        int cpLen = leUtf8Decode(lex->current, remaining, &cp);
        if (cpLen > 0 && leIsUnicodeIdStart(cp)) {
            leAdvanceN(lex, cpLen);
            *tok = leLexIdent(lex);
            return true;
        }
    }

    leAdvance(lex);
    *tok = leMakeToken(lex, leSingleChar);
    return true;
}

LEDEF void leUngetToken(leLexer *lex, const leToken *tok) {
    leTokenBufferPushFront(&lex->tokenBuffer, tok);
}

LEDEF bool leNextToken(leLexer *lex, leToken *tok) {
    if (leTokenBufferPop(&lex->tokenBuffer, tok)) {
        return tok->type != leEOF;
    }
    return leNextTokenInternal(lex, tok);
}

LEDEF bool lePeekToken(leLexer *lex, leToken *tok) {
    leToken *buffered = leTokenBufferPeek(&lex->tokenBuffer, 0);
    if (buffered) {
        *tok = *buffered;
        return tok->type != leEOF;
    }
    bool result = leNextTokenInternal(lex, tok);
    leUngetToken(lex, tok);
    return result;
}

LEDEF bool lePeekTokenN(leLexer *lex, leToken *tok, int n) {
    if (n < 0) return false;

    leToken *buffered = leTokenBufferPeek(&lex->tokenBuffer, n);
    if (buffered) {
        *tok = *buffered;
        return tok->type != leEOF;
    }

    int needed = n - lex->tokenBuffer.count + 1;
    for (int i = 0; i < needed; i++) {
        leToken t;
        bool hasMore = leNextTokenInternal(lex, &t);
        leTokenBufferPushBack(&lex->tokenBuffer, &t);
        if (!hasMore) break;
    }

    buffered = leTokenBufferPeek(&lex->tokenBuffer, n);
    if (buffered) {
        *tok = *buffered;
        return tok->type != leEOF;
    }
    return false;
}

#endif
#endif