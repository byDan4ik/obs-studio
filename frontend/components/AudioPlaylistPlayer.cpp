/******************************************************************************
    Copyright (C) 2025 by the OBS Project

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "AudioPlaylistPlayer.hpp"

#include <OBSApp.hpp>
#include <obs-module.h>
#include <qt-wrappers.hpp>

#include <algorithm>
#include <QColor>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMetaObject>
#include <QPalette>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

void OBSMediaEnded(void *data, calldata_t *)
{
	auto *player = static_cast<AudioPlaylistPlayer *>(data);
	QMetaObject::invokeMethod(player, "HandleMediaEnded", Qt::QueuedConnection);
}

}

AudioPlaylistPlayer::AudioPlaylistPlayer(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QHBoxLayout(this);
	auto *playerWidget = new QWidget(this);
	auto *playerLayout = new QVBoxLayout(playerWidget);
	auto *selectLayout = new QHBoxLayout();
	auto *fileButton = new QPushButton(QTStr("MusicPlayer.ChooseFile"), playerWidget);
	auto *folderButton = new QPushButton(QTStr("MusicPlayer.ChooseFolder"), playerWidget);
	playlistButton = new QPushButton(QTStr("MusicPlayer.ShowPlaylist"), playerWidget);

	selectLayout->addWidget(fileButton);
	selectLayout->addWidget(folderButton);
	selectLayout->addWidget(playlistButton);
	playerLayout->addLayout(selectLayout);

	trackLabel = new QLabel(QTStr("MusicPlayer.NoTrack"), playerWidget);
	trackLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	trackLabel->setMinimumWidth(220);
	playerLayout->addWidget(trackLabel);

	auto *progressLayout = new QHBoxLayout();
	positionLabel = new QLabel(QStringLiteral("00:00"), playerWidget);
	durationLabel = new QLabel(QStringLiteral("00:00"), playerWidget);
	progressSlider = new QSlider(Qt::Horizontal, playerWidget);
	progressSlider->setRange(0, 1000);
	progressSlider->setEnabled(false);
	progressLayout->addWidget(positionLabel);
	progressLayout->addWidget(progressSlider, 1);
	progressLayout->addWidget(durationLabel);
	playerLayout->addLayout(progressLayout);

	auto *controlsLayout = new QHBoxLayout();
	previousButton = new QPushButton(playerWidget);
	previousButton->setIcon(QIcon(QStringLiteral(":/res/images/media/media_previous.svg")));
	previousButton->setToolTip(QTStr("MusicPlayer.PreviousTrack"));
	playPauseButton = new QPushButton(playerWidget);
	nextButton = new QPushButton(playerWidget);
	nextButton->setIcon(QIcon(QStringLiteral(":/res/images/media/media_next.svg")));
	nextButton->setToolTip(QTStr("MusicPlayer.NextTrack"));
	controlsLayout->addStretch();
	controlsLayout->addWidget(previousButton);
	controlsLayout->addWidget(playPauseButton);
	controlsLayout->addWidget(nextButton);
	controlsLayout->addStretch();
	playerLayout->addLayout(controlsLayout);
	playerLayout->addStretch();
	layout->addWidget(playerWidget);

	playlistPanel = new QWidget(this);
	auto *playlistLayout = new QVBoxLayout(playlistPanel);
	playlistWidget = new QTreeWidget(playlistPanel);
	playlistWidget->setColumnCount(2);
	playlistWidget->setHeaderLabels({QTStr("MusicPlayer.Playlist.Track"), QTStr("MusicPlayer.Playlist.Status")});
	playlistWidget->setRootIsDecorated(false);
	playlistWidget->setUniformRowHeights(true);
	playlistWidget->setMinimumWidth(240);
	playlistWidget->header()->setStretchLastSection(false);
	playlistWidget->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	playlistWidget->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	playlistLayout->addWidget(playlistWidget);
	playlistPanel->setVisible(false);
	layout->addWidget(playlistPanel, 1);

	progressTimer = new QTimer(this);
	progressTimer->setInterval(250);

	connect(fileButton, &QPushButton::clicked, this, &AudioPlaylistPlayer::ChooseFile);
	connect(folderButton, &QPushButton::clicked, this, &AudioPlaylistPlayer::ChooseFolder);
	connect(playlistButton, &QPushButton::clicked, this, &AudioPlaylistPlayer::TogglePlaylist);
	connect(previousButton, &QPushButton::clicked, this, &AudioPlaylistPlayer::PlayPreviousTrack);
	connect(nextButton, &QPushButton::clicked, this, &AudioPlaylistPlayer::PlayNextTrack);
	connect(playPauseButton, &QPushButton::clicked, this, &AudioPlaylistPlayer::TogglePlayback);
	connect(playlistWidget, &QTreeWidget::itemClicked, this, &AudioPlaylistPlayer::PlaySelectedTrack);
	connect(progressSlider, &QSlider::sliderReleased, this, &AudioPlaylistPlayer::SeekToPosition);
	connect(progressTimer, &QTimer::timeout, this, &AudioPlaylistPlayer::UpdateProgress);

	RefreshPlaylist();
	UpdatePlaybackButton();
	progressTimer->start();
}

AudioPlaylistPlayer::~AudioPlaylistPlayer()
{
	progressTimer->stop();
	mediaEndedSignal.Disconnect();

	if (source && sourceActive) {
		obs_source_dec_active(source);
		sourceActive = false;
	}
}

void AudioPlaylistPlayer::ChooseFile()
{
	const QStringList paths = QFileDialog::getOpenFileNames(this, QTStr("MusicPlayer.ChooseFile"), QString(),
								QTStr("MusicPlayer.MP3Files"));
	if (paths.isEmpty())
		return;

	AddTracks(paths);
}

void AudioPlaylistPlayer::ChooseFolder()
{
	const QString folder = QFileDialog::getExistingDirectory(this, QTStr("MusicPlayer.ChooseFolder"));
	if (folder.isEmpty())
		return;

	const QFileInfoList files = QDir(folder).entryInfoList({QStringLiteral("*.mp3"), QStringLiteral("*.MP3")},
							      QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
	QStringList newTracks;
	newTracks.reserve(files.size());
	for (const QFileInfo &file : files)
		newTracks.push_back(file.absoluteFilePath());

	AddTracks(newTracks);
}

void AudioPlaylistPlayer::AddTracks(const QStringList &newTracks)
{
	if (newTracks.isEmpty()) {
		if (tracks.isEmpty())
			trackLabel->setText(QTStr("MusicPlayer.NoMp3Files"));
		return;
	}

	const bool queueWasEmpty = tracks.isEmpty();
	tracks.append(newTracks);
	RefreshPlaylist();

	if (queueWasEmpty || currentTrack < 0) {
		const int nextTrack = NextUnplayedTrack();
		if (nextTrack >= 0)
			PlayTrack(nextTrack);
	}

	UpdatePlaybackButton();
}

void AudioPlaylistPlayer::PlayTrack(int index)
{
	if (index < 0 || index >= tracks.size())
		return;

	if (currentTrack >= 0 && currentTrack != index)
		playedTracks.insert(currentTrack);

	currentTrack = index;
	PlayCurrentTrack();
}

void AudioPlaylistPlayer::PlayCurrentTrack()
{
	if (currentTrack < 0 || currentTrack >= tracks.size())
		return;

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", QT_TO_UTF8(tracks.at(currentTrack)));
	obs_data_set_bool(settings, "looping", false);
	obs_data_set_bool(settings, "restart_on_activate", false);
	obs_data_set_bool(settings, "close_when_inactive", false);
	obs_data_set_bool(settings, "clear_on_media_end", true);

	if (!source) {
		source = obs_source_create_private("ffmpeg_source", "Background Music Player", settings);
		if (!source) {
			blog(LOG_ERROR, "Unable to create the background music player source");
			trackLabel->setText(QTStr("MusicPlayer.SourceUnavailable"));
			RefreshPlaylist();
			UpdatePlaybackButton();
			return;
		}

		mediaEndedSignal.Connect(obs_source_get_signal_handler(source), "media_ended", OBSMediaEnded, this);
		obs_source_set_monitoring_enabled(source, true);
		obs_source_inc_active(source);
		sourceActive = true;
	} else {
		obs_source_update(source, settings);
	}

	trackLabel->setToolTip(tracks.at(currentTrack));
	UpdateTrackLabel();
	RefreshPlaylist();
	UpdatePlaybackButton();
	UpdateProgress();
}

void AudioPlaylistPlayer::PlayPreviousTrack()
{
	if (tracks.isEmpty())
		return;

	if (currentTrack > 0) {
		PlayTrack(currentTrack - 1);
	} else if (currentTrack == 0) {
		if (source)
			obs_source_media_restart(source);
	} else {
		for (int index = tracks.size() - 1; index >= 0; --index) {
			if (playedTracks.contains(index)) {
				PlayTrack(index);
				return;
			}
		}
	}
}

void AudioPlaylistPlayer::PlayNextTrack()
{
	if (currentTrack >= 0)
		playedTracks.insert(currentTrack);

	const int nextTrack = NextUnplayedTrack();
	if (nextTrack >= 0) {
		PlayTrack(nextTrack);
		return;
	}

	currentTrack = -1;
	if (source)
		obs_source_media_stop(source);
	trackLabel->setText(QTStr("MusicPlayer.NoTrack"));
	progressSlider->setEnabled(false);
	progressSlider->setValue(0);
	positionLabel->setText(QStringLiteral("00:00"));
	durationLabel->setText(QStringLiteral("00:00"));
	RefreshPlaylist();
	UpdatePlaybackButton();
}

void AudioPlaylistPlayer::TogglePlayback()
{
	if (currentTrack < 0) {
		const int nextTrack = NextUnplayedTrack();
		if (nextTrack >= 0)
			PlayTrack(nextTrack);
		return;
	}

	if (!source) {
		PlayCurrentTrack();
		return;
	}

	const enum obs_media_state state = obs_source_media_get_state(source);
	if (state == OBS_MEDIA_STATE_PLAYING)
		obs_source_media_play_pause(source, true);
	else if (state == OBS_MEDIA_STATE_PAUSED)
		obs_source_media_play_pause(source, false);
	else
		obs_source_media_restart(source);

	UpdatePlaybackButton();
}

void AudioPlaylistPlayer::TogglePlaylist()
{
	const bool showPlaylist = !playlistPanel->isVisible();
	playlistPanel->setVisible(showPlaylist);
	playlistButton->setText(QTStr(showPlaylist ? "MusicPlayer.HidePlaylist" : "MusicPlayer.ShowPlaylist"));
}

void AudioPlaylistPlayer::PlaySelectedTrack()
{
	const QTreeWidgetItem *item = playlistWidget->currentItem();
	if (!item)
		return;

	PlayTrack(item->data(0, Qt::UserRole).toInt());
}

void AudioPlaylistPlayer::HandleMediaEnded()
{
	if (!source || obs_source_media_get_state(source) != OBS_MEDIA_STATE_ENDED || currentTrack < 0)
		return;

	playedTracks.insert(currentTrack);
	currentTrack = -1;
	RefreshPlaylist();
	PlayNextTrack();
}

void AudioPlaylistPlayer::UpdateProgress()
{
	if (!source || currentTrack < 0) {
		UpdatePlaybackButton();
		return;
	}

	const int64_t duration = obs_source_media_get_duration(source);
	const int64_t position = obs_source_media_get_time(source);
	if (duration > 0) {
		progressSlider->setEnabled(true);
		if (!progressSlider->isSliderDown())
			progressSlider->setValue((int)std::clamp(position * 1000 / duration, int64_t(0), int64_t(1000)));
	} else {
		progressSlider->setEnabled(false);
		progressSlider->setValue(0);
	}

	positionLabel->setText(FormatTime(position));
	durationLabel->setText(FormatTime(duration));
	UpdatePlaybackButton();
}

void AudioPlaylistPlayer::UpdateTrackLabel()
{
	if (currentTrack < 0 || currentTrack >= tracks.size()) {
		trackLabel->setText(QTStr("MusicPlayer.NoTrack"));
		return;
	}

	const QString fileName = QFileInfo(tracks.at(currentTrack)).fileName();
	trackLabel->setText(QTStr("MusicPlayer.TrackOf")
				    .arg(currentTrack + 1)
				    .arg(tracks.size())
				    .arg(fileName));
}

int AudioPlaylistPlayer::NextUnplayedTrack() const
{
	for (int index = 0; index < tracks.size(); ++index) {
		if (!playedTracks.contains(index) && index != currentTrack)
			return index;
	}

	return -1;
}

void AudioPlaylistPlayer::RefreshPlaylist()
{
	playlistWidget->clear();

	const QPalette palette = playlistWidget->palette();
	const QColor playedColor = palette.color(QPalette::Disabled, QPalette::Text);
	const QColor currentBackground = palette.color(QPalette::Highlight);
	const QColor currentForeground = palette.color(QPalette::HighlightedText);

	for (int index = 0; index < tracks.size(); ++index) {
		auto *item = new QTreeWidgetItem(playlistWidget);
		item->setText(0, QFileInfo(tracks.at(index)).fileName());
		item->setData(0, Qt::UserRole, index);
		item->setToolTip(0, tracks.at(index));

		if (index == currentTrack) {
			item->setText(1, QTStr("MusicPlayer.Playlist.Current"));
			for (int column = 0; column < playlistWidget->columnCount(); ++column) {
				item->setBackground(column, currentBackground);
				item->setForeground(column, currentForeground);
			}
		} else if (playedTracks.contains(index)) {
			item->setText(1, QTStr("MusicPlayer.Playlist.Played"));
			for (int column = 0; column < playlistWidget->columnCount(); ++column)
				item->setForeground(column, playedColor);
		} else {
			item->setText(1, QTStr("MusicPlayer.Playlist.Waiting"));
		}

		if (index == currentTrack)
			playlistWidget->setCurrentItem(item);
	}
}

void AudioPlaylistPlayer::UpdatePlaybackButton()
{
	const bool hasTracks = !tracks.isEmpty();
	previousButton->setEnabled(hasTracks);
	nextButton->setEnabled(NextUnplayedTrack() >= 0);
	playPauseButton->setEnabled(hasTracks);

	const bool playing = source && obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING;
	playPauseButton->setIcon(QIcon(playing ? QStringLiteral(":/res/images/media/media_pause.svg")
					       : QStringLiteral(":/res/images/media/media_play.svg")));
	playPauseButton->setToolTip(QTStr(playing ? "MusicPlayer.Pause" : "MusicPlayer.Play"));
}

void AudioPlaylistPlayer::SeekToPosition()
{
	if (!source || currentTrack < 0)
		return;

	const int64_t duration = obs_source_media_get_duration(source);
	if (duration <= 0)
		return;

	obs_source_media_set_time(source, duration * progressSlider->value() / 1000);
}

QString AudioPlaylistPlayer::FormatTime(int64_t milliseconds) const
{
	const int totalSeconds = (int)std::max<int64_t>(0, milliseconds / 1000);
	const int hours = totalSeconds / 3600;
	const int minutes = (totalSeconds / 60) % 60;
	const int seconds = totalSeconds % 60;
	if (hours > 0)
		return QStringLiteral("%1:%2:%3").arg(hours, 2, 10, QLatin1Char('0')).arg(minutes, 2, 10, QLatin1Char('0')).arg(
			seconds, 2, 10, QLatin1Char('0'));

	return QStringLiteral("%1:%2").arg(totalSeconds / 60, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10,
											     QLatin1Char('0'));
}
