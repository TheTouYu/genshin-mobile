#include <windows.h>
#include <stdio.h>
static HANDLE L;
static void lg(const char* s){ DWORD w; if (L!=INVALID_HANDLE_VALUE) WriteFile(L,s,(DWORD)strlen(s),&w,0); }
static DWORD WINAPI w(LPVOID){
    lg("seh-test start\n");
    __try { *(volatile int*)0 = 1; lg("seh-test NOT caught (bad)\n"); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        char b[64]; _snprintf(b,64,"seh-test caught 0x%08lx\n",(unsigned long)GetExceptionCode()); lg(b); }
    lg("seh-test after\n");
    return 0;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID){
    if (r!=DLL_PROCESS_ATTACH) return TRUE;
    wchar_t p[MAX_PATH]; DWORD n=GetModuleFileNameW(h,p,MAX_PATH); if(!n) return TRUE;
    wcscat(p,L".sehtest.txt");
    L=CreateFileW(p,GENERIC_WRITE,FILE_SHARE_READ,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);
    HANDLE t=CreateThread(0,0,w,0,0,0); if(t) CloseHandle(t);
    return TRUE;
}
