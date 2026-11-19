#include <windows.h>
#include <stdio.h>
int main(void) {
    FILE* f = fopen("sysmetrics.log", "a");
    const int idx[] = {86 /*SM_TABLETPC*/, 94 /*SM_DIGITIZER*/, 95 /*SM_MAXIMUMTOUCHES*/, 0, 1, 2};
    for (int i = 0; i < 6; i++)
        fprintf(f, "GetSystemMetrics(%d) = %d\n", idx[i], GetSystemMetrics(idx[i]));
    fflush(f); fclose(f);
    return 0;
}
