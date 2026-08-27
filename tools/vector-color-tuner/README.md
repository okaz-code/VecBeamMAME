# MAME Vector Primary Color Tuner

`index.html` をブラウザで開くだけで使用できます。ネット接続やインストールは不要です。

- Direct Primary モード用に、赤・緑・青を Hue / Saturation / Brightness で個別調整します。
- 無彩色成分は白のまま保持されるため、RGB一次色の調整で白いドットが着色しません。
- MAMEへ反映する場合は右側の「MAME chain JSON」を使用します。
- 「リセット（RGB基準色）」は全色を Hue Shift 0°、Saturation 1.0、Brightness 1.0へ戻します。チェインの現行デフォルト値を読み込む機能ではありません。
- プレビューはMAMEの修正後SDR経路と同じく、線形色相保持ロールオフの後にRGB各成分へ個別ガンマを適用します。
- HEXとCanvasはSDR表示用の色相・彩度の目安です。HDR/EDRの実輝度やディスプレイ固有のトーンマップは再現しないため、Brightness 1.0超過と高輝度色は実機で確認してください。
- Saturationを1.0より上げると負成分を0へクリップするため、強い設定では彩度だけでなく色相も変わる場合があります。
- 従来の CIE xy/Y モードもチェイン側の Advanced 選択肢として残しています。
