/* bd remora-4ei.98 — reproduce the ReZygisk monitor.c defect and show the fix, outside Android.
   Mirrors prepare_environment()'s loop exactly: read module.prop line by line and append each to a
   static char[1024], split around the description line. The overflow is CUMULATIVE — no single
   line is oversized, the running total is. With _FORTIFY_SOURCE each strcat becomes __strcat_chk
   with dest size 0x400 and aborts the moment the total would pass 1023. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char pre_section[1024];
static char post_section[1024];

int main(int argc, char **argv) {
    const int use_strlcat = argc > 1 && strcmp(argv[1], "fixed") == 0;
    /* A module.prop of the size ReZygisk's own banner grows to: many short lines, none near 1024. */
    char line[1024];
    int appended = 0;
    for (int i = 0; i < 60; ++i) {
        snprintf(line, sizeof(line), "someKey%02d=a moderately long but entirely ordinary value\n", i);
        if (use_strlcat)
            strlcat(pre_section, line, sizeof(pre_section));
        else
            strcat(pre_section, line);  /* __strcat_chk under _FORTIFY_SOURCE */
        appended += (int)strlen(line);
        printf("  line %2d: appended=%4d  held=%4zu\n", i, appended, strlen(pre_section));
        fflush(stdout);
    }
    printf("SURVIVED: %s held %zu bytes of %d appended\n",
           use_strlcat ? "strlcat" : "strcat", strlen(pre_section), appended);
    return 0;
}
