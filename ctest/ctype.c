/* maize-76 AC 7353: ctype.h core self-check. Walks a representative ASCII byte
 * range and asserts each predicate against an independent reference computed
 * inline (so a copy-paste error in ctype.c cannot be masked by the same error
 * here), plus toupper/tolower over the cased and non-cased bytes. Prints a single
 * "ctype PASS" / "ctype FAIL" line. */
#include "ctype.h"
#include "stdio.h"

static int ok = 1;

static void check(int cond) { if (!cond) ok = 0; }

/* Independent references (do not call the library). */
static int ref_digit(int c) { return c >= '0' && c <= '9'; }
static int ref_upper(int c) { return c >= 'A' && c <= 'Z'; }
static int ref_lower(int c) { return c >= 'a' && c <= 'z'; }
static int ref_alpha(int c) { return ref_upper(c) || ref_lower(c); }
static int ref_alnum(int c) { return ref_alpha(c) || ref_digit(c); }
static int ref_xdigit(int c) {
    return ref_digit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}
static int ref_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
static int ref_print(int c) { return c >= 0x20 && c <= 0x7E; }
static int ref_cntrl(int c) { return (c >= 0 && c <= 0x1F) || c == 0x7F; }
static int ref_punct(int c) { return ref_print(c) && c != ' ' && !ref_alnum(c); }
static int ref_blank(int c) { return c == ' ' || c == '\t'; }
static int ref_graph(int c) { return ref_print(c) && c != ' '; }

/* Normalize predicate results to 0/1 for comparison. */
static int b(int x) { return x ? 1 : 0; }

int
main(void)
{
    int c;

    for (c = 0; c <= 0x7F; c++) {
        check(b(isdigit(c))  == ref_digit(c));
        check(b(isalpha(c))  == ref_alpha(c));
        check(b(isalnum(c))  == ref_alnum(c));
        check(b(isspace(c))  == ref_space(c));
        check(b(isupper(c))  == ref_upper(c));
        check(b(islower(c))  == ref_lower(c));
        check(b(isxdigit(c)) == ref_xdigit(c));
        check(b(isprint(c))  == ref_print(c));
        check(b(ispunct(c))  == ref_punct(c));
        check(b(iscntrl(c))  == ref_cntrl(c));
        check(b(isblank(c))  == ref_blank(c));
        check(b(isgraph(c))  == ref_graph(c));

        if (ref_lower(c))
            check(toupper(c) == c - ('a' - 'A'));
        else
            check(toupper(c) == c);

        if (ref_upper(c))
            check(tolower(c) == c + ('a' - 'A'));
        else
            check(tolower(c) == c);
    }

    /* A few explicit boundary spot-checks. */
    check(isdigit('0') && isdigit('9') && !isdigit('a') && !isdigit('/'));
    check(isalpha('A') && isalpha('z') && !isalpha('@') && !isalpha('['));
    check(isspace(' ') && isspace('\t') && !isspace('x'));
    check(ispunct('!') && ispunct('~') && !ispunct(' ') && !ispunct('a'));
    check(iscntrl(0) && iscntrl(0x7F) && !iscntrl(' '));
    check(isblank(' ') && isblank('\t') && !isblank('x'));
    check(isgraph('!') && isgraph('~') && !isgraph(' ') && !isgraph('\n'));
    check(toupper('a') == 'A' && tolower('Z') == 'z' && toupper('5') == '5');

    puts(ok ? "ctype PASS" : "ctype FAIL");
    return 0;
}
