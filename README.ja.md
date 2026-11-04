# DualSense32

[English](README.md) | [日本語](README.ja.md)

PS5 のコントローラ（DualSense）と PS4 のコントローラ（DualShock 4）を
ESP32 から使うための Arduino ライブラリです。
ボタン・スティック・トリガ・タッチパッド・ジャイロ・バッテリが読め、
ライトバーの色と振動が出せます。DualSense ならマイク LED と
アダプティブトリガ（引き金の手応え）も出せます。

**Bluetooth Classic を持たない ESP32-S3 / S2 でも無線で使えます。**
USB ドングルを無線機として直接叩き、その上のホストスタック（HCI・ペアリング・
L2CAP・SDP）をこのライブラリが自前で持っているためです。

どちらのパッドも 3 経路すべてで使えます。どちらが繋がっているかはリンクが判別し、
推測はしません。そのパッドにできない要求は、黙って捨てずに断ります。

> **取扱説明書** — ZIP を展開したところから最初の 1 本が動くまで、図とサンプル付き
> - 日本語: [docs/MANUAL-ja.md](docs/MANUAL-ja.md)。リポジトリ直下の
>   `取扱説明書を開く.html`（ブラウザ）／ `取扱説明書.pdf`（印刷用・A4 31 ページ）も同梱
> - English: [docs/MANUAL-en.md](docs/MANUAL-en.md), or open
>   `docs/DualSense32-manual-en.html` / `docs/DualSense32-manual-en.pdf` locally

---

## 1. どれで繋ぐか

繋ぎ方は 3 通りで、**手持ちのボードで決まります。**

| | ボード | 追加で要るもの | 特徴 |
|---|---|---|---|
| **1_Dongle** | ESP32-S3 / S2 | Bluetooth ドングル（USB クラス `E0/01/01`）＋ USB-C → USB-A 変換 | 無線。ESP32 内蔵の WiFi とは別の無線機 |
| **2_USBDirect** | ESP32-S3 / S2 | USB-C ケーブル（C to C） | 有線。ペアリング不要 |
| **3_BTClassic** | 初代 ESP32 のみ | なし | 無線。手軽 |

ESP32-S3 / C3 / C6 / H2 には **Bluetooth Classic の無線が載っていません**（BLE のみ）。
どちらのパッドも Classic なので、S3 で無線を使うには外付けのドングルが要ります。
逆に初代 ESP32 には Classic がありますが、USB ホスト機能がないのでドングルは使えません。

### ドングルの条件

| 項目 | 条件 |
|---|---|
| USB クラス | `E0/01/01`。これが標準 HCI ドングルの印です |
| 対応 | Bluetooth Classic（BR/EDR）。**BLE 専用は不可** |
| ファームウェア | 内蔵のもの。ホストから流し込んでから動く型は対象外です |
| 電源・速度 | バスパワー 100 mA 程度、フルスピード |
| 形状 | USB-A。S3 は USB-C なので変換が要ります |

箱の Bluetooth 5.x / 6.0 という表記は関係ありません。BLE 専用のものだけ避けてください。

開発に使ったのは **UGREEN Bluetooth 6.0 アダプタ**（Windows のデバイスマネージャ上は
`BARROT Bluetooth 6.0 Adapter`）。ESP32-S3 から見えた値は
`33FA:0012` / class `E0/01/01` / product `UGREEN BT6.0 Adapter` / full speed で、
ファームウェアの転送なしで動きました。

**ドングルは S3 のネイティブ USB 側**に挿します。もう一方は PC に挿したままにしてください
（書き込み・シリアルモニタ・電源）。どちらがネイティブ USB か分からないボードは、
`4_Troubleshooting/WhatIsOnUSB` を書き込んで挿し替えると分かります。

---

## 2. インストール

配布 ZIP（`DualSense32-1.7.0.zip`）を展開して、`DualSense32` フォルダを丸ごと置きます。

| OS | 置き場所 |
|---|---|
| Windows | `ドキュメント\Arduino\libraries\DualSense32\` |
| macOS | `~/Documents/Arduino/libraries/DualSense32/` |
| Linux | `~/Arduino/libraries/DualSense32/` |

`library.properties` が `DualSense32/` の直下にある状態が正しい階層です。
ZIP のまま **スケッチ > ライブラリをインクルード > .ZIP形式のライブラリをインストール**
に渡しても入ります。IDE を再起動すると **ファイル > スケッチ例 > DualSense32** に出ます。

git から入れる場合:

```
cd ~/Arduino/libraries          # Windows は ドキュメント\Arduino\libraries
git clone https://github.com/yuuki1998-gif/DualSense32
```

開発と動作確認は **Arduino ESP32 core 3.3.11** で行っています。

### ボードの設定（ツールメニュー）

| 繋ぎ方 | 設定 |
|---|---|
| 初代 ESP32（3_BTClassic） | **Partition Scheme = Huge APP (3MB No OTA/1MB SPIFFS)**。既定のままだと `text section exceeds available space` で書き込めません |
| ESP32-S3（1_Dongle / 2_USBDirect） | **USB CDC On Boot = Enabled**。シリアルモニタが無反応のときはここ |
| 無線で初めて繋ぐとき | **Erase All Flash Before Sketch Upload = Enabled**。古い鍵が残っていると認証に失敗し続けます |

---

## 3. 最初の 1 本

**ファイル > スケッチ例 > DualSense32 >（自分の繋ぎ方）> Basics** がこれです。

```cpp
#include <DualSense32.h>

DualSense32 pad;                       // ① 用意する

void setup() {
  Serial.begin(115200);
  if (!pad.begin()) Serial.println("begin failed");   // ② 始める
}

void loop() {
  pad.update();                        // ③ 毎回これを呼び出す
  if (!pad.connected()) return;

  if (pad.pressed(DS_CROSS)) Serial.println("cross down");   // ④ 聞く

  if (pad.down(DS_CROSS)) {
    pad.lightbar(255, 0, 0);           // ⑤ 命じる
    pad.rumble(120, 120);
  } else {
    pad.lightbar(0, 160, 255);
    pad.rumble(0, 0);
  }
}
```

**3 経路の違いは 1 行目だけです。** 下のコードは 1 文字も変わりません。

```cpp
DualSense32 pad;                            // 自動。初代 ESP32 なら内蔵無線、S3/S2 ならドングル
DualSense32 pad(ds_link_usb_hid());        // 有線（S3）  + #include <ds_link_usb_hid.h>
```

**無引数がふつうの書き方です。** `#include <DualSense32.h>` だけで済みます。
初代 ESP32 には Bluetooth Classic があって USB ホストが無い、S3 / S2 はその逆なので、
チップが決まれば無線の経路は 1 通りしかありません。ライブラリがそれを選びます。

**明示が要るのは有線（S3）だけです。** S3 ではドングルもパッドも同じ USB ポートに
挿さるので、どちらのつもりかはコンパイル時には分かりません。既定はドングルです。

経路を明示したいときは、いつでも書けます。

```cpp
DualSense32 pad(ds_bt_bluedroid());        // 初代 ESP32  + #include <ds_bt_bluedroid.h>
DualSense32 pad(ds_link_usb_hci());        // ドングル    + #include <ds_link_usb_hci.h>
```

`update()` を書き忘れると、ボタンを押しても反応せず、ライトバーの色も変わりません。
エラーは出ません。通信は裏のタスクが回しているので、そのまま読むと
「スティックは今のフレーム、ボタンは前のフレーム」という混ざった値になります。
`update()` はその瞬間をコピーし、設定した出力もまとめて送ります
（`send()` を自分で書く必要はありません）。

**ペアリング**は 1 回目だけ、**CREATE**（DualShock 4 は **SHARE**）**を押したまま
PS** を押し、ライトバーが速く点滅したら離します。2 回目以降は PS ボタンだけです。
どちらのパッドも**ホストを 1 台しか覚えません**。

### どちらのパッドか

DualShock 4 にはアダプティブトリガもマイクもミュートボタンもプレイヤー LED も
ありません。逆に DualSense に無いものが 1 つあり、パッド自身がライトバーを
点滅させられます（`lightbarBlink(on, off)`、10 ms 単位）。
タッチパッドは DualSense が 1920 x 1080、DualShock 4 は 1920 x 942 です。
書き方は 2 通りあります。

`DualSense32` は繋がったパッドを実行時に見分けるので、1 つのバイナリで
どちらでも動きます。

```cpp
if (pad.supports(DS_CAP_ADAPTIVE_TRIGGERS)) pad.triggerRight(block);
```

任意のハードを触る setter（`triggerLeft` `triggerRight` `micLed`
`playerLeds` `playerLedsMask` `lightbarBlink`）は `bool` を返し、
そのパッドにできないときは false を返して何もしません。断った回数は数えていて、
`printEvents()` が 1 件につき 1 回だけ報告します。組み立ての段で落ちたフィールドは、
届いたフィールドと外から見分けがつかないからです。

型でパッドを名指しすると、確認がコンパイル時に移ります。

```cpp
#include <DualShock4.h>

DualShock4 pad(ds_link_usb_hid());
```

無いハードは `= delete` してあるので、`pad.triggerRight(b)` `pad.micLed(2)`
`pad.playerLeds(...)` はビルドエラーになり、どの呼び出しかがエラーに出ます。
ボタンはふつう `DS_CROSS`、`DS_CREATE` などの共通名で書けます。Sony がパッドに印刷した名前を使いたいときだけ、`DS4_SHARE` などの別名も使えます。`DS4_MUTE` は存在しないので、
ミュートボタンを取りにいくと `error: 'DS4_MUTE' was not declared in this scope`
になります。`DualShock4` 型に `DS5_CROSS` のような DualSense 専用の別名を書くとビルドエラーです。共通ボタンは `DS_CROSS` と書きます。
`connected()` は本当に DualShock 4 が繋がっているときだけ真になり、
それ以外が繋がっているときは `mismatched()` が真になります。

`begin()` はこの型なら **DualShock 4 を想定して**始まります。書くことは増えません。
MAC 指定もそのまま使えます。

```cpp
pad.begin();                        // 繋がりに来た DualShock 4
pad.begin("AA:BB:CC:DD:EE:FF");     // その 1 台
```

素の `DualSense32` で書くときは `begin()` に渡せます。

```cpp
DualSense32 pad(ds_bt_bluedroid());
pad.begin(DS_DEVICE_DUALSHOCK4);                          // 想定を伝える
pad.begin(DS_DEVICE_DUALSHOCK4, "AA:BB:CC:DD:EE:FF");     // 想定 ＋ MAC
pad.begin();                                              // 従来どおり任せる
```

**これは「想定」であって「上書き」ではありません。** Bluetooth ではパッドが最初の
フルレポートを送るまで名乗らないので、その間に出す**出力**がどちらの形式か決まりません。
想定を伝えておけば 1 フレーム目から正しく出せます。伝えていない場合、出力は捨てずに
**名乗るまで保留**されます（設定は消えません）。

リンクが本当に判別したら想定は必ず負けます。違っていれば `device()` がそう言い、
`DualShock4::mismatched()` が真になります。プロダクト ID そのものを上書きしたい
（クローンなど）ときは `ds_link_usb_hid_force_device()` の方です。

入力は想定に関係なく正しく読めます。判別待ちの間に流れるレポート `0x01` は
両パッドでレイアウトが同じだからです（ライブラリが `0x01` からモデルを推測しないのも
これが理由）。

---

## 4. 早見表

```cpp
pad.down(DS_CROSS)          // 押されている間ずっと
pad.pressed(DS_CROSS)       // 押した瞬間だけ
pad.released(DS_CROSS)      // 離した瞬間だけ

pad.leftX()  pad.leftY()     // -128〜127（Y は下が正）
pad.rightX() pad.rightY()
pad.l2()     pad.r2()        // 0〜255
pad.battery()                // 0〜10
pad.raw().touch[0].x         // タッチパッド 0〜1919 / 0〜1079（DS4 は 0〜941）
pad.gyroYaw()  pad.accelZ()  // deg/s と G（校正済み・そのまま使える）

pad.supports(DS_CAP_ADAPTIVE_TRIGGERS)   // このパッドにできること
pad.isDualShock4()                       // どちらのパッドか

pad.lightbar(r, g, b);       // 各 0〜255
pad.rumble(left, right);     // 各 0〜255
pad.lightbarBlink(on, off);  // DualShock 4 のみ。10 ms 単位
pad.playerLeds(10101);       // 光らせたい形をそのまま
pad.micLed(1);               // 0 消灯 / 1 点灯 / 2 点滅

uint8_t b[DS5_TRIGGER_BLOCK_SIZE];
ds5_trigger_weapon(b, 3, 6, 8);        // 銃の引き金。効果は全 10 種（DualSense のみ）
pad.triggerRight(b);

pad.deadzone(8);             // スティックの遊び
pad.address(buf);            // 繋がっている相手の MAC
pad.printEvents(Serial);     // 変化したものだけ 1 行ずつ
```

ボタン名の一覧、タッチパッドの座標系、トリガ効果 10 種の引数は取説の 6〜7 章にあります。

---

## 5. サンプル

```
1_Dongle/         ESP32-S3 / S2 ＋ ドングル
2_USBDirect/      ESP32-S3 / S2 ＋ ケーブル直結
3_BTClassic/      初代 ESP32
    Basics/       繋がったことを確かめる最短のスケッチ
    ReceiveData/  パッドが返す値を全部表示
    SendData/     こちらから出せるものを全部
    DualShock4/   PS4 パッドと、違うところ（3 経路すべてにあります）
    DualShock4Only/  同じ内容を型で名指しする版（2_USBDirect のみ）
    EitherPad/    1 本のバイナリで両方のパッド。supports() で分岐（2_USBDirect のみ）
    Ds4Lightbar/  DS4 が自分で点滅させるライトバーを状態表示に使う（2_USBDirect のみ）
    Ds4Touchpad/  タッチパッドを実際に使う。2 点タッチとドラッグ（2_USBDirect のみ）
4_Troubleshooting/  DongleStepByStep / WhatIsOnUSB / EventLog / Ds4PairingInfo
5_SelfTest/         パッドもドングルも不要の自己テスト
```

繋ぎ方ごとのスケッチは、コンストラクタの 1 行以外は同じです。
`2_USBDirect/FeatureReports` と `4_Troubleshooting/Ds4PairingInfo` だけは例外で、
パッドの feature report を読むのは有線でしかできません。

---

## 6. 困ったとき

| 症状 | 原因 | 対処 |
|---|---|---|
| `text section exceeds available space` | Partition Scheme が既定のまま | **Huge APP** にする（初代 ESP32） |
| シリアルモニタに何も出ない | USB CDC On Boot が無効 | **Enabled** にする（S3） |
| ペアリングモードにしても繋がらない | ESP32 が古い鍵を覚えている | **Erase All Flash = Enabled** で書き込み直し、パッドは CREATE + PS |
| 繋がった直後に切れる | 両側の鍵が食い違っている | 上と同じ。**両方**忘れさせる |
| 別のパッドがいつまで待っても見つからない | ボンドが残っている間はインクワイアリを意図的に止めているため。覚えている相手はパッドの方からページしてくるので、こちらが探していると応答できない | `ds_link_usb_hci_clear_bond()` してからリセット |
| アダプティブトリガやマイク LED が効かない | DualShock 4 にはどちらも無い | 呼ぶ前に `pad.supports(...)` で聞く。`printEvents()` が断った旨を出す |
| ボタン 1 回で何十回も反応する | `down()` を使っている | `pressed()` に変える |
| ライトバーも振動も効かない | `update()` を呼び出していない | `loop()` の先頭で `pad.update()` |
| 誰も触っていないのに動く | デッドゾーンが足りない | `pad.deadzone(12)` などに上げる |

残りは取説の 9 章に。診断用のスケッチは `4_Troubleshooting/` にあります。

---

## 7. 中身

各層は独立していて、上から使っても途中から使ってもかまいません。

```
    your sketch
        |
    ds_transport_t              1 つの継ぎ目、3 つのリンク
        |
   +----+--------------------+---------------------+
   |                         |                     |
ds_bt_bluedroid      ds_link_usb_hci      ds_link_usb_hid
 (初代 ESP32)           ds_l2cap  ds_sdp    (有線)
        |               ds_hci_host              |
        |               ds_hci                   |
        |               ds_usb_hci           ds_usb_hid
        |                    |                    |
        +--------------------+--------------------+
                             |
                       ds_protocol      レポートの解釈と組み立て
```

下に第三者の Bluetooth スタックはありません。`ds_link_usb_hci` がドングルの
HCI を直接叩き、その上の L2CAP と SDP も自前です。
`ds_protocol` `ds_hci` `ds_hci_host` `ds_l2cap` `ds_sdp` は `string.h` 以外に依存せず、
ブロックせず、確保せず、時計も読みません。だから接続とペアリングの全会話を
ハードウェアなしで机上で再生できます。

どちらのパッドかはリンクが決めて下へ渡します。レポートからの推測はしません。
USB ではプロダクト ID から決まります。両パッドとも入力レポート 0x01 の 64 バイトで
答えるので、レポートだけでは区別がつかないためです。Bluetooth にはプロダクト ID が
無いので、入力レポート ID で決めます。0x31 が DualSense、0x11 が DualShock 4 です。
0x01 は何も示しません。フィーチャレポートを要求するまでは両方が送ってくる ID なので、
これで推測することは意図的にしていません。認識するプロダクト ID は
0x05C4（CUH-ZCT1）・0x09CC（CUH-ZCT2）・0x0BA0（USB アダプタ）で、
クローンなどは `ds_link_usb_hid_force_device()` で上書きできます。

C の API がそのまま本体で、C++ クラスはその上の数百行です。
独自のリンクを書く、HCI の会話に触る、C++ が使えない、といった場合は C 側を直接どうぞ。

### 検証

| テスト | アサーション | ハード要否 |
|---|---|---|
| `5_SelfTest/ProtocolCore` | 57 | 不要 |
| `5_SelfTest/Ds4Core` | 162 | 不要 |
| `5_SelfTest/HciPackets` | 133 | 不要 |
| `5_SelfTest/HciHost` | 317 | 不要 |
| `5_SelfTest/L2cap` | 177 | 不要 |
| | **846 / 全通過** | |

期待値は Linux の `hid-playstation.c` や仕様書から**別途書き起こした**もので、
このライブラリの出力から作っていません。写し間違いは両側に焼き付かず、
テストの失敗として出ます。実機では ESP32-S3-DevKitC-1 ＋ ドングルで毎秒約 390 レポート、
送信失敗なし。根拠は [VERIFICATION.md](VERIFICATION.md) に。

DualShock 4 は ESP32-S3 で、USB 直結とドングル経由の両方を CUH-ZCT2 で通しました。
加速度の大きさは 1.001 G、静止時のジャイロは各軸 0.5 deg/s 以内。これは DualShock 4
側のオフセットで読み、パッドが名乗った時点でキャリブレーションを読み直していなければ
成立しない値です。初代 ESP32 の Bluedroid 経路はコードは入っていますが、
実機での確認はまだです。

### 罠になりやすいところ

**ジャイロは校正すると単位が変わります。** 生値はパッドごとに違いますが 1 deg/s あたり
おおよそ 16 カウント、校正後は正確に 1024 カウントです。加速度は校正前後どちらも
8192 LSB per g で変わらないので、この非対称さが取り違えの元になります。
`pad.gyroYaw()` と `pad.accelZ()` はすでに割り算済みです。

**Bluetooth 出力のシーケンスタグは CRC の内側にあります。** `ds_output_t::sequence`
は組み立てる前に設定してください。組み立てた後のバッファに直接書き込むと署名が壊れ、
以後のフレームがすべて無視されます。

**USB 経路は全部ポーリングです。** `loop()` の中でただの `delay()` を使うとパイプが止まり、
ドングル経路では Bluetooth も止まります。待つ間は `tr->poll(tr)` を回してください。

---

## ライセンス

Copyright 2026 yuuki1998 — [PolyForm Noncommercial License 1.0.0](LICENSE)

**非商用利用に限ります。** 個人の学習・趣味・研究、学校やその他の非営利団体
（例: 学校のロボコン部が学生大会に出る）での利用・改変・再配布ができます。
商用での利用はこのライセンスの対象外です。必要な場合は作者に別途ご相談ください。
再配布するときは [LICENSE](LICENSE)、[NOTICE](NOTICE)、
[LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) を一緒に配ってください。

`src/stack/` と `src/osi/` のヘッダだけは例外で、Espressif の Bluedroid 内部からの抜粋
（Copyright Broadcom Corporation / Google Inc.）です。こちらは
[Apache-2.0](LICENSES/Apache-2.0.txt) のままで、
それぞれの表示を保持しています。使うのは初代 ESP32 用の `ds_bt_bluedroid.c` だけで、
ドングル経路と有線経路は触りません。

出典は Bluetooth Core Specification（L2CAP は Vol 3 Part A、HCI は Vol 4 Part B / E）、
USB HID 仕様、Linux の `drivers/hid/hid-playstation.c`。
アダプティブトリガのビット詰めは Nielk1 の TriggerEffectGenerator が記述した形式に従い、
実装は独立に書いています。食い違ったところは実機で決めました。

DualSense、DualShock、PlayStation は Sony Interactive Entertainment Inc. の商標です。
本プロジェクトは Sony とは無関係で、承認も受けていません。
