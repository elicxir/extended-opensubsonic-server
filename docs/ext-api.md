# `/ext`：独自拡張 API

作品・フォルダ・楽曲・編曲・関係・仮想アルバム・候補を扱う REST 風の API です。標準の `/rest` には手を加えません。

## 共通の約束

- 本文と応答は JSON です（`Content-Type: application/json`）。
- 認証は `X-Api-Key` ヘッダーを基本にします。`/rest` と同じクエリの認証（`apiKey`、`u`+`t`+`s`）も使えます。
- 成功は 200。作成は 201、本文なしは 204 です。
- 失敗は HTTP のステータスと次の本文です。

  ```json
  {"error": {"code": "not_found", "message": "Album not found"}}
  ```

  | ステータス | `code` | 場面 |
  | --- | --- | --- |
  | 400 | `bad_request` | 本文がない・壊れている、値が不正 |
  | 401 | `unauthorized` | 認証できない |
  | 403 | `forbidden` | 一般ユーザーがライブラリを編集しようとした |
  | 404 | `not_found` | ID やパスが存在しない |
  | 405 | `method_not_allowed` | パスはあるがメソッドが違う |
  | 409 | `conflict` | 重複、既に判断済みの候補、物理アルバムの曲の編集 |
  | 409 | `tag_managed` | タグから作られたものの削除や改名（次のスキャンで戻るため） |
  | 429 | `too_many_requests` | ログインの失敗が多すぎる |
  | 500 | `internal` | 内部エラー |

- 一覧は `limit`（既定 100、上限 500）と `offset` で区切り、`{"items": [...], "total": N}` で返します。
- 曲は OpenSubsonic の `Child` と同じ形に、次の項目を足して返します。

  | 項目 | 意味 |
  | --- | --- |
  | `compositionId`、`arrangementId` | 曲が属する楽曲と編曲（ないときは `null`） |
  | `arrangementProvenance` | その紐付けの出どころ（`manual`・`tag`・`rule`・`llm`） |
  | `titleIds` | 曲が属する作品 |
  | `virtualAlbumIds` | 曲が入っている仮想アルバム |
  | `performanceKind`、`recordedDate` | 演奏の種類（`live`・`studio`）と収録日 |

- アルバムは `AlbumID3` の形に `kind`（`physical`・`virtual`）と `provenance` を足して返します。
- 楽曲、編曲、作品、フォルダ、関係も `provenance` を持ちます。推定（`rule`・`llm`）の関係には `confidence` も付きます。
- `?includeEstimated=false` を付けると、推定（出どころ 1）の紐付けを除いて返します。
- 権限：ライブラリの編集、候補の判断と登録、書き出しと読み込みは管理者だけです。読み取り、好きなバージョン、自分の API キーは一般ユーザーも使えます。

## 読み取り

| メソッドとパス | 返すもの |
| --- | --- |
| `GET /ext/info` | `apiVersion`（1）、`serverVersion`、`features`（グラフがあれば `graph` を含みます） |
| `GET /ext/scan` | `scanning`、`phase`、`count`、`folderCount`、`last`（直近のスキャンの件数） |
| `GET /ext/folders` | 全フォルダの平らな一覧（`id`、`name`、`parentId`、`sortOrder`、`albumCount`、`childCount`、`provenance`）。木はクライアントが組みます |
| `GET /ext/folders/{id}` | `folder`、`children`（直下のフォルダ）、`albums` |
| `GET /ext/albums?kind=all\|physical\|virtual` | アルバムの一覧（読みの順） |
| `GET /ext/albums/{id}` | アルバム、`song`（曲）、`folderIds`。物理なら `virtualAlbums`（その曲を含む仮想アルバム）、仮想なら `parentAlbums`（曲の元の物理アルバム） |
| `GET /ext/albums/{id}/titles` | `groups`：作品ごとの `{"title", "songs"}`。作品のない曲は `title: null`。複数の作品に属する曲は各グループに出ます。グループは最初の曲の曲順で並びます |
| `GET /ext/titles?query=` | 作品の一覧（`songCount` 付き） |
| `GET /ext/titles/{id}` | `title`、`albums`、`songs`（発売日、アルバム、曲順） |
| `GET /ext/compositions?query=` | 楽曲の一覧 |
| `GET /ext/compositions/{id}` | `composition`（作曲者付き）、`arrangements`（`{"arrangement", "songs"}`）、`relations` |
| `GET /ext/songs/{id}` | 曲 |
| `GET /ext/songs/{id}/versions` | 下記 |
| `GET /ext/search?q=` | 下記 |
| `GET /ext/graph?seed=` | 下記。関連する曲のグラフ（節と辺） |
| `GET /ext/suggestions?status=&rule=&method=` | 候補の一覧。`status` の既定は `pending`。確信度の高い順 |
| `GET /ext/suggestions/{id}` | 候補 |
| `GET /ext/preferred` | 自分の「好きなバージョン」の一覧 |

### `GET /ext/songs/{id}/versions`

```json
{
  "song": { "id": "…", "title": "…", "compositionId": "c1", "arrangementId": "a1" },
  "composition": { "id": "c1", "name": "…", "composers": [{"id": "…", "name": "…"}], "provenance": "tag" },
  "arrangements": [
    { "arrangement": { "id": "a2", "name": "Piano Ver.", "kind": "piano", "isImplicit": false }, "songs": [ … ] }
  ],
  "performances": [ … ],
  "related": [
    { "relation": { "id": "r1", "type": "derivative", "subtype": "tv_size", "direction": "out", "provenance": "rule" },
      "target": { "type": "track", "song": { … } } }
  ]
}
```

- `arrangements`：同じ楽曲の、この曲以外の編曲（曲のあるものだけ）。
- `performances`：同じ編曲の、この曲以外の曲。
- `related`：この曲と、この曲の楽曲に付いた関係。`direction` は、この曲（または楽曲）が `from` 側なら `out`、`to` 側なら `in`。`target` は相手で、曲なら `song`、楽曲なら `composition`。

### `GET /ext/search?q=`

`compositions`（`{"composition", "songs"}`、全編曲の曲）、`titles`（`{"title", "songCount", "songs"}`、先頭 10 曲）、`songs`、`albums`（仮想も含む）、`artists` を返します。楽曲のまとまりに入った曲は `songs` に重ねて出しません。`limit` で各種類の件数を変えられます（既定 20）。

### `GET /ext/graph?seed=`

ひとつの曲（または楽曲・作品・人物・アルバム）を中心に、つながりを節（`nodes`）と辺（`edges`）で返します。関連する曲をグラフで描くための API です。座標は返しません。配置はクライアントが決めます。

- まとまりは節にします。同じ楽曲の曲どうしは直接つながず、楽曲の節につなぎます。作品、人物、アルバムも同じです。
- 曲どうし（または曲と楽曲）を直接つなぐのは「関係」（`medley` など）だけです。

| パラメータ | 意味 | 既定 |
| --- | --- | --- |
| `seed` | 中心の節。`種類:ID` の形。種類は `song`（`track` も可）・`composition`・`title`・`artist`・`album` | 必須 |
| `depth` | たどる段数（0〜4）。曲 → 楽曲 → 別の曲で 2 段です | 2 |
| `include` | たどる辺の種類（`,` 区切り）：`composition`・`relation`・`title`・`artist`・`album` | `composition,relation,title` |
| `roles` | `artist` の辺を役割で絞ります（例：`composer,arranger`） | 全部 |
| `limit` | 節の上限（1〜1000）。中心に近い節から残します | 200 |
| `fanout` | 中心以外の節は、相手がこの数より多いと開きません（1〜1000） | 50 |
| `includeEstimated` | `false` で推定の紐付けを除きます | `true` |

```json
{
  "seed": "song:t1",
  "depth": 2,
  "nodes": [
    { "id": "song:t1", "type": "song", "label": "Main Theme", "distance": 0, "degree": 2, "expanded": true,
      "song": { … } },
    { "id": "composition:c1", "type": "composition", "label": "Main Theme", "distance": 1, "degree": 3,
      "expanded": true, "composition": { … } },
    { "id": "artist:a1", "type": "artist", "label": "Sato", "distance": 1, "degree": 466, "expanded": false,
      "artist": { "id": "a1", "name": "Sato" } }
  ],
  "edges": [
    { "from": "song:t1", "to": "composition:c1", "type": "performance", "arrangementId": "ar1",
      "arrangementName": null, "arrangementKind": "original", "isOriginal": true, "provenance": "tag" },
    { "from": "song:t4", "to": "composition:c1", "type": "quote", "directed": true, "relationId": "r1",
      "subtype": null, "position": null, "note": null, "provenance": "manual" }
  ],
  "truncated": false
}
```

節：

- `id` は `種類:ID`。そのまま次の呼び出しの `seed` に使えます。
- 種類と同じ名前の項目に本体が入ります（`song` は曲、`album` はアルバム、`composition`・`title` は読み取り API と同じ形、`artist` は `id` と `name`）。
- `distance`：中心からの段数。節は中心に近い順に並びます。
- `degree`：その節の相手の数（`include` の範囲で数えます）。
- `expanded`：相手が全部この応答に入っていれば `true`。`false` の節は、その節を `seed` にして呼ぶと続きが取れます。

辺：

| `type` | 向き（`from` → `to`） | 追加の項目 |
| --- | --- | --- |
| `performance` | 曲 → 楽曲 | `arrangementId`、`arrangementName`、`arrangementKind`、`isOriginal`（原曲なら `true`）。編曲は節にせず、ここに入れます |
| `medley`・`quote`・`derivative`・`cover`・`related` | 関係の `from` → `to` | `directed: true`、`relationId`、`subtype`、`position`、`note` |
| `title` | 曲 → 作品 | |
| `credit` | 曲・楽曲 → 人物 | `role`（`composer`・`arranger`・`artist` など）、`subRole`（楽器）。役割ごとに 1 本 |
| `album` | 曲 → アルバム | `virtual`（仮想アルバムなら `true`） |

- どの辺も `provenance` を持ちます。推定（`rule`・`llm`）には `confidence` も付きます。
- 辺は、応答に入った節どうしのものを全部返します（端の節どうしの辺も含みます）。
- `truncated`：`limit` に達して、たどるのをやめたときに `true`。
- 人物やアルバムは相手が数百になるので、`include` に書いたときだけたどります。書いても、`fanout` を超える節は開かずに `degree` だけ返します。

## 編集（管理者）

書き込みはすべて手作業（出どころ 3）として保存します。タグ由来のものを削除すると 409 `tag_managed` です。紐付けを外すと「除外」の行として残り、再スキャンしても戻りません。

| メソッドとパス | 本文 | 説明 |
| --- | --- | --- |
| `POST /ext/relations` | `from`・`to`（`{"type": "track"\|"composition", "id"}`）、`type`、`subtype?`、`position?`、`note?` | `type` は `medley`・`quote`・`derivative`・`cover`・`related`。向きは「`from` が `to` を含む・引用する・元にする」。同じ組は 409 |
| `PUT /ext/relations/{id}` | `type?`、`subtype?`、`position?`、`note?` | |
| `DELETE /ext/relations/{id}` | | |
| `POST /ext/compositions` | `name`、`composers?`（名前の配列）、`musicBrainzWorkId?` | 原曲（暗黙の編曲）も一緒に作ります |
| `PUT /ext/compositions/{id}` | `name?`、`composers?`、`musicBrainzWorkId?` | 編集した楽曲は手作業扱いになり、タグから参照されなくなっても残ります |
| `DELETE /ext/compositions/{id}` | | タグ由来は 409 |
| `POST /ext/compositions/{id}/arrangements` | `name`、`kind?`、`arrangers?` | |
| `PUT /ext/arrangements/{id}` | `name?`、`kind?`、`arrangers?` | |
| `DELETE /ext/arrangements/{id}` | | タグ由来は 409 |
| `PUT /ext/songs/{id}/arrangement` | `arrangementId` | 曲を編曲に紐付けます |
| `DELETE /ext/songs/{id}/arrangement` | | 手作業の紐付けを消し、タグの紐付けに戻します |
| `POST /ext/titles` | `name`、`sortName?`、`kind?`、`year?` | 同じ名前（正規化して比較）は 409 |
| `PUT /ext/titles/{id}` | `name?`、`sortName?`、`kind?`、`year?` | |
| `DELETE /ext/titles/{id}` | | タグ由来は 409 |
| `PATCH /ext/songs/{id}/titles` | `add?`、`remove?`（作品 ID の配列） | |
| `POST /ext/folders` | `name`、`parentId?`、`sortOrder?` | 同じパスは 409 |
| `PUT /ext/folders/{id}` | `name?`、`parentId?`、`sortOrder?` | タグ由来のフォルダは `sortOrder` だけ変えられます。自分の下には移せません（400） |
| `DELETE /ext/folders/{id}` | | タグ由来は 409。下位フォルダも消えます |
| `PATCH /ext/albums/{id}/folders` | `add?`、`remove?`（フォルダ ID） | |
| `POST /ext/albums` | `name`、`sortName?`、`albumArtist?`、`tracks?`（曲 ID の配列、この順） | 仮想アルバムを作ります |
| `PATCH`（または `PUT`）`/ext/albums/{id}` | `name?`、`sortName?` | 上書き。`null` で上書きを消してタグの値に戻します |
| `DELETE /ext/albums/{id}` | | 手作業の仮想アルバムだけ。物理アルバムとタグ由来の仮想アルバムは 409 |
| `PATCH /ext/albums/{id}/tracks` | `add?`（曲 ID か `{"id", "disc", "track"}`）、`remove?` | 仮想アルバムだけ（物理は 409） |
| `PATCH /ext/songs/{id}` | `title?`、`sortTitle?`、`subtitle?` | 上書き。`null` でタグの値に戻します。検索にもすぐ反映されます |

### 好きなバージョン（一般ユーザーも可）

| メソッドとパス | 本文 |
| --- | --- |
| `PUT /ext/compositions/{id}/preferred` | `songId` |
| `DELETE /ext/compositions/{id}/preferred` | |

シャッフルで同じ楽曲が重ならないようにするとき、クライアントはこの曲を優先します。

## 候補と推定

候補は、サーバーの規則（スキャンのたびに走る）か、外部のツール（LLM など）が出します。承認されるまでは本体のデータに入りません。

### 候補の形

```json
{
  "id": "…", "rule": "derivative", "method": "rule", "model": null, "score": 1.0,
  "status": "pending", "createdAt": "…", "decidedAt": null,
  "summary": "「Main Theme (TV Size) (Box)」→「Main Theme (Box)」: derivative（tv_size）",
  "targets": [{"type": "track", "id": "…", "name": "…"}],
  "proposal": { "action": "relation", … },
  "evidence": null
}
```

- `status`：`pending`、`accepted`、`auto_applied`、`rejected`、`stale`（タグや手作業で同じ情報が入って不要になった）。
- `rule` の規則（サーバーがスキャンのたびに出すもの）：

  | 規則 | 条件 |
  | --- | --- |
  | `same_composition` | 楽曲が決まっていない曲どうしで、基本の曲名（派生の印と末尾の括弧書きを除いたもの）と作曲者が一致する |
  | `performance_variant` | 同じ楽曲・基本の曲名・編曲者なのに、編曲が分かれている |
  | `derivative` | 「(TV Size)」「Instrumental」「Off Vocal」「Remix」などの曲と、同じアルバム（なければ同じ作品）の元の曲 |
  | `medley` | 曲名を `〜`・`～`・` / `・` - ` で分けた部分が別の曲名や楽曲名と一致する。末尾の「～…～」などは区切る前に除き、曲名に「メドレー」「Medley」があれば 2 つ以上、なければ 3 つ以上の一致が要る |

### `proposal` の種類

| `action` | 項目 | 承認すると |
| --- | --- | --- |
| `link_composition` | `name`、`composers`、`tracks`、`compositionId?` | 楽曲（なければ作る）の原曲に曲を紐付け |
| `merge_arrangements` | `arrangementId`、`tracks` | 曲をその編曲に紐付け |
| `relation` | `type`、`subtype?`、`from`、`to`、`position?`、`note?` | 関係を作る |
| `medley` | `from`、`parts`（`[{"position", "to"}]`） | メドレーの関係を部分ごとに作る |
| `link_title` | `tracks`、`titleId?` または `titleName` | 作品（なければ作る）に曲を入れる |
| `virtual_album` | `name`、`tracks`（順番どおり） | 仮想アルバム（なければ作る）に曲を入れる |

### 判断（管理者）

| メソッドとパス | 本文 | 説明 |
| --- | --- | --- |
| `POST /ext/suggestions/{id}/accept` | `{"edits": {…}}`（任意） | `edits` は `proposal` に重ねます（JSON Merge Patch）。修正したときは手作業（3）、そのままなら推定（1）として保存します |
| `POST /ext/suggestions/{id}/reject` | | 却下した候補は二度と出ません（`fingerprint` で判定） |
| `POST /ext/suggestions/batch` | `{"accept": [ids], "reject": [ids]}` | まとめて 1 トランザクションで |

判断済みの候補をもう一度判断すると 409 です。

### 推定の登録（管理者、`eos-estimate` などのツール用）

`POST /ext/suggestions`

```json
{"suggestions": [
  {"rule": "title", "model": "モデル名", "score": 0.95,
   "evidence": {"reason": "…", "urls": ["https://…"]},
   "proposal": {"action": "link_title", "titleName": "Game Z", "tracks": ["…"]}}
]}
```

- `method` は `llm` として保存します。`score` は確信度（0〜1）です。
- 1 件ずつ確かめて保存します。不正な 1 件は `errors` に入り、他は登録されます。
- 同じ推定（規則名と `proposal` が同じ）は `skipped` になります。却下済みのものも含みます。
- 設定 `estimate.auto_apply_min_confidence` が 0 より大きく、`score` がそれ以上なら、その場で推定（1）として取り込み、`auto_applied` にします。
- 応答（201）：`{"created": [ids], "autoApplied": N, "skipped": N, "errors": [{"index", "code", "message"}]}`

## ログインと API キー

| メソッドとパス | 本文 | 説明 |
| --- | --- | --- |
| `POST /ext/auth/login` | `username`、`password`、`deviceName` | 認証なしで呼べます。端末用の API キーを発行します（201：`id`、`key`、`username`、`admin`）。失敗の数は `/rest` と同じく制限します |
| `GET /ext/apikeys` | | 自分のキーの一覧（キーそのものは返しません） |
| `POST /ext/apikeys` | `deviceName` | 新しいキー（`key` はこのときだけ返します） |
| `DELETE /ext/apikeys/{id}` | | 無効にします。他人のキーは管理者だけ |

## 書き出しと読み込み（管理者）

- `GET /ext/export`：手作業と承認済みの情報、判断済みの候補、上書き、好きなバージョンを JSON で返します（`format: "eos-export"`、`version: 1`）。
- `POST /ext/import`：書き出した JSON を読み込みます。新しいデータベースでも、スキャンのあとに読み込めば元に戻ります。
  - 曲は ID、PID、音声の MD5、パスの順で照合します。アルバムは ID、PID の順です。
  - 照合できなかった数を `unmatched` で返します。
  - 2 回読み込んでも重複しません。
- 応答：`{"imported": {"titles": N, …}, "unmatched": {"tracks": N, "albums": N}}`
- 類似度（下記）は書き出しに入りません。ツールで作り直します。

## 類似度（管理者、`audio-embed` などのツール用）

曲どうしの類似度を、種類（`kind`）ごとに丸ごと入れ替えて保存します。音が似ているかどうかのような目安で、楽曲や関係のような「事実」ではありません。候補の承認の流れには乗らず、出どころも持ちません。設計は [similarity.md](similarity.md) にあります。

| メソッドとパス | 説明 |
| --- | --- |
| `GET /ext/similarity` | 種類の一覧（`items`、`total`） |
| `PUT /ext/similarity/{kind}` | その種類を丸ごと入れ替えます（1 トランザクション） |
| `DELETE /ext/similarity/{kind}` | その種類を消します（204。ない種類は 404） |

`PUT` の本文：

```json
{
  "model": "laion/larger_clap_music",
  "params": { "windows": 6, "centered": true, "k": 50 },
  "trackCount": 1823,
  "pairs": [["<曲 ID>", "<曲 ID>", 0.8197], ...]
}
```

- `kind` は `a-z`・`0-9`・`_` の 1〜64 文字です。
- `model` は必須です。`params` は任意のオブジェクトで、そのまま一覧に返します。`trackCount` は計算に入った曲の数で、省くと組のある曲の数になります。
- 組の向きは問いません。同じ組が 2 回あれば点数の高いほうを残し、同じ曲どうしの組は捨てます。
- 知らない曲 ID を含む組は飛ばし、`skipped` に数を返します。
- 点数の尺度は種類ごとに決まります。種類をまたいで比べません。
- 本文の上限は 8 MB です（曲ごと 50 件で、1823 曲が約 4 MB）。
- 応答：`{"kind", "pairCount", "skipped", "trackCount"}`
- 一覧の 1 件：`{"kind", "model", "params", "trackCount", "pairCount", "createdAt"}`（`createdAt` は UNIX ミリ秒）
- 曲が削除される（`purge`）と、その曲の組も消えます。`missing` の曲の組は残ります。

## WebSocket

`GET /ext/session`：共有リモコンの WebSocket です。[sync.md](sync.md) を見てください。
