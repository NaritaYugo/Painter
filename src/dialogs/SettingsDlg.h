#pragma once

#include <QDialog>
#include <QSlider>
#include <QPointF>
#include <QVector>

class QListWidget;
class QStackedWidget;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class ToneCurveEditor;

// ---------------------------------------------------------------------------
// SettingsDlg
//
// 左にカテゴリ一覧、右に詳細設定を表示する一般的な設定ウィンドウ。
// QSettings を使って永続化する。
//
// 「保存して閉じる」で確定、「キャンセル」で変更を捨てて閉じる。
// 値は OK 時にまとめて QSettings に書き込む (即時反映はしない設計)。
// ---------------------------------------------------------------------------
class SettingsDlg : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDlg(QWidget *parent = nullptr);

    // QSettings から設定値を読み込んで返すための静的ヘルパー群
    // (アプリ起動時に MainWindow 側から呼んで初期値を取得する用)
    struct Values {
        int     defaultCanvasW   = 1920;
        int     defaultCanvasH   = 1080;
        bool    autoSaveEnabled  = false;
        int     autoSaveInterval = 5; // 分

        // Undo履歴の最大保持数。ペン等のストローク単位のUndoはタイル(256x256px)
        // 差分のみを保持するため軽いが、キャンバスサイズ変更のUndoはレイヤーごとの
        // 全内容スナップショットを前後2枚持つため非常に重い。両者が同じ履歴に
        // 積まれる設計上、大きくしすぎるとキャンバスサイズ変更を繰り返した際に
        // メモリを圧迫しうる点に注意(既定値・上限はCLIP STUDIO PAINT/Photoshopの
        // 初期値を参考にしつつ、この設計に合わせてやや控えめにしてある)。
        int     undoHistoryLimit   = 50;

        // カラーサークルの色相ツイスト(°/ΔL)。LCスクエアの上下の余白部分で、
        // 明度を振ったときにどれだけ色相を Yellow / Blue 側へずらすか。
        // 元はカラーサークル下部の「D」スライダーだったが、色ではなくサークルの
        // 振る舞いの設定なのでここへ移した(ColorWheelWidget::setHueTwist)。
        int     hueTwist           = 20;

        // 全ツール共通の筆圧カーブの制御点(x=生の筆圧, y=使う筆圧、ともに0..255)。
        // 使っているタブレットの硬さの癖をここで一度ならす想定で、この後さらに
        // ツール設定側の筆圧カーブが掛かる(PressureCurve参照)。
        // 既定は恒等(2点)= 何もしない。
        QVector<QPointF> pressureCurve;

        QString projectSaveDir; // 「名前を付けて保存」の既定の保存先フォルダ
        QString exportSaveDir;  // 「画像を書き出し」の既定の保存先フォルダ
    };

    static Values loadValues();      // QSettings から読み込み
    static void   saveValues(const Values &v); // QSettings に書き込み

private:
    QListWidget    *categoryList = nullptr;
    QStackedWidget *pageStack    = nullptr;

    Values current; // ダイアログ内で編集中の値 (OK 時に確定)

    // 各カテゴリページのウィジェット (読み戻し用に保持)
    QSpinBox       *canvasWSpin        = nullptr;
    QSpinBox       *canvasHSpin        = nullptr;
    QCheckBox      *autoSaveCheck      = nullptr;
    QSpinBox       *autoSaveIntervalSpin = nullptr;

    QSpinBox       *undoLimitSpin      = nullptr;
    QSpinBox       *hueTwistSpin       = nullptr;

    QLineEdit      *projectDirEdit     = nullptr;
    QLineEdit      *exportDirEdit      = nullptr;

    ToneCurveEditor *pressureCurveEditor_ = nullptr;
    QPushButton     *pressureDeleteBtn_   = nullptr;

    QWidget *createGeneralPage();
    QWidget *createInputPage();
    QWidget *createFilePage();

private slots:
    void onAccept();
    void browseProjectDir();
    void browseExportDir();
};
