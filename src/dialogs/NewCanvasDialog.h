#pragma once

#include <QDialog>
#include <QString>
#include <QList>

class QSpinBox;
class QDoubleSpinBox;
class QComboBox;
class QLabel;
class QCheckBox;
class CanvasPreviewWidget;

class NewCanvasDialog : public QDialog
{
    Q_OBJECT
public:
    explicit NewCanvasDialog(QWidget *parent = nullptr);

    int canvasWidth() const;
    int canvasHeight() const;
    bool wrapX() const; // 左右にループ
    bool wrapY() const; // 上下にループ

    QSize sizeHint() const override { return QSize(1200, 600); }

protected:
    void showEvent(QShowEvent *event) override;

private:
    bool m_centeredOnce = false;

    // 編集された項目の種類を定義
    enum class EditSource {
        Pixel,
        Dpi,
        PrintSize
    };

    // 印刷プリセット用の構造体
    struct PrintPreset {
        QString name;
        double w;
        double h;
        QString unit;
    };

    bool m_isUpdating = false;
    QString m_currentUnit = "mm";
    QList<PrintPreset> m_printPresets;

    // UIウィジェットのポインタ
    QComboBox *m_printUnitCb;
    QComboBox *m_pxCb;
    QComboBox *m_dpiCb;
    QComboBox *m_printCb;

    QSpinBox *m_pxWSpin;
    QSpinBox *m_pxHSpin;
    QSpinBox *m_dpiWSpin;
    QSpinBox *m_dpiHSpin;
    QDoubleSpinBox *m_printWSpin;
    QDoubleSpinBox *m_printHSpin;
    
    QCheckBox *m_swapToggle;
    QCheckBox *m_wrapXToggle; // 左右にループ
    QCheckBox *m_wrapYToggle; // 上下にループ
    CanvasPreviewWidget *m_previewWidget;

    // ヘルパーメソッド
    void recalculate(EditSource trigger); // 何が変更されたかを引数で受け取る
    void updatePrintComboBox(const QString& unit);
    double convertUnit(double value, const QString& from, const QString& to);
    double getInches(double value, const QString& unit);
    double fromInches(double inches, const QString& unit);
};
