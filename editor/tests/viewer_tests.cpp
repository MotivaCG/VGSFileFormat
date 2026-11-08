// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Victor M. Feliz. All rights reserved.
//
// The viewer's playback, built here with the editor's sources it shares: these tests write
// their captures with the authoring code, which the viewer itself never links.

#include "viewerwindow.h"
#include "editortheme.h"
#include "playbackcheck.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>
#include <optional>
#include <random>

namespace {
struct ViewerFixture {
    QTemporaryDir dir;
    std::unique_ptr<ViewerWindow> window;
    Viewport *viewport = nullptr;
    RangeSlider *slider = nullptr;
    // A capture of `frames` frames, played the packed way unless `floats`.
    bool open(int frames, bool floats) {
        if (!dir.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        const QString path = dir.filePath("play.vgs");
        if (!playbackcheck::writeCapture(path, frames).isEmpty()) return false;
        if (floats) qputenv("VGS_VIEWER_FLOATS", "1");
        window = std::make_unique<ViewerWindow>();
        qunsetenv("VGS_VIEWER_FLOATS");
        window->resize(1200, 800); window->show();
        if (!QTest::qWaitForWindowExposed(window.get())) return false;
        viewport = window->findChild<Viewport *>(); slider = window->findChild<RangeSlider *>("captureRangeSlider");
        if (!viewport || !slider) return false;
        window->openFile(path);
        if (!QTest::qWaitFor([&] { return viewport->frame() != nullptr; }, 30000)) return false;
        QTest::qWait(300); return true;
    }
    int shown() const { return viewport->frame() ? int(std::lround(viewport->frame()->seconds * playbackcheck::Fps)) : -1; }
    // The button: a shortcut depends on where the focus is.
    void play() { if (auto *button = window->findChild<QAbstractButton *>("playPause")) button->click(); else QFAIL("No play button."); }
    bool packed() const { return viewport->frame() && viewport->frame()->packed; }
};
struct DecodeDelay {
    explicit DecodeDelay(int ms) { qputenv("VGS_TEST_DECODE_DELAY_MS", QByteArray::number(ms)); }
    ~DecodeDelay() { qunsetenv("VGS_TEST_DECODE_DELAY_MS"); }
};
}

class ViewerTests : public QObject {
    Q_OBJECT
    static void modes() { QTest::addColumn<bool>("floats"); QTest::newRow("packed") << false; QTest::newRow("floats") << true; }
private slots:
    void playsEachFrameOnTime_data() { modes(); }
    void playsEachFrameOnTime() {
        QFETCH(bool, floats);
        ViewerFixture f; QVERIFY(f.open(120, floats));
        playbackcheck::Watch watch(f.viewport, f.slider);
        f.play(); QTest::qWait(300); watch.clear(); QTest::qWait(2500);
        const auto shown = watch.shown; f.play();
        QCOMPARE(f.packed(), !floats);
        playbackcheck::checkOnTime(shown, 0, 119, 2, 1, 0.9);
        QVERIFY2(shown.size() >= 60, qPrintable(QString("%1 frames in 2.5 s").arg(shown.size())));
        QTRY_COMPARE(f.shown(), f.slider->playheadValue());
        QTest::qWait(200); QCOMPARE(f.shown(), f.slider->playheadValue());
    }
    void aimsAheadWhenDecodingIsSlow_data() { modes(); }
    void aimsAheadWhenDecodingIsSlow() {
        QFETCH(bool, floats);
        ViewerFixture f; QVERIFY(f.open(200, floats));
        DecodeDelay delay(70);
        playbackcheck::Watch watch(f.viewport, f.slider);
        f.play(); QTest::qWait(600); watch.clear(); QTest::qWait(3000);
        const auto shown = watch.shown; f.play();
        playbackcheck::checkOnTime(shown, 0, 199, 2, 3, 0.0);
        QVERIFY2(shown.size() >= 18 && shown.size() <= 50, qPrintable(QString("%1 frames in 3 s").arg(shown.size())));
        int onTime = 0; for (const auto &s : shown) if (s.frame == s.playhead) ++onTime;
        QVERIFY2(onTime * 10 >= int(shown.size()) * 6, qPrintable(QString("%1 of %2 frames on time").arg(onTime).arg(shown.size())));
        QTRY_COMPARE_WITH_TIMEOUT(f.shown(), f.slider->playheadValue(), 3000);
    }
    void stoppingShowsTheFrameAtThePlayhead_data() { modes(); }
    void stoppingShowsTheFrameAtThePlayhead() {
        QFETCH(bool, floats);
        ViewerFixture f; QVERIFY(f.open(300, floats));
        std::mt19937 random(11);
        for (int delay : {0, 70}) {
            std::optional<DecodeDelay> slow; if (delay) slow.emplace(delay);
            for (int i = 0; i < 6; ++i) {
                f.play(); QTest::qWait(40 + int(random() % 400)); f.play();
                QTRY_COMPARE_WITH_TIMEOUT(f.shown(), f.slider->playheadValue(), 3000);
                QTest::qWait(150); QCOMPARE(f.shown(), f.slider->playheadValue());
            }
        }
    }
    void seekingWhilePlayingShowsTheFrameAskedFor_data() { modes(); }
    void seekingWhilePlayingShowsTheFrameAskedFor() {
        QFETCH(bool, floats);
        ViewerFixture f; QVERIFY(f.open(120, floats));
        for (int target : {45, 3, 100}) {
            f.play(); QTest::qWait(500);
            emit f.slider->playheadChanged(target);
            QTRY_COMPARE_WITH_TIMEOUT(f.shown(), target, 3000);
            QTest::qWait(200); QCOMPARE(f.shown(), target); QCOMPARE(f.slider->playheadValue(), target);
        }
    }
    void fastAndLoopedPlaybackKeepsToTheRange_data() { modes(); }
    void fastAndLoopedPlaybackKeepsToTheRange() {
        QFETCH(bool, floats);
        ViewerFixture f; QVERIFY(f.open(120, floats));
        auto *loop = f.window->findChild<QCheckBox *>("playbackLoop"); auto *speed = f.window->findChild<QDoubleSpinBox *>("playbackSpeed"); QVERIFY(loop && speed);
        loop->setChecked(true); f.slider->setRangeValues(20, 40); emit f.slider->rangeChanged(20, 40, 20);
        for (double rate : {1.0, 2.0, 4.0}) {
            speed->setValue(rate);
            playbackcheck::Watch watch(f.viewport, f.slider);
            f.play(); QTest::qWait(300); watch.clear(); QTest::qWait(2000);
            const auto shown = watch.shown; f.play();
            playbackcheck::checkOnTime(shown, 20, 40, rate > 2 ? 4 : 2, rate > 2 ? 2 : 1, rate > 2 ? 0.5 : 0.8);
            int wraps = 0; for (size_t i = 1; i < shown.size(); ++i) if (shown[i].frame < shown[i - 1].frame) ++wraps;
            QVERIFY2(wraps >= int(rate * 2) - 1, qPrintable(QString("%1 loops at %2x").arg(wraps).arg(rate)));
            QTRY_COMPARE(f.shown(), f.slider->playheadValue());
        }
    }
};

int main(int argc, char **argv) {
    QSurfaceFormat format; format.setVersion(3, 3); format.setProfile(QSurfaceFormat::CoreProfile); format.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv); EditorTheme::install();
    QCoreApplication::setOrganizationName("Viewer Tests"); QCoreApplication::setApplicationName("Viewer Tests");
    ViewerTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "viewer_tests.moc"
