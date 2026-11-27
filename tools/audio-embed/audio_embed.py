#!/usr/bin/env python3
"""One vector per song from a pretrained audio model, and the songs nearest to a song.

This finds songs that *sound* alike (instruments, mood, tempo). It does not find other
versions of the same tune: a piano version sits next to other piano pieces, not next to
its original. That is what audio-match is for.

How a song becomes a vector:
  1. 10-second windows are read at even steps through the file (at most --windows)
  2. each window goes through CLAP (music checkpoint) and gives a 512-dim vector
  3. the windows are averaged and the result is scaled to length 1
Two songs are compared by the dot product of their vectors (cosine similarity).

Vectors are cached per song in --cache/<model>/<track id>.npy, so a second run only
embeds new songs.

  audio_embed.py embed --db eos.db --music /music --cache /cache
  audio_embed.py near  --db eos.db --cache /cache "Zufall" [--other-albums] [-k 10]
  audio_embed.py check --db eos.db --cache /cache     # how close are known versions?
"""
import argparse
import os
import sqlite3
import sys
import time

import numpy as np

MODEL = "laion/larger_clap_music"
RATE = 48000      # what CLAP expects
WINDOW_S = 10     # CLAP looks at 10 seconds at a time
FEATURE_VERSION = 1


def open_db(path):
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True)


def tracks(db):
    """(id, path, duration_s, title, album, album_id) of every song that is not missing."""
    return db.execute(
        "SELECT t.id, t.path, t.duration_ms / 1000.0, t.title, al.name, al.id FROM track t "
        "JOIN album al ON al.id = t.album_id WHERE t.missing = 0 ORDER BY al.name, t.disc_number, t.track_number"
    ).fetchall()


def cache_dir(args):
    d = os.path.join(args.cache, f"{args.model.replace('/', '--')}-v{FEATURE_VERSION}-w{args.windows}")
    os.makedirs(d, exist_ok=True)
    return d


def read_windows(path, max_windows):
    """Mono 48 kHz windows of WINDOW_S seconds, evenly spread, skipping the very start and end."""
    import soundfile as sf
    import soxr

    with sf.SoundFile(path) as f:
        rate, total = f.samplerate, f.frames
        size = WINDOW_S * rate
        if total <= size:
            starts = [0]
        else:
            n = int(min(max_windows, max(1, total // (2 * size))))
            # centres at 1/(n+1), 2/(n+1), ... of the song
            starts = [int((i + 1) * total / (n + 1) - size / 2) for i in range(n)]
        out = []
        for s in starts:
            f.seek(max(0, min(s, max(0, total - size))))
            x = f.read(size, dtype="float32", always_2d=True).mean(axis=1)
            if rate != RATE:
                x = soxr.resample(x, rate, RATE)
            out.append(x.astype(np.float32))
        return out


def cmd_embed(args):
    import torch
    from transformers import ClapModel, ClapProcessor

    torch.set_num_threads(args.threads)
    out_dir = cache_dir(args)
    todo = [t for t in tracks(open_db(args.db)) if not os.path.exists(os.path.join(out_dir, t[0] + ".npy"))]
    if args.limit:
        todo = todo[: args.limit]
    print(f"{len(todo)} songs to embed -> {out_dir}", flush=True)
    if not todo:
        return
    model = ClapModel.from_pretrained(args.model).eval()
    processor = ClapProcessor.from_pretrained(args.model)

    started, done, failed, seconds = time.time(), 0, 0, 0.0
    for tid, rel, duration, title, _album, _aid in todo:
        try:
            windows = read_windows(os.path.join(args.music, rel), args.windows)
            vectors = []
            for i in range(0, len(windows), args.batch):
                inputs = processor(audios=windows[i : i + args.batch], sampling_rate=RATE, return_tensors="pt")
                with torch.inference_mode():
                    vectors.append(model.get_audio_features(**inputs).numpy())
            v = np.concatenate(vectors).mean(axis=0)
            v /= np.linalg.norm(v) + 1e-12
            tmp = os.path.join(out_dir, tid + ".tmp.npy")
            np.save(tmp, v.astype(np.float32))
            os.replace(tmp, os.path.join(out_dir, tid + ".npy"))
            done += 1
            seconds += duration
        except Exception as e:  # one unreadable file must not stop the run
            failed += 1
            print(f"  failed: {rel}: {e}", file=sys.stderr, flush=True)
        if (done + failed) % 50 == 0:
            el = time.time() - started
            left = el / (done + failed) * (len(todo) - done - failed)
            print(f"  {done + failed}/{len(todo)}  {el / 60:.1f} min, about {left / 60:.0f} min left", flush=True)
    print(f"done: {done} embedded, {failed} failed, {(time.time() - started) / 60:.1f} min "
          f"for {seconds / 3600:.1f} h of music", flush=True)


def load(args):
    """ids, rows (same order) and the matrix of vectors, for the songs that have one."""
    out_dir = cache_dir(args)
    rows, vectors = [], []
    for t in tracks(open_db(args.db)):
        p = os.path.join(out_dir, t[0] + ".npy")
        if os.path.exists(p):
            rows.append(t)
            vectors.append(np.load(p))
    if not rows:
        sys.exit(f"no vectors in {out_dir}: run 'embed' first")
    return rows, np.stack(vectors)


def cmd_near(args):
    rows, m = load(args)
    q = args.query.lower()
    hits = [i for i, r in enumerate(rows) if r[0] == args.query or q in r[3].lower()]
    if not hits:
        sys.exit(f"no song with id or title like {args.query!r}")
    for i in hits[: args.max_queries]:
        print(f"\n{rows[i][3]}  [{rows[i][4]}]  {rows[i][0]}")
        sims = m @ m[i]
        shown = 0
        for j in np.argsort(-sims):
            if j == i or (args.other_albums and rows[j][5] == rows[i][5]):
                continue
            print(f"  {sims[j]:.3f}  {rows[j][3]}  [{rows[j][4]}]")
            shown += 1
            if shown >= args.k:
                break


def cmd_check(args):
    """Known versions of one composition as a yardstick: where do they rank among the neighbours?"""
    rows, m = load(args)
    index = {r[0]: i for i, r in enumerate(rows)}
    db = open_db(args.db)
    groups = {}
    for tid, comp in db.execute(
            "SELECT e.track_id, a.composition_id FROM track_arrangement_eff e JOIN arrangement a ON a.id = e.arrangement_id"):
        if tid in index:
            groups.setdefault(comp, []).append(index[tid])
    sims = m @ m.T
    np.fill_diagonal(sims, -1)
    order = np.argsort(-sims, axis=1)
    rank = np.empty_like(order)
    rank[np.arange(len(rows))[:, None], order] = np.arange(len(rows))[None, :]
    ranks, same_album, cross_album = [], [], []
    for members in groups.values():
        for a in members:
            for b in members:
                if a == b:
                    continue
                ranks.append(rank[a, b] + 1)
                (same_album if rows[a][5] == rows[b][5] else cross_album).append(rank[a, b] + 1)
    for name, r in (("all pairs", ranks), ("same album", same_album), ("other album", cross_album)):
        if r:
            r = np.array(r)
            print(f"{name:12} n={len(r):5}  median rank {int(np.median(r)):4}  in top 10: {np.mean(r <= 10):.0%}  "
                  f"in top 50: {np.mean(r <= 50):.0%}")
    top = order[:, :10]
    albums = np.array([r[5] for r in rows])
    print(f"neighbours from the same album (top 10): {np.mean(albums[top] == albums[:, None]):.0%}")
    upper = sims[np.triu_indices(len(rows), 1)]
    print(f"similarity of all pairs: median {np.median(upper):.3f}, 99th percentile {np.percentile(upper, 99):.3f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", required=True, help="the eos database (opened read-only)")
    ap.add_argument("--cache", default=os.environ.get("EOS_EMBED_CACHE", "/cache/audio-embed"))
    ap.add_argument("--model", default=MODEL)
    ap.add_argument("--windows", type=int, default=6, help="most 10-second windows per song")
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("embed")
    e.add_argument("--music", default=os.environ.get("EOS_MUSIC", "/music"))
    e.add_argument("--threads", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    e.add_argument("--batch", type=int, default=6)
    e.add_argument("--limit", type=int, default=0, help="stop after this many songs (for timing)")
    e.set_defaults(fn=cmd_embed)
    n = sub.add_parser("near")
    n.add_argument("query", help="a track id, or part of a title")
    n.add_argument("-k", type=int, default=10)
    n.add_argument("--other-albums", action="store_true", help="leave out songs of the same album")
    n.add_argument("--max-queries", type=int, default=3)
    n.set_defaults(fn=cmd_near)
    c = sub.add_parser("check")
    c.set_defaults(fn=cmd_check)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
