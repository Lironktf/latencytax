// A market maker running on the live feed, in shadow, with a page to watch it.
//
// Everything else here is a replay of files. This is the same engine, the same
// reconstruction and the same agent, fed by Hyperliquid's live WebSocket through
// scripts/live_bridge.py, which does the TLS and the handshake and nothing else.
// The lines it writes are the same shape the collectors write, so the C++ side
// cannot tell which it is reading, and Reconstructor::run is written in terms of
// the same two incremental calls this uses.
//
// It does not trade. It holds no key, it opens no connection to the exchange,
// and the socket the bridge opens is a market data socket. The quotes it shows
// are quotes it would have posted, and the fills are what the queue model says
// would have happened to them. That is a simulation standing next to a live
// market, not participation in one.
//
//   python3 scripts/live_bridge.py | ./build/liveshadow --port=8080
//
// then open http://localhost:8080
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/engine.hpp"
#include "ml/features.hpp"
#include "ml/model.hpp"
#include "replay/loader.hpp"
#include "replay/reconstruct.hpp"
#include "sim/agent.hpp"

using namespace ltx;

namespace {

struct Args {
  int port = 8080;
  double latency_ms = 1.0;
  double gamma = 5.0;
  double k = 8.6820;
  double beta = 0.00008052;
  int requote = 3;
  double size_eth = 0.5;
  double max_inv_eth = 25.0;
  double maker_bps = 1.5;
  double taker_bps = 4.5;
  std::string queue_model;
  double gate = 0.40;
  bool quiet = false;
};

void usage() {
  std::printf(
      "liveshadow - the market maker on the live feed, in shadow, with a page\n"
      "\n"
      "usage: scripts/live_bridge.py | liveshadow [options]\n"
      "  --port=N           http port, default 8080\n"
      "  --latency=N        agent reaction latency in ms, default 1\n"
      "  --gamma=N          inventory risk aversion, default 5\n"
      "  --requote=N        ticks of drift tolerated, default 3\n"
      "  --size=N           quote size in ETH, default 0.5\n"
      "  --max-inv=N        inventory limit in ETH, default 25\n"
      "  --maker-bps=N      default 1.5, Hyperliquid tier 0\n"
      "  --queue-model=F    trained queue model, enables the gate\n"
      "  --gate=P           skip a level below this predicted fill probability\n"
      "  --quiet\n"
      "\n"
      "it places no orders and holds no key.\n");
}

// Everything the page shows. Written by the feed thread, read by the http
// thread, under one lock. A request a second against a lock held for the length
// of a struct copy is not a contention problem worth solving cleverly.
struct Shared {
  std::mutex m;
  std::int64_t started_ms = 0;
  std::int64_t last_event_ms = 0;
  std::uint64_t books = 0, trades = 0, lines = 0, parse_errors = 0;
  std::uint64_t windows = 0, recon_mismatch = 0, unexpected_trades = 0;
  double best_bid = 0, best_ask = 0;
  std::vector<std::pair<double, double>> bids, asks;   // price, size
  // Agent.
  double agent_bid = 0, agent_ask = 0;
  double inventory = 0, cash = 0, fees = 0, equity = 0;
  std::uint64_t fills = 0, swept = 0, requotes = 0, gated = 0;
  double notional = 0;
  double mo5 = 0, mo30 = 0;
  std::uint64_t mo_n = 0;
  std::deque<std::pair<std::int64_t, double>> recent_fills;   // ts, signed qty
  double last_fill_px = 0;
  std::int64_t last_fill_ms = 0;
};

Shared g;

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// The page. Self contained, no network fetches, same palette as the figures in
// docs/, and it follows the reader's colour scheme.
const char* kPage = R"HTML(<!doctype html>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>latencytax live shadow</title>
<style>
:root{color-scheme:light dark;
--bg:#fcfcfb;--card:#f4f4f2;--ink:#0b0b0b;--ink2:#52514e;--ink3:#7a7974;--line:#e3e3df;
--bid:#1baf7a;--ask:#e34948;--acc:#2a78d6;--warn:#eda100}
@media (prefers-color-scheme:dark){:root{
--bg:#1a1a19;--card:#232322;--ink:#fff;--ink2:#c3c2b7;--ink3:#8f8e86;--line:#343432;
--bid:#199e70;--ask:#e66767;--acc:#3987e5;--warn:#c98500}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);
font:14px/1.5 ui-sans-serif,-apple-system,"Segoe UI",Roboto,Helvetica,sans-serif}
.wrap{max-width:1040px;margin:0 auto;padding:28px 20px 60px}
h1{font-size:20px;margin:0 0 2px;font-weight:640}
.sub{color:var(--ink3);font-size:12.5px;margin-bottom:6px}
.note{background:var(--card);border:1px solid var(--line);border-radius:8px;
padding:10px 13px;font-size:12.5px;color:var(--ink2);margin:14px 0 20px}
.note b{color:var(--ink)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(168px,1fr));gap:12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px 14px}
.k{font-size:11px;color:var(--ink3);text-transform:uppercase;letter-spacing:.04em}
.v{font-size:19px;font-weight:620;font-variant-numeric:tabular-nums;margin-top:3px}
.v small{font-size:12px;font-weight:400;color:var(--ink3)}
h2{font-size:13px;margin:26px 0 9px;font-weight:620}
table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums;font-size:13px}
th{text-align:right;font-weight:500;color:var(--ink3);font-size:11px;padding:3px 8px;
text-transform:uppercase;letter-spacing:.04em}
th:first-child{text-align:left}
td{text-align:right;padding:3px 8px;border-top:1px solid var(--line)}
td:first-child{text-align:left;color:var(--ink2)}
.bid{color:var(--bid)}.ask{color:var(--ask)}.mine{font-weight:700;color:var(--acc)}
.bar{height:8px;border-radius:3px;background:var(--acc);opacity:.35}
.ok{color:var(--bid)}.bad{color:var(--ask)}
footer{margin-top:30px;color:var(--ink3);font-size:12px}
a{color:var(--acc)}
</style>
<div class="wrap">
<h1>latencytax, live shadow</h1>
<div class="sub" id="sub">connecting</div>
<div class="note"><b>This places no orders.</b> It is the same engine, the same
reconstruction and the same market making agent that run over the archived files,
pointed at Hyperliquid's live feed. The quotes below are quotes it would have
posted; the fills are what the queue model says would have happened to them. No
key exists in this repository and the only socket open to the exchange is a
market data socket.</div>

<div class="grid" id="tiles"></div>

<h2>book, top five each side</h2>
<table id="book"><thead><tr><th>side</th><th>price</th><th>size</th><th>depth</th></tr>
</thead><tbody></tbody></table>

<h2>reconstruction, checked live against every snapshot</h2>
<table id="fid"><tbody></tbody></table>

<footer>refreshes every second &middot;
<a href="https://github.com/Lironktf/latencytax">github.com/Lironktf/latencytax</a>
</footer>
</div>
<script>
const f=(x,d=2)=>x==null?'-':x.toLocaleString(undefined,{minimumFractionDigits:d,maximumFractionDigits:d});
const dur=s=>{s=Math.floor(s);const h=Math.floor(s/3600),m=Math.floor(s%3600/60);
return h?`${h}h ${m}m`:(m?`${m}m ${s%60}s`:`${s}s`)};
function tile(k,v,sub){return `<div class="card"><div class="k">${k}</div>
<div class="v">${v}${sub?` <small>${sub}</small>`:''}</div></div>`}
async function tick(){
 let s; try{ s=await (await fetch('state.json',{cache:'no-store'})).json() }catch(e){ return }
 document.getElementById('sub').textContent =
   `up ${dur(s.uptime_s)} · ${s.books.toLocaleString()} book updates · `+
   `${s.trades.toLocaleString()} prints · last event ${f(s.since_event_s,1)}s ago`;
 const spread = s.best_ask&&s.best_bid ? s.best_ask-s.best_bid : 0;
 const mid = s.best_ask&&s.best_bid ? (s.best_ask+s.best_bid)/2 : 0;
 document.getElementById('tiles').innerHTML =
  tile('mid', f(mid,1), 'USD')+
  tile('spread', f(spread,1), mid?`${f(spread/mid*1e4,3)} bps`:'')+
  tile('my quotes', s.agent_bid&&s.agent_ask?`${f(s.agent_bid,1)} / ${f(s.agent_ask,1)}`:'none','')+
  tile('inventory', f(s.inventory,3), 'ETH')+
  tile('fills', s.fills.toLocaleString(), s.fills?`${f(100*s.swept/s.fills,0)}% swept`:'')+
  tile('shadow p&l', f(s.equity,2), 'USD, net of fees')+
  tile('edge', s.notional>0?f(s.equity/s.notional*1e4,3):'-', 'bps of notional')+
  tile('markout 5s', s.mo_n?f(s.mo5,3):'-', 'bps');
 let rows='';
 for(let i=s.asks.length-1;i>=0;i--){const a=s.asks[i];
  rows+=`<tr><td class="ask">ask${a[0]===s.agent_ask?' <span class="mine">mine</span>':''}</td>
  <td class="ask">${f(a[0],1)}</td><td>${f(a[1],3)}</td>
  <td><div class="bar" style="width:${Math.min(100,a[1]/s.depth_max*100)}%"></div></td></tr>`}
 for(const b of s.bids){
  rows+=`<tr><td class="bid">bid${b[0]===s.agent_bid?' <span class="mine">mine</span>':''}</td>
  <td class="bid">${f(b[0],1)}</td><td>${f(b[1],3)}</td>
  <td><div class="bar" style="width:${Math.min(100,b[1]/s.depth_max*100)}%"></div></td></tr>`}
 document.querySelector('#book tbody').innerHTML=rows;
 const bad=s.recon_mismatch||s.unexpected_trades;
 document.querySelector('#fid tbody').innerHTML=
  `<tr><td>windows closed</td><td>${s.windows.toLocaleString()}</td></tr>
   <tr><td>level positions wrong</td><td class="${bad?'bad':'ok'}">${s.recon_mismatch}</td></tr>
   <tr><td>unexpected trades</td><td class="${bad?'bad':'ok'}">${s.unexpected_trades}</td></tr>
   <tr><td>lines in / parse errors</td><td>${s.lines.toLocaleString()} / ${s.parse_errors}</td></tr>
   <tr><td>requotes / gated</td><td>${s.requotes.toLocaleString()} / ${s.gated.toLocaleString()}</td></tr>`;
}
tick(); setInterval(tick,1000);
</script>
)HTML";

std::string state_json() {
  std::lock_guard<std::mutex> lk(g.m);
  const std::int64_t t = now_ms();
  std::string s = "{";
  auto num = [&](const char* k, double v, bool last = false) {
    char b[96];
    std::snprintf(b, sizeof(b), "\"%s\":%.8g%s", k, v, last ? "" : ",");
    s += b;
  };
  num("uptime_s", g.started_ms ? (t - g.started_ms) / 1000.0 : 0);
  num("since_event_s", g.last_event_ms ? (t - g.last_event_ms) / 1000.0 : 0);
  num("books", static_cast<double>(g.books));
  num("trades", static_cast<double>(g.trades));
  num("lines", static_cast<double>(g.lines));
  num("parse_errors", static_cast<double>(g.parse_errors));
  num("windows", static_cast<double>(g.windows));
  num("recon_mismatch", static_cast<double>(g.recon_mismatch));
  num("unexpected_trades", static_cast<double>(g.unexpected_trades));
  num("best_bid", g.best_bid);
  num("best_ask", g.best_ask);
  num("agent_bid", g.agent_bid);
  num("agent_ask", g.agent_ask);
  num("inventory", g.inventory);
  num("equity", g.equity);
  num("fees", g.fees);
  num("notional", g.notional);
  num("fills", static_cast<double>(g.fills));
  num("swept", static_cast<double>(g.swept));
  num("requotes", static_cast<double>(g.requotes));
  num("gated", static_cast<double>(g.gated));
  num("mo5", g.mo5);
  num("mo30", g.mo30);
  num("mo_n", static_cast<double>(g.mo_n));
  double dmax = 1e-9;
  for (const auto& b : g.bids) dmax = std::max(dmax, b.second);
  for (const auto& a : g.asks) dmax = std::max(dmax, a.second);
  num("depth_max", dmax);
  auto arr = [&](const char* k, const std::vector<std::pair<double, double>>& v, bool last) {
    s += "\"";
    s += k;
    s += "\":[";
    for (std::size_t i = 0; i < v.size(); ++i) {
      char b[64];
      std::snprintf(b, sizeof(b), "%s[%.4f,%.6f]", i ? "," : "", v[i].first, v[i].second);
      s += b;
    }
    s += last ? "]" : "],";
  };
  arr("bids", g.bids, false);
  arr("asks", g.asks, true);
  s += "}";
  return s;
}

void serve(int port, std::atomic<bool>& stop) {
  const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = ::htonl(INADDR_ANY);
  a.sin_port = ::htons(static_cast<std::uint16_t>(port));
  if (::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(ls, 8) != 0) {
    std::fprintf(stderr, "cannot listen on %d\n", port);
    return;
  }
  std::fprintf(stderr, "# watch it at http://localhost:%d\n", port);
  while (!stop.load(std::memory_order_relaxed)) {
    const int fd = ::accept(ls, nullptr, nullptr);
    if (fd < 0) break;
    char req[2048];
    const ssize_t n = ::recv(fd, req, sizeof(req) - 1, 0);
    if (n > 0) {
      req[n] = '\0';
      const bool wants_state = std::strstr(req, "GET /state.json") != nullptr;
      const std::string body = wants_state ? state_json() : std::string(kPage);
      char head[256];
      const int hn = std::snprintf(
          head, sizeof(head),
          "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
          "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
          wants_state ? "application/json" : "text/html; charset=utf-8", body.size());
      ::send(fd, head, static_cast<std::size_t>(hn), MSG_NOSIGNAL);
      std::size_t off = 0;
      while (off < body.size()) {
        const ssize_t w = ::send(fd, body.data() + off, body.size() - off, MSG_NOSIGNAL);
        if (w <= 0) break;
        off += static_cast<std::size_t>(w);
      }
    }
    ::close(fd);
  }
  ::close(ls);
}

// Drives the agent off the same two callbacks the replay uses.
class LiveObserver final : public ReplayObserver {
 public:
  LiveObserver(Agent& ag, ltx::ml::MarketState& ms) : ag_(ag), ms_(ms) {}

  void on_snapshot(const Snapshot& s, const OrderBook& book) override {
    if (!book.has_bid() || !book.has_ask()) return;
    ms_.on_snapshot(s);
    now_ms_ = s.time_ms;
    const Tick bb = book.best_bid(), ba = book.best_ask();
    mids_.emplace_back(s.time_ms, 0.5 * (bb + ba) * 0.1);
    while (mids_.size() > 20000) mids_.pop_front();
    const Tick abt = ag_.agent_bid_tick(), aat = ag_.agent_ask_tick();
    ag_.on_snapshot(s.time_ms * 1000, bb, ba, book.qty_at(bb), book.qty_at(ba),
                    abt == kInvalidTick ? 0 : book.qty_at(abt),
                    aat == kInvalidTick ? 0 : book.qty_at(aat));
    publish(book);
  }

  void on_trade_print(const RawTrade& t, const OrderBook& book) override {
    if (!book.has_bid() || !book.has_ask()) return;
    ms_.on_trade(t);
    now_ms_ = t.time_ms;
    ag_.on_print(t.time_ms * 1000, t.aggressor, t.tick, t.qty,
                 book.qty_at(book.best_bid()), book.qty_at(book.best_ask()));
  }

  std::int64_t now_ms() const { return now_ms_; }
  const ltx::ml::MarketState& state() const { return ms_; }

 private:
  // Markout against the mid at the time each fill matured, using the same
  // definition the experiment uses: the first observation at or after t + h.
  double markout(std::int64_t at_ms, double px, double sign, std::int64_t h) const {
    for (const auto& m : mids_) {
      if (m.first >= at_ms + h) return sign * (m.second - px) / px * 1e4;
    }
    return 0.0;
  }

  void publish(const OrderBook& book) {
    LevelView lv[5];
    std::vector<std::pair<double, double>> bids, asks;
    const std::size_t nb = book.top_levels(Side::Buy, 5, lv);
    for (std::size_t i = 0; i < nb; ++i) {
      bids.emplace_back(lv[i].tick * 0.1, static_cast<double>(lv[i].qty) / kQtyScale);
    }
    const std::size_t na = book.top_levels(Side::Sell, 5, lv);
    for (std::size_t i = 0; i < na; ++i) {
      asks.emplace_back(lv[i].tick * 0.1, static_cast<double>(lv[i].qty) / kQtyScale);
    }

    double mo5 = 0, mo30 = 0;
    std::uint64_t mon = 0;
    for (const Fill& f : ag_.fills()) {
      if (f.taker) continue;
      const std::int64_t t = f.ts_us / 1000;
      const double px = f.tick * 0.1;
      const double sign = f.side == Side::Buy ? 1.0 : -1.0;
      if (mids_.empty() || mids_.back().first < t + 30000) continue;
      mo5 += markout(t, px, sign, 5000);
      mo30 += markout(t, px, sign, 30000);
      ++mon;
    }

    std::lock_guard<std::mutex> lk(g.m);
    g.best_bid = book.best_bid() * 0.1;
    g.best_ask = book.best_ask() * 0.1;
    g.bids = std::move(bids);
    g.asks = std::move(asks);
    const Tick abt = ag_.agent_bid_tick(), aat = ag_.agent_ask_tick();
    g.agent_bid = abt == kInvalidTick ? 0 : abt * 0.1;
    g.agent_ask = aat == kInvalidTick ? 0 : aat * 0.1;
    g.inventory = static_cast<double>(ag_.inventory()) / kQtyScale;
    g.equity = ag_.equity(0.5 * (g.best_bid + g.best_ask));
    g.fees = ag_.fees_paid();
    g.notional = ag_.maker_notional() + ag_.taker_notional();
    g.fills = ag_.maker_fills();
    g.swept = ag_.swept_fills();
    g.requotes = ag_.requotes();
    g.gated = ag_.gated();
    g.mo_n = mon;
    g.mo5 = mon ? mo5 / mon : 0;
    g.mo30 = mon ? mo30 / mon : 0;
  }

  Agent& ag_;
  ltx::ml::MarketState& ms_;
  std::deque<std::pair<std::int64_t, double>> mids_;
  std::int64_t now_ms_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    auto num = [&](const char* k, double& o) {
      if (s.rfind(k, 0) == 0) { o = std::atof(s.c_str() + std::strlen(k)); return true; }
      return false;
    };
    double d = 0;
    if (s == "--help" || s == "-h") { usage(); return 0; }
    else if (num("--port=", d)) a.port = static_cast<int>(d);
    else if (num("--latency=", a.latency_ms)) {}
    else if (num("--gamma=", a.gamma)) {}
    else if (num("--k=", a.k)) {}
    else if (num("--beta=", a.beta)) {}
    else if (num("--requote=", d)) a.requote = static_cast<int>(d);
    else if (num("--size=", a.size_eth)) {}
    else if (num("--max-inv=", a.max_inv_eth)) {}
    else if (num("--maker-bps=", a.maker_bps)) {}
    else if (num("--taker-bps=", a.taker_bps)) {}
    else if (num("--gate=", a.gate)) {}
    else if (s.rfind("--queue-model=", 0) == 0) a.queue_model = s.substr(14);
    else if (s == "--quiet") a.quiet = true;
    else { std::fprintf(stderr, "unknown argument %s\n", s.c_str()); usage(); return 1; }
  }

  AgentConfig ac;
  ac.gamma = a.gamma;
  ac.k = a.k;
  ac.beta = a.beta;
  ac.requote_ticks = a.requote;
  ac.latency_us = static_cast<std::int64_t>(a.latency_ms * 1000.0 + 0.5);
  ac.quote_size = static_cast<Qty>(a.size_eth * kQtyScale + 0.5);
  ac.max_inventory = static_cast<Qty>(a.max_inv_eth * kQtyScale + 0.5);
  ac.maker_bps = a.maker_bps;
  ac.taker_bps = a.taker_bps;
  Agent agent(ac);

  ltx::ml::QueueModel model;
  ltx::ml::MarketState ms;
  if (!a.queue_model.empty() && model.load(a.queue_model)) {
    std::fprintf(stderr, "# queue model loaded, gate %.2f\n", a.gate);
  }

  BookConfig cfg;
  cfg.min_tick = 1;
  cfg.max_tick = 262144;
  cfg.max_orders = 1u << 16;
  cfg.id_map_capacity = 1u << 17;
  NullSink sink;
  MatchingEngine eng(cfg, &sink);
  Reconstructor rec(eng);
  LiveObserver obs(agent, ms);
  rec.set_observer(&obs);

  if (model.loaded()) {
    agent.set_gate([&](Side side, Tick tick, Qty level_size) {
      if (level_size <= 0 || !ms.ready()) return true;
      ltx::ml::Query q;
      q.side = side;
      q.tick = tick;
      q.level_eth = static_cast<double>(level_size) / kQtyScale;
      q.orders = 1;
      q.q_eth = q.level_eth;
      const Snapshot& sn = ms.snapshot();
      const Tick touch = side == Side::Buy ? sn.bids[0].tick : sn.asks[0].tick;
      q.offset_ticks = static_cast<int>(side == Side::Buy ? touch - tick : tick - touch);
      if (q.offset_ticks < 0) q.offset_ticks = 0;
      return model.predict(ms, q, obs.now_ms()) >= static_cast<float>(a.gate);
    });
  }

  std::atomic<bool> stop{false};
  std::thread http([&] { serve(a.port, stop); });

  {
    std::lock_guard<std::mutex> lk(g.m);
    g.started_ms = now_ms();
  }

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.size() < 3) continue;
    {
      std::lock_guard<std::mutex> lk(g.m);
      ++g.lines;
      g.last_event_ms = now_ms();
    }
    const char tag = line[0];
    const std::string_view payload(line.data() + 2, line.size() - 2);
    if (tag == 'B') {
      Snapshot s;
      if (!parse_snapshot(payload, 1, s)) {
        std::lock_guard<std::mutex> lk(g.m);
        ++g.parse_errors;
        continue;
      }
      rec.push_snapshot(s);
      const ReplayStats& st = rec.stats();
      std::lock_guard<std::mutex> lk(g.m);
      ++g.books;
      g.windows = st.windows;
      g.recon_mismatch = st.total.recon_level_mismatch;
      g.unexpected_trades = st.total.recon_unexpected_trades;
    } else if (tag == 'T') {
      RawTrade t;
      if (!parse_trade(payload, 1, t)) {
        std::lock_guard<std::mutex> lk(g.m);
        ++g.parse_errors;
        continue;
      }
      rec.push_trade(t);
      std::lock_guard<std::mutex> lk(g.m);
      ++g.trades;
    } else if (tag == '#') {
      continue;
    }
  }

  stop.store(true);
  // Nudge the accept loop so the thread can see the flag.
  {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    sa.sin_port = ::htons(static_cast<std::uint16_t>(a.port));
    ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    ::close(fd);
  }
  http.join();
  return 0;
}
