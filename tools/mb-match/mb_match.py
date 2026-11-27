#!/usr/bin/env python3
"""Experimental: find compositions and arrangements in an eos library from MusicBrainz.

For each physical album in eos, find the MusicBrainz release, pair tracks with its
recordings (by disc/track position, checked by duration), read the works the
recordings perform and the work-to-work "arrangement" relationships, and propose to
eos (POST /ext/suggestions) that tracks performing related works belong to one
composition: the original work's tracks to its original arrangement, the arranged
works' tracks to arrangements named after those works. Optionally the parent work
("parts") becomes a work (作品) in eos.

With --aliases, tracks left over (no release, or no work on the recording) are also
looked up by title: tracks with the same title (notes removed) and composer form a
group, each group's title is searched among MusicBrainz work names and aliases, and a
work is accepted only when one of its composers matches the group's composer tag.
Groups that land on the same work become one composition, so titles in different
languages ("Bloody Tears" / 「血の涙」) are joined when MusicBrainz lists one as an
alias of the other.

Only the standard library is used. MusicBrainz is asked at most once per second.
The server and API key come from --server / --key or EOS_SERVER / EOS_API_KEY.

  mb_match.py [--server URL] [--key eos_...] [--album TEXT ...] [--dry-run]
              [--aliases [--alias-title TEXT ...]]
"""
import argparse
import difflib
import json
import os
import re
import sys
import time
import unicodedata
import urllib.parse
import urllib.request

MB = "https://musicbrainz.org/ws/2"
USER_AGENT = "eos-mb-match/0.1 (experimental)"
_last_request = 0.0


def mb_get(path, **params):
    global _last_request
    wait = 1.1 - (time.time() - _last_request)
    if wait > 0:
        time.sleep(wait)
    params["fmt"] = "json"
    url = f"{MB}/{path}?{urllib.parse.urlencode(params)}"
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    attempts = 8
    for attempt in range(attempts):
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                _last_request = time.time()
                return json.load(r)
        except urllib.error.HTTPError as e:
            _last_request = time.time()
            # 503: rate limited or briefly unavailable. Back off 2, 4, 8 ... s (at most a
            # minute, never less than Retry-After) instead of losing the whole run: a
            # throttled client keeps getting "Retry-After: 1" if it obeys it to the letter.
            if e.code in (502, 503, 504) and attempt < attempts - 1:
                retry_after = e.headers.get("Retry-After", "")
                delay = max(int(retry_after) if retry_after.isdigit() else 0, min(60, 2 ** (attempt + 1)))
                print(f"  MusicBrainz answered {e.code}; trying again in {delay} s", file=sys.stderr, flush=True)
                time.sleep(delay)
                continue
            raise
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:  # no answer at all
            _last_request = time.time()
            if attempt < attempts - 1:
                delay = min(60, 2 ** (attempt + 1))
                print(f"  MusicBrainz did not answer ({e}); trying again in {delay} s", file=sys.stderr, flush=True)
                time.sleep(delay)
                continue
            raise


class Eos:
    def __init__(self, server, key):
        self.server, self.key = server.rstrip("/"), key

    def call(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.server + path, data=data, method=method,
                                     headers={"X-Api-Key": self.key, "Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=60) as r:
            text = r.read()
            return json.loads(text) if text else None


def norm(s):
    s = unicodedata.normalize("NFKC", s or "").casefold()
    s = "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in s)
    return "".join(c for c in s if unicodedata.category(c)[0] not in "PSZC")


def lucene(s):
    return re.sub(r'([+\-!(){}\[\]^"~*?:\\/]|&&|\|\|)', r"\\\1", s)


def find_release(album, songs):
    """The MusicBrainz release for an eos album: same number of tracks and a close title.

    First the exact title as a phrase; when that finds nothing with the right number of
    tracks, the title's words (spelling and spacing differ: "SF 2" / "SF2")."""
    want = len(songs)
    words = " ".join(lucene(w) for w in re.split(r"\s+", album["name"]) if w.strip())
    best = None
    for query in (f'release:"{lucene(album["name"])}"', f"release:({words})"):
        res = mb_get("release/", query=query, limit=25)
        for rel in res.get("releases", []):
            count = rel.get("track-count") or sum(m.get("track-count", 0) for m in rel.get("media", []))
            similarity = difflib.SequenceMatcher(None, norm(rel["title"]), norm(album["name"])).ratio()
            if similarity < 0.8:
                continue
            score = (count == want, round(similarity, 2), rel.get("status") == "Official", rel.get("score", 0))
            if best is None or score > best[0]:
                best = (score, rel)
        if best and best[0][0]:
            return best[1], None
    return None, f"no release with {want} tracks (closest: {best[1]['title'] if best else '-'})"


def pair_tracks(songs, release):
    """eos song id -> MusicBrainz track (by disc and track number, duration within 5 s)."""
    by_pos = {}
    for m in release.get("media", []):
        for t in m.get("tracks", []):
            by_pos[(m.get("position", 1), t.get("position"))] = t
    pairs, mismatched = {}, 0
    for s in songs:
        t = by_pos.get((s.get("discNumber", 1), s.get("track")))
        if not t:
            continue
        mb_len = (t.get("length") or t["recording"].get("length") or 0) / 1000
        if mb_len and s.get("duration") and abs(mb_len - s["duration"]) > 5:
            mismatched += 1
            continue
        pairs[s["id"]] = t
    return pairs, mismatched


def works_of(track):
    rec = track["recording"]
    return [r["work"] for r in rec.get("relations", []) if r.get("type") == "performance" and "work" in r]


def people(work, role):
    return [r["artist"]["name"] for r in work.get("relations", []) if r.get("type") == role and "artist" in r]


# Notes removed before comparing titles; the same idea as the server's rules
# (server/suggest/rules.cpp): derivative suffixes, then trailing bracketed notes.
DERIVATIVE = [re.compile(p, re.I) for p in (
    r"\(?\s*TV\s*Size\s*\)?", r"\(?\s*(TV|Short)\s*(Ver\.?|Version|Edit)\s*\)?",
    r"\(?\s*Instrumental\s*\)?", r"\(?\s*Off\s*Vocal\s*\)?", r"\(?\s*Karaoke\s*\)?",
    r"\s*[(\[][^()\[\]]*\b(Remix(ed)?|RMX)\b[^()\[\]]*[)\]]", r"\s+(Remix|RMX)(\s*\.?\s*\d+)?\s*$")]
TRAILING = re.compile(r"\s*(\([^()]*\)|\[[^\[\]]*\]|（[^（）]*）|【[^【】]*】|～[^～]*～|〜[^〜]*〜|[-－][^-－]*[-－])\s*$")


def strip_notes(title):
    for p in DERIVATIVE:
        title = p.sub(" ", title)
    for _ in range(4):
        nxt = TRAILING.sub("", title)
        if nxt == title or not nxt.strip():
            break
        title = nxt
    return title.strip()


def composers_of(song):
    return {norm(c["artist"]["name"]) for c in song.get("contributors", []) if c.get("role") == "composer"}


def works_by_title(title, composers):
    """MusicBrainz works whose name or alias is this title (notes removed) and that share
    a composer with the tag, as [(rank, work)]; empty when there is no composer to check."""
    if not composers:
        return []
    want = norm(title)
    res = mb_get("work/", query=f'work:"{lucene(title)}" OR alias:"{lucene(title)}"', limit=25)
    found = []
    for w in res.get("works", []):
        names = [w["title"]] + [a["name"] for a in w.get("aliases", [])]
        exact = any(norm(n) == want for n in names)
        if not exact and not any(norm(strip_notes(n)) == want for n in names):
            continue
        if not composers & {norm(c) for c in people(w, "composer")}:
            continue
        found.append(((exact, w.get("score", 0), -len(w["title"])), w))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default=os.environ.get("EOS_SERVER", "http://127.0.0.1:4540"))
    ap.add_argument("--key", default=os.environ.get("EOS_API_KEY"))
    ap.add_argument("--album", action="append", default=[], help="only albums whose name contains this")
    ap.add_argument("--dry-run", action="store_true", help="print the proposals, do not send them")
    ap.add_argument("--titles", action="store_true", help="also propose works (作品) from MusicBrainz 'parts'")
    ap.add_argument("--min-tracks", type=int, default=2, help="smallest group of library tracks to propose")
    ap.add_argument("--aliases", action="store_true",
                    help="also look up left-over tracks by title among work names and aliases")
    ap.add_argument("--alias-title", action="append", default=[],
                    help="with --aliases, only titles that contain this (one MusicBrainz query per title)")
    args = ap.parse_args()
    if not args.key:
        sys.exit("an API key is needed: --key or EOS_API_KEY")
    eos = Eos(args.server, args.key)

    albums = eos.call("GET", "/ext/albums?kind=physical&limit=500")["items"]
    if args.album:
        albums = [a for a in albums if any(x in a["name"] for x in args.album)]

    work_info = {}       # work id -> {title, composers, arrangers, original_of: set, arranged_from: set, parent}
    track_work = {}      # eos track id -> work id
    track_label = {}
    releases_used = {}
    stats = {"albums": len(albums), "albums_matched": 0, "tracks": 0, "tracks_paired": 0, "tracks_with_work": 0,
             "duration_mismatch": 0}

    all_songs = []
    for album in albums:
        songs = eos.call("GET", f"/ext/albums/{album['id']}")["song"]
        stats["tracks"] += len(songs)
        all_songs += [dict(s, albumName=album["name"]) for s in songs]
        found, why = find_release(album, songs)
        if not found:
            print(f"  ✗ {album['name']}: {why}")
            continue
        release = mb_get(f"release/{found['id']}",
                         inc="recordings+recording-level-rels+work-rels+work-level-rels+artist-rels")
        pairs, mismatched = pair_tracks(songs, release)
        stats["albums_matched"] += 1
        stats["tracks_paired"] += len(pairs)
        stats["duration_mismatch"] += mismatched
        releases_used[album["name"]] = found["id"]
        with_work = 0
        for s in songs:
            t = pairs.get(s["id"])
            if not t:
                continue
            ws = works_of(t)
            if not ws:
                continue
            w = ws[0]
            with_work += 1
            track_work[s["id"]] = w["id"]
            track_label[s["id"]] = f'{s["title"]} ({album["name"]})'
            info = work_info.setdefault(w["id"], {"title": w["title"], "composers": [], "arrangers": [],
                                                  "originals": set(), "arrangements": set(), "parent": None})
            info["composers"] = people(w, "composer") or info["composers"]
            info["arrangers"] = people(w, "arranger") or info["arrangers"]
            for r in w.get("relations", []):
                other = r.get("work")
                if not other:
                    continue
                work_info.setdefault(other["id"], {"title": other["title"], "composers": [], "arrangers": [],
                                                   "originals": set(), "arrangements": set(), "parent": None})
                if r.get("type") == "arrangement":
                    # backward: this work is an arrangement of `other`; forward: `other` arranges this one
                    if r.get("direction") == "backward":
                        info["originals"].add(other["id"])
                        work_info[other["id"]]["arrangements"].add(w["id"])
                    else:
                        info["arrangements"].add(other["id"])
                        work_info[other["id"]]["originals"].add(w["id"])
                elif r.get("type") == "parts" and r.get("direction") == "backward":
                    info["parent"] = (other["id"], other["title"])
        stats["tracks_with_work"] += with_work
        print(f"  ✓ {album['name']}: release {found['id']}, {len(pairs)}/{len(songs)} tracks paired, "
              f"{with_work} with a work" + (f", {mismatched} length mismatches" if mismatched else ""))

    # Group works connected by "arrangement" into one composition, rooted at the original.
    parent = {}

    def find(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    # Only follow "arrangement of" from works that have library tracks to their direct
    # original. Forward links and works outside the library are not followed: popular
    # pieces (Touhou, say) have hundreds of fan arrangements, and medley-style works
    # that arrange several originals would chain unrelated pieces together.
    library_works = set(track_work.values())
    medley_like = 0
    for wid in work_info:
        find(wid)
    for wid in library_works:
        originals = work_info[wid]["originals"]
        if len(originals) == 1:
            parent[find(next(iter(originals)))] = find(wid)
        elif len(originals) > 1:
            medley_like += 1
    clusters = {}
    for wid in work_info:
        clusters.setdefault(find(wid), set()).add(wid)

    proposals = []
    arrangement_links = []
    for members in clusters.values():
        tracks = [t for t, w in track_work.items() if w in members]
        if len(tracks) < args.min_tracks:
            continue
        roots = [w for w in members if not (work_info[w]["originals"] & members)] or sorted(members)
        roots.sort(key=lambda w: (w not in library_works, w))  # prefer an original we have
        root = roots[0]
        rinfo = work_info[root]
        composition = {"name": rinfo["title"], "musicBrainzWorkId": root}
        composers = rinfo["composers"] or next((work_info[w]["composers"] for w in members if work_info[w]["composers"]), [])
        if composers:
            composition["composers"] = composers
        works_with_tracks = sorted({track_work[t] for t in tracks})
        if len(works_with_tracks) > 1:
            arrangement_links.append([(work_info[w]["title"], [track_label[t] for t in tracks if track_work[t] == w])
                                      for w in works_with_tracks])
        for w in works_with_tracks:
            group = [t for t in tracks if track_work[t] == w]
            proposal = {"action": "link_arrangement", "composition": composition, "tracks": group}
            if w != root:
                proposal["arrangement"] = {"name": work_info[w]["title"]}
                if work_info[w]["arrangers"]:
                    proposal["arrangement"]["arrangers"] = work_info[w]["arrangers"]
            proposals.append({
                "rule": "musicbrainz_arrangement" if len(works_with_tracks) > 1 else "musicbrainz_work",
                "model": "musicbrainz",
                "score": 0.95,
                "evidence": {"source": "MusicBrainz", "works": [f"https://musicbrainz.org/work/{x}" for x in sorted(members)],
                             "reason": "recordings paired by disc/track position and length; works linked by "
                                       "MusicBrainz 'arrangement' relationships" if len(works_with_tracks) > 1
                             else "recordings of the same MusicBrainz work"},
                "proposal": proposal,
            })
    if args.aliases:
        covered = {t for p in proposals for t in p["proposal"]["tracks"]}
        alias_proposals, alias_links = by_alias(all_songs, covered, args)
        proposals += alias_proposals
    if args.titles:
        by_parent = {}
        for t, w in track_work.items():
            p = work_info[w]["parent"]
            if p:
                by_parent.setdefault(p, []).append(t)
        for (pid, ptitle), tracks in by_parent.items():
            if len(tracks) >= args.min_tracks:
                proposals.append({"rule": "musicbrainz_parts", "model": "musicbrainz", "score": 0.9,
                                  "evidence": {"source": "MusicBrainz", "works": [f"https://musicbrainz.org/work/{pid}"]},
                                  "proposal": {"action": "link_title", "titleName": ptitle, "tracks": tracks}})

    print("\n== coverage")
    for k, v in stats.items():
        print(f"  {k}: {v}")
    print(f"  works: {len(work_info)}, compositions proposed: "
          f"{sum(1 for p in proposals if p['proposal']['action'] == 'link_arrangement' and 'arrangement' not in p['proposal'])}")
    print(f"  works arranging several originals (not merged): {medley_like}")
    print(f"  compositions with arrangements found in the library: {len(arrangement_links)}")
    for link in arrangement_links:
        print("   - " + "  <->  ".join(f"{title} [{len(labels)}]" for title, labels in link))
        for title, labels in link:
            for l in labels:
                print(f"        {title}: {l}")
    if args.aliases:
        print(f"  compositions found by title and alias: {len(alias_links)}")
        for wtitle, groups in alias_links:
            print(f"   - {wtitle}: " + "  <->  ".join(f"{t} [{len(ids)}]" for t, ids in groups))
    print(f"  proposals: {len(proposals)}")

    if args.dry_run or not proposals:
        if args.dry_run:
            print(json.dumps(proposals[:3], ensure_ascii=False, indent=1))
        return
    sent = {"created": 0, "skipped": 0, "errors": 0}
    for i in range(0, len(proposals), 100):
        r = eos.call("POST", "/ext/suggestions", {"suggestions": proposals[i:i + 100]})
        sent["created"] += len(r["created"])
        sent["skipped"] += r["skipped"]
        sent["errors"] += len(r["errors"])
        for e in r["errors"]:
            print("  error:", e, file=sys.stderr)
    print(f"  sent: {sent}")


def by_alias(songs, covered, args):
    """Proposals for tracks not covered yet, found by title among MusicBrainz work aliases."""
    groups = {}  # (normalized title, composers) -> (title, [song])
    for s in songs:
        if s["id"] in covered:
            continue
        title = strip_notes(s["title"])
        composers = composers_of(s)
        if not composers or not norm(title):
            continue
        if args.alias_title and not any(x.casefold() in s["title"].casefold() for x in args.alias_title):
            continue
        key = (norm(title), frozenset(composers))
        groups.setdefault(key, (title, []))[1].append(s)
    candidates = []  # (title, [song], [(rank, work)])
    for (_, composers), (title, members) in sorted(groups.items(), key=lambda kv: kv[1][0]):
        found = works_by_title(title, set(composers))
        if found:
            candidates.append((title, members, found))
    # A title may match several works (duplicates, the same piece entered per game). Pick
    # for each group the work that the most groups match, so that groups in different
    # languages land on the same work; then the best-ranked one.
    votes = {}
    for _, _, found in candidates:
        for wid in {w["id"] for _, w in found}:
            votes[wid] = votes.get(wid, 0) + 1
    by_work = {}  # work id -> (work, [(title, [song])])
    for title, members, found in candidates:
        _, w = max(found, key=lambda rw: (votes[rw[1]["id"]], rw[0]))
        by_work.setdefault(w["id"], (w, []))[1].append((title, members))
    proposals, links = [], []
    for wid, (w, found) in by_work.items():
        tracks = [s["id"] for _, members in found for s in members]
        if len(tracks) < args.min_tracks:
            continue
        titles = sorted({t for t, _ in found})
        links.append((w["title"], [(t, [s["id"] for s in m]) for t, m in found]))
        composition = {"name": w["title"], "musicBrainzWorkId": wid}
        if people(w, "composer"):
            composition["composers"] = people(w, "composer")
        proposals.append({
            "rule": "musicbrainz_alias",
            "model": "musicbrainz",
            "score": 0.85 if len(titles) > 1 else 0.8,
            "evidence": {"source": "MusicBrainz", "works": [f"https://musicbrainz.org/work/{wid}"],
                         "titles": titles,
                         "aliases": [w["title"]] + [a["name"] for a in w.get("aliases", [])],
                         "reason": "track titles (notes removed) match the work's name or aliases, and the "
                                   "composer tag matches the work's composer"},
            "proposal": {"action": "link_arrangement", "composition": composition, "tracks": tracks},
        })
    return proposals, links


if __name__ == "__main__":
    main()
