/**
 * ag-manager — GUI version management tool
 * Uses Win32 API for native Windows interface.
 * Links against ag-update-lib for update operations.
 */
#include "version.h"
#include "ag_updater.h"

#ifdef _WIN32

#include <windows.h>
#include <commctrl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>

#pragma comment(lib, "comctl32.lib")

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
    g_hinst = hInstance;

    /* Init common controls for ListView and ProgressBar */
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icex);

    /* Register window class */
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "AGManagerClass";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExA(&wc);

    /* Create main window */
    char title[64];
    snprintf(title, sizeof(title), "AGUpdater Manager v%s", APP_VERSION_STRING);

    g_hwnd = CreateWindowExA(0, "AGManagerClass", title,
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, WIN_W, WIN_H,
        NULL, NULL, hInstance, NULL);

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    /* Initial refresh */
    do_refresh();

    /* Message loop */
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return (int)msg.wParam;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        /* Version list (ListView) */
        g_list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | WS_BORDER |
            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            10, 10, WIN_W - 40, 240,
            hwnd, (HMENU)IDC_LIST_VERSIONS, g_hinst, NULL);

        ListView_SetExtendedListViewStyle(g_list,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        /* Add columns */
        LVCOLUMNA col;
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        col.cx = 80;  col.pszText = (LPSTR)"Version";  col.iSubItem = 0;
        ListView_InsertColumn(g_list, 0, &col);

        col.cx = 240; col.pszText = (LPSTR)"Description"; col.iSubItem = 1;
        ListView_InsertColumn(g_list, 1, &col);

        col.cx = 80;  col.pszText = (LPSTR)"Size";     col.iSubItem = 2;
        ListView_InsertColumn(g_list, 2, &col);

        col.cx = 140; col.pszText = (LPSTR)"Date";     col.iSubItem = 3;
        ListView_InsertColumn(g_list, 3, &col);

        /* Refresh button */
        g_btn_refresh = CreateWindowA("BUTTON", "Refresh",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 260, 80, 28, hwnd, (HMENU)IDC_BTN_REFRESH, g_hinst, NULL);

        /* Download button */
        g_btn_download = CreateWindowA("BUTTON", "Download",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            100, 260, 80, 28, hwnd, (HMENU)IDC_BTN_DOWNLOAD, g_hinst, NULL);
        EnableWindow(g_btn_download, FALSE);

        /* Detail area */
        g_detail = CreateWindowExA(WS_EX_CLIENTEDGE, "STATIC", "",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 300, WIN_W - 40, 100,
            hwnd, (HMENU)IDC_STATIC_DETAIL, g_hinst, NULL);

        /* Progress bar */
        g_progress = CreateWindowExA(0, PROGRESS_CLASSA, "",
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
        SetWindowTextA(g_detail, "Failed to check for updates.");
        break;
    }

    case WM_CHECK_DONE: {
        /* Populate ListView from g_versions (filled by background thread) */
        ListView_DeleteAllItems(g_list);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (int i = 0; i < (int)g_versions.size(); ++i) {
                add_list_item(i, g_versions[i]);
            }
        }
        if (g_versions.empty()) {
            SetWindowTextA(g_detail, "No versions available.");
        } else {
            SetWindowTextA(g_detail, "Select a version to see details.");
        }
        break;
    }

    case WM_DOWNLOAD_ERROR: {
        SendMessage(g_progress, PBM_SETPOS, 0, 0);
        EnableWindow(g_btn_download, TRUE);
        EnableWindow(g_btn_refresh, TRUE);
        MessageBoxA(hwnd, "Download failed.", "Error", MB_OK | MB_ICONERROR);
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

        int result = MessageBoxA(hwnd,
            "Download complete. Install now?\n"
            "The application will close to apply the update.",
            "Install Update", MB_YESNO | MB_ICONQUESTION);

        if (result == IDYES) {
            ag_error_t err = ag_apply_update(fp.c_str());
            if (err == AG_OK) {
                PostQuitMessage(0);
            } else {
                MessageBoxA(hwnd, "Failed to start updater.",
                           "Error", MB_OK | MB_ICONERROR);
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
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static void add_list_item(int index, const ag_version_info_t &info)
{
    LVITEMA item;
    memset(&item, 0, sizeof(item));
    item.mask = LVIF_TEXT;
    item.iItem = index;
    item.iSubItem = 0;
    item.pszText = (LPSTR)info.version;
    ListView_InsertItem(g_list, &item);

    ListView_SetItemText(g_list, index, 1, (LPSTR)info.description);

    std::string size_str = format_size(info.file_size);
    ListView_SetItemText(g_list, index, 2, (LPSTR)size_str.c_str());

    ListView_SetItemText(g_list, index, 3, (LPSTR)info.created_at);
}

static void update_detail()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_selected < 0 || g_selected >= (int)g_versions.size()) {
        SetWindowTextA(g_detail, "Select a version to see details.");
        return;
    }

    const ag_version_info_t &v = g_versions[g_selected];
    char buf[2048];
    snprintf(buf, sizeof(buf),
        "Version: %s\r\n"
        "Description: %s\r\n"
        "Size: %s\r\n"
        "SHA256: %s\r\n"
        "Date: %s",
        v.version, v.description,
        format_size(v.file_size).c_str(),
        v.file_sha256, v.created_at);
    SetWindowTextA(g_detail, buf);
}

/* Callback for ag_check_update — runs on background thread */
static void on_check_callback(ag_error_t error, const ag_version_info_t *info,
                               int update_count, void *user_data)
{
    (void)user_data;

    if (error != AG_OK && error != AG_ERR_NO_UPDATE) {
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

    /* Post message to UI thread to update list */
    PostMessage(g_hwnd, WM_CHECK_DONE, 0, 0);
}

static void do_refresh()
{
    SetWindowTextA(g_detail, "Loading...");
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
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_downloading = false;
        }
        PostMessage(g_hwnd, WM_DOWNLOAD_ERROR, (WPARAM)error, 0);
        return;
    }

    if (file_path != NULL) {
        /* Download complete — store path and notify UI thread */
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
    fprintf(stderr, "ag-manager: This program requires Windows.\n");
    return 1;
}

#endif /* _WIN32 */
