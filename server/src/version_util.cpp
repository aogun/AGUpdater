#include "version_util.h"
#include <cstdlib>
#include <algorithm>

bool semver_parse(const std::string &version_str, SemVer &out)
{
    if (version_str.empty()) {
        return false;
    }

    const char *s = version_str.c_str();
    char *end = NULL;

    /* Parse MAJOR */
    long major_val = strtol(s, &end, 10);
    if (end == s || *end != '.' || major_val < 0) {
        return false;
    }
    if (end - s > 1 && s[0] == '0') {
        return false;
    }
    s = end + 1;

    /* Parse MINOR */
    long minor_val = strtol(s, &end, 10);
    if (end == s || *end != '.' || minor_val < 0) {
        return false;
    }
    if (end - s > 1 && s[0] == '0') {
        return false;
    }
    s = end + 1;

    /* Parse PATCH */
    long patch_val = strtol(s, &end, 10);
    if (end == s || *end != '\0' || patch_val < 0) {
        return false;
    }
    if (end - s > 1 && s[0] == '0') {
        return false;
    }

    out.major = static_cast<int>(major_val);
    out.minor = static_cast<int>(minor_val);
    out.patch = static_cast<int>(patch_val);
    return true;
}

bool semver_validate(const std::string &version_str)
{
    SemVer tmp;
    return semver_parse(version_str, tmp);
}

int semver_compare(const SemVer &a, const SemVer &b)
{
    if (a.major != b.major) return a.major - b.major;
    if (a.minor != b.minor) return a.minor - b.minor;
    return a.patch - b.patch;
}

int semver_compare_str(const std::string &a, const std::string &b)
{
    SemVer va, vb;
    if (!semver_parse(a, va) || !semver_parse(b, vb)) {
        return 0;
    }
    return semver_compare(va, vb);
}

std::vector<std::string> semver_filter_newer(
    const std::vector<std::string> &versions,
    const std::string &current_version)
{
    SemVer current;
    if (!semver_parse(current_version, current)) {
        return std::vector<std::string>();
    }

    std::vector<std::string> result;
    for (size_t i = 0; i < versions.size(); ++i) {
        SemVer v;
        if (semver_parse(versions[i], v) && semver_compare(v, current) > 0) {
            result.push_back(versions[i]);
        }
    }

    std::sort(result.begin(), result.end(),
        [](const std::string &a, const std::string &b) {
            return semver_compare_str(a, b) < 0;
        });

    return result;
}
