#ifndef AG_SERVER_VERSION_UTIL_H
#define AG_SERVER_VERSION_UTIL_H

#include <string>
#include <vector>

struct SemVer {
    int major;
    int minor;
    int patch;
};

/* Parse "MAJOR.MINOR.PATCH" into SemVer. Returns false if invalid. */
bool semver_parse(const std::string &version_str, SemVer &out);

/* Validate version string matches semver format */
bool semver_validate(const std::string &version_str);

/* Compare two SemVer: returns <0 if a<b, 0 if a==b, >0 if a>b */
int semver_compare(const SemVer &a, const SemVer &b);

/* Compare two version strings. Returns <0, 0, >0. Returns 0 if either is invalid. */
int semver_compare_str(const std::string &a, const std::string &b);

/* Filter versions greater than current_version, sorted ascending */
std::vector<std::string> semver_filter_newer(
    const std::vector<std::string> &versions,
    const std::string &current_version);

#endif /* AG_SERVER_VERSION_UTIL_H */
