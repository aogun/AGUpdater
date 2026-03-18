#include "version_util.h"
#include "log.h"
#include <cstdlib>

bool ag_semver_validate(const std::string &version_str)
{
    LOG_TRACE("ag_semver_validate: input=%s", version_str.c_str());

    if (version_str.empty()) return false;

    const char *s = version_str.c_str();
    char *end = NULL;

    /* MAJOR */
    long val = strtol(s, &end, 10);
    if (end == s || *end != '.' || val < 0) return false;
    if (end - s > 1 && s[0] == '0') return false;
    s = end + 1;

    /* MINOR */
    val = strtol(s, &end, 10);
    if (end == s || *end != '.' || val < 0) return false;
    if (end - s > 1 && s[0] == '0') return false;
    s = end + 1;

    /* PATCH */
    val = strtol(s, &end, 10);
    if (end == s || *end != '\0' || val < 0) return false;
    if (end - s > 1 && s[0] == '0') return false;

    LOG_TRACE("ag_semver_validate: %s is valid", version_str.c_str());
    return true;
}
