/**
 * SDD-021: the <base href> rewrite that lets the web UI live underneath a Home
 * Assistant Ingress prefix.
 *
 * Two things are being pinned here, and the second matters more than the first.
 *
 * The first is that the rewrite works: a page served under
 * /api/hassio_ingress/<token>/ has to be told so, or every asset and every API
 * call resolves against Home Assistant's own root.
 *
 * The second is that it is SAFE. X-Ingress-Path is an inbound header and
 * hms-cpap is routinely reachable on a LAN with nothing in front of it, so
 * anybody who can reach the port can send that header. Reflecting it into an
 * HTML attribute unchecked is an injection. Every hostile shape below is a
 * thing an attacker would actually try.
 */

#include <gtest/gtest.h>

#include "web/IngressBase.h"

#include <string>

using namespace hms_cpap::ingress;

namespace {

/// The shape our own index.html has.
std::string page(const std::string& base = "/") {
    return "<!doctype html><html><head><meta charset=\"utf-8\">"
           "<base href=\"" + base + "\">"
           "<title>CpapDash</title></head><body><app-root></app-root></body></html>";
}

std::string hrefIn(const std::string& html) {
    const size_t h = html.find("href=\"");
    if (h == std::string::npos) return "";
    const size_t start = h + 6;
    const size_t end = html.find('"', start);
    return html.substr(start, end - start);
}

const char* kIngress = "/api/hassio_ingress/xY3kQp7Rm2nB8vLd";

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// What Home Assistant actually sends
// ─────────────────────────────────────────────────────────────────────────────

TEST(IngressBase, RewritesTheBaseHrefToThePrefix) {
    std::string html = page();
    ASSERT_TRUE(rewriteBaseHref(html, kIngress));
    EXPECT_EQ(hrefIn(html), std::string(kIngress) + "/");
}

TEST(IngressBase, AlwaysEndsInASlash) {
    // Without the trailing slash a browser treats the last segment as a file
    // name and drops it, so every relative URL would resolve one level too high
    // and the whole rewrite would be worse than useless.
    std::string html = page();
    ASSERT_TRUE(rewriteBaseHref(html, "/api/hassio_ingress/token"));
    EXPECT_EQ(hrefIn(html), "/api/hassio_ingress/token/");

    std::string already = page();
    ASSERT_TRUE(rewriteBaseHref(already, "/api/hassio_ingress/token/"));
    EXPECT_EQ(hrefIn(already), "/api/hassio_ingress/token/")
        << "a prefix that already ends in a slash must not gain a second one";
}

TEST(IngressBase, LeavesTheRestOfTheDocumentAlone) {
    std::string html = page();
    const std::string before = html;
    ASSERT_TRUE(rewriteBaseHref(html, kIngress));

    EXPECT_NE(html.find("<app-root></app-root>"), std::string::npos);
    EXPECT_NE(html.find("<title>CpapDash</title>"), std::string::npos);
    EXPECT_EQ(html.size(), before.size() + std::string(kIngress).size())
        << "the rewrite changed more than the href value";
}

TEST(IngressBase, DoesNothingWhenTheHrefIsAlreadyRight) {
    std::string html = page(std::string(kIngress) + "/");
    EXPECT_FALSE(rewriteBaseHref(html, kIngress))
        << "a re-serve of an already-correct page should report no change";
}

// ─────────────────────────────────────────────────────────────────────────────
// Documents we are not entitled to edit
// ─────────────────────────────────────────────────────────────────────────────

TEST(IngressBase, DoesNotInsertABaseTagWhereThereIsNone) {
    // This runs over every HTML response, some of which we did not author. A
    // page with no <base> is left exactly as it was rather than being edited
    // into a shape its author did not choose.
    std::string html = "<!doctype html><html><head><title>x</title></head><body>hi</body></html>";
    const std::string before = html;
    EXPECT_FALSE(rewriteBaseHref(html, kIngress));
    EXPECT_EQ(html, before);
}

TEST(IngressBase, IgnoresAnHrefThatIsNotInsideTheBaseTag) {
    // The <base> tag is malformed (no href), and a later anchor has one. The
    // rewrite must not reach past the tag it was looking at and corrupt a link.
    std::string html = "<html><head><base></head><body><a href=\"/settings\">go</a></body></html>";
    const std::string before = html;
    EXPECT_FALSE(rewriteBaseHref(html, kIngress));
    EXPECT_EQ(html, before) << "the rewrite escaped the <base> tag and hit an anchor";
}

// ─────────────────────────────────────────────────────────────────────────────
// Hostile header values
// ─────────────────────────────────────────────────────────────────────────────

TEST(IngressBase, RejectsQuotesAndMarkup) {
    // The injection this whole guard exists for: close the attribute, close the
    // tag, open a script.
    for (const std::string& hostile : {
             std::string("/x\"><script>alert(1)</script>"),
             std::string("/x'><script>alert(1)</script>"),
             std::string("/x\">"),
             std::string("/<img src=x onerror=alert(1)>"),
         }) {
        EXPECT_FALSE(isSafePath(hostile)) << hostile;

        std::string html = page();
        const std::string before = html;
        EXPECT_FALSE(rewriteBaseHref(html, hostile)) << hostile;
        EXPECT_EQ(html, before) << "injected: " << hostile;
    }
}

TEST(IngressBase, RejectsTraversalAndProtocolShapes) {
    for (const std::string& bad : {
             std::string("/../../etc/passwd"),
             std::string("//evil.example.com/"),   // protocol-relative: a whole new origin
             std::string("http://evil.example.com/"),
             std::string("javascript:alert(1)"),
             std::string("/a//b"),
         }) {
        EXPECT_FALSE(isSafePath(bad)) << bad;
    }
}

TEST(IngressBase, RejectsWhitespaceControlCharactersAndNonAscii) {
    for (const std::string& bad : {
             std::string("/a b"),
             std::string("/a\tb"),
             std::string("/a\nb"),
             std::string("/a\rb"),
             std::string("/a\0b", 4),
             std::string("/caf\xc3\xa9"),
             std::string("/a\\b"),
         }) {
        EXPECT_FALSE(isSafePath(bad)) << "accepted a hostile path";
    }
}

TEST(IngressBase, RejectsTheDegenerateCases) {
    EXPECT_FALSE(isSafePath(""));
    EXPECT_FALSE(isSafePath("relative/path"));  // must be absolute
    EXPECT_FALSE(isSafePath(std::string("/") + std::string(600, 'a')));  // over the cap
}

TEST(IngressBase, AcceptsWhatIngressActuallySends) {
    EXPECT_TRUE(isSafePath("/"));
    EXPECT_TRUE(isSafePath(kIngress));
    EXPECT_TRUE(isSafePath("/api/hassio_ingress/aBc-123_x.y~z"));
}
