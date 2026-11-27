// ping, getLicense, getOpenSubsonicExtensions, getScanStatus, startScan,
// getMusicFolders, getUser
#include "api/subsonic/router.h"

namespace eos::api {
namespace {

Node scan_status_node(Services& svc) {
  auto s = svc.scanner.status();
  Node n("scanStatus");
  n.set("scanning", s.scanning).set("count", s.count).set("folderCount", s.folder_count);
  if (s.last_scan_ms) n.set("lastScan", iso_time(s.last_scan_ms));
  return n;
}

}  // namespace

void add_system_handlers(HandlerTable& t) {
  t.add("ping", [](Ctx&, Node&) {});

  t.add("getLicense", [](Ctx&, Node& root) { root.child(Node("license").set("valid", true)); });

  t.add("getOpenSubsonicExtensions", [](Ctx&, Node& root) {
    std::vector<Node> exts;
    for (const char* name : {"apiKeyAuthentication", "formPost", "indexBasedQueue"}) {
      Node e("openSubsonicExtensions");
      e.set("name", name).scalars("versions", {Value(int64_t{1})});
      exts.push_back(std::move(e));
    }
    root.list("openSubsonicExtensions", std::move(exts), true);
  });

  t.add("getScanStatus", [](Ctx& c, Node& root) { root.child(scan_status_node(c.svc)); });

  t.add("startScan", [](Ctx& c, Node& root) {
    c.require_admin();
    c.svc.start_scan(c.bool_param("fullScan", false));
    auto n = scan_status_node(c.svc);
    n.set("scanning", true);
    root.child(std::move(n));
  });

  t.add("getMusicFolders", [](Ctx& c, Node& root) {
    std::vector<Node> folders;
    auto st = c.db.prepare("SELECT id, name FROM library ORDER BY id");
    while (st.step()) folders.push_back(Node("musicFolder").set("id", st.int64(0)).set("name", st.text(1)));
    Node n("musicFolders");
    n.list("musicFolder", std::move(folders));
    root.child(std::move(n));
  });

  t.add("getUser", [](Ctx& c, Node& root) {
    auto name = c.param("username").value_or(c.user.username);
    if (name != c.user.username) c.require_admin();
    auto st = c.db.prepare("SELECT username, is_admin FROM user WHERE username = ?");
    st.bind(1, name);
    if (!st.step()) throw SubsonicError(err::not_found, "User not found");
    bool admin = st.int64(1) != 0;
    Node n("user");
    n.set("username", st.text(0))
        .set("scrobblingEnabled", true)
        .set("adminRole", admin)
        .set("settingsRole", true)
        .set("downloadRole", true)
        .set("uploadRole", false)
        .set("playlistRole", true)
        .set("coverArtRole", true)
        .set("commentRole", false)
        .set("podcastRole", false)
        .set("streamRole", true)
        .set("jukeboxRole", false)
        .set("shareRole", false)
        .set("videoConversionRole", false);
    auto lib = c.db.prepare("SELECT id FROM library ORDER BY id");
    std::vector<Value> ids;
    while (lib.step()) ids.emplace_back(lib.int64(0));
    n.scalars("folder", std::move(ids));
    root.child(std::move(n));
  });
}

}  // namespace eos::api
