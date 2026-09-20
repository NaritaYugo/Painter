# Tiepolo
- デザインが固定化されていたペイントツールを1から見直して、絵を描く際に使いやすい独自機能を盛り込んだペイントソフトです
- QtとOpenGLを使用しています

<img width="1919" height="1079" alt="image" src="https://github.com/user-attachments/assets/f74ada44-d46d-4380-8862-a62d944bf01b" />


## オリジナル機能
#### ツリー形式のレイヤービュー
- これまでのペイントソフトでは、レイヤーを一列に重ねるUIが主流で、これだとレイヤーが増えたときに縦が長くなり、スクロールして探す手間が発生していました。
- 本アプリでは、クリッピングマスクを横方向に転回し、レイヤーの種類に応じて色や形を分けることで、常にレイヤーの依存関係を1画面で把握できるようにしています。
- このツリーは、レイヤー移動時に真価を発揮します。常にすべてのレイヤーが見えているため、レイヤーの移動が簡単です。

<img width="3566" height="1764" alt="イラスト" src="https://github.com/user-attachments/assets/44739529-bf75-4c05-8645-4a5781da260e" />

#### OKLCHカラーサークル
- 近年、CSS等、人間の知覚に基づいて色を選択できる**OKLCH色空間**が採用される例が増えてきましたが、色空間の形状が対称的ではなく、ペイントソフトのUIには使用されていませんでした。
- それとは別に、絵画では、「光が当たっている部分を描くときは色を黄色側に寄せる」「影の部分を描くときは色を青色側に寄せる」と、良い色選びができることが知られていました。
- これを使用し、OKLCHを拡張することで、色相サークルをずらすことなく、矩形内で自然と「きれいな色」が選べるカラーサークルになっています。

<img width="3566" height="1764" alt="イラスト2" src="https://github.com/user-attachments/assets/b3358eef-4967-40c5-952f-ad15bf054491" />

#### わがままに答える塗りつぶし機能
- 塗りつぶし(バケツツール)と言えば、隙間閉じ機能は基本ですが、隙間閉じの閾値を大きくすると、髪の毛などの細い隙間でも塗り止まってしまい、奥まで塗ってくれません。
- そこで、
   - すぐ奥に広い空間がある(=ここで止まらなければ漏れ出てしまう)場合は塗り止まる
   - 奥も狭い(=ここで焦って止まらなくても、奥まで塗り切ってから停止判定すれば十分である)場合は塗り続ける
  という独自のアルゴリズムで、高速かつ正確に判定しています。

<img width="3566" height="1764" alt="イラスト3" src="https://github.com/user-attachments/assets/a536eec5-908e-4428-9af2-1a099e829fcb" />
- 以下の手順でO(N)で塗りつぶせます
  - JFAによってSDFを作成
  - SDFが閾値 $R$ 未満の領域narrowと $R$ 以上の領域deepに分ける
  - 連続したdeepの領域を部屋と呼ぶことにする
  - スタート地点が属する部屋以外の部屋から壁判定を逆流
  - 残りを塗りつぶす
<img width="3566" height="1764" alt="イラスト4" src="https://github.com/user-attachments/assets/4ae857bb-ebdb-418c-b21a-280612960dd1" />

#### キャンバスのループ
- キャンバスの端を上下・左右でシームレスに繋げるループ機能を搭載。
- 左端を越えたストロークがそのまま右端へリアルタイムに描画されるため、タイリング可能なテクスチャの作成が容易になります。
- 変形ツールを使用してテクスチャ全体をにスクロールさせながら描画することも可能です。

<img width="1014" height="521" alt="image" src="https://github.com/user-attachments/assets/007b0c98-f93e-4add-8659-361ac5e0b88f" />


#### 自由にGLSLを書けるカスタムシェーダー
GLSL（OpenGL Shading Language）を用いて、元の色から変換後の色を計算する独自のプログラムを組み込めます。手描きでは困難な精密なグラデーションや幾何学パターンの生成など、テクスチャ作成を強力にサポート。アイデア次第で、オリジナルの画像編集エフェクトやフィルターを自作することも可能です。

<img width="835" height="868" alt="image" src="https://github.com/user-attachments/assets/19756f32-df0e-4b30-8da5-1dbae6aee249" />

## 機能一覧
通常のペイントソフトにある機能の多くに対応しています。
- ファイル：独自形式tploのほか、psd、png、jpeg、abrに対応
- 処理：変形(2種類)、色調補正(5種類)、効果(2種類)、カスタムシェーダー
- ツール：ペン、消しゴム、エアブラシ、塗りつぶし、スポイト、移動、回転、ぼかし、ゆがみ、テキスト
- ツール設定：ブラシ先端画像、手振れ補正等
- レイヤー：合成モード(27種類)、クリッピングマスク、レイヤーマスク、レイヤーフォルダ、テキストレイヤー、レイヤーフィルター

## フォルダ構成
```
Painter/
├─ platform/
│  └─ Windows向けリソース、アプリアイコン
│
├─ resources/
│  ├─ icons/       各種アイコン画像
│  ├─ textures/    ブラシ先端、紙質テクスチャ
│  ├─ shaders/
│  │  ├─ fill/     塗りつぶし用
│  │  ├─ header/   GLSLの共通インクルード用
│  │  ├─ paint/    画像更新
│  │  └─ render/   レイヤー合成、表示
│  ├─ texts/       バージョン、初期レイアウト
│  ├─ style.qss    アプリ全体のQtスタイル
│  └─ resources.qrc
│
├─ src/
│  ├─ app/         メインウィンドウとアプリ全体の制御
│  ├─ canvas/      キャンバス、タブ、入力、描画処理の統合
│  ├─ document/    ドキュメント・レイヤー・Undoのデータモデル
│  ├─ rendering/   GPUレイヤー管理、シェーダー、合成処理
│  ├─ io/          PSD、ABR等のI/O
│  ├─ licensing/   Pro版のライセンス検証
│  ├─ actions/     画像編集処理
│  ├─ tools/
│  │  ├─ core/     ツール共通インターフェース・設定・実行環境
│  │  ├─ canvas/   ペンなど、キャンバス上で継続使用するツール
│  │  └─ actions/  アクション内部で使用する画像処理ツール
│  ├─ docks/       レイヤー、カラー、ツールなどのドックUI
│  ├─ dialogs/     設定・画像処理用のダイアログやパネル
│  ├─ components/  UIパーツ
│  └─ shortcuts/   キーボードショートカット関連
│
├─ tools/
│  └─ license/     Pro版ライセンスの生成
│
├─ CMakeLists.txt
├─ CMakePresets.json
└─ vcpkg.json
```

## 設計の概要
### 入口～メインウィンドウ
- src/main.cpp：初期化、MainWindow作成
- src/app/：全体のウィンドウ
  - MainWindowWorkspace：ワークスペース
  - MainWindowChrome：タイトルバーなどの外観
  - MainWindowFiles：ファイル操作
  - MainWindowSettings：設定の反映と保存
  - MainWindowEvents：ウィンドウイベント
  - MainWindowShortcuts：ショートカット
  - MainWindowNative：Windows固有処理
### キャンバス
- src/canvas/：OpenGLを使ったキャンバスウィジェット
  - CanvasWidgetRendering：OpenGL初期化、ペイント、画面表示
  - CanvasWidgetInput：ペン入力
  - CanvasWidgetLayers：レイヤー操作
  - CanvasWidgetCompositing：GPU上のレイヤー合成と画像取得
  - CanvasWidgetGeometry：キャンバスサイズ
  - CanvasWidgetSelection：選択範囲
  - CanvasWidgetUndo：Undo / Redo
  - CanvasWidgetActions：処理アクション
### ドキュメント
- src/document/CanvasDocument：キャンバスをタイルに区切って保持
### ドック
- src/docks/：メインウィンドウ上に自由に配置できるUIドック
   - BrushSizeDock：ブラシサイズ
   - ColorCircleDock：カラーサークル
   - NavigatorDock：ナビゲーター(キャンバスプレビュー、移動)
   - ToolDock：ツール
   - ToolPropertyDock：ツールプロパティ(ツールの詳細設定)
   - ToolPresetDock：ツールプロパティの保存
   - LayerDock：レイヤー
   - layers/：レイヤーUIパーツ
### ツール
- src/tools/canvas/：ペン・消しゴムなど、どれかを選択してキャンバスに対して使用するツール
### 処理
- src/actions/：フィルターや変形など、画像に対して操作を適用する処理

<img width="1013" height="512" alt="image" src="https://github.com/user-attachments/assets/aeb51ba9-91cc-40dc-a9c0-9ce48afc1c8c" />




