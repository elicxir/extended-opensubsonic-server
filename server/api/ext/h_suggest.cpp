// Suggestions: accept / reject / batch, and estimates submitted by tools such as
// eos-estimate (design: 公式情報と推定情報の区別).
#include "api/ext/model.h"
#include "api/ext/suggestions.h"
#include "core/crypto.h"
#include "core/ids.h"

namespace eos::api::ext {
namespace {

using njson = nlohmann::json;

struct Pending {
  std::string id, status;
  njson payload;
};

Pending load(db::Connection& db, const std::string& id) {
  auto st = db.prepare("SELECT id, status, payload FROM suggestion WHERE id = ?");
  st.bind(1, id);
  if (!st.step()) throw not_found("Suggestion not found: " + id);
  return {st.text(0), st.text(1), njson::parse(st.text(2))};
}

// Accepts one suggestion on the write connection. `edits` (an object) is merged into
// the proposal; an edited proposal is written as manual data (source 3).
void accept(db::Connection& w, const std::string& id, const njson& edits, int64_t user) {
  auto s = load(w, id);
  if (s.status != "pending" && s.status != "stale")
    throw conflict("Suggestion is already " + s.status);
  int source = 1;
  if (edits.is_object() && !edits.empty()) {
    s.payload.merge_patch(edits);
    source = 3;
  }
  apply_proposal(w, s.payload, source, id);
  w.run("UPDATE suggestion SET status = 'accepted', payload = ?, decided_at = ?, decided_by = ? WHERE id = ?",
        s.payload.dump(), db::now_ms(), user, id);
}

void reject(db::Connection& w, const std::string& id, int64_t user) {
  auto s = load(w, id);
  if (s.status != "pending" && s.status != "stale") throw conflict("Suggestion is already " + s.status);
  w.run("UPDATE suggestion SET status = 'rejected', decided_at = ?, decided_by = ? WHERE id = ?", db::now_ms(), user,
        id);
}

}  // namespace

void add_suggestion_routes(ExtRouter& r) {
  r.add(
      "POST", "/ext/suggestions/{}/accept",
      [](ExtCtx& c) {
        njson edits = c.req.body.empty() ? njson() : c.body().value("edits", njson());
        c.write([&](db::Connection& w) { accept(w, c.args[0], edits, c.user.id); }, true);
        return ok(suggestion_json(c.db, c.args[0]));
      },
      Access::admin);

  r.add(
      "POST", "/ext/suggestions/{}/reject",
      [](ExtCtx& c) {
        c.write([&](db::Connection& w) { reject(w, c.args[0], c.user.id); });
        return ok(suggestion_json(c.db, c.args[0]));
      },
      Access::admin);

  r.add(
      "POST", "/ext/suggestions/batch",
      [](ExtCtx& c) {
        const auto& b = c.body();
        auto list = [&](const char* key) {
          std::vector<std::string> out;
          if (b.contains(key))
            for (const auto& v : b[key]) out.push_back(v.get<std::string>());
          return out;
        };
        auto acc = list("accept"), rej = list("reject");
        c.write(
            [&](db::Connection& w) {
              for (const auto& id : acc) accept(w, id, njson(), c.user.id);
              for (const auto& id : rej) reject(w, id, c.user.id);
            },
            !acc.empty());
        return ok({{"accepted", acc.size()}, {"rejected", rej.size()}});
      },
      Access::admin);

  // Estimates from outside (method "llm"): {"suggestions": [{rule, proposal, score,
  // model?, evidence?, origin?, originRef?}]}. origin says what the estimate rests on:
  // external (an outside database states the link), audio, name, or llm (the default). With estimate.auto_apply_min_confidence > 0, confident ones
  // are applied at once as source 1 and marked auto_applied.
  r.add(
      "POST", "/ext/suggestions",
      [](ExtCtx& c) {
        const auto& b = c.body();
        if (!b.contains("suggestions") || !b["suggestions"].is_array())
          throw bad_request("suggestions must be an array");
        const double threshold = c.svc.cfg.estimate_auto_apply_min_confidence;
        json new_ids = json::array();
        json errors = json::array();
        int64_t skipped = 0, auto_applied = 0;
        bool reindex = false;
        c.write(
            [&](db::Connection& w) {
              int64_t index = -1;
              for (const auto& s : b["suggestions"]) {
                ++index;
                w.exec("SAVEPOINT item");
                try {
                  if (!s.is_object()) throw bad_request("each suggestion must be an object");
                  auto rule = s.value("rule", std::string("llm"));
                  auto proposal = s.value("proposal", njson());
                  double score = s.value("score", 0.0);
                  if (score < 0 || score > 1) throw bad_request("score must be between 0 and 1");
                  auto origin = s.value("origin", std::string("llm"));
                  if (!valid_origin(origin)) throw bad_request("origin must be external, audio, name or llm");
                  std::optional<std::string> origin_ref;
                  if (s.contains("originRef") && !s["originRef"].is_null()) {
                    if (!s["originRef"].is_string()) throw bad_request("originRef must be a string");
                    origin_ref = s["originRef"].get<std::string>();
                  }
                  validate_proposal(w, proposal);
                  auto digest = crypto::sha256("llm|" + rule + "|" + proposal.dump());
                  auto fingerprint = crypto::hex(digest.data(), 16);
                  if (w.scalar_int("SELECT 1 FROM suggestion WHERE fingerprint = ?", fingerprint)) {
                    ++skipped;  // already known, whatever its status (rejected ones stay rejected)
                    w.exec("RELEASE item");
                    continue;
                  }
                  auto id = ids::random_id();
                  bool apply = threshold > 0 && score >= threshold;
                  w.run(
                      "INSERT INTO suggestion (id, rule, method, model, evidence, fingerprint, payload, score, status, "
                      "created_at, decided_at, origin, origin_ref) VALUES (?, ?, 'llm', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                      id, rule, s.contains("model") ? std::optional(s["model"].get<std::string>()) : std::nullopt,
                      s.contains("evidence") ? std::optional(s["evidence"].dump()) : std::nullopt, fingerprint,
                      proposal.dump(), score, apply ? "auto_applied" : "pending", db::now_ms(),
                      apply ? std::optional(db::now_ms()) : std::nullopt, origin, origin_ref);
                  if (apply) {
                    apply_proposal(w, proposal, 1, id);
                    ++auto_applied;
                    reindex = true;
                  }
                  new_ids.push_back(id);
                  w.exec("RELEASE item");
                } catch (const ExtError& e) {
                  // Undo whatever this item wrote; the others stay.
                  w.exec("ROLLBACK TO item");
                  w.exec("RELEASE item");
                  errors.push_back({{"index", index}, {"code", e.code}, {"message", e.what()}});
                } catch (const db::DbError& e) {
                  w.exec("ROLLBACK TO item");
                  w.exec("RELEASE item");
                  errors.push_back({{"index", index}, {"code", "conflict"}, {"message", e.what()}});
                }
              }
            },
            false);
        if (reindex) c.write([](db::Connection&) {}, true);
        return created(
            json{{"created", new_ids}, {"autoApplied", auto_applied}, {"skipped", skipped}, {"errors", errors}});
      },
      Access::admin);
}

}  // namespace eos::api::ext
