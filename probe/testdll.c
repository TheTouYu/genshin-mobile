#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r){
    (void)r;
    if (reason == DLL_PROCESS_ATTACH){
        wchar_t p[MAX_PATH]; DWORD n = GetModuleFileNameW(h, p, MAX_PATH); (void)n;
        wcscat(p, L".attached.txt");
        HANDLE f = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE){ DWORD w; WriteFile(f, "ok", 2, &w, NULL); CloseHandle(f); }
    }
    return TRUE;
}
