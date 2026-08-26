#pragma once

#include <string>

namespace hms_cpap::ingress {

/**
 * SDD-021: serving the web UI underneath a Home Assistant Ingress prefix.
 *
 * Home Assistant proxies an add-on's UI at /api/hassio_ingress/<token>/ and
 * tells the add-on where it is with an X-Ingress-Path header on every request.
 * Nothing else is rewritten: the browser still resolves /main.js and /api/...
 * against the HOST, which is Home Assistant itself, so an app that assumes it
 * lives at the root loads its page and then 404s every single call it makes.
 *
 * The fix has two halves. This is the server half: give the page a <base href>
 * that points at the prefix, so relative URLs resolve underneath it. The client
 * half is an Angular interceptor that turns the app's root-absolute /api calls
 * into relative ones (frontend/src/app/interceptors/api-base.interceptor.ts).
 *
 * Everything here is a no-op when the header is absent, which is every other
 * way hms-cpap is run: plain Docker, the desktop app, the native service.
 */

/**
 * Is this header value safe to place inside a double-quoted HTML attribute?
 *
 * This matters more than it looks. The value is an inbound header, and
 * hms-cpap is frequently reachable on the LAN without Home Assistant in front
 * of it, so anyone who can reach the port can send this header. Reflecting it
 * into the page unchecked would be an HTML injection with extra steps.
 *
 * Accepted: a non-empty absolute path, at most 512 characters, made only of
 * unreserved URL characters and '/'. That admits every real Ingress path and
 * excludes quotes, angle brackets, whitespace, backslashes, control characters
 * and anything non-ASCII. '..' and '//' are refused as well, since neither can
 * appear in a genuine Ingress prefix and both are traversal shapes.
 */
bool isSafePath(const std::string& path);

/**
 * Point the document's <base href> at [ingress_path].
 *
 * Rewrites the first <base href="..."> in [html] in place, and returns whether
 * anything changed. A document with no <base> tag is left ALONE rather than
 * having one inserted: this rewrites a page we ship and know the shape of, and
 * silently editing arbitrary HTML is how a proxy becomes a liability.
 *
 * The caller is expected to have run [isSafePath] first; this refuses an unsafe
 * path anyway rather than trusting that.
 */
bool rewriteBaseHref(std::string& html, const std::string& ingress_path);

}  // namespace hms_cpap::ingress
