# つながりの出所（設計）

楽曲・作品・関係などのつながり 1 本ごとに、「何を根拠にしたか」を持たせるための設計です。グラフの「確度の高いつながりだけ」（kleulir の Sure links）を、規則の名前ではなく出所で絞れるようにします。

状態（2026-11-28）：**実装済み**（マイグレーション 006、`/ext/graph` の `origin=`、`POST /ext/suggestions` の `origin`・`originRef`、書き出しと読み込み、`mb-match`・`audio-match`）。API の約束は [ext-api.md](ext-api.md) にあります。設計から変えた点は「実装で変えたこと」にまとめました。

## いまの問題

- 外から送られる候補（`POST /ext/suggestions`）は、作り方の欄（`method`）がすべて `llm` になります。DB の CHECK 制約で、`rule` か `llm` しか入らないためです。MusicBrainz に書いてあることも、音の照合も、LLM の推測も、同じ `llm` として記録されます。
- 出所を表しているのは、ツールが自由に付ける規則の名前（`musicbrainz_work` など）と、`evidence` の中身（MusicBrainz の URL など）だけです。どちらも決まった形がなく、サーバーには意味がわかりません。
- つながりの行（`track_arrangement` など）は、出所を候補の ID（`suggestion_id`）経由でしか引けません。**書き出し（`/ext/export`）はこの ID を運ばない**ので、読み込んだ先では出所がすべて消えます。
- `source`（1 推定・2 タグ・3 手動）は、どれを優先するかの段です。推定の中の違い（外部 DB に書いてあるのか、名前が似ているだけか）は表せません。

サンプル DB（2026-11-28 に作り直したもの）で、推定のつながりの規則を数えた結果：

| 表 | 規則 | 行数 | 実際の根拠 |
| --- | --- | --- | --- |
| `track_arrangement` | `musicbrainz_work` | 82 | MusicBrainz が「この録音はこの楽曲」と書いている |
| | `musicbrainz_arrangement` | 12 | MusicBrainz が「この楽曲はあの楽曲の編曲」と書いている |
| | `musicbrainz_alias` | 164 | 曲名を MusicBrainz の楽曲の別名と照らしただけ |
| | `audio_match` | 92 | 音の照合（chroma） |
| | `same_composition` | 243 | 曲名と作曲者が同じ（サーバーの規則） |
| `track_title` | `musicbrainz_parts` | 382 | MusicBrainz が「この楽曲はあの作品の一部」と書いている |
| `relation` | `derivative` | 1 | 曲名の「(Remix)」など（サーバーの規則） |

`musicbrainz_alias` は名前に `musicbrainz` が付きますが、MusicBrainz がつながりを書いているわけではありません。規則の名前だけでは確度を判断できない例です。

## 方針

- つながりの**行そのもの**に、出所の種類（`origin`）と参照先（`origin_ref`）を持たせます。候補を経由せずに引けて、書き出しにもそのまま乗ります。
- `source` はそのまま残します。`source` は「どれが勝つか」、`origin` は「何を根拠にしたか」で、役割が違います。
- 出所の種類は、サーバーが意味を知っている小さな語彙にします。規則の名前は今までどおり自由です。
- 種類を決めるのは、候補を作った側（サーバーの規則、またはツール）です。サーバーは規則の名前から推し量りません。

## 出所の種類（`origin`）

| 値 | 意味 | `source` | `origin_ref` の例 |
| --- | --- | --- | --- |
| `tag` | ファイルのタグ | 2 | タグの名前（`MUSICBRAINZ_WORKID` など）。省略してよい |
| `manual` | 人が付けた | 3 | なし |
| `external` | 外部 DB が、そのつながりを直接書いている | 1 | 根拠の記録の URL（`https://musicbrainz.org/work/…`） |
| `audio` | 音の解析（照合や類似） | 1 | 手法の名前や点数の出どころ。省略してよい |
| `name` | 名前（曲名・別名・作曲者名）の一致 | 1 | 照らした相手。外部 DB の別名なら、その記録の URL |
| `llm` | 言語モデルの推測 | 1 | モデル名 |

- 推定（`source` 1）の行だけでなく、タグと手動の行にも入れます。絞り込みを 1 つの列で書けるようにするためです。
- 上の表の規則は、`musicbrainz_work`・`musicbrainz_arrangement`・`musicbrainz_parts` が `external`、`musicbrainz_alias`・`same_composition`・`derivative` が `name`、`audio_match` が `audio` になります。
- **`external` の線引き：** 外部 DB が「A と B はつながっている」と書いていることが条件です。手元の曲を外部 DB の記録に結び付ける段（録音の照合）も含めて、外部の ID で決まるときだけ `external` にします。`mb-match` は録音を、曲順と長さで MusicBrainz のリリースと突き合わせています。アルバムに MusicBrainz のリリース ID のタグがあれば、照合は ID で決まります。ないときは、その段の確からしさを `confidence` に落とします。

## テーブル（マイグレーション 006 の案）

出所を持たせる表：`suggestion`、`track_arrangement`、`track_title`、`relation`、`participant`、`album_track`、`album_folder`。

```sql
ALTER TABLE suggestion ADD COLUMN origin TEXT
  CHECK (origin IN ('tag', 'manual', 'external', 'audio', 'name', 'llm'));
ALTER TABLE suggestion ADD COLUMN origin_ref TEXT;
-- つながりの 6 表にも、同じ 2 列を足す（NULL は「わからない」）。
```

- SQLite の `ADD COLUMN` で足せます。表を作り直す必要はありません。CHECK 付きでも足せます。
- 既存の行は、マイグレーションの中で埋めます。
  - `source` 2 は `tag`、3 は `manual`。
  - `source` 1 は、候補（`suggestion_id`）の規則の名前を上の対応で引きます。`external` の行には、`evidence` の `works` の先頭の URL を `origin_ref` に写します。
  - どれにも当たらない行（候補が見つからない、書き出しから読み込んだ行など）は NULL のままにします。
- ツールの規則の名前をマイグレーションに書くのは、この一度だけです。ツールがこのリポジトリにあるので許します。

## 書き込み

- `POST /ext/suggestions` の各候補が `origin` を取ります：`{"origin": "external", "originRef": "https://musicbrainz.org/work/…"}`。省略すると `llm` です（今までと同じ）。
- サーバーの規則（`same_composition`・`derivative` など）は、候補に `origin` を自分で入れます。
- 承認（と自動の取り込み）で、候補の `origin` と `origin_ref` を、それが書くすべての行に写します。
- スキャンでタグから作る行には `tag`、手動の編集 API で作る行には `manual` を入れます。
- 書き出しと読み込みで、2 列とも運びます。形式の版（`version: 1`）は上げません。足すだけなので、古い書き出しも読めます。

## 読む側

- 読み取り API とグラフの辺に、`origin` と `originRef` を出します（`provenance` の隣）。
- `/ext/graph` は `origin=external,tag,manual` のように、出所の種類で絞れるようにします。今の `rules=` はまだ push していないので、置き換えます。
- kleulir の Sure links は、`origin=external,tag,manual` を使います。
- kleulir の詳細の欄に出所を出し、`external` なら参照先の URL を開けるようにします（後回しでよい）。

## 決めていないこと

- **参照先を、曲ごとに持つか。** `link_arrangement` の候補 1 件は、複数の曲をまとめてつなぎます。曲ごとに根拠の録音が違うので、本当は曲ごとに `origin_ref` を持てます（候補に `originRefs: {曲 ID: URL}` を足す）。最初は候補単位（楽曲の URL）にします。
- **楽曲・作品そのものの出所。** `composition` や `title` などの物にも `source` と `suggestion_id` があります。物の出所は、つながりの絞り込みには要らないので、今回は入れません。`composition.mbz_work_id` が、すでに外部の参照になっています。
- **`method` の扱い。** `origin` があれば、`method`（`rule`・`llm`）はほぼ要らなくなります。互換のために残し、新しいコードでは `origin` を見ます。

## 実装で変えたこと

- **タグと手動の行には書き込まない。** 列は NULL のままにし、読むときに `source` から `tag`・`manual` を出します（`origin_of()`）。スキャンと編集 API の書き込みを全部直さずに済み、`source` と食い違うこともありません。そのため、列の CHECK は推定の 4 語（`external`・`audio`・`name`・`llm`）だけです。
- **承認の最後に写す。** 承認で書く行は種類ごとに違う INSERT で作られるので、1 つずつ直す代わりに、承認の最後に「この候補が書いた推定の行（`suggestion_id` が同じで `source` 1）」へまとめて写します。編集して承認した行（手作業、3）には写しません。
- **参与者（`participant`）の出所は書き出さない。** 書き出しは、楽曲の作曲者などを名前の一覧として運んでいて、行ごとの形を持たないためです。グラフの絞り込み（作曲者の辺）には、DB の中の値を使います。
- **`rules=` は外した。** push する前だったので、`origin=` に置き換えました。辺の `rule` は残します。
- `mb-match` は、`musicbrainz_work`・`musicbrainz_arrangement` の参照先を、その曲たちの楽曲（Work）にします。マイグレーションで埋めた古い行は、`evidence` の `works` の先頭（まとまりの代表）になっています。

サンプル DB で当てた結果（006 のバックフィル）：

| 表 | `external` | `name` | `audio` |
| --- | --- | --- | --- |
| `track_arrangement` | 94 | 407 | 92 |
| `track_title` | 382 | 0 | 0 |
| `relation` | 0 | 1 | 0 |

## 採らなかった案

- **候補の表だけに持たせる。** 変更は小さく済みますが、書き出しと読み込みで消えます。候補を経由しない行（タグ・手動）にも付きません。
- **出所を別の表にまとめる。** 表ごとに主キーの形が違う（`track_arrangement` は曲と段、`relation` は ID）ので、参照の仕方が込み入ります。消したときの連鎖も、表ごとに書くことになります。
- **規則の名前から、サーバーが推し量る。** 今の `rules=` がこれです。規則が増えるたびに、クライアントとサーバーの両方の一覧を直すことになり、`musicbrainz_alias` のように名前が誤解を招くこともあります。
