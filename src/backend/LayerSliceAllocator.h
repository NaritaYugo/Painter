#pragma once

#include "backend/CanvasDocument.h"
#include <QOpenGLFunctions_4_3_Core>
#include <QVector>
#include <QPair>
#include <functional>

// ---------------------------------------------------------------------------
// LayerSliceAllocator
// ---------------------------------------------------------------------------
// タイル1枚 = layerTexArray(GL_TEXTURE_2D_ARRAY)の1スライス。GPUは1本のテクスチャ
// 配列に GL_MAX_ARRAY_TEXTURE_LAYERS(多くのGPUで2048)を超えるスライスを持てない
// ため、同サイズのテクスチャ配列を最大 MAX_TILE_BANKS 本まで「バンク」として並べ、
// グローバルなスライス番号 si を (bank = si / slicesPerBank, local = si % slicesPerBank)
// に分解して扱う(実質的にタイル数の上限をVRAM/バンク数まで引き上げる)。
//
// メモリ効率のため、最初のバンク(bank 0)は従来通り小さく始めて必要になるたびに
// 2倍ずつ伸長する(realloc+コピー)。bank 0 が上限(slicesPerBank)に達して初めて、
// 追加のバンク(bank 1..)を「最初からフルサイズ」で遅延生成する(その時点で既に
// 2048タイル=数百MB使っているので、フルサイズ確保が過大にはならない)。
//
// バンクテクスチャの実体(GLuint[])はGLWidgetが所有する配列を指しており、このクラスは
// 「いつ・どのバンクをどれだけ確保/伸長するか」というスライス管理ロジックだけを持つ。
// スライス空間の割り当て/解放(freeRanges_)はグローバルスライス番号のまま行うので、
// 上位(CanvasDocument)から見た挙動は従来の単一配列版と完全に同じ。
// ---------------------------------------------------------------------------
class LayerSliceAllocator
{
public:
    // banks: GLWidgetが所有する GLuint[MAX_TILE_BANKS] への先頭ポインタ(このクラスが
    //        生成・伸長・差し替えを行う)。slicesPerBank: 1バンクあたりの最大スライス数
    //        (= GL_MAX_ARRAY_TEXTURE_LAYERS)。
    void setup(QOpenGLFunctions_4_3_Core *gl, GLuint *banks, int tileSize, int slicesPerBank)
    {
        gl_ = gl;
        banks_ = banks;
        tileSize_  = tileSize;
        slicesPerBank_ = qMax(1, slicesPerBank);
    }

    int slicesPerBank() const { return slicesPerBank_; }

    // 初期テクスチャ(bank 0 のみ)を生成する(initTextures()から呼ぶ)。
    void createInitial(int initialSliceCount)
    {
        for (int i = 0; i < MAX_TILE_BANKS; i++) banks_[i] = 0;
        bankCount_       = 1;
        bank0Slices_     = qMin(qMax(1, initialSliceCount), slicesPerBank_);
        nextSlice_       = 0;
        freeRanges_.clear();

        banks_[0] = createBankTexture(bank0Slices_);
    }

    // CanvasDocument::SliceAllocFn としてそのまま渡せる。
    // まず解放済みの連続領域(freeRanges_)から十分な大きさのものを探し(best-fit)、
    // 見つかればそこを再利用する。見つからなければ従来通り末尾を伸長する
    // (必要ならバンクを追加生成する)。返すsi空間は「全バンクを連結した」通し番号。
    int allocContiguousSlices(int count)
    {
        if (count <= 0) return -1;

        int bestIdx = -1;
        for (int i = 0; i < freeRanges_.size(); i++) {
            if (freeRanges_[i].second < count) continue;
            if (bestIdx < 0 || freeRanges_[i].second < freeRanges_[bestIdx].second)
                bestIdx = i;
        }
        if (bestIdx >= 0) {
            const int base = freeRanges_[bestIdx].first;
            if (freeRanges_[bestIdx].second == count) {
                freeRanges_.remove(bestIdx);
            } else {
                freeRanges_[bestIdx].first  += count;
                freeRanges_[bestIdx].second -= count;
            }
            return base;
        }

        // 末尾伸長: nextSlice_ + count までを実テクスチャで裏打ちできるよう、bank 0 の
        // 伸長・新規バンク生成を繰り返す。バンク上限/VRAM不足で確保できなければ-1。
        if (!ensureCapacity(nextSlice_ + count)) {
            qWarning() << "layerTexArray: これ以上タイル(スライス)を確保できません"
                       << "(バンク上限" << MAX_TILE_BANKS << "本 ×" << slicesPerBank_ << "スライス、"
                       << "またはVRAM不足)";
            return -1;
        }
        int base = nextSlice_;
        nextSlice_ += count;
        return base;
    }

    // CanvasDocument::SliceFreeFn としてそのまま渡せる。
    // 解放されたスライスは freeRanges_ (開始位置でソート済みの連続区間リスト)へ挿入し、
    // 隣接する区間があれば結合する。
    void freeSlice(int sliceIndex)
    {
        if (sliceIndex < 0) return;

        int lo = 0, hi = freeRanges_.size();
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (freeRanges_[mid].first < sliceIndex) lo = mid + 1;
            else hi = mid;
        }
        freeRanges_.insert(lo, qMakePair(sliceIndex, 1));

        if (lo + 1 < freeRanges_.size() &&
            freeRanges_[lo].first + freeRanges_[lo].second == freeRanges_[lo + 1].first) {
            freeRanges_[lo].second += freeRanges_[lo + 1].second;
            freeRanges_.remove(lo + 1);
        }
        if (lo > 0 &&
            freeRanges_[lo - 1].first + freeRanges_[lo - 1].second == freeRanges_[lo].first) {
            freeRanges_[lo - 1].second += freeRanges_[lo].second;
            freeRanges_.remove(lo);
        }
    }

    // 現在確保済みの総スライス数(全バンク合計)
    int  allocatedSlices() const { return (bankCount_ - 1) * slicesPerBank_ + bank0OrFull(bankCount_ - 1); }
    int  nextSlice()       const { return nextSlice_; }
    int  bankCount()       const { return bankCount_; }

    // 読み込み前などに、あらかじめ totalSlices ぶんの容量を実テクスチャで裏打ち
    // しておく(bank 0 の伸長・追加バンク生成を「まとめて1回」で済ませ、読み込み中に
    // スライスが足りなくなるたびに伸長+コピーが繰り返されるのを避ける)。
    // 成功したか(VRAM不足/バンク上限で確保しきれなければ false。その場合でも
    // 部分的に確保済みで、以降は従来通り必要時に伸長するだけなので致命的ではない)。
    bool reserve(int totalSlices) { return ensureCapacity(totalSlices); }

    // resetDocument()用: 割り当て状態だけリセットする
    // (テクスチャの中身をゼロクリアするのは呼び出し側=GLWidgetの責務のまま)
    void resetAllocationState()
    {
        freeRanges_.clear();
        nextSlice_ = 0;
    }

    // テクスチャが差し替わった/バンクが増えた直後に呼ばれる
    // (ToolContext.layerTexBanks の更新や、fillEngine_.setTextures()の更新に使う)
    std::function<void()> onTextureRecreated;

private:
    QOpenGLFunctions_4_3_Core *gl_ = nullptr;
    GLuint *banks_ = nullptr;      // GLuint[MAX_TILE_BANKS]
    int tileSize_  = 256;
    int slicesPerBank_ = 2048;

    int bankCount_   = 0;          // 生成済みバンク数(>=1)
    int bank0Slices_ = 0;         // bank 0 の実スライス数(伸長中は slicesPerBank_ 未満)。
                                   // bankCount_>=2 のときは常に slicesPerBank_(=満杯)。
    int nextSlice_ = 0;            // 未使用領域の先頭(グローバルスライス番号)
    QVector<QPair<int,int>> freeRanges_; // 解放済み連続区間 (先頭, 個数) 昇順

    // バンクindex b の実スライス数(容量計算用)。b<0は0。
    int bank0OrFull(int b) const {
        if (b < 0) return 0;
        return (b == 0) ? bank0Slices_ : slicesPerBank_;
    }

    // グローバルスライス number までを実テクスチャで裏打ちできるよう、bank 0 の伸長
    // および新規バンク生成を行う。成功したか(VRAM不足/バンク上限なら false)を返す。
    bool ensureCapacity(int needSlices)
    {
        while (allocatedSlices() < needSlices) {
            // まだ bank 0 のみで、bank 0 に伸長余地があるならまず bank 0 を伸ばす。
            if (bankCount_ == 1 && bank0Slices_ < slicesPerBank_) {
                int target = qMin(qMax(bank0Slices_ * 2, qMin(needSlices, slicesPerBank_)), slicesPerBank_);
                if (target <= bank0Slices_) target = slicesPerBank_; // 端数対策で必ず前進
                if (!growBank0(target)) return false;
                continue;
            }
            // ここに来る時点で最後のバンクは満杯。新規バンクをフルサイズで生成する。
            if (bankCount_ >= MAX_TILE_BANKS) return false; // バンク上限
            GLuint tex = createBankTexture(slicesPerBank_);
            if (tex == 0) return false;                     // VRAM不足等
            banks_[bankCount_] = tex;
            bankCount_++;
            if (onTextureRecreated) onTextureRecreated();
        }
        return true;
    }

    // 指定スライス数のバンクテクスチャを1本生成して返す(失敗時0)。
    GLuint createBankTexture(int sliceCount)
    {
        GLuint tex = 0;
        gl_->glGenTextures(1, &tex);
        gl_->glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        gl_->glGetError();
        gl_->glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, tileSize_, tileSize_, sliceCount);
        if (gl_->glGetError() != GL_NO_ERROR) {
            gl_->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
            gl_->glDeleteTextures(1, &tex);
            return 0;
        }
        gl_->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl_->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl_->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl_->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return tex;
    }

    // bank 0 を newSliceCount スライスへ伸長する(immutable storageのため新規生成+コピー)。
    // 成功したか(VRAM不足なら false、既存テクスチャは無傷)を返す。
    bool growBank0(int newSliceCount)
    {
        Q_ASSERT(newSliceCount > bank0Slices_ && newSliceCount <= slicesPerBank_);

        GLuint newTex = createBankTexture(newSliceCount);
        if (newTex == 0) return false;

        // 既存スライスを新テクスチャへコピーする。スライス1枚ずつではなく、
        // glCopyImageSubDataのdepth引数で全スライスを1回のドライバ呼び出しでまとめて
        // コピーする(伸長のたびに数百〜数千回のGL呼び出しを発生させていたのが
        // 重いファイルの読み込みが遅い主因だったため)。
        if (bank0Slices_ > 0) {
            gl_->glCopyImageSubData(
                banks_[0], GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0,
                newTex,    GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0,
                tileSize_, tileSize_, bank0Slices_
            );
        }
        gl_->glDeleteTextures(1, &banks_[0]);
        banks_[0] = newTex;
        bank0Slices_ = newSliceCount;

        if (onTextureRecreated) onTextureRecreated();

        qDebug() << "layerTexArray bank0 grown to" << newSliceCount << "slices"
                 << "(" << (qint64)tileSize_ * tileSize_ * 4 * newSliceCount / 1024 / 1024 << "MB )";
        return true;
    }
};
