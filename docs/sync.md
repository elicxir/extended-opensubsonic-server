# 同期：共有リモコン（`/ext/session`）

サーバーはユーザーごとに 1 つのキューと再生状態を持ちます。各端末はそれを共有するリモコンとして振る舞います。音を鳴らすのは「再生端末」1 台だけで、ほかの端末は表示と操作だけをします。同時に操作したときは後から届いたものが勝ち、再生位置は数秒ずれてもかまいません。

## 接続

- `GET /ext/session` を WebSocket に切り替えます。認証は `X-Api-Key` ヘッダー（または `?apiKey=`）です。
- 認証に失敗すると、接続のあとに `{"type": "error", "message": "Unauthorized"}` を送って閉じます（クローズコード 1008）。
- メッセージはすべて JSON のテキストフレームで、`type` を持ちます。
- サーバーは 30 秒ごとに ping を送ります。
- 最初に `hello` を送ってください。それまでのほかのメッセージには `error` を返します。

## キューの項目

キューの各項目は `{"entryId", "trackId"}` です。`entryId` はサーバーが振ります。並べ替えや削除は `entryId` で指すので、同じ曲が 2 回入っていても取り違えません。曲の詳しい情報は `/rest/getSong` や `/ext/songs/{id}` で取ってください。

## 端末 → サーバー

| `type` | 項目 | 説明 |
| --- | --- | --- |
| `hello` | `deviceId`（1〜128 文字、端末ごとに固定）、`deviceName?`、`client?` | 端末を登録します。`deviceId` は 1 人のユーザーのものです（別のユーザーが使うと `error`） |
| `queue.edit` | `ops`（下記） | 届いた順にそのまま適用します |
| `control` | `action` と値（下記） | リモコンの操作 |
| `progress` | `entryId?`、`positionMs`、`playing`、`volume?`、`muted?` | 再生端末が今の状態を知らせます。10 秒ごとと、曲の切り替え、シーク、一時停止、音量の変更のとき |
| `transfer` | `targetDeviceId` | 再生端末を切り替えます。相手は接続中でなければなりません |

### `queue.edit` の `ops`

| `op` | 項目 | 説明 |
| --- | --- | --- |
| `replace` | `tracks`、`currentIndex?`（既定 0）、`positionMs?` | キューを丸ごと置き換えます |
| `insert` | `tracks`、`afterEntryId?` または `index?` | 既定は末尾。`afterEntryId` の項目がもうなければ、この操作は捨てます |
| `remove` | `entryIds` | 存在しない ID は無視します |
| `move` | `entryId`、`toIndex` | |
| `clear` | | |

- 存在しない曲（`trackId`）は入れません。
- 再生中の項目を消すと、その位置に来た次の項目が再生中になり、位置は 0 に戻ります。最後の項目を消したときは前の項目です。
- 再生中の項目がないときに `insert` すると、入れた最初の項目が再生中になります。
- 何も変わらなかった編集は配りません。

### `control` の `action`

| `action` | 値 | 説明 |
| --- | --- | --- |
| `play`、`pause` | | 再生中の項目がなければ先頭から |
| `seek` | `positionMs` | |
| `jump` | `entryId` | その項目の頭から |
| `next` | | 最後の項目なら停止します |
| `previous` | | 3 秒より後ろなら同じ曲の頭へ、それ以前なら前の曲へ |
| `volume` | `volume`（0〜100） | 再生端末の音量 |
| `mute` | `muted?`（省略で反転） | 再生端末の消音 |

再生端末がないときの `volume` と `mute` は `error` です。

## サーバー → 端末

| `type` | 項目 | 送るとき |
| --- | --- | --- |
| `state` | `queue`、`revision` と下の再生状態の項目すべて | `hello` の直後と、キューが変わったとき（全端末へ） |
| `playback` | `revision`、`currentEntryId`、`positionMs`、`playing`、`activeDeviceId`、`volume`、`muted`、`at` | `control`、`progress`、`transfer` のたび（全端末へ） |
| `devices` | `devices`：`[{"id", "name", "client", "connected", "active", "volume", "muted"}]` | 端末の接続・切断、`transfer` のとき |
| `error` | `message` | 送った端末へだけ |

- `revision` はキューの中身が変わるたびに 1 増えます。
- 再生位置の表示は、`playing` が `true` なら `positionMs + (今のサーバー時刻 − at)` で進めます。`at` は UNIX ミリ秒のサーバー時刻です。
- `volume` と `muted` は再生端末のものです。再生端末がないときは `null` です。

## 決まり

- 競合は扱いません。`queue.edit` も `control` も届いた順に適用し、結果を全端末に配ります。
- `progress` は再生端末のものだけを使い、ほかの端末からのものは黙って捨てます。再生端末が決まっていないときは、最初に `progress` を送った端末が再生端末になります。
- 再生端末が切断しても、再生端末のままです。ほかの端末には `devices` で「切断中」と見えます。
- キューは保存してから配ります。再生位置は一時停止・切り替え・再生端末の切断のときと、再生中は 30 秒ごとに保存します。
- REST の `savePlayQueue`・`savePlayQueueByIndex` も同じキューを置き換え、接続中の端末に `state` を配ります。`getPlayQueue` は再生端末が報告した最新の位置を返します。

## 再生端末の切り替え

1. どれかの端末が `transfer`（`targetDeviceId`）を送ります。
2. サーバーは、今の再生端末が最後に報告した位置を引き継いで再生端末を替え、`playback` と `devices` を配ります。
3. 前の再生端末は、自分が再生端末でなくなったのを見て音を止めます。
4. 新しい再生端末は、`currentEntryId` を `positionMs` から鳴らし、すぐに `progress` を送ります。数秒戻ったり飛んだりしてもかまいません。

## 切断と再接続

- 切断中の端末は、手元のキューで再生を続けます。
- 再接続したら `hello` を送り、受け取った `state` に合わせます。
- まだ再生端末なら、鳴らし続けていた位置を `progress` で送ります。それが新しい位置になります。再生端末でなくなっていたら音を止めます。

## 音量

- 音量（0〜100）と消音は端末ごとに持ち、サーバーに保存します。端末を再起動したり再接続したりしても、前の音量に戻ります。
- リモコンからは `control` の `volume`・`mute` で再生端末の音量を変えます。
- 再生端末側でボタンやつまみで変えたときは、`progress` に `volume`・`muted` を入れて知らせます。
- 再生端末を切り替えると、切り替え先の端末の音量になります。
- その値を mpv の音量に当てるか、ALSA のミキサーに当てるかは、クライアントが決めます。

## 再生回数

- 音を鳴らした端末（再生端末）が `/rest/scrobble` を呼びます。リモコン役の端末は呼びません。
- 何秒聴いたら 1 回とみなすかは、クライアントが決めます。サーバーは `progress` から回数を数えません。
- 切断中に再生した分は、手元に記録しておきます。再接続したら、`time`（再生した時刻、ミリ秒）を付けてまとめて送ります（`id` と `time` は複数送れます）。
- 曲が始まったら `submission=false` を送ってもかまいません。今のサーバーはこれを記録しません。
- 既存のクライアント（Feishin など）は自分で `scrobble` を呼ぶので、この約束と合います。

## ReplayGain

- 音量の調整はクライアントがします。サーバーは配信の音量を変えません。
- 変換しない配信（FLAC など）では、mpv の `--replaygain` がファイルのタグを使えます。
- 変換した配信にはタグが付きません。曲情報の `replayGain`（`trackGain`、`albumGain`、`trackPeak`、`albumPeak`）を使い、mpv の音量（`volume-gain` など）に反映してください。
- トラック単位かアルバム単位かはクライアントが選びます。仮想アルバムには独自のアルバム値がないので、トラック単位を使います（`albumGain` は物理アルバムの値です）。

## 例

```
→ {"type":"hello","deviceId":"pi-livingroom","deviceName":"Pi","client":"eos-pi/0.1"}
← {"type":"state","revision":3,"queue":[{"entryId":"E1","trackId":"T1"},{"entryId":"E2","trackId":"T2"}],
   "currentEntryId":"E1","positionMs":42000,"playing":false,"activeDeviceId":"pi-livingroom",
   "volume":60,"muted":false,"at":1795750000000}
← {"type":"devices","devices":[{"id":"pi-livingroom","name":"Pi","client":"eos-pi/0.1",
   "connected":true,"active":true,"volume":60,"muted":false}]}
→ {"type":"control","action":"play"}
← {"type":"playback","revision":3,"currentEntryId":"E1","positionMs":42000,"playing":true,
   "activeDeviceId":"pi-livingroom","volume":60,"muted":false,"at":1795750001000}
→ {"type":"progress","entryId":"E1","positionMs":52000,"playing":true}
→ {"type":"queue.edit","ops":[{"op":"insert","afterEntryId":"E1","tracks":["T9"]}]}
← {"type":"state","revision":4,"queue":[…],…}
```
