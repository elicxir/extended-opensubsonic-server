# 引継ぎ：関連楽曲のグラフ

2026-11-27 時点の作業の引継ぎです。ノート PC からデスクトップへ環境を移すために書きました。作業が終わったらこのファイルは消してかまいません。

- ブランチ：`claude/related-songs-graph-feature-2c3e65`（`main` の `36e59c1` から分岐）
- やりたいこと：関連する曲をグラフで表示する。グラフは 2 種類に分ける。
  - **関連グラフ**：楽曲・作品・関係という「事実」のつながり。→ 実装済み
  - **類似グラフ**：音が似ている曲のつながり。→ 試作の途中

## できていること

### 1. 関連グラフの API（`GET /ext/graph`）

- 実装：`server/api/ext/h_graph.cpp`
- 仕様：`docs/ext-api.md` の「`GET /ext/graph?seed=`」
- テスト：`tests/test_ext.cpp` の `GraphAroundASong`、`GraphPeopleAndAlbumsOnRequest`

決めたこと：

- 曲を中心にした部分グラフを返す。全体グラフは返さない。座標も返さない。
- まとまり（楽曲・作品・人物・アルバム）は節にする。曲どうしを直接つなぐのは「関係」だけ。
- 編曲は節にしない。`performance` の辺の属性（`arrangementId` など）に入れる。
- 既定でたどるのは `composition,relation,title`。人物とアルバムは `include` に書いたときだけ。
- 相手が多すぎる節（`fanout`、既定 50 超）は開かず、`degree` だけ返す。クライアントはその節を `seed` にして呼び直す。

テストの状況：

- グラフのテスト 2 件は通る。
- 全体のテストは、最後の修正の前に残り 94 件が通った。修正後に全体は回し直していない。

### 2. サンプルデータ

実ライブラリ（1823 曲）の DB をコピーし、未承認の候補 292 件をすべて承認したものを作った。元の DB は変えていない。

| | 承認前 | 承認後 |
| --- | --- | --- |
| 楽曲 | 4 | 239 |
| 楽曲に属する曲 | 23 | 584 |
| 作品 | 0 | 16（360 曲） |
| 関係 | 0 | 1 |

このデータで `/ext/graph` が動くことを確かめた（既定の呼び出しで 3〜40 ms）。

### 3. 類似の試作（`tools/audio-embed`）

- CLAP の音楽用モデル（`laion/larger_clap_music`、512 次元）で、曲を 1 本のベクトルにする。
- 1 曲から 10 秒の窓を均等に最大 6 か所とり、平均する。
- コマンドは `embed`（計算）、`near`（近い曲の一覧）、`check`（同じ楽曲の別バージョンが近傍の何位に来るか）。
- ノート PC では GPU が使えず（内蔵 Radeon Vega）、CPU で 1 曲 約 3.6 秒。73 曲で中止した。
- **類似の質はまだ見ていない。**

## git に入っていないもの

移行先に自動では届かない。必要なものは手でコピーする。

| もの | 場所（ノート PC） | 移行 |
| --- | --- | --- |
| サンプル DB（承認後） | このワークツリーの `data/graph-sample/eos.db` | コピーする。作り直しもできる（下記） |
| 承認前のコピー | `data/graph-sample/eos.before.db` | 比較用。なくてもよい |
| サンプル用の設定 | `data/graph-sample/eos.toml` | 下に中身を書いた |
| CLAP のモデルと 73 曲分のベクトル | `data/graph-sample/cache/embed/` | 運ばない。移行先で作り直す |
| 実ライブラリの DB | `~/extended-opensubsonic-server/data/eos.db` | 本体の移行に含める |
| 音楽ファイル | `~/navidrome/music` | 本体の移行に含める |
| **メインのチェックアウトの未コミットの変更** | `~/extended-opensubsonic-server`（ブランチ `m5-sync`） | **要注意。下記** |

### メインのチェックアウトの未コミットの変更

`m5-sync` に、コミットしていない変更がある（このブランチには入っていない）。

- 候補の種類 `link_arrangement`（`server/api/ext/suggestions.cpp` など）
- `tools/audio-match/`、`tools/mb-match/`、`docker/tools.Dockerfile`、`docs/tools.md`

サンプル DB の候補はこのコードで承認した。この変更を先にコミットして push しないと、移行先でサンプル DB を作り直せない。

## 移行先でやること

1. `m5-sync` の未コミットの変更をコミットして push する（ノート PC 側で）。
2. このブランチを取得してビルドとテストをする。

   ```bash
   scripts/dev.sh test
   ```

3. サンプル DB を置く。コピーするか、次の手順で作り直す。
   1. 実 DB を SQLite の backup API でコピーする（WAL なので `cp` は使わない）。
   2. `m5-sync` のビルドの `eosd` を、下の設定でコピーに対して動かす。
   3. `eos-admin user add <名前> --admin` で管理者を作り、`/ext/auth/login` で API キーを取る。
   4. `GET /ext/suggestions?status=pending` の ID を全部 `POST /ext/suggestions/batch` の `accept` に入れる。

   ```toml
   # ライブラリのパスは「存在しない」ものにする。起動時のスキャンが
   # そのライブラリで失敗し、曲には触らない（missing にならず、規則も走らない）。
   [server]
   listen = "0.0.0.0:4540"
   data_dir = "/sample"
   log_level = "info"

   [[library]]
   name = "Music"
   path = "/nonexistent-music"

   [scanner]
   interval = "0"
   ```

4. このブランチの `eosd` をサンプル DB で動かし、グラフを引いてみる。

   ```bash
   docker run -d --name eos-graph-try -u $(id -u):$(id -g) -e HOME=/tmp -v $PWD:/src:ro -v $PWD/data/graph-sample:/sample -p 127.0.0.1:14598:4540 eos-dev /src/build/eosd --config /sample/eos.toml
   ```

   試すとよい呼び出し（ID はサンプル DB のもの）：

   - `seed=song:7NdNjBCLJpyXCTdWzorp6h`：「Zufall」。原曲 2 曲とピアノ版が 3 枚のアルバムをまたぐ
   - `seed=song:0URFSMIo1HsZS5ZabuAkmx&depth=3`：「Bloody Tears RMX」。唯一の関係（`derivative`）が出る
   - `seed=composition:0svxeNbgMdMZGKoHQq319S&depth=1`：「Vampire Killer」。17 曲のいちばん大きい楽曲

5. 埋め込みを計算する。

   ```bash
   docker build -t eos-embed tools/audio-embed
   ```

   ```bash
   docker run --rm -u $(id -u):$(id -g) -e HOME=/tmp -v $PWD/data/graph-sample:/sample -v <音楽のフォルダ>:/music:ro -v $PWD/data/graph-sample/cache/embed:/cache eos-embed --db /sample/eos.db --cache /cache/audio-embed embed
   ```

   GPU を使うときに変えるところ：

   - `tools/audio-embed/Dockerfile`：PyTorch を CPU 版（`--index-url …/whl/cpu`）から CUDA 版に替える。
   - `audio_embed.py` の `cmd_embed`：モデルと入力を GPU に載せ（`.to("cuda")`）、結果を `.cpu()` で戻す。
   - `docker run` に `--gpus all` を付ける。

6. `check` と `near` で質を見る（`embed` と同じマウントで、最後のコマンドだけ替える）。

   - `check`：全体の傾向。同じアルバムの曲が上位を占める割合も出る。
   - `near "Zufall" --other-albums`：同じアルバムを除いた近い曲。

## まだ決めていないこと

- **類似グラフの API。** 案は、同じ `/ext/graph` に `include=similar` を足し、節は曲だけ、辺は上位 k 件で `score` 付き。応答の形を関連グラフと同じにして、クライアントの描画を共通にする。
- **類似度の保存。** 案は、曲の組・種類・点数を持つテーブルを足し、ツールが丸ごと入れ替える。候補（`suggestion`）の承認の流れには乗せない。`audio-match` の点数も、種類違いとして同じテーブルに入れられる。
- **同じアルバムの扱い。** 類似では同じアルバムの曲が上位を占めやすい。除くか、減点するか。
- **未承認の候補をグラフに点線で出すか**（`includePending`）。入れていない。
- **`audio-embed` の曲の一覧の取り方。** 今は DB を直接読む。`audio-match` と同じく API 経由にするかは、使えると分かってから。

## 残っている作業

- `client/libeos` に `graph()` を足す。libeos は別のブランチ（`claude/libeos-cpp-api-client-5bb0f9`）にある。kleulir はそれを使う。
- 類似グラフ：質の確認 → 保存の形 → API → テストとドキュメント。

## 候補の承認で見つかった問題（グラフとは別件）

サンプル DB を作るときに見つかった。承認側の問題で、直していない。

- **空の楽曲が 9 件できる。** 一括承認の中で、後の候補が曲を別の楽曲へ移し、殻が残る。
- **「血の涙」が 2 つに割れる。** MusicBrainz に紐付いた 15 曲の楽曲が空になり、「Bloody Tears」（11 曲）と別の「血の涙」（4 曲）に分かれる。曲名による `same_composition` が、MusicBrainz 由来の紐付けを上書きしている。
- **同名のジングルがまとまる。** 「Game Over」13 曲、「Stage Clear」8 曲など。曲名が同じだけの別の曲と思われる（聴いての確認はしていない）。
- **関係がほとんどない。** 全体で 1 件。グラフは「曲 → 楽曲」「曲 → 作品」の辺ばかりになる。
