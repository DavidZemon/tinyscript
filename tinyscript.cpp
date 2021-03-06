/* Tinyscript interpreter
 *
 * Copyright 2016 Total Spectrum Software Inc.
 *
 * +--------------------------------------------------------------------
 * ¦  TERMS OF USE: MIT License
 * +--------------------------------------------------------------------
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * +--------------------------------------------------------------------
 */

//
// a very tiny scripting language
//
//#define DEBUG
#ifdef DEBUG
#include <PropWare/hmi/output/printer.h>
#endif

#define MAX_EXPR_LEVEL 5

#include <string.h>
#include <stdlib.h>
#include "tinyscript.h"

#define SYMSTACK_SIZE (2048/sizeof(Sym))
#define VALSTACK_SIZE (1024/sizeof(Val))

static int  arena_size;
static Byte *arena;

static Sym    *symptr;
static Val    *valptr;
static String pc;  // instruction pointer

// fetch the next token
static int             curToken;
static String          token;
static Val             tokenVal;


Val stringeq (String ai, String bi) {
    const Byte *a, *b;
    int        i, len;

    if (ai.len != bi.len) {
        return 0;
    }
    a      = ai.ptr;
    b      = bi.ptr;
    len    = ai.len;
    for (i = 0; i < len; i++) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return 1;
}

//
// Utility functions
//
static void
PrintString (String s) {
    int        len  = s.len;
    const Byte *ptr = s.ptr;
    while (len > 0) {
        outchar(*ptr);
        ptr++;
        --len;
    }
}

static void
Newline (void) {
    outchar('\n');
}

static void
PrintNumber (Val v) {
    unsigned long x;
    unsigned      base   = 10;
    int           prec   = 1;
    int           digits = 0;
    int           c;
    char          buf[32];

    if (v < 0) {
        outchar('-');
        x = -v;
    } else {
        x = v;
    }
    while (x > 0 || digits < prec) {
        c = x % base;
        x = x / base;
        if (c < 10) c += '0';
        else c = (c - 10) + 'a';
        buf[digits++] = c;
    }
    // now output
    while (digits > 0) {
        --digits;
        outchar(buf[digits]);
    }
}

// look up a symbol by name
static Sym *
LookupSym (String name) {
    Sym *s;

    s = symptr;
    while ((intptr_t) s > (intptr_t) arena) {
        --s;
        if (stringeq(s->name, name)) {
            return s;
        }
    }
    return NULL;
}

//
// parse an expression or statement
//

#define TOK_SYMBOL 'A'
#define TOK_NUMBER 'N'
#define TOK_STRING 'S'
#define TOK_IF     'i'
#define TOK_ELSE   'e'
#define TOK_WHILE  'w'
#define TOK_PRINT  'p'
#define TOK_VAR    'v'
#define TOK_VARDEF 'V'
#define TOK_PROC   'f'
#define TOK_BUILTIN 'B'
#define TOK_BINOP  'o'
#define TOK_PROCDEF 'F'
#define TOK_SYNTAX_ERR 'Z'

static void ResetToken () {
    token.len = 0;
    token.ptr = pc.ptr;
}

//
// get next character from the program counter
// returns -1 on end of file
static int GetChar () {
    int c;
    if (pc.len == 0)
        return -1;
    c = *pc.ptr;
    pc.ptr++;
    pc.len--;
    token.len++;
    return c;
}

static void IgnoreLastChar () {
    token.len--;
}

//
// undo last getchar
// (except at end of input)
//
static void UngetChar () {
    pc.len++;
    pc.ptr--;
    token.len--;
}

static int charin (int c, const char *str) {
    while (*str) {
        if (c == *str++) return 1;
    }
    return 0;
}

static int isidpunct (int c) {
    return charin(c, ".:_");
}

static int isidentifier (int c) {
    return isalpha(c) || isidpunct(c);
}

static int notquote (int c) {
    return (c >= 0) && !charin(c, "\"\n");
}

static void
GetSpan (int (*testfn) (int)) {
    int c;
    do {
        c = GetChar();
    } while (testfn(c));
    if (c != -1) {
        UngetChar();
    }
}

static int
isoperator (int c) {
    return charin(c, "+-/*=<>&|^");
}

static int
doNextToken (int israw) {
    int c;
    int r = -1;

    ResetToken();
    for (; ;) {
        c = GetChar();
        if (isspace(c)) {
            ResetToken();
        } else {
            break;
        }
    }

    if (c == '#') {
        // comment
        do {
            c = GetChar();
        } while (c >= 0 && c != '\n');
        r = c;
    } else if (isdigit(c)) {
        GetSpan(isdigit);
        r = TOK_NUMBER;
    } else if (isalpha(c)) {
        Sym *sym;
        GetSpan(isidentifier);
        r = TOK_SYMBOL;
        // check for special tokens
        if (!israw) {
            sym = LookupSym(token);
            if (sym) {
                if (sym->type == TOKEN) {
                    r = sym->value;
                } else if (sym->type == PROC) {
                    r = TOK_PROC;
                } else if (sym->type == BUILTIN) {
                    r = TOK_BUILTIN;
                } else {
                    r = TOK_VAR;
                }
                tokenVal = sym->value;
            }
        }
    } else if (isoperator(c)) {
        Sym *sym;
        GetSpan(isoperator);
        sym = LookupSym(token);
        if (sym && (sym->type & 0xff) == OPERATOR) {
            r        = TOK_BINOP | (sym->type & 0xff00);
            tokenVal = sym->value;
        } else {
            r = TOK_SYNTAX_ERR;
        }
    } else if (c == '{') {
        int bracket = 1;
        ResetToken();
        while (bracket > 0) {
            c = GetChar();
            if (c < 0) {
                return TOK_SYNTAX_ERR;
            }
            if (c == '}') {
                --bracket;
            } else if (c == '{') {
                ++bracket;
            }
        }
        IgnoreLastChar();
        r = TOK_STRING;
    } else if (c == '"') {
        ResetToken();
        GetSpan(notquote);
        c = GetChar();
        if (c < 0) return TOK_SYNTAX_ERR;
        IgnoreLastChar();
        r = TOK_STRING;
    } else {
        r = c;
    }
#ifdef DEBUG
    pwOut << "Token[" << (char) r << " / 0x";
    pwOut.put_uint((unsigned int) r, 16, 2);
    pwOut << "] = ";
    PrintString(token);
    pwOut << '\n';
#endif
    curToken = r;
    return r;
}

static int NextToken () {
    return doNextToken(0);
}

static int NextRawToken () {
    return doNextToken(1);
}

// push a number on the result stack
// this stack grows down from the top of the arena

void
Push (Val x) {
    --valptr;
    if ((intptr_t) valptr < (intptr_t) symptr) {
        // out of memory!
        abort();
    }
    *valptr = x;
}

// pop a number off the result stack
Val
Pop () {
    Val r = 0;
    if ((intptr_t) valptr < (intptr_t) (arena + arena_size)) {
        r = *valptr++;
    }
    return r;
}

// convert a string to a number
Val
StringToNum (String s) {
    Val        r    = 0;
    int        c;
    const Byte *ptr = s.ptr;
    int        len  = s.len;
    while (len-- > 0) {
        c = *ptr++;
        if (!isdigit(c)) break;
        r = 10 * r + (c - '0');
    }
    return r;
}

// define a symbol
Sym *
DefineSym (String name, int typ, Val value) {
    Sym *s = symptr;

    if (name.ptr == NULL) {
        return NULL;
    }
    symptr++;
    if ((intptr_t) symptr >= (intptr_t) valptr) {
        //out of memory
        return NULL;
    }
    s->name  = name;
    s->value = value;
    s->type  = typ;
    return s;
}

static Sym *
DefineVar (String name) {
    return DefineSym(name, INT, 0);
}

String
Cstring (const char *str) {
    String x;

    x.len = strlen(str);
    x.ptr = str;
    return x;
}

int
TinyScript_Define (const char *name, int typ, Val val) {
    Sym *s;
    s = DefineSym(Cstring(name), typ, val);
    if (!s) return TS_ERR_NOMEM;
    return TS_ERR_OK;
}

extern int ParseExpr ();

static int
ParseExprList (int *countptr) {
    int err;
    int count = 0;
    int c;

    do {
        err = ParseExpr();
        if (err != TS_ERR_OK) {
            return err;
        }
        count++;
        c = curToken;
        if (c == ',') {
            NextToken();
        }
    } while (c == ',');
    *countptr = count;
    return TS_ERR_OK;
}

// parse a value; for now, just a number
// returns 0 if valid, non-zero if syntax error

static int
ParseExpr0 () {
    int c;
    int err;
    c = curToken;
    if (c == '(') {
        NextToken();
        err = ParseExpr();
        if (err == TS_ERR_OK) {
            c = curToken;
            if (c == ')') {
                NextToken();
                return TS_ERR_OK;
            }
        }
        return err;
    } else if (c == TOK_NUMBER) {
        Push(StringToNum(token));
        NextToken();
        return TS_ERR_OK;
    } else if (c == TOK_VAR) {
        Push(tokenVal);
        NextToken();
        return TS_ERR_OK;
    } else if (c == TOK_BUILTIN) {
        Cfunc op         = (Cfunc) tokenVal;
        int   paramCount = 0;
        Val   arg[MAX_BUILTIN_PARAMS];
        c = NextToken();
        if (c != '(') return TS_ERR_SYNTAX;
        c = NextToken();
        if (c != ')') {
            err = ParseExprList(&paramCount);
            c   = curToken;
        }
        if (c != ')') {
            return TS_ERR_SYNTAX;
        }
        NextToken();
        // make sure at least 4 params on on the stack
        while (paramCount < MAX_BUILTIN_PARAMS) {
            Push(0);
            paramCount++;
        }
        // we now have "paramCount" items pushed on to the stack
        // pop em off
        // we silently ignore parameters past the max
        while (paramCount > MAX_BUILTIN_PARAMS) {
            Pop();
            --paramCount;
        }
        while (paramCount > 0) {
            --paramCount;
            arg[paramCount] = Pop();
        }
        Push(op(arg[0], arg[1], arg[2], arg[3]));
        return TS_ERR_OK;
    } else if ((c & 0xff) == TOK_BINOP) {
        // binary operator
        Opfunc op = (Opfunc) tokenVal;
        NextToken();
        err = ParseExpr();
        if (err == TS_ERR_OK) {
            Val v = Pop();
            Push(op(0, v));
        }
        return err;
    } else {
        return TS_ERR_SYNTAX;
    }
}

// parse a level n expression
// level 0 is the lowest level
static int
ParseExprLevel (int n) {
    int err = TS_ERR_OK;
    int c;

    if (n == 0) {
        return ParseExpr0();
    }
    err = ParseExprLevel(n - 1);
    if (err != TS_ERR_OK) return err;
    c = curToken;
    while ((c & 0xff) == TOK_BINOP) {
        int    level = (c >> 8) & 0xff;
        Opfunc op    = (Opfunc) tokenVal;
        if (level == n) {
            Val a, b;
            NextToken();
            err = ParseExprLevel(n - 1);
            if (err != TS_ERR_OK) {
                Pop();
                return err;
            }
            b = Pop();
            a = Pop();
            Push(op(a, b));
            c = curToken;
        } else {
            break;
        }
    }
    return TS_ERR_OK;
}

int
ParseExpr () {
    return ParseExprLevel(MAX_EXPR_LEVEL);
}

static char *
stack_alloc (int len) {
    int      mask = sizeof(Val) - 1;
    intptr_t base;

    len  = (len + mask) & ~mask;
    base = ((intptr_t) valptr) - len;
    if (base < (intptr_t) symptr) {
        return NULL;
    }
    valptr = (Val *) base;
    return (char *) base;
}

static String
DupString (String orig) {
    String x;
    char   *ptr;
    x.len = orig.len;
    ptr = stack_alloc(x.len);
    if (ptr) {
        memcpy(ptr, orig.ptr, x.len);
    }
    x.ptr = ptr;
    return x;
}

static int ParseString (String str, int saveStrings, int topLevel);

//
// this is slightly different in that it may return the non-erro TS_ERR_ELSE
// to signify that the condition was false
//
static int ParseIf () {
    String ifpart, elsepart;
    int    haveelse = 0;
    Val    cond;
    int    c;
    int    err;

    c   = NextToken();
    err = ParseExpr();
    if (err != TS_ERR_OK) {
        return err;
    }
    cond = Pop();
    c    = curToken;
    if (c != TOK_STRING) {
        return TS_ERR_SYNTAX;
    }
    ifpart = token;
    c      = NextToken();
    if (c == TOK_ELSE) {
        c = NextToken();
        if (c != TOK_STRING) {
            return TS_ERR_SYNTAX;
        }
        elsepart = token;
        haveelse = 1;
        NextToken();
    }
    if (cond) {
        err = ParseString(ifpart, 0, 0);
    } else if (haveelse) {
        err = ParseString(elsepart, 0, 0);
    }
    if (err == TS_ERR_OK && !cond) err = TS_ERR_OK_ELSE;
    return err;
}

//
// parse one statement
// 1 is true if we need to save strings we encounter (we've been passed
// a temporary string)
//
static int
ParseStmt (int saveStrings) {
    int    c;
    String name;
    Val    val;
    int    err;

    c = curToken;

    if (c == TOK_VARDEF) {
        // a definition var a=x
        c = NextRawToken(); // we want to get VAR_SYMBOL directly
        if (c != TOK_SYMBOL) return TS_ERR_SYNTAX;
        if (saveStrings) {
            name = DupString(token);
        } else {
            name = token;
        }
        if (!DefineVar(name)) {
            return TS_ERR_NOMEM;
        }
        c = TOK_VAR;
    }
    if (c == TOK_VAR) {
        // is this a=expr?
        Sym *s;
        name = token;
        c    = NextToken();
        // we expect the "=" operator
        // verify that it is "="
        if (token.ptr[0] != '=' || token.len != 1) {
            return TS_ERR_SYNTAX;
        }
        NextToken();
        err = ParseExpr();
        if (err != TS_ERR_OK) {
            return err;
        }
        val = Pop();
        s   = LookupSym(name);
        if (!s) {
            return TS_ERR_UNKNOWN_SYM; // unknown symbol
        }
        s->value = val;
    } else if (c == TOK_PRINT) {
        print_more:
        c = NextToken();
        if (c == TOK_STRING) {
            PrintString(token);
            NextToken();
        } else {
            err = ParseExpr();
            if (err != TS_ERR_OK) {
                return err;
            }
            val = Pop();
            PrintNumber(val);
        }
        if (curToken == ',')
            goto print_more;
        Newline();
    } else if (c == TOK_IF) {
        err = ParseIf();
        if (err == TS_ERR_OK_ELSE) {
            err = TS_ERR_OK;
        }
    } else if (c == TOK_WHILE) {
        String savepc;
        savepc = pc;
        again:
        err    = ParseIf();
        if (err == TS_ERR_OK_ELSE) {
            return TS_ERR_OK;
        } else if (err == TS_ERR_OK) {
            pc = savepc;
            goto again;
        }
        return err;
    } else if (c == TOK_BUILTIN) {
        err = ParseExpr0();
        if (err == TS_ERR_OK) {
            (void) Pop(); // fix up the stack
        }
        return err;
    } else if (c == TOK_PROC) {
        Sym    *sym = LookupSym(token);
        String body;
        if (!sym) return TS_ERR_SYNTAX;
        body.ptr = (const char *) sym->value;
        body.len = sym->aux;
        return ParseString(body, 0, 0);
    } else if (c == TOK_PROCDEF) {
        Sym    *sym;
        String name;
        String body;
        c = NextRawToken(); // do not interpret the symbol
        if (c != TOK_SYMBOL) return TS_ERR_SYNTAX;
        name = token;
        c    = NextToken();
        if (c != TOK_STRING) return TS_ERR_SYNTAX;
        body = token;
        // FIXME here is where things get tricky: we have to
        // save a pointer to the string into the Sym
        // on small machines pack them together
        // on large ones, use an "aux" field
        if (saveStrings) {
            name = DupString(name);
            body = DupString(body);
        }
        sym  = DefineSym(name, PROC, (intptr_t) body.ptr);
        if (!sym) return TS_ERR_NOMEM;
        sym->aux = body.len;
        NextToken();
    } else {
        return TS_ERR_SYNTAX;
    }
    return TS_ERR_OK;
}

static int
ParseString (String str, int saveStrings, int topLevel) {
    String savepc = pc;
    Sym *savesymptr = symptr;
    int c;
    int r;

    pc = str;
    for (; ;) {
        c = NextToken();
        while (c == '\n' || c == ';') {
            c = NextToken();
        }
        if (c < 0) break;
        r = ParseStmt(saveStrings);
        if (r != TS_ERR_OK) return r;
        c = curToken;
        if (c == '\n' || c == ';' || c < 0) {
            /* ok */
        } else {
            return TS_ERR_SYNTAX;
        }
    }
    pc = savepc;
    if (!topLevel) {
        // restore variable context
        symptr = savesymptr;
    }
    return TS_ERR_OK;
}

//
// builtin functions
//
static Val prod (Val x, Val y) {
    return x * y;
}

static Val quot (Val x, Val y) {
    return x / y;
}

static Val sum (Val x, Val y) {
    return x + y;
}

static Val diff (Val x, Val y) {
    return x - y;
}

static Val bit_and(Val x, Val y) {
return x &y;
}

static Val bit_or(
Val x, Val
y) {
return x|
  y;
}

static Val bitxor (Val x, Val y) {
    return x ^ y;
}

static Val shl (Val x, Val y) {
    return x << y;
}

static Val shr (Val x, Val y) {
    return x >> y;
}

static Val equals (Val x, Val y) {
    return x == y;
}

static Val ne (Val x, Val y) {
    return x != y;
}

static Val lt (Val x, Val y) {
    return x < y;
}

static Val le (Val x, Val y) {
    return x <= y;
}

static Val gt (Val x, Val y) {
    return x > y;
}

static Val ge (Val x, Val y) {
    return x >= y;
}

static struct def {
    const char *name;
    int        toktype;
    intptr_t   val;
} defs[] = {
        // keywords
        {"if",    TOKEN, TOK_IF},
        {"else",  TOKEN, TOK_ELSE},
        {"while", TOKEN, TOK_WHILE},
        {"print", TOKEN, TOK_PRINT},
        {"var",   TOKEN, TOK_VARDEF},
        {"proc",  TOKEN, TOK_PROCDEF},
        // operators
        {"*",     BINOP(1), (intptr_t) prod},
        {"/",     BINOP(1), (intptr_t) quot},
        {"+",     BINOP(2), (intptr_t) sum},
        {"-",     BINOP(2), (intptr_t) diff},
        {"&",     BINOP(3), (intptr_t) bit_and},
        {"|",     BINOP(3), (intptr_t) bit_or },
        {"^",     BINOP(3), (intptr_t) bitxor},
        {">>",    BINOP(3), (intptr_t) shr},
        {"<<",    BINOP(3), (intptr_t) shl},
        {"=",     BINOP(4), (intptr_t) equals},
        {"<>",    BINOP(4), (intptr_t) ne},
        {"<",     BINOP(4), (intptr_t) lt},
        {"<=",    BINOP(4), (intptr_t) le},
        {">",     BINOP(4), (intptr_t) gt},
        {">=",    BINOP(4), (intptr_t) ge},

        {NULL, 0,           0}
};

int
TinyScript_Init (void *mem, int mem_size) {
    int i;
    int err;

    arena      = (Byte *) mem;
    arena_size = mem_size;
    symptr     = (Sym *) arena;
    valptr     = (Val *) (arena + arena_size);
    for (i     = 0; defs[i].name; i++) {
        err = TinyScript_Define(defs[i].name, defs[i].toktype, defs[i].val);
        if (err != TS_ERR_OK)
            return err;
    }
    return TS_ERR_OK;
}

int
TinyScript_Run (const char *buf, int saveStrings, int topLevel) {
    return ParseString(Cstring(buf), saveStrings, topLevel);
}
