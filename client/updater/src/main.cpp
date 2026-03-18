/**
 * ag-updater
 * Extracts a zip file and overwrites files in the target directory.
 * Usage: ag-updater <zip_path>
 * Target directory is the directory where ag-updater.exe resides.
 */
#include "version.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define PATH_SEP '\\'
#else
#include <unistd.h>
#include <sys/stat.h>
#include <libgen.h>
#define PATH_SEP '/'
#endif

#include "unzip.h"

static const int MAX_FILENAME = 512;
static const int READ_BUF_SIZE = 65536;
static const int RETRY_COUNT = 3;
static const int RETRY_DELAY_MS = 1000;

static std::string get_exe_dir(const char *argv0)
{
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return std::string(".");
    }
    /* Find last backslash */
    char *last_sep = strrchr(path, '\\');
    if (last_sep != NULL) {
        *last_sep = '\0';
    }
    return std::string(path);
#else
    (void)argv0;
    char path[4096];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len <= 0) {
        return std::string(".");
    }
    path[len] = '\0';
    /* Find last slash */
    char *last_sep = strrchr(path, '/');
    if (last_sep != NULL) {
        *last_sep = '\0';
    }
    return std::string(path);
#endif
}

static std::string get_exe_name()
{
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        const char *last_sep = strrchr(path, '\\');
        if (last_sep != NULL) {
            return std::string(last_sep + 1);
        }
        return std::string(path);
    }
    return std::string("ag-updater.exe");
#else
    char path[4096];
    ssize_t rlen = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (rlen > 0) {
        path[rlen] = '\0';
        const char *last_sep = strrchr(path, '/');
        if (last_sep != NULL) {
            return std::string(last_sep + 1);
        }
        return std::string(path);
    }
    return std::string("ag-updater");
#endif
}

static bool make_dirs(const std::string &path)
{
    std::string dir;
    for (size_t i = 0; i < path.size(); ++i) {
        char c = path[i];
        if (c == '/' || c == '\\') {
            if (!dir.empty()) {
#if defined(_WIN32) && !defined(__MINGW32__)
                _mkdir(dir.c_str());
#else
                mkdir(dir.c_str(), 0755);
#endif
            }
        }
        dir += c;
    }
    if (!dir.empty()) {
#ifdef _WIN32
        _mkdir(dir.c_str());
#else
        mkdir(dir.c_str(), 0755);
#endif
    }
    return true;
}

static bool file_exists(const std::string &path)
{
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
#else
    return access(path.c_str(), F_OK) == 0;
#endif
}

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

/* Normalize path separators: convert '/' to native separator */
static std::string normalize_path(const std::string &path)
{
    std::string result = path;
    for (size_t i = 0; i < result.size(); ++i) {
        if (result[i] == '/' || result[i] == '\\') {
            result[i] = PATH_SEP;
        }
    }
    return result;
}

/* Find the first directory prefix in the zip (the root dir inside the zip) */
static std::string find_first_dir(unzFile zf)
{
    if (unzGoToFirstFile(zf) != UNZ_OK) {
        return std::string();
    }

    do {
        char filename[MAX_FILENAME];
        unz_file_info fi;
        if (unzGetCurrentFileInfo(zf, &fi, filename, sizeof(filename),
                                  NULL, 0, NULL, 0) != UNZ_OK) {
            continue;
        }

        std::string name(filename);
        /* Look for first entry that contains a '/' - the part before it is the root dir */
        size_t slash = name.find('/');
        if (slash != std::string::npos && slash > 0) {
            return name.substr(0, slash + 1); /* Include trailing slash */
        }
    } while (unzGoToNextFile(zf) == UNZ_OK);

    return std::string();
}

static bool extract_file(unzFile zf, const std::string &dest_path)
{
    if (unzOpenCurrentFile(zf) != UNZ_OK) {
        fprintf(stderr, "  Failed to open entry in zip\n");
        return false;
    }

    /* Ensure parent directory exists */
    std::string parent;
    size_t last_sep = dest_path.find_last_of("/\\");
    if (last_sep != std::string::npos) {
        parent = dest_path.substr(0, last_sep);
        make_dirs(parent);
    }

    /* Try to write file with retry for locked files */
    FILE *out = NULL;
    for (int attempt = 0; attempt < RETRY_COUNT; ++attempt) {
        out = fopen(dest_path.c_str(), "wb");
        if (out != NULL) break;
        if (attempt < RETRY_COUNT - 1) {
            fprintf(stderr, "  File locked, retry %d/%d: %s\n",
                    attempt + 1, RETRY_COUNT - 1, dest_path.c_str());
            sleep_ms(RETRY_DELAY_MS);
        }
    }

    if (out == NULL) {
        fprintf(stderr, "  Cannot write: %s\n", dest_path.c_str());
        unzCloseCurrentFile(zf);
        return false;
    }

    char buf[READ_BUF_SIZE];
    int bytes_read;
    bool ok = true;

    while ((bytes_read = unzReadCurrentFile(zf, buf, sizeof(buf))) > 0) {
        if (fwrite(buf, 1, static_cast<size_t>(bytes_read), out) !=
            static_cast<size_t>(bytes_read)) {
            fprintf(stderr, "  Write error: %s\n", dest_path.c_str());
            ok = false;
            break;
        }
    }

    if (bytes_read < 0) {
        fprintf(stderr, "  Read error from zip\n");
        ok = false;
    }

    fclose(out);
    unzCloseCurrentFile(zf);
    return ok;
}

int main(int argc, char *argv[])
{
    fprintf(stdout, "ag-updater v%s\n", APP_VERSION_STRING);

    if (argc < 2) {
        fprintf(stderr, "Usage: ag-updater <zip_path>\n");
        return 1;
    }

    std::string zip_path = argv[1];
    std::string target_dir = get_exe_dir(argv[0]);
    std::string self_name = get_exe_name();

    fprintf(stdout, "ZIP: %s\n", zip_path.c_str());
    fprintf(stdout, "Target: %s\n", target_dir.c_str());

    /* Verify zip file exists */
    if (!file_exists(zip_path)) {
        fprintf(stderr, "Error: ZIP file not found: %s\n", zip_path.c_str());
        return 1;
    }

    /* Open zip file */
    unzFile zf = unzOpen(zip_path.c_str());
    if (zf == NULL) {
        fprintf(stderr, "Error: Cannot open ZIP: %s\n", zip_path.c_str());
        return 1;
    }

    /* Find first directory in zip */
    std::string root_dir = find_first_dir(zf);
    if (root_dir.empty()) {
        fprintf(stderr, "Error: No directory found in ZIP\n");
        unzClose(zf);
        return 1;
    }
    fprintf(stdout, "Root dir in ZIP: %s\n", root_dir.c_str());

    /* Extract all files under root_dir to target_dir */
    int extracted = 0;
    int skipped = 0;
    int errors = 0;

    if (unzGoToFirstFile(zf) != UNZ_OK) {
        fprintf(stderr, "Error: Cannot read ZIP entries\n");
        unzClose(zf);
        return 1;
    }

    do {
        char filename[MAX_FILENAME];
        unz_file_info fi;
        if (unzGetCurrentFileInfo(zf, &fi, filename, sizeof(filename),
                                  NULL, 0, NULL, 0) != UNZ_OK) {
            continue;
        }

        std::string name(filename);

        /* Skip entries not under root_dir */
        if (name.size() <= root_dir.size() ||
            name.compare(0, root_dir.size(), root_dir) != 0) {
            continue;
        }

        /* Get relative path (strip root_dir prefix) */
        std::string rel_path = name.substr(root_dir.size());
        if (rel_path.empty()) {
            continue;
        }

        /* Skip directories (entries ending with '/') */
        if (rel_path[rel_path.size() - 1] == '/') {
            std::string dir_path = target_dir + std::string(1, PATH_SEP) +
                                    normalize_path(rel_path);
            make_dirs(dir_path);
            continue;
        }

        /* Skip self (ag-updater.exe) */
        std::string base_name = rel_path;
        size_t last_sep = rel_path.find_last_of("/\\");
        if (last_sep != std::string::npos) {
            base_name = rel_path.substr(last_sep + 1);
        }
        if (base_name == self_name) {
            fprintf(stdout, "  Skip self: %s\n", rel_path.c_str());
            ++skipped;
            continue;
        }

        /* Build destination path */
        std::string dest = target_dir + std::string(1, PATH_SEP) +
                           normalize_path(rel_path);

        /* Zip slip protection: reject paths containing ".." to prevent
         * directory traversal attacks */
        std::string norm_dest = normalize_path(dest);
        if (norm_dest.find("..") != std::string::npos) {
            fprintf(stderr, "  SKIP (path traversal): %s\n", rel_path.c_str());
            ++skipped;
            continue;
        }

        fprintf(stdout, "  Extract: %s\n", rel_path.c_str());
        if (extract_file(zf, dest)) {
            ++extracted;
        } else {
            ++errors;
        }

    } while (unzGoToNextFile(zf) == UNZ_OK);

    unzClose(zf);

    fprintf(stdout, "Done: %d extracted, %d skipped, %d errors\n",
            extracted, skipped, errors);

    /* Delete temporary zip file */
    if (remove(zip_path.c_str()) == 0) {
        fprintf(stdout, "Deleted: %s\n", zip_path.c_str());
    }

    return errors > 0 ? 1 : 0;
}
