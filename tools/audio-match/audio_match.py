#!/usr/bin/env python3
"""Find which original each arrangement plays by comparing the audio, and propose it to eos.

For each track of the albums given with --album (piano versions, orchestral versions,
remakes ...), the tool compares the music with candidate originals and, when one stands
out, proposes to eos (POST /ext/suggestions) that the track is an arrangement of it.

How the music is compared (the usual cover-song method):
  1. chroma (CENS): the strength of the 12 pitch classes, about 2 frames a second,
     smoothed; it follows the harmony and melody, not the instruments or the loudness
  2. key: the candidate is shifted to the two keys whose overall chroma fits best
  3. cross-recurrence: frame pairs that are mutual nearest neighbours (3-frame windows)
  4. Smith-Waterman local alignment over that matrix: the length of the longest passage
     the two tracks share, allowing tempo changes and cuts
The best candidate is judged by how far it stands out from the others (z score), so
there must be enough candidates (--min-candidates).

Candidates: the tracks of the albums given with --against, or, without it, every track
that shares a composer with the track being matched (other albums only).

Features are cached per track in --cache, so a second run only aligns.

  audio_match.py --album "Piano Pieces" --against "Original Soundtrack" [--dry-run]

The server and API key come from --server / --key or EOS_SERVER / EOS_API_KEY.
"""
import argparse
import concurrent.futures
import json
import os
import sys
import urllib.parse
import urllib.request

import numpy as np

SAMPLE_RATE = 22050
HOP = 2048          # ~10.8 chroma frames a second
SMOOTH = 21         # CENS smoothing window (~2 s)
DOWNSAMPLE = 5      # -> ~2.2 frames a second
EMBED = 3           # frames per window when comparing
KAPPA = 0.1         # nearest-neighbour share for the recurrence matrix
FEATURE_VERSION = 1


class Eos:
    def __init__(self, server, key):
        self.server, self.key = server.rstrip("/"), key

    def call(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.server + path, data=data, method=method,
                                     headers={"X-Api-Key": self.key, "Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=120) as r:
            text = r.read()
            return json.loads(text) if text else None


def library_songs(eos):
    """Every song of every physical album, with its album name."""
    songs = []
    offset = 0
    while True:
        page = eos.call("GET", f"/ext/albums?kind=physical&limit=500&offset={offset}")
        for album in page["items"]:
            for s in eos.call("GET", f"/ext/albums/{urllib.parse.quote(album['id'])}")["song"]:
                songs.append(dict(s, albumName=album["name"]))
        offset += len(page["items"])
        if not page["items"] or offset >= page.get("total", offset):
            return songs


def people(song, role):
    return [c["artist"]["name"] for c in song.get("contributors", []) if c.get("role") == role]


# ---- features ---------------------------------------------------------------

def feature_path(cache, song):
    return os.path.join(cache, f"{song['id']}-{song.get('size', 0)}-v{FEATURE_VERSION}.npy")


def compute_features(args):
    path, dst = args
    import librosa  # imported in the worker: slow to load, not needed for cached runs
    y, sr = librosa.load(path, sr=SAMPLE_RATE, mono=True)
    c = librosa.feature.chroma_cens(y=y, sr=sr, hop_length=HOP, win_len_smooth=SMOOTH)[:, ::DOWNSAMPLE]
    tmp = dst + ".tmp.npy"
    np.save(tmp, c.astype(np.float32))
    os.replace(tmp, dst)
    return dst


def ensure_features(songs, music, cache, workers):
    os.makedirs(cache, exist_ok=True)
    todo = [(os.path.join(music, s["path"]), feature_path(cache, s)) for s in songs
            if not os.path.exists(feature_path(cache, s))]
    missing = [p for p, _ in todo if not os.path.exists(p)]
    if missing:
        sys.exit(f"audio files not found under {music} (e.g. {missing[0]}); set --music to the library folder")
    if todo:
        print(f"  reading {len(todo)} tracks ({len(songs) - len(todo)} cached) ...", flush=True)
        with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as pool:
            for i, _ in enumerate(pool.map(compute_features, todo), 1):
                if i % 25 == 0 or i == len(todo):
                    print(f"    {i}/{len(todo)}", flush=True)


# ---- comparison ---------------------------------------------------------------

def embed(c, m=EMBED):
    n = c.shape[1] - m + 1
    if n < 1:
        return None
    e = np.vstack([c[:, i:i + n] for i in range(m)])
    return e / (np.linalg.norm(e, axis=0, keepdims=True) + 1e-9)


def recurrence(a, b, kappa=KAPPA):
    s = a.T @ b
    ka = max(1, int(kappa * s.shape[1]))
    kb = max(1, int(kappa * s.shape[0]))
    ta = np.partition(s, -ka, axis=1)[:, -ka][:, None]
    tb = np.partition(s, -kb, axis=0)[-kb, :][None, :]
    return ((s >= ta) & (s >= tb)).astype(np.uint8)


def _qmax(R, penalty=1.0, gap=0.7):
    """Smith-Waterman over a binary recurrence matrix: the best local alignment score."""
    n, m = R.shape
    H = np.zeros((n + 1, m + 1))
    best = 0.0
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            d = H[i - 1, j - 1] + (1.0 if R[i - 1, j - 1] else -penalty)
            v = max(0.0, d, H[i - 1, j] - gap, H[i, j - 1] - gap)
            H[i, j] = v
            if v > best:
                best = v
    return best


try:
    from numba import njit
    qmax = njit(cache=True)(_qmax)
except ImportError:  # works without numba, only much slower
    qmax = _qmax


def similarity(a, b):
    """Alignment score of chroma a against chroma b, in the best of b's two closest keys."""
    ea = embed(a)
    if ea is None or b.shape[1] < EMBED:
        return 0.0
    ma, mb = a.mean(axis=1), b.mean(axis=1)
    shifts = np.argsort([-(ma @ np.roll(mb, k)) for k in range(12)])[:2]
    return max(qmax(recurrence(ea, embed(np.roll(b, int(k), axis=0)))) for k in shifts)


def rank(query, candidates, load):
    a = load(query)
    scored = sorted(((similarity(a, load(c)), c) for c in candidates), key=lambda x: -x[0])
    values = np.array([s for s, _ in scored])
    rest = values[1:]
    z = float((values[0] - rest.mean()) / (rest.std() + 1e-9)) if len(rest) > 1 else 0.0
    return scored, z


# ---- proposals ---------------------------------------------------------------

def proposal_for(original, arrangements, album_name, z, scored_by_track, min_z):
    if original.get("compositionId"):
        composition = {"id": original["compositionId"]}
    else:
        composition = {"name": original["title"]}
        if people(original, "composer"):
            composition["composers"] = people(original, "composer")
    arrangement = {"name": album_name}
    arrangers = sorted({n for s in arrangements for n in people(s, "arranger")})
    if arrangers:
        arrangement["arrangers"] = arrangers
    proposal = {"action": "link_arrangement", "composition": composition, "arrangement": arrangement,
                "tracks": [s["id"] for s in arrangements]}
    if not original.get("compositionId"):
        proposal["originalTracks"] = [original["id"]]
    return {
        "rule": "audio_match",
        "model": "chroma-qmax/1",
        "score": 0.9 if z >= min_z else 0.6,
        "evidence": {
            "source": "audio",
            "method": "chroma (CENS) cross-recurrence and Smith-Waterman alignment, key-invariant",
            "original": {"id": original["id"], "title": original["title"], "album": original["albumName"]},
            "matches": [{"id": s["id"], "title": s["title"], "z": round(scored_by_track[s["id"]][1], 1),
                         "top": [{"id": c["id"], "title": c["title"], "album": c["albumName"], "score": round(sc, 1)}
                                 for sc, c in scored_by_track[s["id"]][0][:3]]}
                        for s in arrangements],
        },
        "proposal": proposal,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default=os.environ.get("EOS_SERVER", "http://127.0.0.1:4540"))
    ap.add_argument("--key", default=os.environ.get("EOS_API_KEY"))
    ap.add_argument("--music", default=os.environ.get("EOS_MUSIC", "/music"), help="the library folder (song paths are relative to it)")
    ap.add_argument("--cache", default=os.environ.get("EOS_AUDIO_CACHE", "/data/cache/audio-match"))
    ap.add_argument("--album", action="append", required=True, help="albums to match (name contains this)")
    ap.add_argument("--against", action="append", default=[], help="candidate albums (name contains this)")
    ap.add_argument("--min-z", type=float, default=8.0, help="z score for a confident proposal (score 0.9)")
    ap.add_argument("--low-z", type=float, default=4.0, help="z score for a weak proposal (score 0.6); below, nothing")
    ap.add_argument("--min-candidates", type=int, default=8, help="fewer candidates than this: no proposal")
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--dry-run", action="store_true", help="print the proposals, do not send them")
    args = ap.parse_args()
    if not args.key:
        sys.exit("an API key is needed: --key or EOS_API_KEY")
    eos = Eos(args.server, args.key)

    songs = library_songs(eos)
    queries = [s for s in songs if any(x in s["albumName"] for x in args.album)]
    if not queries:
        sys.exit("no album matches --album")
    query_albums = {s["albumId"] for s in queries}
    if args.against:
        pool = [s for s in songs if any(x in s["albumName"] for x in args.against) and s["albumId"] not in query_albums]
    else:
        pool = [s for s in songs if s["albumId"] not in query_albums]

    def candidates_for(q):
        if args.against:
            return pool
        composers = set(people(q, "composer"))
        return [s for s in pool if composers & set(people(s, "composer"))]

    plan = {q["id"]: candidates_for(q) for q in queries}
    needed = {s["id"]: s for q in queries for s in [q] + plan[q["id"]]}
    print(f"  {len(queries)} tracks to match, {len(needed) - len(queries)} candidates")
    ensure_features(list(needed.values()), args.music, args.cache, args.workers)
    cache = {}

    def load(s):
        if s["id"] not in cache:
            cache[s["id"]] = np.load(feature_path(args.cache, s))
        return cache[s["id"]]

    results = {}
    for q in queries:
        cands = plan[q["id"]]
        if len(cands) < args.min_candidates:
            print(f"  - {q['title'][:40]:<40} only {len(cands)} candidates, skipped")
            continue
        scored, z = rank(q, cands, load)
        results[q["id"]] = (scored, z)
        best = scored[0][1]
        mark = "✓" if z >= args.min_z else ("?" if z >= args.low_z else " ")
        print(f"  {mark} {q['title'][:40]:<40} z={z:5.1f}  {best['title'][:30]} ({best['albumName'][:24]})"
              f"  | next: {scored[1][1]['title'][:24]}", flush=True)

    # Focus: an arrangement album usually draws on one or two source albums. When the
    # confident matches of an album point there, compare its other tracks with those
    # albums only, so a weak match is judged against the right field instead of every
    # track by the same composer.
    if not args.against:
        for album_id in {q["albumId"] for q in queries}:
            members = [q for q in queries if q["albumId"] == album_id and q["id"] in results]
            sources = {}
            for q in members:
                scored, z = results[q["id"]]
                if z >= args.min_z:
                    sources[scored[0][1]["albumId"]] = sources.get(scored[0][1]["albumId"], 0) + 1
            focus = {a for a, n in sources.items() if n >= 2}
            if not focus:
                continue
            names = sorted({s["albumName"] for s in pool if s["albumId"] in focus})
            print(f"  focus for {members[0]['albumName'][:40]}: {', '.join(names)}")
            for q in members:
                if results[q["id"]][1] >= args.min_z:
                    continue
                cands = [s for s in plan[q["id"]] if s["albumId"] in focus]
                if len(cands) < args.min_candidates:
                    continue
                scored, z = rank(q, cands, load)
                results[q["id"]] = (scored, z)
                mark = "✓" if z >= args.min_z else ("?" if z >= args.low_z else " ")
                print(f"  {mark} {q['title'][:40]:<40} z={z:5.1f}  {scored[0][1]['title'][:30]}  (within focus)")

    # One proposal per original and arranging album.
    groups = {}
    for q in queries:
        if q["id"] not in results:
            continue
        scored, z = results[q["id"]]
        if z < args.low_z:
            continue
        original = scored[0][1]
        if original.get("compositionId") and original.get("compositionId") == q.get("compositionId"):
            continue  # already together
        groups.setdefault((original["id"], q["albumId"]), (original, q["albumName"], []))[2].append((q, z))
    proposals = []
    for original, album_name, members in groups.values():
        z = min(z for _, z in members)
        proposals.append(proposal_for(original, [q for q, _ in members], album_name, z, results, args.min_z))

    strong = sum(1 for p in proposals if p["score"] >= 0.9)
    print(f"\n  proposals: {len(proposals)} ({strong} confident, {len(proposals) - strong} weak); "
          f"unmatched: {len(queries) - sum(len(m) for _, _, m in groups.values())}")
    if args.dry_run or not proposals:
        if args.dry_run and proposals:
            print(json.dumps(proposals[0], ensure_ascii=False, indent=1))
        return
    r = eos.call("POST", "/ext/suggestions", {"suggestions": proposals})
    print(f"  sent: created {len(r['created'])}, skipped {r['skipped']}, errors {len(r['errors'])}")
    for e in r["errors"]:
        print("  error:", e, file=sys.stderr)


if __name__ == "__main__":
    main()
