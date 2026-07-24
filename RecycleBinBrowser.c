// Native Windows Recycle Bin Browser (C / Win32 API)
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define APP_NAME L"Recycle Bin Browser"
#define PATH_CAP 32768
#define ID_DRIVE 101
#define ID_REFRESH 102
#define ID_OPEN_BIN 103
#define ID_OPEN_FILE 104
#define ID_REVEAL 105
#define ID_SEARCH 106
#define ID_LANGUAGE 107
#define ID_RESTORE 108
#define ID_BACKUP 109
#define ID_EXPORT 110
#define ID_LIST 200
#define ID_STATUS 201

typedef struct {
    wchar_t payload[PATH_CAP];       // The actual $R... file stored in the Recycle Bin.
    wchar_t original[PATH_CAP];      // The path before deletion, read from the matching $I... file.
    wchar_t deleted[64];
    uint64_t size;
    BOOL has_metadata;
} BIN_ITEM;

static HWND g_drive, g_list, g_status, g_search, g_drive_label, g_search_label;
static wchar_t g_bin_root[PATH_CAP];
static BIN_ITEM **g_items;
static size_t g_item_count, g_item_capacity;
static BOOL g_korean = FALSE;
static int g_sort_column = 3;
static BOOL g_sort_ascending = FALSE;

static const wchar_t *Text(const wchar_t *english, const wchar_t *korean) {
    return g_korean ? korean : english;
}

static void SetText(HWND window, const wchar_t *text) {
    SetWindowTextW(window, text);
}

static const wchar_t *LeafName(const wchar_t *path) {
    const wchar_t *slash = wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

static BOOL IsDirectory(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void FormatSize(uint64_t bytes, wchar_t *out, size_t out_count) {
    const wchar_t *units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024.0 && unit < 4) { value /= 1024.0; unit++; }
    if (unit == 0) swprintf(out, out_count, L"%llu B", (unsigned long long)bytes);
    else swprintf(out, out_count, L"%.1f %ls", value, units[unit]);
}

static void FormatFileTime(uint64_t ticks, wchar_t *out, size_t out_count) {
    FILETIME utc, local;
    SYSTEMTIME st;
    ULARGE_INTEGER data;
    data.QuadPart = ticks;
    utc.dwLowDateTime = data.LowPart;
    utc.dwHighDateTime = data.HighPart;
    if (FileTimeToLocalFileTime(&utc, &local) && FileTimeToSystemTime(&local, &st)) {
        swprintf(out, out_count, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    } else {
        out[0] = L'\0';
    }
}

// $I format v1 starts the UTF-16 path at byte 24.  Windows 8+ v2 places a
// four-byte character count at byte 24 and begins the path at byte 28.
static BOOL ReadRecycleMetadata(const wchar_t *info_path, BIN_ITEM *item) {
    HANDLE file = CreateFileW(info_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BYTE header[28] = {0};
    DWORD read = 0;
    uint64_t version, original_size, deletion_ticks;
    DWORD chars = 0;
    LARGE_INTEGER file_size, offset;
    size_t byte_count;

    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (!ReadFile(file, header, sizeof(header), &read, NULL) || read < 24) { CloseHandle(file); return FALSE; }
    memcpy(&version, header, sizeof(version));
    memcpy(&original_size, header + 8, sizeof(original_size));
    memcpy(&deletion_ticks, header + 16, sizeof(deletion_ticks));
    item->size = original_size;
    FormatFileTime(deletion_ticks, item->deleted, _countof(item->deleted));

    if (version >= 2 && read >= 28) {
        memcpy(&chars, header + 24, sizeof(chars));
        if (chars >= PATH_CAP) chars = PATH_CAP - 1;
        offset.QuadPart = 28;
        byte_count = (size_t)chars * sizeof(wchar_t);
    } else {
        if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart <= 24) { CloseHandle(file); return FALSE; }
        byte_count = (size_t)(file_size.QuadPart - 24);
        if (byte_count >= (PATH_CAP - 1) * sizeof(wchar_t)) byte_count = (PATH_CAP - 1) * sizeof(wchar_t);
        offset.QuadPart = 24;
    }
    if (!SetFilePointerEx(file, offset, NULL, FILE_BEGIN) || !ReadFile(file, item->original, (DWORD)byte_count, &read, NULL)) {
        CloseHandle(file); return FALSE;
    }
    item->original[read / sizeof(wchar_t)] = L'\0';
    CloseHandle(file);
    return item->original[0] != L'\0';
}

static BOOL ContainsInsensitive(const wchar_t *text, const wchar_t *needle) {
    size_t needle_len = wcslen(needle);
    if (!needle_len) return TRUE;
    for (; *text; text++) {
        size_t i;
        for (i = 0; i < needle_len && text[i] && towlower(text[i]) == towlower(needle[i]); i++);
        if (i == needle_len) return TRUE;
    }
    return FALSE;
}

static BOOL MatchesSearch(const BIN_ITEM *item) {
    wchar_t query[256];
    GetWindowTextW(g_search, query, _countof(query));
    return !query[0] || ContainsInsensitive(LeafName(item->has_metadata ? item->original : item->payload), query)
        || ContainsInsensitive(item->original, query) || ContainsInsensitive(item->payload, query);
}

static void AddRow(const BIN_ITEM *item) {
    wchar_t display[PATH_CAP], size_text[48];
    LVITEMW row = {0};
    int index;
    if (!MatchesSearch(item)) return;
    wcsncpy(display, item->has_metadata ? LeafName(item->original) : LeafName(item->payload), PATH_CAP - 1);
    display[PATH_CAP - 1] = L'\0';
    FormatSize(item->size, size_text, _countof(size_text));
    index = ListView_GetItemCount(g_list);
    row.mask = LVIF_TEXT | LVIF_PARAM;
    row.iItem = index;
    row.pszText = display;
    row.lParam = (LPARAM)item;
    ListView_InsertItem(g_list, &row);
    ListView_SetItemText(g_list, index, 1, item->has_metadata ? item->original : Text(L"Original metadata unavailable (no $I file)", L"원본 정보 없음 ($I 파일 없음)"));
    ListView_SetItemText(g_list, index, 2, size_text);
    ListView_SetItemText(g_list, index, 3, item->has_metadata && item->deleted[0] ? item->deleted : L"-");
    ListView_SetItemText(g_list, index, 4, item->payload);
}

static void RenderItems(void) {
    ListView_DeleteAllItems(g_list);
    for (size_t i = 0; i < g_item_count; i++) AddRow(g_items[i]);
}

static BOOL StoreItem(BIN_ITEM *item) {
    if (g_item_count == g_item_capacity) {
        size_t next = g_item_capacity ? g_item_capacity * 2 : 256;
        BIN_ITEM **items = (BIN_ITEM **)realloc(g_items, next * sizeof(*g_items));
        if (!items) return FALSE;
        g_items = items; g_item_capacity = next;
    }
    g_items[g_item_count++] = item;
    return TRUE;
}

static int __cdecl CompareItems(const void *left, const void *right) {
    const BIN_ITEM *a = *(const BIN_ITEM * const *)left;
    const BIN_ITEM *b = *(const BIN_ITEM * const *)right;
    int result = 0;
    const wchar_t *a_name = a->has_metadata ? LeafName(a->original) : LeafName(a->payload);
    const wchar_t *b_name = b->has_metadata ? LeafName(b->original) : LeafName(b->payload);
    switch (g_sort_column) {
    case 0: result = _wcsicmp(a_name, b_name); break;
    case 1: result = _wcsicmp(a->original, b->original); break;
    case 2: result = a->size < b->size ? -1 : a->size > b->size; break;
    case 3: result = _wcsicmp(a->deleted, b->deleted); break;
    case 4: result = _wcsicmp(a->payload, b->payload); break;
    }
    return g_sort_ascending ? result : -result;
}

static void SortAndRender(void) {
    if (g_item_count > 1) qsort(g_items, g_item_count, sizeof(*g_items), CompareItems);
    RenderItems();
}

static void InsertItem(const wchar_t *payload, const WIN32_FIND_DATAW *find_data) {
    BIN_ITEM *item = (BIN_ITEM *)calloc(1, sizeof(BIN_ITEM));
    wchar_t info_path[PATH_CAP];
    if (!item) return;
    wcsncpy(item->payload, payload, PATH_CAP - 1);
    wcsncpy(info_path, payload, PATH_CAP - 1);
    info_path[PATH_CAP - 1] = L'\0';
    {
        wchar_t *name = wcsrchr(info_path, L'\\');
        if (name && name[1] == L'$' && (name[2] == L'R' || name[2] == L'r')) name[2] = L'I';
    }
    item->has_metadata = ReadRecycleMetadata(info_path, item);
    if (!item->has_metadata) {
        ULARGE_INTEGER size;
        size.LowPart = find_data->nFileSizeLow;
        size.HighPart = find_data->nFileSizeHigh;
        item->size = size.QuadPart;
    }
    if (!StoreItem(item)) { free(item); return; }
    AddRow(item);
}

static BOOL IsRecyclePayloadName(const wchar_t *name) {
    return name[0] == L'$' && (name[1] == L'R' || name[1] == L'r');
}

static void ScanDirectory(const wchar_t *folder) {
    wchar_t pattern[PATH_CAP], child[PATH_CAP];
    WIN32_FIND_DATAW data;
    HANDLE find;
    swprintf(pattern, PATH_CAP, L"%ls\\*", folder);
    find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        swprintf(child, PATH_CAP, L"%ls\\%ls", folder, data.cFileName);
        if (IsRecyclePayloadName(data.cFileName)) {
            InsertItem(child, &data);
        } else if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanDirectory(child);
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

static void ClearItems(void) {
    for (size_t i = 0; i < g_item_count; i++) free(g_items[i]);
    free(g_items);
    g_items = NULL; g_item_count = 0; g_item_capacity = 0;
    ListView_DeleteAllItems(g_list);
}

static BOOL GetSelectedItem(BIN_ITEM **selected) {
    int index = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    LVITEMW row = {0};
    if (index < 0) return FALSE;
    row.mask = LVIF_PARAM;
    row.iItem = index;
    if (!ListView_GetItem(g_list, &row) || !row.lParam) return FALSE;
    *selected = (BIN_ITEM *)row.lParam;
    return TRUE;
}

static void ScanRecycleBin(void) {
    wchar_t drive[32], root[8], status[256];
    int count;
    GetWindowTextW(g_drive, drive, _countof(drive));
    if (!iswalpha(drive[0])) { MessageBoxW(NULL, Text(L"Select a drive.", L"드라이브를 선택하세요."), APP_NAME, MB_ICONWARNING); return; }
    swprintf(root, _countof(root), L"%c:\\", towupper(drive[0]));
    swprintf(g_bin_root, PATH_CAP, L"%ls$RECYCLE.BIN", root);
    if (!IsDirectory(g_bin_root)) swprintf(g_bin_root, PATH_CAP, L"%ls$RECYCLE.BINS", root);
    ClearItems();
    if (!IsDirectory(g_bin_root)) {
        SetText(g_status, Text(L"No $RECYCLE.BIN folder was found on this drive.", L"이 드라이브에서 $RECYCLE.BIN 폴더를 찾지 못했습니다."));
        return;
    }
    SetText(g_status, Text(L"Reading Recycle Bin items…", L"휴지통 항목을 읽는 중…"));
    UpdateWindow(g_status);
    ScanDirectory(g_bin_root);
    SortAndRender();
    count = (int)g_item_count;
    if (g_korean) swprintf(status, _countof(status), L"%ls · %d개 항목 · 원래 위치는 삭제 전 경로, 실제 위치는 현재 $R 파일 경로입니다.", g_bin_root, count);
    else swprintf(status, _countof(status), L"%ls · %d items · Original location is the pre-deletion path; actual location is the current $R file path.", g_bin_root, count);
    SetText(g_status, status);
}

static void OpenBinFolder(void) {
    if (IsDirectory(g_bin_root)) ShellExecuteW(NULL, L"open", g_bin_root, NULL, NULL, SW_SHOWNORMAL);
    else MessageBoxW(NULL, Text(L"Click Refresh first.", L"먼저 새로 고침을 누르세요."), APP_NAME, MB_ICONINFORMATION);
}

static void OpenSelectedFile(void) {
    BIN_ITEM *item;
    if (!GetSelectedItem(&item)) { MessageBoxW(NULL, Text(L"Select a file in the list.", L"목록에서 파일을 선택하세요."), APP_NAME, MB_ICONINFORMATION); return; }
    if ((INT_PTR)ShellExecuteW(NULL, L"open", item->payload, NULL, NULL, SW_SHOWNORMAL) <= 32)
        MessageBoxW(NULL, Text(L"The selected file could not be opened.", L"선택한 파일을 열 수 없습니다."), APP_NAME, MB_ICONWARNING);
}

static void RevealSelectedFile(void) {
    BIN_ITEM *item;
    wchar_t arguments[PATH_CAP + 32];
    if (!GetSelectedItem(&item)) { MessageBoxW(NULL, Text(L"Select a file in the list.", L"목록에서 파일을 선택하세요."), APP_NAME, MB_ICONINFORMATION); return; }
    swprintf(arguments, _countof(arguments), L"/select,\"%ls\"", item->payload);
    ShellExecuteW(NULL, L"open", L"explorer.exe", arguments, NULL, SW_SHOWNORMAL);
}

static int CollectSelected(BIN_ITEM ***items) {
    int index = -1, count = 0, capacity = 8;
    BIN_ITEM **selected = (BIN_ITEM **)malloc((size_t)capacity * sizeof(*selected));
    if (!selected) return 0;
    while ((index = ListView_GetNextItem(g_list, index, LVNI_SELECTED)) >= 0) {
        LVITEMW row = {0};
        row.mask = LVIF_PARAM; row.iItem = index;
        if (ListView_GetItem(g_list, &row) && row.lParam) {
            if (count == capacity) {
                BIN_ITEM **grown = (BIN_ITEM **)realloc(selected, (size_t)(capacity * 2) * sizeof(*selected));
                if (!grown) break;
                selected = grown; capacity *= 2;
            }
            selected[count++] = (BIN_ITEM *)row.lParam;
        }
    }
    if (!count) { free(selected); selected = NULL; }
    *items = selected;
    return count;
}

static BOOL RestoreItem(BIN_ITEM *item) {
    wchar_t info_path[PATH_CAP], parent[PATH_CAP], *name;
    int create_result;
    if (!item->has_metadata || !item->original[0] || GetFileAttributesW(item->original) != INVALID_FILE_ATTRIBUTES) return FALSE;
    wcsncpy(parent, item->original, PATH_CAP - 1); parent[PATH_CAP - 1] = L'\0';
    name = wcsrchr(parent, L'\\');
    if (!name) return FALSE;
    *name = L'\0';
    create_result = SHCreateDirectoryExW(NULL, parent, NULL);
    if (create_result != ERROR_SUCCESS && create_result != ERROR_ALREADY_EXISTS && create_result != ERROR_FILE_EXISTS) return FALSE;
    if (!MoveFileExW(item->payload, item->original, MOVEFILE_WRITE_THROUGH)) return FALSE;
    wcsncpy(info_path, item->payload, PATH_CAP - 1); info_path[PATH_CAP - 1] = L'\0';
    name = wcsrchr(info_path, L'\\');
    if (name && name[1] == L'$' && (name[2] == L'R' || name[2] == L'r')) name[2] = L'I';
    DeleteFileW(info_path);
    return TRUE;
}

static void RestoreSelected(void) {
    BIN_ITEM **items = NULL;
    wchar_t prompt[256], result[256];
    int count = CollectSelected(&items), restored = 0;
    if (!count) { MessageBoxW(NULL, Text(L"Select one or more items in the list.", L"목록에서 하나 이상의 항목을 선택하세요."), APP_NAME, MB_ICONINFORMATION); return; }
    swprintf(prompt, _countof(prompt), Text(L"Restore %d selected item(s) to their original locations? Existing files will not be overwritten.", L"선택한 %d개 항목을 원래 위치로 복구할까요? 기존 파일은 덮어쓰지 않습니다."), count);
    if (MessageBoxW(NULL, prompt, APP_NAME, MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) { free(items); return; }
    for (int i = 0; i < count; i++) if (RestoreItem(items[i])) restored++;
    free(items);
    swprintf(result, _countof(result), Text(L"Restored %d of %d selected item(s). Items without metadata, protected targets, or existing target names were skipped.", L"선택한 %d개 중 %d개를 복구했습니다. 메타데이터가 없거나 보호된 대상 또는 같은 이름이 있는 항목은 건너뛰었습니다."), restored, count);
    MessageBoxW(NULL, result, APP_NAME, restored == count ? MB_ICONINFORMATION : MB_ICONWARNING);
    ScanRecycleBin();
}

static BOOL PickFolder(wchar_t *folder, size_t count) {
    BROWSEINFOW browse = {0};
    PIDLIST_ABSOLUTE id;
    browse.lpszTitle = Text(L"Choose a backup folder", L"백업 폴더를 선택하세요");
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    id = SHBrowseForFolderW(&browse);
    if (!id) return FALSE;
    BOOL ok = SHGetPathFromIDListW(id, folder);
    CoTaskMemFree(id);
    (void)count;
    return ok;
}

static void BackupSelected(void) {
    BIN_ITEM **items = NULL;
    wchar_t folder[PATH_CAP], target[PATH_CAP], result[256];
    int count = CollectSelected(&items), copied = 0;
    if (!count) { MessageBoxW(NULL, Text(L"Select one or more files in the list.", L"목록에서 하나 이상의 파일을 선택하세요."), APP_NAME, MB_ICONINFORMATION); return; }
    if (!PickFolder(folder, _countof(folder))) { free(items); return; }
    for (int i = 0; i < count; i++) {
        const wchar_t *name = items[i]->has_metadata ? LeafName(items[i]->original) : LeafName(items[i]->payload);
        swprintf(target, _countof(target), L"%ls\\%ls", folder, name);
        if (GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES && CopyFileW(items[i]->payload, target, TRUE)) copied++;
    }
    free(items);
    swprintf(result, _countof(result), Text(L"Copied %d of %d selected file(s). Existing names and folders were skipped.", L"선택한 %d개 중 %d개 파일을 백업했습니다. 같은 이름과 폴더 항목은 건너뛰었습니다."), copied, count);
    MessageBoxW(NULL, result, APP_NAME, copied == count ? MB_ICONINFORMATION : MB_ICONWARNING);
}

static void WriteUtf8(FILE *file, const wchar_t *text) {
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char *buffer = (char *)malloc((size_t)bytes);
    if (!buffer) return;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, bytes, NULL, NULL);
    fwrite(buffer, 1, (size_t)bytes - 1, file);
    free(buffer);
}

static void WriteCsvCell(FILE *file, const wchar_t *text) {
    size_t length = wcslen(text), written = 0;
    wchar_t *escaped = (wchar_t *)malloc((length * 2 + 3) * sizeof(wchar_t));
    if (!escaped) return;
    escaped[written++] = L'"';
    for (size_t i = 0; i < length; i++) {
        if (text[i] == L'"') escaped[written++] = L'"';
        escaped[written++] = text[i];
    }
    escaped[written++] = L'"'; escaped[written] = L'\0';
    WriteUtf8(file, escaped); free(escaped);
}

static void ExportCsv(void) {
    OPENFILENAMEW dialog = {0};
    wchar_t path[PATH_CAP] = L"recycle-bin-items.csv";
    FILE *file;
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = path; dialog.nMaxFile = _countof(path);
    dialog.lpstrFilter = L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0";
    dialog.lpstrDefExt = L"csv";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog)) return;
    file = _wfopen(path, L"wb");
    if (!file) { MessageBoxW(NULL, Text(L"The CSV file could not be created.", L"CSV 파일을 만들 수 없습니다."), APP_NAME, MB_ICONERROR); return; }
    fputs("\xEF\xBB\xBF", file);
    WriteCsvCell(file, Text(L"File name", L"파일 이름")); fputc(',', file);
    WriteCsvCell(file, Text(L"Original location", L"원래 위치")); fputc(',', file);
    WriteCsvCell(file, Text(L"Size", L"크기")); fputc(',', file);
    WriteCsvCell(file, Text(L"Deleted", L"삭제 시각")); fputc(',', file);
    WriteCsvCell(file, Text(L"Actual location", L"실제 위치")); fputs("\r\n", file);
    for (size_t i = 0; i < g_item_count; i++) if (MatchesSearch(g_items[i])) {
        wchar_t size[48];
        FormatSize(g_items[i]->size, size, _countof(size));
        WriteCsvCell(file, g_items[i]->has_metadata ? LeafName(g_items[i]->original) : LeafName(g_items[i]->payload)); fputc(',', file);
        WriteCsvCell(file, g_items[i]->has_metadata ? g_items[i]->original : L""); fputc(',', file);
        WriteCsvCell(file, size); fputc(',', file);
        WriteCsvCell(file, g_items[i]->deleted); fputc(',', file);
        WriteCsvCell(file, g_items[i]->payload); fputs("\r\n", file);
    }
    fclose(file);
    MessageBoxW(NULL, Text(L"CSV export completed.", L"CSV 내보내기가 완료되었습니다."), APP_NAME, MB_ICONINFORMATION);
}

static void AddColumn(int index, const wchar_t *title, int width) {
    LVCOLUMNW column = {0};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = (LPWSTR)title;
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(g_list, index, &column);
}

static void SetColumnTitle(int index, const wchar_t *title) {
    LVCOLUMNW column = {0};
    column.mask = LVCF_TEXT;
    column.pszText = (LPWSTR)title;
    ListView_SetColumn(g_list, index, &column);
}

static void ApplyLanguage(HWND window) {
    // Keep the product name stable for releases and open-source distribution.
    SetWindowTextW(window, APP_NAME);
    SetText(g_drive_label, Text(L"Drive", L"드라이브"));
    SetText(g_search_label, Text(L"Search", L"검색"));
    SetText(GetDlgItem(window, ID_REFRESH), Text(L"Refresh", L"새로 고침"));
    SetText(GetDlgItem(window, ID_OPEN_BIN), Text(L"Open Recycle Bin", L"휴지통 폴더 열기"));
    SetText(GetDlgItem(window, ID_RESTORE), Text(L"Restore Original", L"원래 위치 복구"));
    SetText(GetDlgItem(window, ID_BACKUP), Text(L"Back Up Selected", L"선택 파일 백업"));
    SetText(GetDlgItem(window, ID_EXPORT), Text(L"Export CSV", L"CSV 내보내기"));
    SetText(GetDlgItem(window, ID_OPEN_FILE), Text(L"Open Selected File", L"선택 파일 열기"));
    SetText(GetDlgItem(window, ID_REVEAL), Text(L"Reveal Selected File", L"선택 위치 열기"));
    SetText(GetDlgItem(window, ID_LANGUAGE), g_korean ? L"English" : L"한국어");
    SetColumnTitle(0, Text(L"File name", L"파일 이름"));
    SetColumnTitle(1, Text(L"Original location (before deletion)", L"원래 위치 (삭제 전)"));
    SetColumnTitle(2, Text(L"Size", L"크기"));
    SetColumnTitle(3, Text(L"Deleted", L"삭제 시각"));
    SetColumnTitle(4, Text(L"Actual location (now)", L"실제 위치 (현재)"));
    RenderItems();
}

static void Layout(HWND window) {
    RECT area;
    int width, height;
    GetClientRect(window, &area);
    width = area.right;
    height = area.bottom;
    MoveWindow(g_drive, 82, 12, 80, 26, TRUE);
    MoveWindow(GetDlgItem(window, ID_REFRESH), 170, 12, 80, 26, TRUE);
    MoveWindow(GetDlgItem(window, ID_OPEN_BIN), 258, 12, 145, 26, TRUE);
    MoveWindow(GetDlgItem(window, ID_LANGUAGE), 411, 12, 85, 26, TRUE);
    MoveWindow(g_search, 68, 48, 160, 26, TRUE);
    MoveWindow(GetDlgItem(window, ID_RESTORE), 238, 48, 125, 26, TRUE);
    MoveWindow(GetDlgItem(window, ID_BACKUP), 373, 48, 125, 26, TRUE);
    MoveWindow(GetDlgItem(window, ID_EXPORT), 508, 48, 95, 26, TRUE);
    MoveWindow(GetDlgItem(window, ID_OPEN_FILE), 613, 48, 145, 26, TRUE);
    MoveWindow(GetDlgItem(window, ID_REVEAL), 766, 48, 150, 26, TRUE);
    MoveWindow(g_list, 12, 82, width - 24, height - 130, TRUE);
    MoveWindow(g_status, 12, height - 42, width - 24, 32, TRUE);
}

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        g_drive_label = CreateWindowW(L"STATIC", L"Drive", WS_CHILD | WS_VISIBLE, 12, 15, 64, 22, window, NULL, NULL, NULL);
        g_drive = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 82, 12, 80, 300, window, (HMENU)ID_DRIVE, NULL, NULL);
        DWORD mask = GetLogicalDrives();
        int select_index = 0, item_index = 0;
        for (int bit = 0; bit < 26; bit++) if (mask & (1u << bit)) {
            wchar_t entry[4] = {(wchar_t)(L'A' + bit), L':', L'\\', L'\0'};
            SendMessageW(g_drive, CB_ADDSTRING, 0, (LPARAM)entry);
            if (bit == 5) select_index = item_index;
            item_index++;
        }
        SendMessageW(g_drive, CB_SETCURSEL, select_index, 0);
        CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE, 170, 12, 80, 26, window, (HMENU)ID_REFRESH, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Open Recycle Bin", WS_CHILD | WS_VISIBLE, 258, 12, 145, 26, window, (HMENU)ID_OPEN_BIN, NULL, NULL);
        CreateWindowW(L"BUTTON", L"한국어", WS_CHILD | WS_VISIBLE, 411, 12, 85, 26, window, (HMENU)ID_LANGUAGE, NULL, NULL);
        g_search_label = CreateWindowW(L"STATIC", L"Search", WS_CHILD | WS_VISIBLE, 12, 51, 52, 22, window, NULL, NULL, NULL);
        g_search = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 68, 48, 160, 26, window, (HMENU)ID_SEARCH, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Restore Original", WS_CHILD | WS_VISIBLE, 238, 48, 125, 26, window, (HMENU)ID_RESTORE, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Back Up Selected", WS_CHILD | WS_VISIBLE, 373, 48, 125, 26, window, (HMENU)ID_BACKUP, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Export CSV", WS_CHILD | WS_VISIBLE, 508, 48, 95, 26, window, (HMENU)ID_EXPORT, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Open Selected File", WS_CHILD | WS_VISIBLE, 613, 48, 145, 26, window, (HMENU)ID_OPEN_FILE, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Reveal Selected File", WS_CHILD | WS_VISIBLE, 766, 48, 150, 26, window, (HMENU)ID_REVEAL, NULL, NULL);
        g_list = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                               12, 82, 900, 500, window, (HMENU)ID_LIST, NULL, NULL);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        AddColumn(0, L"File name", 230);
        AddColumn(1, L"Original location (before deletion)", 360);
        AddColumn(2, L"Size", 95);
        AddColumn(3, L"Deleted", 130);
        AddColumn(4, L"Actual location (now)", 380);
        g_status = CreateWindowW(L"STATIC", L"Select a drive or click Refresh.", WS_CHILD | WS_VISIBLE, 12, 590, 900, 32, window, (HMENU)ID_STATUS, NULL, NULL);
        SendMessageW(g_drive_label, WM_SETFONT, (WPARAM)font, TRUE);
        for (HWND control = g_drive; control; control = NULL) SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(g_status, WM_SETFONT, (WPARAM)font, TRUE);
        return 0;
    }
    case WM_SIZE: Layout(window); return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_DRIVE:
            if (HIWORD(wparam) == CBN_SELCHANGE) ScanRecycleBin();
            return 0;
        case ID_SEARCH:
            if (HIWORD(wparam) == EN_CHANGE) RenderItems();
            return 0;
        case ID_LANGUAGE:
            g_korean = !g_korean;
            ApplyLanguage(window);
            ScanRecycleBin();
            return 0;
        case ID_REFRESH: ScanRecycleBin(); return 0;
        case ID_OPEN_BIN: OpenBinFolder(); return 0;
        case ID_RESTORE: RestoreSelected(); return 0;
        case ID_BACKUP: BackupSelected(); return 0;
        case ID_EXPORT: ExportCsv(); return 0;
        case ID_OPEN_FILE: OpenSelectedFile(); return 0;
        case ID_REVEAL: RevealSelectedFile(); return 0;
        }
        break;
    case WM_NOTIFY:
        if (((LPNMHDR)lparam)->idFrom == ID_LIST && ((LPNMHDR)lparam)->code == NM_DBLCLK) OpenSelectedFile();
        if (((LPNMHDR)lparam)->idFrom == ID_LIST && ((LPNMHDR)lparam)->code == LVN_COLUMNCLICK) {
            int column = ((NMLISTVIEW *)lparam)->iSubItem;
            if (column == g_sort_column) g_sort_ascending = !g_sort_ascending;
            else { g_sort_column = column; g_sort_ascending = TRUE; }
            SortAndRender();
        }
        break;
    case WM_DESTROY:
        ClearItems();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_LISTVIEW_CLASSES};
    WNDCLASSW wc = {0};
    HWND window;
    MSG message;
    (void)previous; (void)command_line;
    InitCommonControlsEx(&controls);
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"RecycleBinBrowserNative";
    wc.lpfnWndProc = WindowProc;
    if (!RegisterClassW(&wc)) return 1;
    window = CreateWindowExW(0, wc.lpszClassName, APP_NAME, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1220, 720, NULL, NULL, instance, NULL);
    if (!window) return 1;
    ShowWindow(window, show);
    UpdateWindow(window);
    ScanRecycleBin();
    while (GetMessageW(&message, NULL, 0, 0)) { TranslateMessage(&message); DispatchMessageW(&message); }
    return (int)message.wParam;
}
