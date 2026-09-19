#pragma once

#include <QDialog>

class QSlider;
class QLabel;
class QPushButton;

// ---------------------------------------------------------------------------
// CalibrationDlg
//
// モニターキャリブレーション用の非モーダルダイアログ。
// 「明るさ・コントラスト・CMY」のスライダーをドラッグするたびに即座に
// valuesChanged()を発行し、MainWindowが全タブの表示へ反映する
// (実データ/カラーモード変換後の最終出力に対する調整のため、確定/キャンセルの
// 概念は持たず、変更は都度そのまま設定として残る)。
// ---------------------------------------------------------------------------
class CalibrationDlg : public QDialog
{
    Q_OBJECT
public:
    explicit CalibrationDlg(QWidget *parent = nullptr);

    // 呼び出し側(toolCfg->calibration())の現在値でスライダーを揃える
    // (シグナルは発行しない)。
    void setValues(int brightness, int contrast, int cyan, int magenta, int yellow);

signals:
    void valuesChanged(int brightness, int contrast, int cyan, int magenta, int yellow);

private:
    QSlider *brightnessSlider_ = nullptr;
    QSlider *contrastSlider_   = nullptr;
    QSlider *cyanSlider_       = nullptr;
    QSlider *magentaSlider_    = nullptr;
    QSlider *yellowSlider_     = nullptr;

    QLabel *brightnessValueLabel_ = nullptr;
    QLabel *contrastValueLabel_   = nullptr;
    QLabel *cyanValueLabel_       = nullptr;
    QLabel *magentaValueLabel_    = nullptr;
    QLabel *yellowValueLabel_     = nullptr;

    QWidget *makeRow(const QString &labelText, QSlider *&sliderOut, QLabel *&valueLabelOut);
    void emitValuesChanged();
    void resetValues();
};
