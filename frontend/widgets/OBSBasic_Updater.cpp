/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

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

#include "OBSBasic.hpp"

#include <dialogs/OBSWhatsNew.hpp>

#ifdef ENABLE_SPARKLE_UPDATER
#include <utility/MacUpdateThread.hpp>
#include <utility/OBSSparkle.hpp>
#endif
#ifdef _WIN32
#include <utility/RemoteTextThread.hpp>
#endif
#if defined(_WIN32) || defined(WHATSNEW_ENABLED)
#include <utility/WhatsNewBrowserInitThread.hpp>
#include <utility/models/whatsnew.hpp>
#endif

#ifdef BROWSER_AVAILABLE
#include <browser-panel.hpp>
#endif
#include <qt-wrappers.hpp>

#ifdef _WIN32
#include <QDesktopServices>
#include <QMessageBox>
#include <QVersionNumber>

#include <nlohmann/json.hpp>
#endif

#ifdef _WIN32
#define UPDATE_CHECK_INTERVAL (60 * 60 * 24 * 4) /* 4 days */
#endif

struct QCef;
struct QCefCookieManager;

extern QCef *cef;
extern QCefCookieManager *panel_cookies;

using namespace std;

namespace {

QPointer<OBSWhatsNew> obsWhatsNew;

template<typename OBSRef> struct SignalContainer {
	OBSRef ref;
	vector<shared_ptr<OBSSignal>> handlers;
};
} // namespace

void OBSBasic::ReceivedIntroJson(const std::string &text)
{
#ifdef WHATSNEW_ENABLED
	if (isClosing()) {
		return;
	}

	WhatsNewList items;
	try {
		nlohmann::json json = nlohmann::json::parse(text);
		items = json.get<WhatsNewList>();
	} catch (nlohmann::json::exception &e) {
		blog(LOG_WARNING, "Parsing whatsnew data failed: %s", e.what());
		return;
	}

	std::string info_url;
	int info_increment = -1;

	/* check to see if there's an info page for this version */
	for (const WhatsNewItem &item : items) {
		if (item.os) {
			WhatsNewPlatforms platforms = *item.os;
#ifdef _WIN32
			if (!platforms.windows) {
				continue;
			}
#elif defined(__APPLE__)
			if (!platforms.macos) {
				continue;
			}
#else
			if (!platforms.linux) {
				continue;
			}
#endif
		}

		int major = 0;
		int minor = 0;

		sscanf(item.version.c_str(), "%d.%d", &major, &minor);
		if (major == LIBOBS_API_MAJOR_VER && minor == LIBOBS_API_MINOR_VER &&
		    item.RC == OBS_RELEASE_CANDIDATE && item.Beta == OBS_BETA) {
			info_url = item.url;
			info_increment = item.increment;
		}
	}

	/* this version was not found, or no info for this version */
	if (info_increment == -1) {
		return;
	}

#if OBS_RELEASE_CANDIDATE > 0
	constexpr const char *lastInfoVersion = "InfoLastRCVersion";
#elif OBS_BETA > 0
	constexpr const char *lastInfoVersion = "InfoLastBetaVersion";
#else
	constexpr const char *lastInfoVersion = "InfoLastVersion";
#endif
	constexpr uint64_t currentVersion = (uint64_t)LIBOBS_API_VER << 16ULL | OBS_RELEASE_CANDIDATE << 8ULL |
					    OBS_BETA;
	uint64_t lastVersion = config_get_uint(App()->GetAppConfig(), "General", lastInfoVersion);
	int current_version_increment = -1;

	if ((lastVersion & ~0xFFFF0000ULL) < (currentVersion & ~0xFFFF0000ULL)) {
		config_set_int(App()->GetAppConfig(), "General", "InfoIncrement", -1);
		config_set_uint(App()->GetAppConfig(), "General", lastInfoVersion, currentVersion);
	} else {
		current_version_increment = config_get_int(App()->GetAppConfig(), "General", "InfoIncrement");
	}

	if (info_increment <= current_version_increment) {
		return;
	}

	config_set_int(App()->GetAppConfig(), "General", "InfoIncrement", info_increment);
	config_save_safe(App()->GetAppConfig(), "tmp", nullptr);

	cef->init_browser();

	WhatsNewBrowserInitThread *wnbit = new WhatsNewBrowserInitThread(QT_UTF8(info_url.c_str()));

	connect(wnbit, &WhatsNewBrowserInitThread::Result, this, &OBSBasic::ShowWhatsNew, Qt::QueuedConnection);

	whatsNewInitThread.reset(wnbit);
	whatsNewInitThread->start();

#else
	UNUSED_PARAMETER(text);
#endif
}

void OBSBasic::ShowWhatsNew(const QString &url)
{
#ifdef BROWSER_AVAILABLE
	if (isClosing()) {
		return;
	}

	if (obsWhatsNew) {
		obsWhatsNew->close();
	}

	obsWhatsNew = new OBSWhatsNew(this, QT_TO_UTF8(url));
#else
	UNUSED_PARAMETER(url);
#endif
}

void OBSBasic::TimedCheckForUpdates()
{
	if (App()->IsUpdaterDisabled()) {
		return;
	}
	if (!config_get_bool(App()->GetAppConfig(), "General", "EnableAutoUpdates")) {
		return;
	}

#if defined(ENABLE_SPARKLE_UPDATER)
	CheckForUpdates(false);
#elif _WIN32
	long long lastUpdate = config_get_int(App()->GetAppConfig(), "General", "LastUpdateCheck");
	uint32_t lastVersion = config_get_int(App()->GetAppConfig(), "General", "LastVersion");

	if (lastVersion < LIBOBS_API_VER) {
		lastUpdate = 0;
		config_set_int(App()->GetAppConfig(), "General", "LastUpdateCheck", 0);
	}

	long long t = (long long)time(nullptr);
	long long secs = t - lastUpdate;

	if (secs > UPDATE_CHECK_INTERVAL) {
		CheckForUpdates(false);
	}
#endif
}

void OBSBasic::CheckForUpdates(bool manualUpdate)
{
#if _WIN32
	if (updateCheckThread && updateCheckThread->isRunning()) {
		return;
	}

	ui->actionCheckForUpdates->setEnabled(false);
	const std::vector<std::string> headers = {"Accept: application/vnd.github+json",
						  "X-GitHub-Api-Version: 2022-11-28"};
	auto *thread = new RemoteTextThread("https://api.github.com/repos/byDan4ik/obs-studio/releases/latest",
					    std::vector<std::string>(headers), {}, {}, 15);
	connect(thread, &RemoteTextThread::Result, this,
		[this, manualUpdate](const std::string &text, const std::string &error) {
			updateCheckFinished();
			config_set_int(App()->GetAppConfig(), "General", "LastUpdateCheck", (long long)time(nullptr));
			config_set_int(App()->GetAppConfig(), "General", "LastVersion", LIBOBS_API_VER);
			config_save_safe(App()->GetAppConfig(), "tmp", nullptr);

			if (!error.empty()) {
				blog(LOG_WARNING, "GitHub release check failed: %s", error.c_str());
				if (manualUpdate) {
					QMessageBox::warning(this, QTStr("GitHubUpdater.Error.Title"),
							     QTStr("GitHubUpdater.Error.Text").arg(QString::fromStdString(error)));
				}
				return;
			}

			QString releaseTag;
			QString releaseUrl;
			QString releaseNotes;
			try {
				const nlohmann::json release = nlohmann::json::parse(text);
				if (release.value("draft", false) || release.value("prerelease", false))
					return;

				releaseTag = QString::fromStdString(release.at("tag_name").get<std::string>());
				releaseUrl = QString::fromStdString(release.at("html_url").get<std::string>());
				releaseNotes = QString::fromStdString(release.value("body", std::string()));
			} catch (const nlohmann::json::exception &exception) {
				blog(LOG_WARNING, "Parsing GitHub release information failed: %s", exception.what());
				if (manualUpdate) {
					QMessageBox::warning(this, QTStr("GitHubUpdater.Error.Title"),
							     QTStr("GitHubUpdater.InvalidRelease"));
				}
				return;
			}

			const QUrl parsedReleaseUrl(releaseUrl);
			const QString expectedPath = QStringLiteral("/byDan4ik/obs-studio/releases/tag/%1")
							     .arg(QString::fromUtf8(QUrl::toPercentEncoding(releaseTag)));
			if (parsedReleaseUrl.scheme() != QStringLiteral("https") ||
			    parsedReleaseUrl.host() != QStringLiteral("github.com") ||
			    parsedReleaseUrl.path() != expectedPath) {
				blog(LOG_WARNING, "Rejected unexpected GitHub release URL: %s", QT_TO_UTF8(releaseUrl));
				if (manualUpdate) {
					QMessageBox::warning(this, QTStr("GitHubUpdater.Error.Title"),
							     QTStr("GitHubUpdater.InvalidRelease"));
				}
				return;
			}

			const QVersionNumber latestVersion = QVersionNumber::fromString(releaseTag);
#ifdef OBS_FORK_RELEASE_VERSION
			const QVersionNumber currentVersion = QVersionNumber::fromString(QStringLiteral(OBS_FORK_RELEASE_VERSION));
#else
			const QVersionNumber currentVersion =
				QVersionNumber::fromString(QString::fromUtf8(obs_get_version_string()));
#endif
			if (latestVersion.isNull() || latestVersion.toString() != releaseTag || currentVersion.isNull()) {
				blog(LOG_WARNING, "Invalid fork version: current=%s latest=%s",
				     obs_get_version_string(), QT_TO_UTF8(releaseTag));
				if (manualUpdate) {
					QMessageBox::warning(this, QTStr("GitHubUpdater.Error.Title"),
							     QTStr("GitHubUpdater.InvalidRelease"));
				}
				return;
			}

			if (QVersionNumber::compare(latestVersion, currentVersion) <= 0) {
				if (manualUpdate) {
					QMessageBox::information(this, QTStr("GitHubUpdater.NoUpdates.Title"),
								 QTStr("GitHubUpdater.NoUpdates.Text"));
				}
				return;
			}

			if (releaseNotes.isEmpty())
				releaseNotes = QTStr("GitHubUpdater.ReleaseNotesUnavailable");

			QMessageBox message(this);
			message.setIcon(QMessageBox::Information);
			message.setWindowTitle(QTStr("GitHubUpdater.NewVersion.Title"));
			message.setText(QTStr("GitHubUpdater.NewVersion.Text").arg(releaseTag));
			message.setInformativeText(releaseNotes);
			QPushButton *openRelease = message.addButton(QTStr("GitHubUpdater.OpenRelease"), QMessageBox::AcceptRole);
			message.addButton(QTStr("Cancel"), QMessageBox::RejectRole);
			message.exec();

			if (message.clickedButton() == openRelease && !QDesktopServices::openUrl(parsedReleaseUrl)) {
				blog(LOG_WARNING, "Failed to open GitHub release page: %s", QT_TO_UTF8(releaseUrl));
			}
		});

	updateCheckThread.reset(thread);
	thread->start();
#elif defined(ENABLE_SPARKLE_UPDATER)
	ui->actionCheckForUpdates->setEnabled(false);

	if (updateCheckThread && updateCheckThread->isRunning()) {
		return;
	}

	MacUpdateThread *mut = new MacUpdateThread(manualUpdate);
	connect(mut, &MacUpdateThread::Result, this, &OBSBasic::MacBranchesFetched, Qt::QueuedConnection);
	updateCheckThread.reset(mut);
	updateCheckThread->start();
#else
	UNUSED_PARAMETER(manualUpdate);
#endif
}

void OBSBasic::MacBranchesFetched(const QString &branch, bool manualUpdate)
{
#ifdef ENABLE_SPARKLE_UPDATER
	static OBSSparkle *updater;

	if (!updater) {
		updater = new OBSSparkle(QT_TO_UTF8(branch), ui->actionCheckForUpdates);
		return;
	}

	updater->setBranch(QT_TO_UTF8(branch));
	updater->checkForUpdates(manualUpdate);
#else
	UNUSED_PARAMETER(branch);
	UNUSED_PARAMETER(manualUpdate);
#endif
}
