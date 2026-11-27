# 類似度の保存（設計）

音の近さのような「曲どうしの類似度」を DB に持つための設計です。類似グラフ（`/ext/graph` の `include=similar`）の土台になります。

状態（2026-11-28）：**保存と書き込みは実装済み**（マイグレーション 005、`/ext/similarity`、`audio_embed.py store`）。API の約束は [ext-api.md](ext-api.md) の「類似度」にあります。読む側（`/ext/graph` の `include=similar`）も実装済みです。

## 方針

- 類似度は、楽曲や関係のような「事実」とは別に持ちます。候補（`suggestion`）の承認の流れには乗せません。出どころ（`source`）も持ちません。
- 計算はツール（`tools/audio-embed` など）がします。サーバーは、ツールが送った結果を種類ごとに**丸ごと入れ替え**、読むだけです。
- 1 行は、曲の組 1 つ（順序なし）と点数です。ツールが曲ごとの上位 K 件（既定 50）を求め、その和集合を送ります。
- 種類（`kind`）を分けて、同じテーブルに入れます。`audio_embed`（CLAP、音の近さ）と `audio_match`（別バージョンの検出）を想定しています。

## テーブル（マイグレーション 005）

```sql
-- 類似度の 1 回分の計算。種類ごとに 1 行で、入れ替えるたびに書き直す。
CREATE TABLE similarity_set (
  kind TEXT PRIMARY KEY,              -- 'audio_embed', 'audio_match', ...
  model TEXT NOT NULL,                -- 'laion/larger_clap_music' など
  params TEXT NOT NULL,               -- JSON：{"windows": 6, "centered": true, "k": 50} など
  track_count INTEGER NOT NULL,       -- 計算に入った曲の数
  pair_count INTEGER NOT NULL,
  created_at INTEGER NOT NULL);

-- 曲の組と点数。組は順序なしで 1 行（track_a < track_b）。
CREATE TABLE similarity (
  kind TEXT NOT NULL REFERENCES similarity_set(kind) ON DELETE CASCADE,
  track_a TEXT NOT NULL REFERENCES track(id) ON DELETE CASCADE,
  track_b TEXT NOT NULL REFERENCES track(id) ON DELETE CASCADE,
  score REAL NOT NULL,
  PRIMARY KEY (track_a, track_b, kind),
  CHECK (track_a < track_b)) WITHOUT ROWID;
CREATE INDEX similarity_b ON similarity(track_b, kind);
```

ある曲 `t` の近い曲（上位 `k` 件）：

```sql
SELECT other, score FROM (
  SELECT track_b AS other, score FROM similarity WHERE kind = ?1 AND track_a = ?2
  UNION ALL
  SELECT track_a, score FROM similarity WHERE kind = ?1 AND track_b = ?2) s
JOIN track t ON t.id = s.other AND t.missing = 0
ORDER BY score DESC LIMIT ?3
```

### なぜこの形か

- **組を 1 行にする。** 点数が対称（A と B の点数 ＝ B と A の点数）なので、向きを持つ必要がありません。行数も半分近くになります。
- **順位の列を持たない。** 和集合には `t` 自身の上位 K 件が全部入ります。それより点数の高い相手は、必ず `t` の上位 K 件に入っています。だから、`t` に触れる行を点数の順に並べて先頭 `k` 件（`k ≤ K`）をとれば、`t` の上位 `k` 件と一致します。同じアルバムを除くとき（下記）も、残りの順は正しいままです。
- **鍵は曲を先にする。** 最初は `(kind, track_a, track_b)` と `(kind, track_b)` にしていました。すると、統計のない SQLite は `track_b` 側の検索に主キーの `kind` だけを使い、その種類の行をほぼ全部読みました（23 ms）。曲を先にした今の形なら、両側とも索引を引きます（0.4 ms）。曲の削除の連鎖（`track_a = ?`、`track_b = ?` だけで探す）も索引を使います。代わりに、種類を丸ごと消すときは表を 1 回なめます。入れ替えのたびに 1 回だけなので、問題にしません。
- **曲 ID で持つ。** ツールのキャッシュは音声の MD5 で引きますが、DB の曲は ID で引きます。ツールが送る前に、送り先の DB の曲 ID に直します。
- **曲が消えたとき。** 削除（purge）されると、`ON DELETE CASCADE` で行も消えます。`missing` の間は、読むときに除きます。
- **点数は種類ごとの尺度。** `audio_embed` は、ライブラリの平均を引いたあとのコサイン（-1〜1）です。種類をまたいで点数を比べることはしません。

### 大きさ（実ライブラリ 1823 曲、`audio_embed`）

| K | 組の数 | 同じアルバムを除いた相手の数（1 曲あたり） | K 位の点数（中央値） |
| --- | --- | --- | --- |
| 10 | 15,132 | 最小 0、中央値 7 | 0.773 |
| 20 | 29,814 | 最小 2、中央値 14 | 0.744 |
| 50 | 72,225 | 最小 10、中央値 37 | 0.694 |

K の既定は 50 にします。同じアルバムを除いても、どの曲にも 10 件以上の相手が残ります。

実ライブラリで測った値（K = 50、72,225 組）：

| | 値 |
| --- | --- |
| DB の増え方（索引込み） | 約 11 MB（5.6 MB → 16.7 MB） |
| `PUT` の本文 | 4.3 MB |
| 入れ替え（`PUT` 1 回、サーバー側） | 0.5〜0.6 秒 |
| 上のクエリ（1 曲の上位 5 件） | 0.4 ms |

入れ替えの時間は、DB をコンテナの中に置いたときの値です。Windows の Docker Desktop で DB をホストのフォルダ（バインドマウント）に置くと、同じ入れ替えに約 25 秒かかりました。ファイルの読み書きが遅いためで、同じ SQL を Windows の上で直接動かすと 0.7 秒でした。

## 書き込み（管理者）

| 呼び出し | 動き |
| --- | --- |
| `GET /ext/similarity` | 種類の一覧（`similarity_set` の中身） |
| `PUT /ext/similarity/{kind}` | その種類を丸ごと入れ替える。1 トランザクション |
| `DELETE /ext/similarity/{kind}` | その種類を消す |

`PUT` の本体：

```json
{
  "model": "laion/larger_clap_music",
  "params": { "windows": 6, "centered": true, "k": 50 },
  "trackCount": 1823,
  "pairs": [["<曲 ID>", "<曲 ID>", 0.851], ...]
}
```

- `kind` は `a-z`・`0-9`・`_` の 1〜64 文字。
- 組の向きは問いません。サーバーが `track_a < track_b` に並べ替えます。同じ組が 2 回あれば、点数の高いほうを残します。同じ曲どうしの組は捨てます。
- 知らない曲 ID の組は飛ばし、応答の `skipped` に件数を返します。ツールが古い DB を読んで送ったときに気づけるようにするためです。
- 7 万組で JSON は 4 MB ほどです。本文の上限（8 MB）に収まるので、1 回の `PUT` で送ります。曲が今の 2 倍を超えたら、分けて送る形を考えます。
- 書き込みは、ほかの書き込みと同じく書き込みキューを通します。

ツール側：`audio_embed.py store --server <URL> [--api-key <キー>] [--kind audio_embed] [--k 50]`。キーは `EOS_API_KEY` でも渡せます。キャッシュ（平均を引いたあと）から曲ごとの上位 K 件を求め、点数を小数 4 桁に丸めて `PUT` します。`--db` には、送り先のサーバーの DB を渡します（組を、その DB の曲 ID で送るため）。

## 書き出しと読み込み（`/ext/export`・`/ext/import`）

類似度は書き出しに入れません。キャッシュとツールから作り直せるうえ、曲 ID は DB ごとに違うからです。

## 読む側（類似グラフ）

実装済みです（2026-11-28）。約束は [ext-api.md](ext-api.md) の「`GET /ext/graph?seed=`」にあります。

- `/ext/graph` の `include` に `similar` を書くと出ます。種類は `similarKind`（既定 `audio_embed`）で選びます。保存されていない種類は 400 です。
- 辺：`{"from": "song:…", "to": "song:…", "type": "similar", "kind": "audio_embed", "score": 0.85, "provenance": "computed"}`。向きはありません。節は曲だけです。
- 1 曲あたりの本数は `similarK`（既定 5、最大 50 ＝ 保存の K）、下限の点数は `similarMinScore` で決めます。
- **同じアルバムは保存のときには除かず、読むときに選びます**（`similarOtherAlbums`、既定 `true` で除く）。実ライブラリでは、上位 10 件の 31 % が同じアルバムの曲でした。
- **類似の辺は中心の曲からだけたどります。** どの曲からも k 本出るので、段を重ねると節がすぐに増えるためです。ほかの経路で入った曲どうしの類似の辺は返します。
- 実ライブラリ（1823 曲、72,225 組）で、中心の曲から 5 本：1 回 20〜70 ms。

## 決めていないこと

- **音声が変わった曲。** ファイルを差し替えると、次にツールを回すまで古い点数が残ります。組ごとに `audio_md5` を持てば検出できますが、今は入れません（入れ替えが丸ごとなので、回し直せば直ります）。
- **`audio_match` の点数が対称か。** 対称でなければ、ツールが送る前に対称にします（大きいほうか平均）。`audio-match` のコードはまだ `main` にないので、確かめていません。

## 採らなかった案

- **ベクトルを DB に持ち、サーバーが毎回計算する。** k やしきい値を呼び出しごとに変えられ、K の上限もなくなります。ただし、ベクトルを出すのは埋め込み系の種類だけで、`audio_match` のように組ごとの点数しかない種類が入りません。サーバーにベクトル計算も要ります。必要になったら、ベクトルのテーブルを別に足します。
- **候補（`suggestion`）として出し、承認させる。** 何万組もあり、1 件ずつ承認するものではありません。類似は「事実」ではなく、目安です。
- **曲ごとの近傍リスト（向きあり、順位付き）。** 同じ組が 2 回入り、行数が倍近くになります。点数が対称なら、点数の順で同じものがとれます。
