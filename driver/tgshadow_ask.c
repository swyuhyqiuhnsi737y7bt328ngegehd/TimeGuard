/*==============================================================================
  tgshadow_ask.c - 关机确认对话框（P4）

  由 tgshadow_svc.exe 在活动用户会话中启动：
      tgshadow_ask.exe <超时秒数> <结果文件>
  两个选择：
      保留本次修改 —— 必须输入正确的家长密码，结果写 "keep"
      还原（默认）—— 结果写 "restore"
  超时、关窗、按 ESC 一律等于"还原"：默认动作永远偏向不保留。
==============================================================================*/
#define _WIN32_WINNT 0x0601
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include "tgsha256.h"

#define ID_PW       1001
#define ID_KEEP     1002
#define ID_RESTORE  1003
#define TIMER_ID    1

static WCHAR  g_ResultPath[MAX_PATH] = L"";
static int    g_Timeout = 30;
static int    g_Remain = 30;
static HWND   g_Hwnd = NULL, g_Pw = NULL, g_Count = NULL, g_Info = NULL;
static HFONT  g_Font = NULL, g_FontBig = NULL;
static BOOL   g_Done = FALSE;

static void WriteResult(const char *text)
{
    HANDLE h;
    DWORD  written = 0;

    if (g_Done) return;
    g_Done = TRUE;
    h = CreateFileW(g_ResultPath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, text, (DWORD)strlen(text), &written, NULL);
        CloseHandle(h);
    }
    DestroyWindow(g_Hwnd);
}

static BOOL CheckParentPassword(const WCHAR *input)
{
    HKEY  key = NULL;
    WCHAR stored[128] = L"";
    WCHAR hash[128] = L"";
    DWORD size = sizeof(stored), type = 0;
    BOOL  ok = FALSE;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TimeGuard\\Shadow",
                      0, KEY_READ, &key) != ERROR_SUCCESS) {
        MessageBoxW(g_Hwnd, L"未设置家长密码，无法保留修改。\n请先运行: tgshadow_svc setpass <密码>",
                    L"TimeGuard", MB_ICONWARNING | MB_OK);
        return FALSE;
    }
    if (RegQueryValueExW(key, L"ParentPasswordHash", NULL, &type,
                         (LPBYTE)stored, &size) != ERROR_SUCCESS || stored[0] == 0) {
        RegCloseKey(key);
        MessageBoxW(g_Hwnd, L"未设置家长密码，无法保留修改。\n请先运行: tgshadow_svc setpass <密码>",
                    L"TimeGuard", MB_ICONWARNING | MB_OK);
        return FALSE;
    }
    RegCloseKey(key);

    if (!TgSha256HexOfPassword(input, hash, _countof(hash))) {
        return FALSE;
    }
    ok = (_wcsicmp(hash, stored) == 0);
    if (!ok) {
        MessageBoxW(g_Hwnd, L"密码不正确，无法保留修改。", L"TimeGuard",
                    MB_ICONERROR | MB_OK);
    }
    return ok;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_Font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        g_FontBig = CreateFontW(-26, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");

        g_Info = CreateWindowExW(0, L"STATIC",
            L"系统即将关机。\n\n本次开机以来对受保护磁盘的所有修改都还在影子区里，\n"
            L"默认会在重启后全部撤销（设备回到本次开机前的状态）。\n\n"
            L"如需保留这些修改，请输入家长密码后点击【保留修改】。",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            24, 18, 520, 120, hwnd, NULL, NULL, NULL);

        g_Count = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  24, 146, 520, 34, hwnd, NULL, NULL, NULL);

        CreateWindowExW(0, L"STATIC", L"家长密码：", WS_CHILD | WS_VISIBLE | SS_LEFT,
                        24, 196, 90, 24, hwnd, NULL, NULL, NULL);

        g_Pw = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
                               116, 192, 300, 30, hwnd, (HMENU)ID_PW, NULL, NULL);

        CreateWindowExW(0, L"BUTTON", L"保留修改（需密码）",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        116, 240, 190, 40, hwnd, (HMENU)ID_KEEP, NULL, NULL);

        CreateWindowExW(0, L"BUTTON", L"还原（不保留）",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        320, 240, 190, 40, hwnd, (HMENU)ID_RESTORE, NULL, NULL);

        SendMessageW(g_Info, WM_SETFONT, (WPARAM)g_Font, TRUE);
        SendMessageW(g_Count, WM_SETFONT, (WPARAM)g_FontBig, TRUE);
        SendMessageW(g_Pw, WM_SETFONT, (WPARAM)g_Font, TRUE);
        SendMessageW(GetDlgItem(hwnd, ID_KEEP), WM_SETFONT, (WPARAM)g_Font, TRUE);
        SendMessageW(GetDlgItem(hwnd, ID_RESTORE), WM_SETFONT, (WPARAM)g_Font, TRUE);
        {
            HWND hLabel = GetDlgItem(hwnd, 0);
            (void)hLabel;
        }

        SetTimer(hwnd, TIMER_ID, 1000, NULL);
        SetForegroundWindow(hwnd);
        SetFocus(g_Pw);
        return 0;

    case WM_TIMER:
        g_Remain--;
        {
            WCHAR buf[128];
            _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                         L"剩余 %d 秒，超时将自动【还原】", g_Remain);
            SetWindowTextW(g_Count, buf);
        }
        if (g_Remain <= 0) {
            WriteResult("restore");
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_KEEP) {
            WCHAR pw[256] = L"";
            GetWindowTextW(g_Pw, pw, _countof(pw));
            if (CheckParentPassword(pw)) {
                WriteResult("keep");
            }
            return 0;
        }
        if (LOWORD(wp) == ID_RESTORE) {
            WriteResult("restore");
            return 0;
        }
        return 0;

    case WM_CLOSE:
        WriteResult("restore");     /* 关窗 = 还原 */
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        if (g_Font) DeleteObject(g_Font);
        if (g_FontBig) DeleteObject(g_FontBig);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int show)
{
    WNDCLASSEXW wc;
    MSG msg;
    int argc = 0;
    LPWSTR *argv;
    int x, y;

    UNREFERENCED_PARAMETER(hPrev);
    UNREFERENCED_PARAMETER(show);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 2) g_Timeout = _wtoi(argv[1]);
    if (g_Timeout <= 0 || g_Timeout > 600) g_Timeout = 30;
    if (argc >= 3) {
        wcsncpy_s(g_ResultPath, _countof(g_ResultPath), argv[2], _TRUNCATE);
    } else {
        wcsncpy_s(g_ResultPath, _countof(g_ResultPath),
                  L"C:\\Windows\\Temp\\tgshadow_ask.result", _TRUNCATE);
    }
    g_Remain = g_Timeout;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"TgShadowAskWnd";
    RegisterClassExW(&wc);

    x = (GetSystemMetrics(SM_CXSCREEN) - 590) / 2;
    y = (GetSystemMetrics(SM_CYSCREEN) - 340) / 2;

    g_Hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                             L"TgShadowAskWnd",
                             L"TimeGuard —— 本次开机以来的修改如何处理？",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             x, y, 590, 340,
                             NULL, NULL, hInst, NULL);
    if (g_Hwnd == NULL) return 0;

    ShowWindow(g_Hwnd, SW_SHOW);
    UpdateWindow(g_Hwnd);

    /* 抢焦点：关机流程里前台窗口可能被系统界面抢走 */
    SetForegroundWindow(g_Hwnd);
    BringWindowToTop(g_Hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* 走到这里说明没写结果（异常退出）——按最安全的"还原"处理 */
    if (!g_Done) {
        WriteResult("restore");
    }
    return 0;
}
