#include "web/IngressBase.h"

#include <algorithm>

namespace hms_cpap::ingress {

namespace {

constexpr size_t kMaxPathLength = 512;

bool isUnreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

}  // namespace

bool isSafePath(const std::string& path) {
    if (path.empty() || path.size() > kMaxPathLength) return false;
    if (path.front() != '/') return false;
    if (path.find("..") != std::string::npos) return false;
    if (path.find("//") != std::string::npos) return false;

    for (const unsigned char c : path) {
        if (c != '/' && !isUnreserved(c)) return false;
    }
    return true;
}

bool rewriteBaseHref(std::string& html, const std::string& ingress_path) {
    if (!isSafePath(ingress_path)) return false;

    // Find the first <base ... href=" and the quote that closes the value.
    const size_t tag = html.find("<base");
    if (tag == std::string::npos) return false;

    const size_t tag_end = html.find('>', tag);
    if (tag_end == std::string::npos) return false;

    const size_t href = html.find("href=\"", tag);
    if (href == std::string::npos || href > tag_end) return false;

    const size_t value_start = href + 6;  // past href="
    const size_t value_end = html.find('"', value_start);
    if (value_end == std::string::npos || value_end > tag_end) return false;

    // A base href must end in a slash. Without one the browser treats the last
    // segment as a file name and drops it, so every relative URL would resolve
    // one level too high and the whole point would be lost.
    std::string replacement = ingress_path;
    if (replacement.back() != '/') replacement.push_back('/');

    if (html.compare(value_start, value_end - value_start, replacement) == 0) {
        return false;  // already correct, nothing to do
    }

    html.replace(value_start, value_end - value_start, replacement);
    return true;
}

}  // namespace hms_cpap::ingress
