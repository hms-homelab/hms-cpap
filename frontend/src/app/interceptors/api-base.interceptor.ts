import { HttpInterceptorFn } from '@angular/common/http';

/**
 * SDD-021: make the app's root-absolute API calls relative to <base href>.
 *
 * Home Assistant Ingress serves this UI at /api/hassio_ingress/<token>/ and
 * rewrites nothing in the payload, so a request to '/api/dashboard' would go to
 * Home Assistant's own /api and 404. The server sets <base href> to the Ingress
 * prefix (see include/web/IngressBase.h), and this turns each call into a path
 * underneath it.
 *
 * Doing it here rather than at the 40 call sites is deliberate. Those calls
 * read better as they are, they all want the same prefix, and a single hook
 * cannot be forgotten by the 41st.
 *
 * OUTSIDE INGRESS THIS IS A NO-OP. document.baseURI is <origin>/ for plain
 * Docker, the desktop app and the native service, so the rewritten URL is the
 * one that was passed in. The same build serves every deployment.
 */
export const apiBaseInterceptor: HttpInterceptorFn = (req, next) => {
  // Only root-absolute paths. An already-relative URL is fine as it is, and an
  // absolute http(s) URL is somebody asking for a specific host on purpose.
  if (!req.url.startsWith('/')) {
    return next(req);
  }

  // baseURI already carries a trailing slash (the server guarantees one, and
  // the default '<base href="/">' has one), so drop the leading slash off the
  // request to avoid '//api'.
  const base = document.baseURI;
  const rewritten = base.endsWith('/')
    ? base + req.url.slice(1)
    : base + req.url;

  // Nothing changed outside Ingress, so hand the original request through
  // rather than cloning it for no reason.
  return next(rewritten === req.url ? req : req.clone({ url: rewritten }));
};
