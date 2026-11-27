# 設定

設定は TOML の 1 ファイルです。場所は `--config` で指定します。指定がなければ環境変数 `EOS_CONFIG`、それもなければ `/etc/eos/eos.toml` を読みます。Docker のイメージには `docker/eos.toml`（音楽は `/music`、データは `/data`）が入っています。

```toml
[server]
listen = "0.0.0.0:4540"
data_dir = "/data"
log_level = "info"

[[library]]
name = "Music"
path = "/music"
```

最低限、`[[library]]` を 1 つ書けば動きます。

## 項目

| 項目 | 既定値 | 説明 |
| --- | --- | --- |
| `server.listen` | `"0.0.0.0:4540"` | 待ち受け（`host:port`、IPv6 は `[::]:4540`） |
| `server.data_dir` | `"/var/lib/eos"` | `eos.db`、`secret.key`、`cache/` を置く場所 |
| `server.secret_key` | `<data_dir>/secret.key` | 保存したパスワードを暗号化する鍵。初回に作ります（権限 0600）。DB のバックアップとは別に保管してください |
| `server.log_level` | `"info"` | `debug`・`info`・`warn`・`error` |
| `http.threads` | `0` | HTTP のスレッド数（0 は CPU の数） |
| `http.trusted_proxies` | `["127.0.0.1", "::1"]` | `X-Forwarded-For` を信じるプロキシ |
| `[[library]]` `name`、`path` | — | 音楽のフォルダ（複数可） |
| `scanner.interval` | `"60m"` | 定期スキャンの間隔（`30s`、`15m`、`1h`、`2d`。0 で止める） |
| `scanner.extensions` | `["flac", "mp3", "m4a", "ogg", "opus"]` | 読む拡張子 |
| `scanner.workers` | `2` | タグを読むスレッド数 |
| `scanner.purge_missing_after_days` | `30` | 消えた曲を消すまでの日数 |
| `scanner.artist_separators` | `["; ", " / "]` | `ARTIST` を分ける区切り |
| `pid.track` | `"musicbrainz_trackid\|albumid,discnumber,tracknumber,title"` | 曲の PID（変えると次は全件スキャン） |
| `pid.album` | `"musicbrainz_albumid\|albumartistid,album,albumversion,releasedate"` | アルバムの PID |
| `tags.<名前>.aliases` | 下記 | このタグとして読むタグ名 |
| `tags.<名前>.split` | `["; "]` | 1 つの値を分ける区切り |
| `tags.<名前>.level` | `"track"` | `"album"` にするとアルバムの全曲の値を合わせて使います |
| `tags.<名前>.max_length` | `1024` | 値の最大の長さ |
| `auth.allow_token` | `true` | トークン認証（`t`/`s`）を受け付けるか |
| `auth.allow_password` | `false` | `p=` 認証を受け付けるか |
| `auth.rate_limit_exempt` | localhost とプライベートアドレス | ログイン失敗の制限をしないアドレス（CIDR） |
| `artwork.album_files` | `["cover.*", "folder.*", "front.*"]` | アルバムの画像として探すファイル名 |
| `artwork.cache_max_mb` | `500` | 縮小したジャケットのキャッシュの上限 |
| `subsonic.ignored_articles` | `"The El La Los Las Le Les"` | 索引で無視する冠詞 |
| `subsonic.virtual_albums` | `"hide"` | `"show"` で `/rest` の一覧に仮想アルバムを出します |
| `transcode.ffmpeg` | `"ffmpeg"` | ffmpeg の実行ファイル |
| `transcode.default_format` | `"opus"` | `format` を指定しない変換の形式 |
| `transcode.max_concurrent` | `4` | 同時に変換できる数 |
| `estimate.auto_apply_min_confidence` | `0` | この確信度以上の LLM の推定は承認なしで取り込みます（0 は取り込まない） |
| `suggest.derivative_patterns` | TV Size、TV Ver.、Instrumental、Off Vocal、Karaoke、Remix の正規表現 | 派生の接尾辞 |
| `suggest.medley_separators` | `["〜", "～", " / ", " - "]` | メドレーの曲名の区切り |

### 独自タグの既定

```toml
[tags.collection_work]   # 作品
aliases = ["collection_work"]
split = ["; "]

[tags.collection]        # フォルダ
aliases = ["collection"]
level = "album"
split = ["; "]

[tags.collection_album]  # 仮想アルバム
aliases = ["collection_album"]
split = ["; "]
```

既存のタグ名を使いたいときは `aliases` に足します（例：`aliases = ["collection_work", "soundtrack_for"]`）。

## 環境変数

| 変数 | 効果 |
| --- | --- |
| `EOS_CONFIG` | 設定ファイルの場所 |
| `EOS_SERVER_LISTEN` | `server.listen` を上書き |
| `EOS_SERVER_DATA_DIR` | `server.data_dir` を上書き |
| `EOS_SERVER_LOG_LEVEL` | `server.log_level` を上書き |
| `EOS_LIBRARY_PATH` | ライブラリを「Music」1 つにしてこのパスを使う |

## Docker Compose

`compose.yaml` は次の環境変数を使います。

| 変数 | 既定 | 説明 |
| --- | --- | --- |
| `EOS_MUSIC_DIR` | `./music` | 音楽（コンテナの `/music` に読み取り専用でマウント） |
| `EOS_DATA_DIR` | `./data` | データ（`/data`） |
| `EOS_PORT` | `4540` | ホスト側のポート |
| `EOS_UID`、`EOS_GID` | `1000` | コンテナを動かすユーザー |
| `TZ` | `Asia/Tokyo` | タイムゾーン |

管理は `docker compose exec eos eos-admin …` で行います（`user add`、`apikey create`、`scan`、`tag-report`、`backup`）。
