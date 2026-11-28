# libeos

eos サーバーのクライアント用ライブラリです。UI は持ちません。デスクトップ（kleulir）と Pi のバックエンドが同じものを使います。サーバーの約束事は [docs/](../../docs/README.md) にあります。

今あるのは `Api`（`/rest` と `/ext` の呼び出し）です。同期（`SyncClient`）、キュー、mpv の制御、シャッフルはこのあと足します。

## 使い方

```cpp
#include <eos/client/api.h>
using namespace eos::client;

// 初回：ユーザー名とパスワードを、この端末用の API キーに替える
auto login = Api::login({.base_url = "http://nas:4540"}, "alice", password, "desktop");

Api api({.base_url = "http://nas:4540", .api_key = login.key, .client_name = "kleulir"});
for (const Album& a : api.album_list({.type = "newest", .size = 50})) { … }
Album album = api.album(id);                       // 曲つき
std::string url = api.stream_url(album.songs[0].id);  // mpv に IPC で渡す

// 関連する曲のグラフ（節と辺）。開いていない節（expanded == false）は、その id を seed にして呼び直す
Graph g = api.graph({.seed = "song:" + album.songs[0].id});
Graph more = api.graph({.seed = g.nodes[1].id, .depth = 1});

// 曲の詳細：楽曲・編曲・作品の名前と出どころ、ファイル、タグ
SongDetail d = api.song_detail(album.songs[0].id);
```

- eos ではない OpenSubsonic サーバー（Navidrome など）には `Api::token_login()` で入ります。返ってきた `username`、`token`、`salt` を `Options` に入れると、`/rest` の呼び出しがトークン認証（`u`、`t`、`s`）で通ります。`/ext` はありません。
- 呼び出しはサーバーの応答を待ちます。UI スレッドではなく作業用のスレッドから呼んでください。
- 1 つの `Api` を複数のスレッドから同時に使えます。接続は開いたまま使い回します。
- 失敗は `eos::client::Error` を投げます。`offline()`（つながらない）、`unauthorized()`、`forbidden()`、`not_found()` で場合分けできます。
- `stream_url()` などの URL は API キーを含みます。コマンドライン引数やログに出さないでください。
- まだ関数にしていない `/ext`（編集、候補、書き出し）は `ext_request()` で JSON のまま呼べます。
- 公開ヘッダーは標準ライブラリだけに依存します（libcurl と nlohmann/json は中で使うだけです）。

## アプリに組み込む

eos のリポジトリをサブモジュールなどで置き、CMake から足します。

```cmake
add_subdirectory(third_party/eos/client/libeos)
target_link_libraries(kleulir PRIVATE eos::client)
```

必要なもの：C++20、CMake 3.21 以上、libcurl 8.11 以上、nlohmann/json 3。

## ビルドとテスト

リポジトリの `scripts/dev.sh test` で、サーバーと一緒にビルドしてテストします。

| テスト | 内容 |
| --- | --- |
| `libeos_tests` | JSON の読み取りと URL の組み立て。サーバーは要りません |
| `libeos_live`（`tests/live.sh`） | 3 曲のライブラリを作って `eosd` を立て、全部の呼び出しを実際に通します |

手元のサーバーに向けるときは、`EOS_TEST_URL`、`EOS_TEST_USER`、`EOS_TEST_PASSWORD` を付けて `libeos_live_tests` を直接動かします（`live.sh` のライブラリを前提にしたテストは失敗します）。
