# タグと ID の約束

サーバーは音楽ファイルのタグを読み、ライブラリを作ります。ファイルのタグを書き換えることはありません。手作業で入れた情報はデータベースに保存し、タグより優先します。

## 読むファイル

- 対象の拡張子：`flac mp3 m4a ogg opus`（設定 `scanner.extensions`）。
- 名前が `.` で始まるファイルとフォルダは読みません。
- `.ndignore` があるフォルダは、その下を丸ごと読みません（中のパターン指定にはまだ対応していません）。

## 標準のタグ

Vorbis Comment の名前で書きます。MP3（ID3v2）や MP4 のタグは、TagLib が同じ名前に読み替えます。

| 意味 | タグ | 備考 |
| --- | --- | --- |
| 曲名 | `TITLE` | ないときはファイル名 |
| サブタイトル | `SUBTITLE` | |
| アーティスト | `ARTISTS`、なければ `ARTIST` | `ARTIST` は `; ` と ` / ` で分けます（設定 `scanner.artist_separators`） |
| アルバム | `ALBUM` | ないときは `[Unknown Album]` |
| アルバムアーティスト | `ALBUMARTISTS`、なければ `ALBUMARTIST` | ないときはアーティスト |
| 作曲・作詞・編曲・指揮 | `COMPOSER`、`LYRICIST`、`ARRANGER`、`CONDUCTOR` | |
| 演奏者 | `PERFORMER` | `名前 (楽器)` の形なら、楽器を役割の補足（`subRole`）にします |
| ディスク・トラック番号 | `DISCNUMBER`、`TRACKNUMBER` | `1/12` の形も読みます |
| ディスクのタイトル | `DISCSUBTITLE` | |
| 日付 | `RELEASEDATE` または `DATE`、`ORIGINALDATE` | 先頭 4 桁を年にします |
| ジャンル | `GENRE` | `;` で複数 |
| 読み（ソート用） | `TITLESORT`、`ALBUMSORT`、`ARTISTSORT`、`ALBUMARTISTSORT` | 検索でも読みとして使います |
| コンピレーション | `COMPILATION` | `1` のとき |
| MusicBrainz | `MUSICBRAINZ_TRACKID`（録音）、`MUSICBRAINZ_RELEASETRACKID`、`MUSICBRAINZ_ALBUMID`、`MUSICBRAINZ_ARTISTID`、`MUSICBRAINZ_ALBUMARTISTID`、`MUSICBRAINZ_WORKID` | |
| ReplayGain | `REPLAYGAIN_TRACK_GAIN`、`REPLAYGAIN_TRACK_PEAK`、`REPLAYGAIN_ALBUM_GAIN`、`REPLAYGAIN_ALBUM_PEAK` | `-6.54 dB` の形 |
| ReplayGain（Opus） | `R128_TRACK_GAIN`、`R128_ALBUM_GAIN` | Q7.8 の整数。ReplayGain 2.0 の基準（-18 LUFS）に換算します：`値 / 256 + 5` dB |

## 独自のタグ

| 意味 | タグ（既定） | 単位 | 値 |
| --- | --- | --- | --- |
| 作品（サントラの元の映画やゲーム） | `COLLECTION_WORK` | 曲 | 複数可（`; ` 区切り） |
| フォルダ | `COLLECTION` | アルバム | 複数可。`/` で入れ子（例：`サントラ/シリーズA`）。アルバムの全曲の値を合わせて使います |
| 仮想アルバム | `COLLECTION_ALBUM` | 曲 | 複数可。値ごとに仮想アルバムを作り、その曲を入れます |
| 楽曲 | `WORK` または `MUSICBRAINZ_WORKID` | 曲 | どちらかがあれば楽曲が決まります（MBID が優先） |
| 編曲の名前 | `ARRANGEMENT` | 曲 | 例：`Piano Ver.`。名前から種類（piano・orchestra・band・acoustic・vocal・other）を推測します |
| 演奏の種類 | `PERFORMANCE_TYPE` | 曲 | `live` または `studio` |
| 収録日 | `RECORDINGDATE` | 曲 | |

- 独自タグの名前は設定の `[tags.<名前>]` で変えたり別名を足したりできます（[configuration.md](configuration.md)）。
- 楽曲が決まった曲は、`ARRANGEMENT` と `ARRANGER` の組で編曲に分かれます。両方ないときは、その楽曲の「原曲」（暗黙の編曲）に入ります。

## ID

- ID は 22 文字の英数字（128 ビットの base62）です。ユーザーとライブラリだけは整数です。
- 曲とアルバムの ID は、最初に見つけたときの PID（タグから計算した値）で、以後は変えません。
  - 曲の PID：`musicbrainz_trackid|albumid,discnumber,tracknumber,title`
  - アルバムの PID：`musicbrainz_albumid|albumartistid,album,albumversion,releasedate`
  - `|` の左から、全部の値がそろう最初の組を使います。値は正規化してからハッシュします。
  - 同じアルバムでも、曲ごとに日付が違うと別のアルバムになります（Navidrome と同じ）。
- 次のときは同じ ID を引き継ぎます。
  1. ファイルを移動した、名前を変えた（同じ PID の曲が消えている）
  2. タグを直してから移動した（FLAC の音声の MD5 が一致する曲がひとつだけ消えている）
  3. アルバムのタグを直した（そのアルバムの曲が全部、新しいアルバムに移った）
- 消えたファイルの曲は `missing` になり、30 日（設定 `scanner.purge_missing_after_days`）後に消えます。それまでに戻れば同じ ID のままです。
- タグから作る作品・フォルダ・楽曲・編曲・仮想アルバムの ID も、名前などから決まります。消えて同じタグで戻ってきても同じ ID になります。手作業で作ったものはランダムな ID です。

## 情報の出どころ（provenance）と優先順位

すべての紐付けは出どころを持ち、強いものが勝ちます。

| 値 | 出どころ | API での表示 |
| --- | --- | --- |
| 3 | 手作業（`/ext` での編集、修正してから承認した候補） | `manual` |
| 2 | タグ | `tag` |
| 1 | 承認された推定（規則による候補、LLM の推定） | `rule` / `llm` |

- 1 対 1 の紐付け（曲 → 編曲）は、出どころがいちばん強い行を使います。
- 多対多の紐付け（曲 ↔ 作品、アルバム ↔ フォルダ、仮想アルバム ↔ 曲）は、全部の和集合から、手作業で「除外」にしたものを除きます。
- 参加者（作曲者など）は、対象と役割の組ごとに、いちばん強い出どころの集まりを使います。
- スキャンが書き換えるのはタグ由来（2）の行だけです。手作業（3）と推定（1）は、スキャンしても残ります。
- 曲名・読み・アルバム名などの上書き（手作業）は、タグが変わっても残ります。上書きを消すとタグの値に戻ります。

## 正規化

検索、PID、名前の比較で同じ処理を使います。

1. NFKC と大文字小文字の統一
2. カタカナをひらがなに（「ー」は残す）
3. 空白、句読点、記号を取り除く

例：`「Final Fantasy」〜ピアノ版〜` → `finalfantasyぴあの版`
