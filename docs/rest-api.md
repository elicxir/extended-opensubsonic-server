# `/rest`：OpenSubsonic 互換 API

`/rest/<名前>` と `/rest/<名前>.view` の両方を、GET とフォームの POST（`formPost` 拡張）で受けます。応答の形は OpenSubsonic の仕様に従います。

## 応答

- `f=json`、`f=xml`（省略時）、`f=jsonp`（`callback`）。
- 包みは必ず `status`、`version`（`1.16.1`）、`type`（`eos`）、`serverVersion`、`openSubsonic: true` を持ちます。
- Subsonic のエラーも HTTP は 200 で返し、本文の `error.code` で伝えます。存在しないエンドポイントだけは HTTP 404 です。

| コード | 意味 | 返す場面 |
| --- | --- | --- |
| 0 | その他 | 内部エラー、変換の同時実行数の上限、不正な値 |
| 10 | 必須パラメータがない | `id` などの欠落、認証情報がない、`savePlayQueueByIndex` の範囲外の `currentIndex` |
| 40 | ユーザー名かパスワードが違う | トークン認証・`p=` 認証の失敗、試行回数の制限中 |
| 41 | トークン認証に対応していない | 設定 `auth.allow_token = false` のとき |
| 42 | その認証方式に対応していない | 設定 `auth.allow_password = false`（既定）のときの `p=` |
| 43 | 複数の認証方式が同時に来た | `apiKey` と `u` を同時に送った、など |
| 44 | API キーが無効 | 存在しない・無効にしたキー、試行回数の制限中 |
| 50 | 権限がない | 一般ユーザーの `startScan`、他人のプレイリストの変更 |
| 70 | 見つからない | 存在しない ID |

## 認証

上から順に、どれかひとつを使います。

1. API キー：`apiKey=<キー>`（クエリ）または `X-Api-Key` ヘッダー。キーは端末ごとに発行します（`eos-admin apikey create`、`POST /ext/auth/login`、`POST /ext/apikeys`）。
2. トークン認証：`u`、`t = md5(パスワード + s)`、`s`。既定で有効です。
3. パスワード：`u`、`p`（`enc:` で始まれば 16 進数）。既定で無効です。

- `apiKey` と `u`・`t`・`s`・`p` を一緒に送ると 43 です。
- 失敗は接続元の IP ごととユーザー名ごとに数えます。10 分に 10 回失敗すると、15 分は照合せずに失敗を返します。
  - 自宅 LAN（localhost とプライベートアドレス。設定 `auth.rate_limit_exempt`）からは制限しません。
  - リバースプロキシの後ろでは、`http.trusted_proxies` から来たときだけ `X-Forwarded-For` を使います。
- ログには `t`、`s`、`p`、`apiKey` の値を出しません。

## 対応する拡張（`getOpenSubsonicExtensions`、認証なしで呼べます）

`apiKeyAuthentication`、`formPost`、`indexBasedQueue`、`transcodeOffset`（いずれも版 1）。

## ID の形

| 対象 | 形 |
| --- | --- |
| 曲、アルバム、アーティスト、プレイリスト | データベースの ID（22 文字） |
| 仮想フォルダ（`getIndexes` / `getMusicDirectory`） | `fo-<ID>`。フォルダのないアルバムは `fo-unfiled`（「（未分類）」） |
| ジャケット（`coverArt`） | `al-<アルバムID>_<更新時刻>`、`mf-<曲ID>_<更新時刻>` |

ジャケットの ID は更新時刻を含むので、値が同じ間は長くキャッシュしてかまいません（`Cache-Control: immutable`）。

## エンドポイント

| 分類 | エンドポイント | 約束 |
| --- | --- | --- |
| システム | `ping`、`getLicense`（常に `valid: true`）、`getOpenSubsonicExtensions` | |
| | `getScanStatus`、`startScan` | `startScan` は管理者だけ。`fullScan=true` で全件。すぐに `scanning: true` を返し、裏で走ります |
| | `getMusicFolders`、`getUser` | 他人の `getUser` は管理者だけ |
| 閲覧 | `getArtists`、`getArtist` | アルバムアーティストだけ。索引は読み（ソート名）の先頭 1 文字。英字は大文字、ひらがなはカタカナにそろえ、それ以外は `#`。冠詞（`subsonic.ignored_articles`）は除きます |
| | `getAlbum`、`getSong`、`getGenres` | 曲はディスク番号、トラック番号の順 |
| | `getIndexes`、`getMusicDirectory` | 仮想フォルダをたどります（フォルダ → 下位フォルダとアルバム → 曲）。フォルダのタグが 1 つもないライブラリでは、アーティスト → アルバム → 曲 |
| 一覧 | `getAlbumList2` | `type`：`random`、`newest`、`highest`、`frequent`、`recent`、`starred`、`alphabeticalByName`、`alphabeticalByArtist`、`byYear`（`fromYear` > `toYear` なら新しい順）、`byGenre`。`size` は既定 10、上限 500 |
| | `getRandomSongs`、`getStarred2` | `getRandomSongs` は `genre`、`fromYear`、`toYear` で絞れます |
| 検索 | `search3` | 下記 |
| 記録 | `scrobble` | `submission=true`（既定）で再生回数と最終再生時刻を増やします。曲のアルバムの回数も増えます。`time`（ミリ秒）を付ければその時刻で記録します。`false` は何も記録しません |
| | `star`、`unstar`、`setRating` | `id`（曲・アルバム・アーティストを自動で判別）、`albumId`、`artistId`。`rating=0` で評価を消します |
| プレイリスト | `getPlaylists`、`getPlaylist`、`createPlaylist`、`updatePlaylist`、`deletePlaylist` | 見えるのは自分のものと公開のもの。変えられるのは持ち主だけ。`updatePlaylist` の `songIndexToRemove` は変更前の番号 |
| キュー | `getPlayQueue`、`savePlayQueue`、`getPlayQueueByIndex`、`savePlayQueueByIndex` | 同期（[sync.md](sync.md)）と同じキュー。保存するとキューを丸ごと置き換え、接続中の端末にすぐ配ります。読むときは再生端末が報告した最新の位置を返します |
| 配信 | `stream`、`download`、`getCoverArt` | 下記 |

### 曲（Child）に載せるもの

標準の項目に加えて、OpenSubsonic の `displayArtist`、`displayAlbumArtist`、`displayComposer`、`artists`、`albumArtists`、`genres`、`contributors`（作曲・作詞・編曲・指揮・演奏。演奏は `subRole` に楽器）、`sortName`、`musicBrainzId`、`bitDepth`、`samplingRate`、`channelCount`、`replayGain` を載せます。

- `artists`、`albumArtists`、`genres`、`contributors` は、空でも配列で返します。
- `replayGain`（`trackGain`、`trackPeak`、`albumGain`、`albumPeak`）は、タグに値があるときだけ載せます。
- 曲の `albumId` は常に物理アルバムです。

### 仮想アルバム

既定（`subsonic.virtual_albums = "hide"`）では、`getAlbumList2`、`getArtist`、`search3` に仮想アルバムを出しません。曲の `albumId` は 1 つしか持てず、既存クライアントのライブラリ同期が崩れるためです。`"show"` にすると出します。`getAlbum` と `getMusicDirectory` は設定によらず仮想アルバムも扱います。

### `search3`

- 検索語を空白で分け、各語を正規化します（[tags.md](tags.md#正規化)）。すべての語がどこかに含まれる曲・アルバム・アーティストを返します。
- 曲は曲名、読み、サブタイトル、アーティスト、アルバム名、作曲者で探します。アルバムは名前、読み、アルバムアーティストで探します。
- 並び：名前の完全一致 → 前方一致 → 名前に含む → 読みに含む → その他。同じなら再生回数、お気に入りの順。
- 3 文字以上の語は索引（FTS5 trigram）、2 文字以下の語は全件からの部分一致で探します。
- `query` が空（`""` も含む）なら全件を返します。ライブラリ全体を同期するクライアントのため、このときの `songCount` は 5000 まで指定できます。

### `stream` とトランスコード

| 要求 | 結果 |
| --- | --- |
| `format=raw`、または `format` も `maxBitRate` もない | 元のファイル |
| `format` が元の形式と同じで、`maxBitRate` がないか元以上 | 元のファイル |
| `format` なし、`maxBitRate` が元のビットレート以上 | 元のファイル |
| それ以外 | 変換（`format` が `opus` / `mp3` 以外なら既定の `opus`） |

- 元のファイルは `Range` に対応します（206、範囲外は 416）。このとき `timeOffset` は無視します（Range でシークしてください）。
- 変換は Opus（Ogg、32〜256 kbps、既定 128）か MP3（96〜320 kbps、既定 192）です。`timeOffset`（秒）から始められます。本文は chunked で、`Content-Length` はありません。`Range` は無視して 200 を返します。
- 変換した配信にはタグが付きません。ReplayGain は API の `replayGain` を使ってください（[sync.md](sync.md#replaygain)）。
- 同時に変換できるのは 4 本までです（`transcode.max_concurrent`）。超えるとエラー 0 です。
- `download` は常に元のファイルを `Content-Disposition: attachment` で返します。

### `getCoverArt`

- アルバム：アルバムのフォルダの画像（`cover.*`、`folder.*`、`front.*` の順。大文字小文字は区別しません）。なければ、最初の曲の埋め込み画像。
- 曲：その曲の埋め込み画像。なければアルバムの画像。
- `size` を付けると、長い辺をその大きさにした JPEG（上限 1200px、拡大はしません）。
- アーティスト：登録された画像（[`/ext/artists/{id}/image`](ext-api.md#アーティストの画像)）。サーバーは自分では探しません。画像のあるアーティストだけ、`getArtists`・`getArtist`・`search3` などのアーティストに `coverArt`（`ar-<ID>_<更新時刻>`）が載ります。画像のないアーティストはエラー 70。

## 再生回数（scrobble）の約束

音を鳴らした端末が `scrobble` を呼びます。サーバーは配信や同期の様子から回数を数えません。詳しくは [sync.md](sync.md#再生回数) を見てください。
