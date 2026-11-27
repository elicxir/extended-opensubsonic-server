"""Tests for audio_match on synthetic chroma (no audio files needed).

  python3 -m unittest test_audio_match      (needs numpy; numba makes it fast)
"""
import unittest

import numpy as np

import audio_match as am


def melody(rng, frames):
    """Chroma of a random tune: one main pitch class per frame plus a little harmony."""
    c = np.full((12, frames), 0.05)
    pitch = rng.integers(12)
    for t in range(frames):
        if t % 4 == 0:
            pitch = (pitch + rng.choice([-2, -1, 1, 2, 5, 7])) % 12
        c[pitch, t] += 1.0
        c[(pitch + 4) % 12, t] += 0.4
        c[(pitch + 7) % 12, t] += 0.3
    return c / np.linalg.norm(c, axis=0, keepdims=True)


def arrange(rng, c, shift, stretch):
    """A different version: another key, another tempo, some noise, an intro of its own."""
    idx = np.clip(np.round(np.arange(0, c.shape[1], 1 / stretch)).astype(int), 0, c.shape[1] - 1)
    out = np.roll(c[:, idx], shift, axis=0) + rng.random((12, len(idx))) * 0.15
    out = np.hstack([melody(rng, 30), out])
    return out / np.linalg.norm(out, axis=0, keepdims=True)


class Similarity(unittest.TestCase):
    def test_arrangement_beats_unrelated_tracks(self):
        rng = np.random.default_rng(1)
        original = melody(rng, 240)
        version = arrange(rng, original, shift=3, stretch=1.2)
        others = [melody(rng, 240) for _ in range(12)]
        songs = [{"id": "orig"}] + [{"id": f"o{i}"} for i in range(len(others))]
        data = {"orig": original, "query": version, **{f"o{i}": o for i, o in enumerate(others)}}
        scored, z = am.rank({"id": "query"}, songs, lambda s: data[s["id"]])
        self.assertEqual(scored[0][1]["id"], "orig")
        self.assertGreater(z, 8)

    def test_too_short_is_zero(self):
        self.assertEqual(am.similarity(np.ones((12, 2)), np.ones((12, 50))), 0.0)


class Proposals(unittest.TestCase):
    def song(self, id, title, composition=None, arrangers=()):
        return {"id": id, "title": title, "albumName": "Album " + id, "compositionId": composition,
                "contributors": [{"role": "composer", "artist": {"name": "Hamauzu"}}] +
                                [{"role": "arranger", "artist": {"name": a}} for a in arrangers]}

    def test_new_composition_takes_the_original_along(self):
        orig = self.song("b", "Tobel")
        q = self.song("a", '"α" 2', arrangers=["Hamauzu"])
        p = am.proposal_for(orig, [q], "Piano Pieces", 9.0, {"a": ([(38.0, orig)], 9.0)}, 8.0)
        self.assertEqual(p["score"], 0.9)
        self.assertEqual(p["proposal"]["composition"], {"name": "Tobel", "composers": ["Hamauzu"]})
        self.assertEqual(p["proposal"]["originalTracks"], ["b"])
        self.assertEqual(p["proposal"]["arrangement"], {"name": "Piano Pieces", "arrangers": ["Hamauzu"]})
        self.assertEqual(p["evidence"]["matches"][0]["top"][0]["title"], "Tobel")

    def test_existing_composition_is_used_by_id(self):
        orig = self.song("b", "Tobel", composition="C1")
        q = self.song("a", '"α" 2')
        p = am.proposal_for(orig, [q], "Piano Pieces", 5.0, {"a": ([(20.0, orig)], 5.0)}, 8.0)
        self.assertEqual(p["score"], 0.6)
        self.assertEqual(p["proposal"]["composition"], {"id": "C1"})
        self.assertNotIn("originalTracks", p["proposal"])
        self.assertEqual(p["proposal"]["arrangement"], {"name": "Piano Pieces"})


if __name__ == "__main__":
    unittest.main()
