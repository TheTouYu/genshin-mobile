#include <windows.h>
#include <setjmp.h>
#include <stdio.h>
static HANDLE L;
static void lg(const char* s){ DWORD w; char b[128]; _snprintf(b,128,"%s",s); if (L!=INVALID_HANDLE_VALUE) WriteFile(L,b,(DWORD)strlen(b),&w,0); }
static jmp_buf g_jb; static volatile LONG g_armed; static DWORD g_exc;
static LONG CALLBACK veh(EXCEPTION_POINTERS* p){
    if (InterlockedCompareExchange(&g_armed,0,0)) { g_exc=p->ExceptionRecord->ExceptionCode; longjmp(g_jb,1); }
    return EXCEPTION_CONTINUE_SEARCH;
}
static DWORD WINAPI w(LPVOID){
    AddVectoredExceptionHandler(1, veh);
    lg("veu-test start\n");
    if (setjmp(g_jb)==0) { InterlockedExchange(&g_armed,1); *(volatile int*)0 = 1; lg("veu-test NOT caught\n"); }
    else { char b[64]; _snprintf(b,64,"veu-test caught 0x%08lx\n",(unsigned long)g_exc); lg(b); }
    lg("veu-test after first\n");
    if (setjmp(g_jb)==0) { InterlockedExchange(&g_armed,1); *(volatile int*)(uintptr_t)0x1234 = 1; lg("veu-test NOT caught2\n"); }
    else { char b[64]; _snprintf(b,64,"veu-test caught2 0x%08lx\n",(unsigned long)g_exc); lg(b); }
    lg("veu-test after second — thread alive\n");
    return 0;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID){
    if (r!=DLL_PROCESS_ATTACH) return TRUE;
    wchar_t p[MAX_PATH]; DWORD n=GetModuleFileNameW(h,p,MAX_PATH); if(!n) return TRUE;
    wcscat(p,L".veutest.txt");
    L=CreateFileW(p,GENERIC_WRITE,FILE_SHARE_READ,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);
    HANDLE t=CreateThread(0,0,w,0,0,0); if(t) CloseHandle(t);
    return TRUE;
}
