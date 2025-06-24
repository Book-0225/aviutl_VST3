# 外部音声処理 AviUtl 拡張編集フィルタプラグイン

AviUtl の拡張編集に「VST3 Host」フィルタ効果とフィルタオブジェクトを追加するプラグインです。

## 注意事項

無保証です。自己責任で使用してください。
このプラグインを利用したことによる、いかなる損害・トラブルについても責任を負いません。

## 動作要件

- AviUtl 1.10 + 拡張編集 0.92

  http://spring-fragrance.mints.ne.jp/aviutl
  - 拡張編集 0.93rc1 等の他バージョンでは動作しません。

- Visual C++ 再頒布可能パッケージ（\[2015/2017/2019/2022\] の x86 対応版が必要）

  https://learn.microsoft.com/ja-jp/cpp/windows/latest-supported-vc-redist

- patch.aul の `r43 謎さうなフォーク版58` (`r43_ss_58`) 以降

  https://github.com/nazonoSAUNA/patch.aul/releases/latest

## 導入方法
- VST3.eef  
  `aviutl.exe` と同階層にある `plugins` フォルダ内に `VST3.eef` ファイルを入れてください。
- VSTHost.exe  
  [VST_host](https://github.com/Book-0225/VST_host)のリリースよりダウンロードしてください。
  その後`aviutl.exe`と同一ディレクトリに導入してください。

## 使い方
- オブジェクトの追加時  
  拡張編集から`VST3 Host`というフィルタオブジェクトを追加します。  
  VST3プラグインを選択を押して64bitの.vst3ファイルを選択します。  
  一度フィルタを適用した部分のプレビューを出します。  
  ※プレビューを出さないとエラーになります。  
  また、読み込み処理には時間がかかるので次の手順に行く前に少し待ってください。(10秒ほど)  
  プラグインGUIを表示を押します。  
  VST3プラグインを操作して設定し終わったらVST3プラグインGUIを非表示にします。  
  ※重要 非表示にする手順を行わないと保存されません。  
  これで適用されているはずです。

- 書き出し時  
  VST3プラグインGUIを全て閉じます。  
  プレビューを最初から最後まで再生します。  
  ※プレビューを再生しないとホストの起動が追い付かずに無音区間が発生してしまいます。  
  書き出しを行います。(出力プラグインに対応しています)  

## 改版履歴

- v0.0.3  
  - `VSTHost.exe`の更新に合わせた修正  
    `load_and_set_state`を使うように変更  
  - データのロードタイミングによるクラッシュの修正  

- v0.0.2  
  `VSTHost.exe`との連携方法の修正

- v0.0.1  
  初版

## License
  MIT License

# Credits

## VST

VST is a trademark of Steinberg Media Technologies GmbH, registered in Europe and other countries.

## aviutl_exedit_sdk

https://github.com/ePi5131/aviutl_exedit_sdk （利用したブランチは[こちら](https://github.com/sigma-axis/aviutl_exedit_sdk/tree/self-use)です．）

---

1条項BSD

Copyright (c) 2022
ePi All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
THIS SOFTWARE IS PROVIDED BY ePi “AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL ePi BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
