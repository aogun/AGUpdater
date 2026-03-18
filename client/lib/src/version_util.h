#ifndef AG_CLIENT_VERSION_UTIL_H
#define AG_CLIENT_VERSION_UTIL_H

#include <string>

/* Validate semver format */
bool ag_semver_validate(const std::string &version_str);

#endif /* AG_CLIENT_VERSION_UTIL_H */
