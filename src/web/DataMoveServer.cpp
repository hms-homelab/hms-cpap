// SDD-067 D16, D18: the move mode. See web/DataMoveServer.h.

#include "web/DataMoveServer.h"

#include "utils/DataDirMove.h"
#include "web/IngressBase.h"

#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef BUILD_WITH_WEB
#include <drogon/drogon.h>
#include <trantor/net/EventLoop.h>
#endif

namespace hms_cpap::datamove {

namespace {

// Self-contained: the Angular bundle is not served in this mode, and the
// static directory may be the very thing being moved. Text in en, es, fr, pt
// and hu, the language picked as the dashboard picks it (the stored choice,
// then the browser's). Every path and number is filled in with textContent.
const std::string kPage = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<base href="/">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CpapDash</title>
<style>
  :root { color-scheme: dark; }
  body { margin: 0; min-height: 100vh; background: #121212; color: #e0e0e0;
         font: 15px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
         display: flex; align-items: center; justify-content: center; }
  .card { width: min(560px, calc(100vw - 32px)); box-sizing: border-box; padding: 1.5rem;
          background: #1e1e1e; border: 1px solid #2c2c2c; border-radius: 8px; }
  h1 { margin: 0 0 .5rem; font-size: 1.25rem; font-weight: 600; color: #fff; }
  p { margin: .5rem 0; }
  .lead, .current { color: #b0b0b0; overflow-wrap: anywhere; }
  .bar { height: 10px; margin: 1rem 0 .5rem; background: #2c2c2c; border-radius: 5px; overflow: hidden; }
  .bar > div { height: 100%; width: 0; background: #4fc3f7; transition: width .3s; }
  .counts { font-variant-numeric: tabular-nums; }
  .current { font-size: .85rem; min-height: 1.3em; }
  .end { margin-top: 1rem; }
  .end.failed { color: #ffb3ad; }
  /* The primary button: solid dashboard blue, as everywhere in hms-cpap. */
  a.open { display: inline-block; margin-top: .75rem; padding: .6rem 1.25rem; border-radius: 6px;
           border: 1px solid #1976d2; background: #1976d2; color: #fff; text-decoration: none;
           font-weight: 600; transition: all .2s; }
  a.open:hover { background: #1565c0; border-color: #1565c0; }
  [hidden] { display: none !important; }
</style>
</head>
<body>
<div class="card" role="status" aria-live="polite">
  <h1 id="title"></h1>
  <p class="lead" id="lead"></p>
  <div class="bar"><div id="fill"></div></div>
  <p class="counts" id="counts"></p>
  <p class="current" id="current"></p>
  <div class="end" id="end" hidden>
    <p id="endText"></p>
    <a class="open" id="open" href="./"></a>
  </div>
</div>
<script>
(function () {
  var T = {
    en: { title: "Moving CpapDash's data",
          lead: "From {from} to {to}. Leave CpapDash running until the move is done.",
          counts: "{done} of {total} files, {bdone} of {btotal}",
          current: "Now: {file}",
          done: "The move is complete. CpapDash is starting and takes a few seconds.",
          ended: "The move has ended. CpapDash is starting and takes a few seconds.",
          failed: "The move did not finish: {error}. CpapDash starts anyway and shows a warning on its pages.",
          open: "Open CpapDash" },
    es: { title: "Moviendo los datos de CpapDash",
          lead: "De {from} a {to}. Deje CpapDash en marcha hasta que termine el traslado.",
          counts: "{done} de {total} archivos, {bdone} de {btotal}",
          current: "Ahora: {file}",
          done: "El traslado ha terminado. CpapDash se está iniciando y tarda unos segundos.",
          ended: "El traslado ha finalizado. CpapDash se está iniciando y tarda unos segundos.",
          failed: "El traslado no terminó: {error}. CpapDash se inicia igualmente y muestra un aviso en sus páginas.",
          open: "Abrir CpapDash" },
    fr: { title: "Déplacement des données de CpapDash",
          lead: "De {from} vers {to}. Laissez CpapDash tourner jusqu'à la fin du déplacement.",
          counts: "{done} sur {total} fichiers, {bdone} sur {btotal}",
          current: "En cours : {file}",
          done: "Le déplacement est terminé. CpapDash démarre, cela prend quelques secondes.",
          ended: "Le déplacement a pris fin. CpapDash démarre, cela prend quelques secondes.",
          failed: "Le déplacement n'a pas abouti : {error}. CpapDash démarre quand même et affiche un avertissement sur ses pages.",
          open: "Ouvrir CpapDash" },
    pt: { title: "Movendo os dados do CpapDash",
          lead: "De {from} para {to}. Deixe o CpapDash em execução até a mudança terminar.",
          counts: "{done} de {total} arquivos, {bdone} de {btotal}",
          current: "Agora: {file}",
          done: "A mudança terminou. O CpapDash está iniciando e leva alguns segundos.",
          ended: "A mudança chegou ao fim. O CpapDash está iniciando e leva alguns segundos.",
          failed: "A mudança não terminou: {error}. O CpapDash inicia mesmo assim e mostra um aviso nas suas páginas.",
          open: "Abrir o CpapDash" },
    hu: { title: "A CpapDash adatainak áthelyezése",
          lead: "Innen: {from}, ide: {to}. Hagyja futni a CpapDash-t, amíg az áthelyezés be nem fejeződik.",
          counts: "{done} / {total} fájl, {bdone} / {btotal}",
          current: "Most: {file}",
          done: "Az áthelyezés befejeződött. A CpapDash indul, ez néhány másodpercig tart.",
          ended: "Az áthelyezés véget ért. A CpapDash indul, ez néhány másodpercig tart.",
          failed: "Az áthelyezés nem fejeződött be: {error}. A CpapDash így is elindul, és figyelmeztetést mutat az oldalain.",
          open: "A CpapDash megnyitása" }
  };
  function pick() {
    var v = null;
    try { v = localStorage.getItem("hms_cpap_lang"); } catch (e) {}
    if (v && T[v]) return v;
    var nav = (navigator.language || (navigator.languages || [])[0] || "").toLowerCase().split("-")[0];
    return T[nav] ? nav : "en";
  }
  var lang = pick(), t = T[lang];
  document.documentElement.setAttribute("lang", lang);
  function fill(s, v) { return s.replace(/\{(\w+)\}/g, function (_, k) { return v[k] != null ? String(v[k]) : ""; }); }
  function bytes(b) {
    var u = ["B", "KB", "MB", "GB", "TB"], i = 0;
    while (b >= 1024 && i < u.length - 1) { b /= 1024; i++; }
    return (i ? b.toFixed(1) : String(b)) + " " + u[i];
  }
  var $ = function (id) { return document.getElementById(id); };
  $("title").textContent = t.title;
  $("open").textContent = t.open;

  function show(s) {
    $("lead").textContent = fill(t.lead, { from: s.from, to: s.to });
    $("fill").style.width = s.percent + "%";
    $("counts").textContent = fill(t.counts, { done: s.files_done, total: s.files_total,
                                                bdone: bytes(s.bytes_done), btotal: bytes(s.bytes_total) });
    $("current").textContent = s.current ? fill(t.current, { file: s.current }) : "";
    if (s.finished) end(s.ok ? "done" : "failed", s.error);
  }
  function end(kind, error) {
    $("end").hidden = false;
    $("end").className = "end" + (kind === "failed" ? " failed" : "");
    $("endText").textContent = fill(t[kind], { error: error || "" });
    $("current").textContent = "";
  }

  // Each answer comes when the move has news; the next request goes out as
  // soon as it lands. When the move process ends the request fails, and the
  // page says the move has ended.
  var seq = -1;
  function next() {
    fetch("api/data-move/status?after=" + seq, { cache: "no-store" })
      .then(function (r) { if (!r.ok) throw new Error(r.status); return r.json(); })
      .then(function (s) { seq = s.seq; show(s); if (!s.finished) next(); })
      .catch(function () { end("ended"); });
  }
  next();
})();
</script>
</body>
</html>
)HTML";

}  // namespace

const std::string& progressPage() { return kPage; }

#ifdef BUILD_WITH_WEB

namespace {

struct Waiter {
    std::uint64_t after;
    trantor::EventLoop* loop;
    drogon::AdviceCallback cb;
};

class State {
public:
    // A progress event from the mover thread.
    void update(const Progress& p) {
        std::vector<Waiter> ready;
        std::string body;
        {
            std::lock_guard<std::mutex> lk(mu_);
            latest_ = p;
            ++seq_;
            body = progressJson(latest_, seq_);
            for (auto it = waiters_.begin(); it != waiters_.end();) {
                if (seq_ > it->after) { ready.push_back(std::move(*it)); it = waiters_.erase(it); }
                else ++it;
            }
        }
        for (auto& w : ready) answer(w, body);
    }

    // A status request: answered now if there is a newer state, held otherwise.
    void ask(std::int64_t after, drogon::AdviceCallback&& cb) {
        std::string body;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (after < 0 || static_cast<std::uint64_t>(after) < seq_ || latest_.finished) {
                body = progressJson(latest_, seq_);
            } else {
                waiters_.push_back({static_cast<std::uint64_t>(after),
                                    trantor::EventLoop::getEventLoopOfCurrentThread(), std::move(cb)});
                return;
            }
        }
        cb(json(body));
    }

private:
    static drogon::HttpResponsePtr json(const std::string& body) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->addHeader("Cache-Control", "no-store");
        resp->setBody(body);
        return resp;
    }
    static void answer(Waiter& w, const std::string& body) {
        auto resp = json(body);
        auto cb = std::move(w.cb);
        // Back on the connection's own loop, where the request arrived.
        if (w.loop) w.loop->queueInLoop([cb = std::move(cb), resp]() { cb(resp); });
        else cb(resp);
    }

    std::mutex mu_;
    Progress latest_;
    std::uint64_t seq_ = 0;
    std::vector<Waiter> waiters_;
};

}  // namespace

int runMoveDataMode(const std::string& from, const std::string& to, int port) {
    auto state = std::make_shared<State>();
    std::cout << "[data-move] moving the data directory from " << from << " to " << to
              << "; progress page on port " << port << std::endl;

    Result result;
    std::thread mover;
    int last_pct = -2;

    Options opts;
    opts.from = from;
    opts.to = to;
    opts.on_progress = [state, &last_pct](const Progress& p) {
        // A line per whole percent and the last one: the Log tab follows the
        // move without a line for every small file.
        const int pct = percentOf(p);
        if (p.finished || pct != last_pct) {
            last_pct = pct;
            std::cout << progressLine(p) << std::endl;
        }
        state->update(p);
        return true;
    };

    drogon::app()
        .setLogLevel(trantor::Logger::kWarn)
        .addListener("0.0.0.0", static_cast<uint16_t>(port))
        .setThreadNum(2);

    // The controllers register themselves, so this one advice answers every
    // request before routing and nothing else is served in this mode.
    drogon::app().registerPreRoutingAdvice(
        [state](const drogon::HttpRequestPtr& req, drogon::AdviceCallback&& stop,
                drogon::AdviceChainCallback&&) {
            if (req->path() == "/api/data-move/status") {
                std::int64_t after = -1;
                try { after = std::stoll(req->getParameter("after")); } catch (...) {}
                state->ask(after, std::move(stop));
                return;
            }
            std::string html = progressPage();
            const std::string& prefix = req->getHeader("x-ingress-path");
            if (!prefix.empty() && ingress::isSafePath(prefix)) ingress::rewriteBaseHref(html, prefix);
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_TEXT_HTML);
            resp->addHeader("Cache-Control", "no-store");
            resp->setBody(std::move(html));
            stop(resp);
        });

    // Started once the server is running, so the quit at the end always has
    // a running loop to stop.
    drogon::app().registerBeginningAdvice([&]() {
        mover = std::thread([&]() {
            result = moveData(opts);
            drogon::app().quit();
        });
    });

    drogon::app().run();
    if (mover.joinable()) mover.join();
    return result.ok ? 0 : 1;
}

#else

int runMoveDataMode(const std::string& from, const std::string& to, int) {
    Options opts;
    opts.from = from;
    opts.to = to;
    int last_pct = -2;
    opts.on_progress = [&last_pct](const Progress& p) {
        const int pct = percentOf(p);
        if (p.finished || pct != last_pct) {
            last_pct = pct;
            std::cout << progressLine(p) << std::endl;
        }
        return true;
    };
    return moveData(opts).ok ? 0 : 1;
}

#endif

}  // namespace hms_cpap::datamove
