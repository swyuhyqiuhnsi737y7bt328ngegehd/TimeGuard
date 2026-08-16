/* fileguard.c — 文件自我保护（纯 C，Cygwin gcc 编译，-mwindows 无控制台）
 *
 * 功能：
 *  1. 占用本目录内所有 .exe 与 config/policy.json 的句柄，共享模式不含 DELETE，
 *     使这些文件在 fileguard 存活期间无法被删除/改名（允许读写）。
 *  2. 作为最后防线：检测到 core.exe 死亡时直接拉起它（仅“已安装”模式下，
 *     即目录内存在 state/installed.flag）。
 *  3. 每 5 秒扫描一次，捕获新出现的文件（例如新建的随机名守望副本）。
 */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>

#define QUERY_LIMITED 0x1000
#define SHARE_RW (FILE_SHARE_READ | FILE_SHARE_WRITE)

static wchar_t g_dir[MAX_PATH];
static wchar_t g_log[MAX_PATH];

static void build_paths(void)
{
    GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    /* 去掉文件名，保留目录（含结尾反斜杠） */
    wchar_t *p = wcsrchr(g_dir, L'\\');
    if (p) *(p + 1) = 0;
    wsprintfW(g_log, L"%lsstate\\logs\\fileguard.log", g_dir);
}

static void ensure_dirs(void)
{
    wchar_t b1[MAX_PATH], b2[MAX_PATH];
    wsprintfW(b1, L"%lsstate", g_dir);
    wsprintfW(b2, L"%lsstate\\logs", g_dir);
    CreateDirectoryW(b1, NULL);
    CreateDirectoryW(b2, NULL);
}

static void log_msg(const wchar_t *msg)
{
    HANDLE h = CreateFileW(g_log, FILE_APPEND_DATA, SHARE_RW, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[640];
    swprintf(line, 640, L"%04d-%02d-%02d %02d:%02d:%02d %ls\r\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
    DWORD w;
    WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &w, NULL);
    CloseHandle(h);
}

static int has_ext(const wchar_t *name, const wchar_t *ext)
{
    const wchar_t *dot = wcsrchr(name, L'.');
    if (!dot) return 0;
    size_t n = wcslen(dot);
    if (n != wcslen(ext)) return 0;
    for (size_t i = 0; i < n; i++) {
        wchar_t a = dot[i], b = ext[i];
        if (a >= L'A' && a <= L'Z') a += 32;
        if (b >= L'A' && b <= L'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static void lock_dir(void)
{
    wchar_t pattern[MAX_PATH];
    wsprintfW(pattern, L"%ls*", g_dir);
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(pattern, &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    int added = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!has_ext(fd.cFileName, L".exe") && wcscmp(fd.cFileName, L"policy.json") != 0) continue;
        wchar_t path[MAX_PATH];
        wsprintfW(path, L"%ls%ls", g_dir, fd.cFileName);
        /* 关键：共享模式不含 FILE_SHARE_DELETE -> 别人无法删除/改名，
           但包含 READ|WRITE -> 别人仍可打开读写、程序仍可运行 */
        HANDLE h = CreateFileW(path, GENERIC_READ, SHARE_RW, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            added++;
            /* 句柄故意不关闭：持续占用到本进程退出 */
        }
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    if (added) log_msg(L"锁定新文件");
}

static int file_exists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static DWORD read_pid(const wchar_t *p)
{
    HANDLE h = CreateFileW(p, GENERIC_READ, SHARE_RW, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[32] = {0};
    DWORD r = 0;
    ReadFile(h, buf, 31, &r, NULL);
    CloseHandle(h);
    return (DWORD)strtoul(buf, NULL, 10);
}

static int proc_alive(DWORD pid)
{
    if (!pid) return 0;
    HANDLE h = OpenProcess(QUERY_LIMITED, FALSE, pid);
    if (!h) return 0;
    CloseHandle(h);
    return 1;
}

/* 最后防线：core 死了就把它拉起来（仅已安装模式；收到退出指令则不再拉起） */
static void ensure_core(void)
{
    wchar_t marker[MAX_PATH], pidf[MAX_PATH], core[MAX_PATH], quitf[MAX_PATH];
    wsprintfW(marker, L"%lsstate\\installed.flag", g_dir);
    if (!file_exists(marker)) return;
    wsprintfW(quitf, L"%lsstate\\quit.flag", g_dir);
    if (file_exists(quitf)) return;   /* 已要求退出（卸载/家长退出），不复活 core */
    wsprintfW(pidf, L"%lsstate\\core.pid", g_dir);
    if (file_exists(pidf) && proc_alive(read_pid(pidf))) return;
    wsprintfW(core, L"%lscore.exe", g_dir);
    if (!file_exists(core)) return;
    wchar_t cmd[MAX_PATH * 2];
    wsprintfW(cmd, L"\"%ls\"", core);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessW(core, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
                       g_dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        log_msg(L"已重新拉起 core.exe");
    }
}

int main(void)
{
    build_paths();
    ensure_dirs();
    /* 单实例 */
    HANDLE m = CreateMutexW(NULL, TRUE, L"Local\\TimeGuard_FileGuard");
    if (m && GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    /* 写自己的 pid（给外部看状态用） */
    wchar_t pidf[MAX_PATH];
    wsprintfW(pidf, L"%lsstate\\fileguard.pid", g_dir);
    wchar_t pidbuf[32];
    wsprintfW(pidbuf, L"%lu", GetCurrentProcessId());
    HANDLE hf = CreateFileW(pidf, GENERIC_WRITE, SHARE_RW, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(hf, pidbuf, (DWORD)(wcslen(pidbuf) * sizeof(wchar_t)), &w, NULL);
        CloseHandle(hf);
    }
    log_msg(L"fileguard 启动");
    lock_dir();
    for (;;) {
        Sleep(5000);
        lock_dir();     /* 捕获新文件 */
        ensure_core();  /* 最后防线 */
    }
    return 0;
}
