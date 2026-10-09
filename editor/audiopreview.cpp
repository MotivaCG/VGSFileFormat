#include "audiopreview.h"
#include <QAudioOutput>
#include <QBuffer>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QUrl>
#include <cmath>

AudioPreview::AudioPreview(QObject *parent) : QObject(parent), player_(new QMediaPlayer(this)), output_(new QAudioOutput(this)) {
    player_->setAudioOutput(output_);
}
AudioPreview::~AudioPreview() { player_->stop(); }

void AudioPreview::setFile(const QString &path) {
    const QString key = QStringLiteral("file:") + QFileInfo(path).absoluteFilePath();
    if (key == key_) return;
    clear(); key_ = key;
    player_->setSource(QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()));
}
void AudioPreview::setBytes(const QByteArray &bytes, const QString &suffix) {
    const QString key = QStringLiteral("bytes:") + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex()) + suffix;
    if (key == key_) return;
    clear(); key_ = key; bytes_ = bytes;
    buffer_ = new QBuffer(&bytes_, this); buffer_->open(QIODevice::ReadOnly);
    // The name only tells the decoder what the bytes are.
    player_->setSourceDevice(buffer_, QUrl(QStringLiteral("track.") + suffix));
}
void AudioPreview::clear() {
    if (key_.isEmpty()) return;
    player_->stop(); player_->setSource({});
    if (buffer_) { buffer_->deleteLater(); buffer_ = nullptr; }
    bytes_.clear(); key_.clear();
}
bool AudioPreview::isPlaying() const { return player_->playbackState() == QMediaPlayer::PlayingState; }

void AudioPreview::follow(double seconds, bool playing, double rate) {
    if (key_.isEmpty()) return;
    const qint64 duration = player_->duration(), wanted = qint64(std::llround(seconds * 1000));
    const bool inside = wanted >= 0 && (duration <= 0 || wanted < duration);
    if (!playing || !inside) {
        if (isPlaying()) player_->pause();
        if (inside && std::abs(player_->position() - wanted) > 1) player_->setPosition(wanted);
        return;
    }
    if (std::abs(player_->playbackRate() - rate) > 1e-6) player_->setPlaybackRate(rate);
    if (!isPlaying()) { player_->setPosition(wanted); player_->play(); return; }
    // Kept in step: a jump (a seek, a loop) or drift beyond what is heard as out of sync.
    if (std::abs(player_->position() - wanted) > 120) player_->setPosition(wanted);
}
