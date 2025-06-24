# 外部音声処理 AviUtl 拡張編集フィルタプラグイン

AviUtl の拡張編集に、外部のオーディオプラグイン（VST3など）を読み込むためのフィルタオブジェクトを追加するプラグインです。

## 注意事項

無保証です。自己責任で使用してください。
このプラグインを利用したことによる、いかなる損害・トラブルについても責任を負いません。

## 動作要件

- AviUtl 1.10 + 拡張編集 0.92
  - http://spring-fragrance.mints.ne.jp/aviutl
  - 拡張編集 0.93rc1 等の他バージョンでは動作しません。

- Visual C++ 再頒布可能パッケージ（[2015/2017/2019/2022] の x86 対応版が必要）
  - https://learn.microsoft.com/ja-jp/cpp/windows/latest-supported-vc-redist

- patch.aul の `r43 謎さうなフォーク版58` (`r43_ss_58`) 以降
  - https://github.com/nazonoSAUNA/patch.aul/releases/latest

## 導入方法

1. **`External_Audio_Processing.eef`**
    - `aviutl.exe` と同階層にある `plugins` フォルダ内に `External_Audio_Processing.eef` ファイルを入れてください。

2. **ホストプログラムの配置**
    - `aviutl.exe` と同じ階層に `audio_exe` という名前のフォルダを作成してください。
    - 使用したいオーディオプラグイン形式に対応したホストプログラム（例: `VstHost.exe`）を、この `audio_exe` フォルダ内に配置してください。
      - VST3用ホスト: [VST_host](https://github.com/Book-0225/VST_host) のリリースから `VstHost.exe` をダウンロードしてください。

3. **ホストプログラムの関連付け**
    - `audio_exe` フォルダ内に `audio_plugin_link.ini` という名前でテキストファイルを作成します。
    - このファイルに、プラグインの拡張子と、それに対応するホストプログラム名を以下のように記述します。

    ```ini
    [Mappings]
    ; 形式: .拡張子=ホストプログラムの実行ファイル名
    .vst3=VstHost.exe
    ```

## 使い方

- **オブジェクトの追加と設定**
  1. 拡張編集のタイムラインで右クリックし、「メディアオブジェクトの追加」→「フィルタ効果」から「Audio Plugin Host」を追加します。（フィルタオブジェクトとして追加することも可能です）
  2. 設定ダイアログの「プラグインを選択」ボタンを押し、使用したいオーディオプラグインファイル（`.vst3`など）を選択します。
  3. **一度、フィルタを適用した部分をプレビュー再生します。**
      - これにより、対応するホストプログラムがバックグラウンドで起動します。読み込みに時間がかかる場合があるため、10秒ほど待ってから次の手順に進んでください。
  4. 「プラグインGUIを表示」ボタンを押して、プラグインの画面を開き、設定を調整します。
  5. 設定が終わったら、**必ず「プラグインGUIを非表示」ボタンを押してGUIを閉じてください。**
      - **重要**: この操作を行わないと、プラグインの状態がプロジェクトファイルに保存されません。

- **書き出し時**
  1. 開いているプラグインのGUIをすべて「プラグインGUIを非表示」ボタンで閉じます。
  2. 書き出し範囲のプレビューを最初から最後まで再生することをお勧めします。
      - これにより、ホストプログラムの処理が安定し、書き出し時に無音区間が発生するのを防ぎます。
  3. AviUtlの書き出し機能（「ファイル」→「プラグイン出力」など）で動画を出力します。

## 開発者向け情報: 独自ホストプログラムの作成

このプラグインは、標準化されたIPC（プロセス間通信）の仕組みを通じて、さまざまな種類のオーディオプラグインホストと連携できます。独自のホストプログラムを作成する際は、以下の仕様に従ってください。

### 起動コマンドライン引数

ホストプログラムは以下のコマンドライン引数を受け取れるように設計してください。

```"YourHost.exe" -uid <unique_id> -pipe <pipe_base_name> -shm <shm_base_name> -event_ready <event_ready_base_name> -event_done <event_done_base_name>```

- `-uid`: 64bitのユニークID。IPCオブジェクト名の一意性を確保するために使用します。
- `-pipe`, `-shm`, `-event_ready`, `-event_done`: 各IPCオブジェクトのベース名。ホスト側はこれらに `_` と `<unique_id>` を付与して実際のオブジェクト名とします。（例: `\\.\pipe\AviUtlVstBridge_123456789`）

### プロセス間通信（IPC）

- **名前付きパイプ (Named Pipe)**
  - AviUtlプラグインからのコマンド受信と、それに対する応答の送信に使用します。
  - テキストベースのプロトコルで、コマンドは改行文字 (`\n`) で終端されます。
- **共有メモリ (Shared Memory)**
  - オーディオデータの受け渡しに使用します。
  - 構造は以下の通りです。

    ```cpp
    #pragma pack(push, 1)
    struct AudioSharedData {
        double  sampleRate;  // 44100.0, 48000.0 など
        int32_t numSamples;  // このブロックで処理するサンプル数 (最大2048)
        int32_t numChannels; // チャンネル数 (1 or 2)
    };
    #pragma pack(pop)

    // メモリレイアウト
    // [AudioSharedData]
    // [float[2048]] // Input L
    // [float[2048]] // Input R
    // [float[2048]] // Output L
    // [float[2048]] // Output R
    ```

- **イベントオブジェクト (Event)**
  - 処理の同期に使用します。
  - `EVENT_CLIENT_READY`: AviUtlプラグインが共有メモリへのデータ書き込みを完了したことをホストに通知します。
  - `EVENT_HOST_DONE`: ホストがオーディオ処理を完了し、共有メモリへの結果書き込みが終わったことをAviUtlプラグインに通知します。

### コマンドプロトコル（名前付きパイプ経由）

ホストプログラムは以下のコマンドを解釈できるようにしてください。

- `load_plugin "<path>" <sample_rate> <max_block_size>`
  - 指定されたパスのプラグインを読み込み、初期化します。
  - 応答: `OK\n` または `Error: ...\n`
- `load_and_set_state "<path>" <sample_rate> <max_block_size> <state_base64>`
  - プラグインを読み込み、Base64エンコードされた状態でリストアします。
  - 応答: `OK\n` または `Error: ...\n`
- `get_state`
  - 現在のプラグインの状態をBase64エンコードされた文字列で要求します。
  - 応答: `OK <state_base64>\n` または `Error: ...\n`
- `show_gui` / `hide_gui`
  - プラグインのGUIの表示/非表示を切り替えます。
  - 応答: `OK\n` または `Error: ...\n`
- `exit`
  - ホストプロセスを正常に終了させます。
  - 応答: `OK\n`

## 改版履歴

- **v0.1.0**
  - 複数種類のプラグイン形式に対応する汎用的な基盤に刷新。
  - `audio_exe`フォルダと`audio_plugin_link.ini`による、ホストプログラムの動的な選択機構を導入。
  - ファイル選択ダイアログがINIファイルの内容に応じて動的にフィルタを生成するように修正。
  - プラグイン名を「VST3 Host」から「Audio Plugin Host」に変更。
  - **注意 前バージョンまでとは互換性がありません。**
- **v0.0.3**
  - `VSTHost.exe`の更新に合わせ、状態復元コマンド `load_and_set_state` を使うように変更。
  - データのロードタイミングによるクラッシュを修正。
- **v0.0.2**
  - `VSTHost.exe`との連携方法の修正。
- **v0.0.1**
  - 初版。

## License
MIT License

## Credits

### aviutl_exedit_sdk
https://github.com/ePi5131/aviutl_exedit_sdk （利用したブランチは[こちら](https://github.com/sigma-axis/aviutl_exedit_sdk/tree/self-use)です．）

---

1条項BSD

Copyright (c) 2022
ePi All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
THIS SOFTWARE IS PROVIDED BY ePi “AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL ePi BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
