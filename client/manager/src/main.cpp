/**
 * ag-manager — GUI version management tool
 * Uses Win32 API for native Windows interface.
 * Links against ag-update-lib for update operations.
 */
#include "version.h"
#include "ag_updater.h"
#include "log.h"

#ifdef _WIN32

#include <windows.h>
#include <commctrl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>

#pragma comment(lib, "comctl32.lib")

/* UTF-8 to wide string conversion */
static std::wstring utf8_to_wide(const char *utf8)
{
    if (!utf8 || !utf8[0]) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (len <= 0) return std::wstring();
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &ws[0], len);
    ws.resize(len - 1); /* remove null terminator */
    return ws;
}

static std::wstring utf8_to_wide(const std::string &utf8)
{
    return utf8_to_wide(utf8.c_str());
}

/* Control IDs */
#define IDC_LIST_VERSIONS   1001
#define IDC_BTN_REFRESH     1002
#define IDC_BTN_DOWNLOAD    1003
#define IDC_STATIC_DETAIL   1004
#define IDC_PROGRESS        1005

/* Window dimensions */
static const int WIN_W = 720;
static const int WIN_H = 520;

/* Global state */
static HWND g_hwnd = NULL;
static HWND g_list = NULL;
static HWND g_btn_refresh = NULL;
static HWND g_btn_download = NULL;
static HWND g_detail = NULL;
static HWND g_progress = NULL;
static HINSTANCE g_hinst = NULL;

/* Custom window messages for thread-safe UI updates */
#define WM_CHECK_ERROR      (WM_APP + 1)
#define WM_CHECK_DONE       (WM_APP + 2)
#define WM_DOWNLOAD_ERROR   (WM_APP + 3)
#define WM_DOWNLOAD_PROGRESS (WM_APP + 4)
#define WM_DOWNLOAD_DONE    (WM_APP + 5)

static std::vector<ag_version_info_t> g_versions;
static std::mutex g_mutex;
static int g_selected = -1;
static bool g_downloading = false;
static int g_dl_percent = 0;
static std::string g_dl_file_path;

/* Forward declarations */
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static void do_refresh();
static void do_download();
static void update_detail();
static void add_list_item(int index, const ag_version_info_t &info);

static std::string format_size(int64_t bytes)
{
    char buf[32];
    if (bytes < 1024) {
        snprintf(buf, sizeof(buf), "%lld B", (long long)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    }
    return std::string(buf);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    log_init_file("ag-manager.log");
    LOG_INFO("ag-manager v%s starting", APP_VERSION_STRING);
    g_hinst = hInstance;

    /* Init common controls for ListView and ProgressBar */
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icex);

    /* Register window class */
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AGManagerClass";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    /* Create main window */
    std::wstring title = utf8_to_wide(
        std::string("AGUpdater Manager v") + APP_VERSION_STRING);

    g_hwnd = CreateWindowExW(0, L"AGManagerClass", title.c_str(),
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, WIN_W, WIN_H,
        NULL, NULL, hInstance, NULL);

    LOG_INFO("Main window created");
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    /* Initial refresh */
    do_refresh();

    /* Message loop */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        /* Version list (ListView) - use W version for Unicode */
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER |
            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            10, 10, WIN_W - 40, 240,
            hwnd, (HMENU)IDC_LIST_VERSIONS, g_hinst, NULL);

        ListView_SetExtendedListViewStyle(g_list,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        /* Add columns */
        LVCOLUMNW col;
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        col.cx = 80;  col.pszText = (LPWSTR)L"Version";     col.iSubItem = 0;
        SendMessageW(g_list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);

        col.cx = 240; col.pszText = (LPWSTR)L"Description"; col.iSubItem = 1;
        SendMessageW(g_list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);

        col.cx = 80;  col.pszText = (LPWSTR)L"Size";        col.iSubItem = 2;
        SendMessageW(g_list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);

        col.cx = 140; col.pszText = (LPWSTR)L"Date";        col.iSubItem = 3;
        SendMessageW(g_list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);

        /* Refresh button */
        g_btn_refresh = CreateWindowW(L"BUTTON", L"Refresh",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 260, 80, 28, hwnd, (HMENU)IDC_BTN_REFRESH, g_hinst, NULL);

        /* Download button */
        g_btn_download = CreateWindowW(L"BUTTON", L"Download",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            100, 260, 80, 28, hwnd, (HMENU)IDC_BTN_DOWNLOAD, g_hinst, NULL);
        EnableWindow(g_btn_download, FALSE);

        /* Detail area */
        g_detail = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 300, WIN_W - 40, 100,
            hwnd, (HMENU)IDC_STATIC_DETAIL, g_hinst, NULL);

        /* Progress bar */
        g_progress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            10, 410, WIN_W - 40, 20,
            hwnd, (HMENU)IDC_PROGRESS, g_hinst, NULL);
        SendMessage(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessage(g_progress, PBM_SETPOS, 0, 0);

        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_REFRESH:
            do_refresh();
            break;
        case IDC_BTN_DOWNLOAD:
            do_download();
            break;
        }
        break;

    case WM_NOTIFY: {
        NMHDR *nmhdr = (NMHDR *)lParam;
        if (nmhdr->idFrom == IDC_LIST_VERSIONS) {
            if (nmhdr->code == LVN_ITEMCHANGED) {
                NMLISTVIEW *nmlv = (NMLISTVIEW *)lParam;
                if (nmlv->uNewState & LVIS_SELECTED) {
                    g_selected = nmlv->iItem;
                    EnableWindow(g_btn_download, !g_downloading);
                    update_detail();
                }
            }
        }
        break;
    }

    case WM_CHECK_ERROR: {
        LOG_ERROR("Failed to check for updates");
        SetWindowTextW(g_detail, L"Failed to check for updates.");
        break;
    }

    case WM_CHECK_DONE: {
        /* Populate ListView from g_versions (filled by background thread) */
        SendMessageW(g_list, LVM_DELETEALLITEMS, 0, 0);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (int i = 0; i < (int)g_versions.size(); ++i) {
                add_list_item(i, g_versions[i]);
            }
        }
        if (g_versions.empty()) {
            SetWindowTextW(g_detail, L"No versions available.");
        } else {
            SetWindowTextW(g_detail, L"Select a version to see details.");
        }
        break;
    }

    case WM_DOWNLOAD_ERROR: {
        LOG_ERROR("Download failed");
        SendMessage(g_progress, PBM_SETPOS, 0, 0);
        EnableWindow(g_btn_download, TRUE);
        EnableWindow(g_btn_refresh, TRUE);
        MessageBoxW(hwnd, L"Download failed.", L"Error", MB_OK | MB_ICONERROR);
        break;
    }

    case WM_DOWNLOAD_PROGRESS: {
        int pct;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            pct = g_dl_percent;
        }
        SendMessage(g_progress, PBM_SETPOS, pct, 0);
        break;
    }

    case WM_DOWNLOAD_DONE: {
        SendMessage(g_progress, PBM_SETPOS, 100, 0);

        std::string fp;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            fp = g_dl_file_path;
        }

        int result = MessageBoxW(hwnd,
            L"Download complete. Install now?\n"
            L"The application will close to apply the update.",
            L"Install Update", MB_YESNO | MB_ICONQUESTION);

        if (result == IDYES) {
            ag_error_t err = ag_apply_update(fp.c_str());
            if (err == AG_OK) {
                LOG_INFO("Updater launched, exiting manager");
                PostQuitMessage(0);
            } else {
                LOG_ERROR("Failed to start updater, error=%d", (int)err);
                MessageBoxW(hwnd, L"Failed to start updater.",
                           L"Error", MB_OK | MB_ICONERROR);
            }
        }

        SendMessage(g_progress, PBM_SETPOS, 0, 0);
        EnableWindow(g_btn_download, TRUE);
        EnableWindow(g_btn_refresh, TRUE);
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static void lv_set_item_text_w(HWND list, int item, int sub, const wchar_t *text)
{
    LVITEMW lvi;
    memset(&lvi, 0, sizeof(lvi));
    lvi.iSubItem = sub;
    lvi.pszText = (LPWSTR)text;
    SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)item, (LPARAM)&lvi);
}

static void add_list_item(int index, const ag_version_info_t &info)
{
    std::wstring ver_w = utf8_to_wide(info.version);
    LVITEMW item;
    memset(&item, 0, sizeof(item));
    item.mask = LVIF_TEXT;
    item.iItem = index;
    item.iSubItem = 0;
    item.pszText = (LPWSTR)ver_w.c_str();
    SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&item);

    std::wstring desc_w = utf8_to_wide(info.description);
    lv_set_item_text_w(g_list, index, 1, desc_w.c_str());

    std::wstring size_w = utf8_to_wide(format_size(info.file_size));
    lv_set_item_text_w(g_list, index, 2, size_w.c_str());

    std::wstring date_w = utf8_to_wide(info.created_at);
    lv_set_item_text_w(g_list, index, 3, date_w.c_str());
}

static void update_detail()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_selected < 0 || g_selected >= (int)g_versions.size()) {
        SetWindowTextW(g_detail, L"Select a version to see details.");
        return;
    }

    const ag_version_info_t &v = g_versions[g_selected];
    std::wstring text =
        L"Version: " + utf8_to_wide(v.version) + L"\r\n" +
        L"Description: " + utf8_to_wide(v.description) + L"\r\n" +
        L"Size: " + utf8_to_wide(format_size(v.file_size)) + L"\r\n" +
        L"SHA256: " + utf8_to_wide(v.file_sha256) + L"\r\n" +
        L"Date: " + utf8_to_wide(v.created_at);
    SetWindowTextW(g_detail, text.c_str());
}

/* Callback for ag_check_update — runs on background thread */
static void on_check_callback(ag_error_t error, const ag_version_info_t *info,
                               int update_count, void *user_data)
{
    (void)user_data;

    if (error != AG_OK && error != AG_ERR_NO_UPDATE) {
        LOG_ERROR("on_check_callback: check update failed, error=%d", (int)error);
        PostMessage(g_hwnd, WM_CHECK_ERROR, (WPARAM)error, 0);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_versions.clear();
        if (info != NULL && update_count > 0) {
            for (int i = 0; i < update_count; ++i) {
                g_versions.push_back(info[i]);
            }
        }
    }

    LOG_DEBUG("on_check_callback: received %d version(s)", update_count);
    /* Post message to UI thread to update list */
    PostMessage(g_hwnd, WM_CHECK_DONE, 0, 0);
}

static void do_refresh()
{
    LOG_DEBUG("do_refresh: checking for updates");
    SetWindowTextW(g_detail, L"Loading...");
    ListView_DeleteAllItems(g_list);
    g_selected = -1;
    EnableWindow(g_btn_download, FALSE);

    /* HACK: Using version "0.0.0" to retrieve all available versions.
     * Ideally, ag-update-lib should expose a dedicated ag_list_versions()
     * function, or manager should call GET /api/v1/client/versions directly.
     * TODO: Replace with proper API when ag_updater.h is extended. */
    ag_check_update("ag-manager", "0.0.0", on_check_callback, NULL);
}

/* Callback for ag_download_update — runs on background thread.
 * Must NOT directly operate UI controls. Uses PostMessage to
 * dispatch events to the main (UI) thread. */
static void on_download_callback(ag_error_t error,
                                  const ag_download_progress_t *progress,
                                  const char *file_path,
                                  void *user_data)
{
    (void)user_data;

    if (error != AG_OK) {
        LOG_ERROR("on_download_callback: download failed, error=%d", (int)error);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_downloading = false;
        }
        PostMessage(g_hwnd, WM_DOWNLOAD_ERROR, (WPARAM)error, 0);
        return;
    }

    if (file_path != NULL) {
        /* Download complete — store path and notify UI thread */
        LOG_INFO("on_download_callback: download complete, file=%s", file_path);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_downloading = false;
            g_dl_file_path = file_path;
            g_dl_percent = 100;
        }
        PostMessage(g_hwnd, WM_DOWNLOAD_DONE, 0, 0);
        return;
    }

    if (progress != NULL) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_dl_percent = progress->percent;
        }
        PostMessage(g_hwnd, WM_DOWNLOAD_PROGRESS, 0, 0);
    }
}

static void do_download()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_selected < 0 || g_selected >= (int)g_versions.size()) {
        return;
    }
    if (g_downloading) return;

    g_downloading = true;
    LOG_INFO("do_download: starting download for version %s", g_versions[g_selected].version);
    EnableWindow(g_btn_download, FALSE);
    EnableWindow(g_btn_refresh, FALSE);
    SendMessage(g_progress, PBM_SETPOS, 0, 0);

    ag_download_update(&g_versions[g_selected], on_download_callback, NULL);
}

#else /* Non-Windows stub */

#include <cstdio>

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    LOG_ERROR("ag-manager: This program requires Windows.");
    return 1;
}

#endif /* _WIN32 */
