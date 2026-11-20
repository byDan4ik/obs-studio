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

#pragma once

#include <obs.hpp>

#include <QSet>
#include <QStringList>
#include <QWidget>

class QLabel;
class QPushButton;
class QSlider;
class QTimer;
class QTreeWidget;

class AudioPlaylistPlayer : public QWidget {
	Q_OBJECT

public:
	explicit AudioPlaylistPlayer(QWidget *parent = nullptr);
	~AudioPlaylistPlayer() override;

private slots:
	void ChooseFile();
	void ChooseFolder();
	void PlayPreviousTrack();
	void PlayNextTrack();
	void TogglePlayback();
	void TogglePlaylist();
	void PlaySelectedTrack();
	void HandleMediaEnded();
	void UpdateProgress();

private:
	void AddTracks(const QStringList &tracks);
	void PlayTrack(int index);
	void PlayCurrentTrack();
	int NextUnplayedTrack() const;
	void RefreshPlaylist();
	void UpdateTrackLabel();
	void UpdatePlaybackButton();
	void SeekToPosition();
	QString FormatTime(int64_t milliseconds) const;

	QStringList tracks;
	QSet<int> playedTracks;
	int currentTrack = -1;
	OBSSourceAutoRelease source;
	OBSSignal mediaEndedSignal;
	QPushButton *playlistButton = nullptr;
	QLabel *trackLabel = nullptr;
	QLabel *positionLabel = nullptr;
	QLabel *durationLabel = nullptr;
	QSlider *progressSlider = nullptr;
	QPushButton *playPauseButton = nullptr;
	QPushButton *previousButton = nullptr;
	QPushButton *nextButton = nullptr;
	QWidget *playlistPanel = nullptr;
	QTreeWidget *playlistWidget = nullptr;
	QTimer *progressTimer = nullptr;
	bool sourceActive = false;
};
