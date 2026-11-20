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
#include <QByteArrayView>
#include <QDesktopServices>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressDialog>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QVersionNumber>

#include <cstring>
#include <stdexcept>

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

#ifdef _WIN32
const char *forkUpdateScript = R"PS(
param(
    [Parameter(Mandatory = $true)][ValidateSet('Prepare', 'Apply')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$Archive,
    [Parameter(Mandatory = $true)][string]$InstallRoot,
    [Parameter(Mandatory = $true)][string]$StageRoot,
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$LogPath,
    [int]$ProcessId = 0
)

$ErrorActionPreference = 'Stop'

function Write-UpdateLog([string]$Message) {
    [System.IO.File]::AppendAllText($LogPath, "$(Get-Date -Format o) $Message`r`n")
}

try {
    $installFull = [System.IO.Path]::GetFullPath($InstallRoot).TrimEnd('\')
    $stageFull = [System.IO.Path]::GetFullPath($StageRoot).TrimEnd('\')
    $installParent = [System.IO.Directory]::GetParent($installFull).FullName
    $stageParent = [System.IO.Directory]::GetParent($stageFull).FullName
    if (-not [string]::Equals($installParent, $stageParent, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'The staging directory must be beside the OBS installation.'
    }

    $payload = Join-Path $stageFull 'payload'
    if ($Mode -eq 'Prepare') {
        if (-not (Test-Path -LiteralPath $Archive -PathType Leaf)) {
            throw 'The downloaded release archive is missing.'
        }

        Add-Type -AssemblyName System.IO.Compression.FileSystem
        New-Item -ItemType Directory -Path $payload -ErrorAction Stop | Out-Null
        $payloadPrefix = [System.IO.Path]::GetFullPath($payload).TrimEnd('\') + '\'
        $zip = [System.IO.Compression.ZipFile]::OpenRead($Archive)
        try {
            if ($zip.Entries.Count -eq 0 -or $zip.Entries.Count -gt 100000) {
                throw 'The release archive has an invalid number of entries.'
            }

            [long]$totalBytes = 0
            foreach ($entry in $zip.Entries) {
                $relative = $entry.FullName.Replace('/', '\')
                if ([string]::IsNullOrWhiteSpace($relative) -or
                    [System.IO.Path]::IsPathRooted($relative) -or
                    $relative -match '(^|\\)\.{1,2}(\\|$)' -or
                    $relative -match '^config(\\|$)' -or
                    $relative.Contains(':') -or
                    (($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) {
                    throw 'The release archive contains an unsafe path or link.'
                }

                $destination = [System.IO.Path]::GetFullPath((Join-Path $payload $relative))
                if (-not $destination.StartsWith($payloadPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                    throw 'The release archive contains a path outside its installation directory.'
                }

                if ($entry.FullName.EndsWith('/')) {
                    New-Item -ItemType Directory -Force -Path $destination | Out-Null
                    continue
                }

                $totalBytes += $entry.Length
                if ($totalBytes -gt 4GB) {
                    throw 'The release archive expands beyond the allowed size.'
                }

                New-Item -ItemType Directory -Force -Path ([System.IO.Path]::GetDirectoryName($destination)) | Out-Null
                [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $false)
            }
        } finally {
            $zip.Dispose()
        }

        if (-not (Test-Path -LiteralPath (Join-Path $payload 'portable_mode') -PathType Leaf) -or
            -not (Test-Path -LiteralPath (Join-Path $payload 'bin\64bit\obs64.exe') -PathType Leaf)) {
            throw 'The archive is not a valid byDan4ik portable x64 release.'
        }

        $userConfig = Join-Path $installFull 'config'
        if (Test-Path -LiteralPath $userConfig -PathType Container) {
            Copy-Item -LiteralPath $userConfig -Destination $payload -Recurse -Force
        }
        exit 0
    }

    $payloadExe = Join-Path $payload 'bin\64bit\obs64.exe'
    if (-not (Test-Path -LiteralPath $payloadExe -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $payload 'portable_mode') -PathType Leaf)) {
        throw 'The staged update is incomplete.'
    }

    $running = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if ($running) {
        if ($running.Path -and -not [string]::Equals(
                [System.IO.Path]::GetFullPath($running.Path),
                [System.IO.Path]::GetFullPath((Join-Path $installFull 'bin\64bit\obs64.exe')),
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw 'The process being updated is not the OBS installation at the expected location.'
        }
        $running.WaitForExit()
    }

    $backup = "$installFull.backup-$([guid]::NewGuid().ToString('N'))"
    $swapped = $false
    try {
        Move-Item -LiteralPath $installFull -Destination $backup
        Move-Item -LiteralPath $payload -Destination $installFull
        $swapped = $true
        Start-Process -FilePath (Join-Path $installFull 'bin\64bit\obs64.exe') -WorkingDirectory $installFull
    } catch {
        if ($swapped -and (Test-Path -LiteralPath $installFull)) {
            Move-Item -LiteralPath $installFull -Destination $payload
        }
        if (Test-Path -LiteralPath $backup) {
            Move-Item -LiteralPath $backup -Destination $installFull
        }
        throw
    }

    try {
        Remove-Item -LiteralPath $backup -Recurse -Force
    } catch {
        Write-UpdateLog "The previous installation remains at '$backup': $($_.Exception.Message)"
    }
    try {
        Remove-Item -LiteralPath $stageFull -Recurse -Force
    } catch {
        Write-UpdateLog "The staging directory remains at '$stageFull': $($_.Exception.Message)"
    }
    try {
        Remove-Item -LiteralPath $PSCommandPath -Force
    } catch {
        Write-UpdateLog "The updater script remains at '$PSCommandPath': $($_.Exception.Message)"
    }
    exit 0
} catch {
    $message = "Update $Mode to version $Version failed: $($_.Exception.Message)"
    Write-UpdateLog $message
    if ($Mode -eq 'Apply') {
        try {
            Add-Type -AssemblyName System.Windows.Forms
            [System.Windows.Forms.MessageBox]::Show(
                "$message`r`n`r`nDetails: $LogPath",
                'OBS Studio update failed',
                [System.Windows.Forms.MessageBoxButtons]::OK,
                [System.Windows.Forms.MessageBoxIcon]::Error
            ) | Out-Null
        } catch {
            Write-UpdateLog "Could not display the update error: $($_.Exception.Message)"
        }
    }
    exit 1
}
)PS";
#endif

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

#ifdef _WIN32
void OBSBasic::DownloadForkRelease(const QUrl &assetUrl, const QString &version, const QByteArray &sha256, qint64 size)
{
	if (forkUpdateInProgress)
		return;

	QDir installRoot(QCoreApplication::applicationDirPath());
	if (!installRoot.cdUp() || !installRoot.cdUp() ||
	    !QFileInfo::exists(installRoot.filePath(QStringLiteral("portable_mode")))) {
		QMessageBox::warning(this, QTStr("GitHubUpdater.Error.Title"), QTStr("GitHubUpdater.PortableOnly"));
		return;
	}

	forkUpdateInProgress = true;
	ui->actionCheckForUpdates->setEnabled(false);

	QTemporaryDir stageDir(installRoot.filePath(QStringLiteral(".OBS-Studio-update-XXXXXX")));
	stageDir.setAutoRemove(false);
	if (!stageDir.isValid()) {
		forkUpdateInProgress = false;
		updateCheckFinished();
		QMessageBox::warning(this, QTStr("GitHubUpdater.Error.Title"),
				     QTStr("GitHubUpdater.PrepareFailed").arg(stageDir.errorString()));
		return;
	}

	const QString stagePath = stageDir.path();
	const QString archivePath = QDir(stagePath).filePath(QStringLiteral("release.zip"));
	const QString scriptPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
					   .filePath(QStringLiteral("obs-studio-updater-%1.ps1")
							     .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
	QFile script(scriptPath);
	if (!script.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
	    script.write(forkUpdateScript) != static_cast<qint64>(strlen(forkUpdateScript))) {
		const QString error = script.errorString();
		script.remove();
		QDir(stagePath).removeRecursively();
		forkUpdateInProgress = false;
		updateCheckFinished();
		QMessageBox::warning(this, QTStr("GitHubUpdater.Error.Title"),
				     QTStr("GitHubUpdater.PrepareFailed").arg(error));
		return;
	}
	script.close();

	struct DownloadState {
		QSaveFile file;
		QCryptographicHash hash{QCryptographicHash::Sha256};
		qint64 expectedSize;
		qint64 received = 0;
		QString error;

		DownloadState(const QString &path, qint64 size) : file(path), expectedSize(size) {}
	};

	auto state = std::make_shared<DownloadState>(archivePath, size);
	if (!state->file.open(QIODevice::WriteOnly)) {
		const QString error = state->file.errorString();
		script.remove();
		QDir(stagePath).removeRecursively();
		forkUpdateInProgress = false;
		updateCheckFinished();
		QMessageBox::warning(this, QTStr("GitHubUpdater.Error.Title"),
				     QTStr("GitHubUpdater.PrepareFailed").arg(error));
		return;
	}

	auto *progress = new QProgressDialog(QTStr("GitHubUpdater.Downloading").arg(version), QTStr("Cancel"), 0,
					     1000, this);
	progress->setWindowTitle(QTStr("GitHubUpdater.Downloading.Title"));
	progress->setWindowModality(Qt::WindowModal);
	progress->setMinimumDuration(0);
	progress->setAutoClose(false);
	progress->setValue(0);

	auto *manager = new QNetworkAccessManager(this);
	QNetworkRequest request(assetUrl);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setTransferTimeout(30000);
	auto *reply = manager->get(request);

	connect(progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
	connect(reply, &QNetworkReply::readyRead, this, [reply, state, progress]() {
		const QByteArray data = reply->readAll();
		if (data.isEmpty())
			return;

		state->received += data.size();
		if (state->received > state->expectedSize) {
			state->error = QStringLiteral("The downloaded archive is larger than the release asset.");
			reply->abort();
			return;
		}

		if (state->file.write(data) != data.size()) {
			state->error = state->file.errorString();
			reply->abort();
			return;
		}
		state->hash.addData(QByteArrayView(data));
		progress->setValue(static_cast<int>((state->received * 1000) / state->expectedSize));
	});

	connect(reply, &QNetworkReply::finished, this,
		[this, reply, manager, progress, state, scriptPath, stagePath, archivePath, installRoot, version, sha256]() {
			const QByteArray remaining = reply->readAll();
			if (!remaining.isEmpty()) {
				const QByteArray &data = remaining;
				state->received += data.size();
				if (state->received > state->expectedSize) {
					state->error =
						QStringLiteral("The downloaded archive is larger than the release asset.");
				} else if (state->file.write(data) != data.size()) {
					state->error = state->file.errorString();
				} else {
					state->hash.addData(QByteArrayView(data));
				}
			}

			const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			const QString host = reply->url().host().toLower();
			const bool trustedHost = host == QStringLiteral("github.com") ||
						 host == QStringLiteral("githubusercontent.com") ||
						 host.endsWith(QStringLiteral(".githubusercontent.com"));
			QString error = state->error;
			if (error.isEmpty() && reply->error() != QNetworkReply::NoError)
				error = reply->errorString();
			if (error.isEmpty() && status != 200)
				error = QStringLiteral("GitHub returned HTTP %1.").arg(status);
			if (error.isEmpty() && (reply->url().scheme() != QStringLiteral("https") || !trustedHost))
				error = QStringLiteral("The release download redirected to an untrusted host.");
			if (error.isEmpty() && state->received != state->expectedSize)
				error = QStringLiteral("The downloaded archive size does not match the release metadata.");
			if (error.isEmpty() && state->hash.result().toHex() != sha256)
				error = QStringLiteral("The downloaded archive failed its SHA-256 integrity check.");
			if (error.isEmpty() && !state->file.commit())
				error = state->file.errorString();
			if (!error.isEmpty())
				state->file.cancelWriting();
			const bool cancelled = reply->error() == QNetworkReply::OperationCanceledError &&
					       state->error.isEmpty();

			progress->close();
			progress->deleteLater();
			reply->deleteLater();
			manager->deleteLater();

			auto finish = [this, stagePath, scriptPath]() {
				QFile::remove(scriptPath);
				if (!QDir(stagePath).removeRecursively())
					blog(LOG_WARNING, "Could not remove canceled or failed update staging directory: %s",
					     QT_TO_UTF8(stagePath));
				forkUpdateInProgress = false;
				updateCheckFinished();
			};

			if (!error.isEmpty()) {
				finish();
				if (!cancelled) {
					blog(LOG_WARNING, "Fork release download failed: %s", QT_TO_UTF8(error));
					QMessageBox::warning(this, QTStr("GitHubUpdater.Error.Title"),
							     QTStr("GitHubUpdater.DownloadFailed").arg(error));
				}
				return;
			}

			const QString powershell = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
			if (powershell.isEmpty()) {
				finish();
				QMessageBox::warning(this, QTStr("GitHubUpdater.Error.Title"),
						     QTStr("GitHubUpdater.PrepareFailed")
							     .arg(QStringLiteral("PowerShell was not found.")));
				return;
			}

			auto *prepare = new QProcess(this);
			prepare->setProgram(powershell);
			prepare->setArguments({QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
					       QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
					       QStringLiteral("-File"), scriptPath, QStringLiteral("-Mode"),
					       QStringLiteral("Prepare"), QStringLiteral("-Archive"), archivePath,
					       QStringLiteral("-InstallRoot"), installRoot.absolutePath(),
					       QStringLiteral("-StageRoot"), stagePath, QStringLiteral("-Version"), version,
					       QStringLiteral("-LogPath"),
					       QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
						       .filePath(QStringLiteral("OBS-Studio-update-%1.log").arg(version))});
			prepare->setProcessChannelMode(QProcess::MergedChannels);
			connect(prepare, &QProcess::errorOccurred, this,
				[this, prepare, stagePath, scriptPath](QProcess::ProcessError processError) {
					if (processError != QProcess::FailedToStart)
						return;
					const QString error = prepare->errorString();
					prepare->deleteLater();
					QFile::remove(scriptPath);
					QDir(stagePath).removeRecursively();
					forkUpdateInProgress = false;
					updateCheckFinished();
					QMessageBox::warning(this, QTStr("GitHubUpdater.Error.Title"),
							     QTStr("GitHubUpdater.PrepareFailed").arg(error));
				});
			connect(prepare, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
				[this, prepare, stagePath, scriptPath, installRoot, version]() {
					const QByteArray output = prepare->readAll();
					const int exitCode = prepare->exitCode();
					prepare->deleteLater();

					if (exitCode != 0) {
						QFile::remove(scriptPath);
						QDir(stagePath).removeRecursively();
						forkUpdateInProgress = false;
						updateCheckFinished();
						const QString details = QString::fromLocal8Bit(output).trimmed();
						blog(LOG_WARNING, "Preparing fork release failed: %s", QT_TO_UTF8(details));
						QMessageBox::warning(this, QTStr("GitHubUpdater.Error.Title"),
								     QTStr("GitHubUpdater.PrepareFailed").arg(details));
						return;
					}

					QMessageBox message(this);
					message.setIcon(QMessageBox::Information);
					message.setWindowTitle(QTStr("GitHubUpdater.Install.Title"));
					message.setText(QTStr("GitHubUpdater.Install.Text").arg(version));
					QPushButton *install = message.addButton(QTStr("GitHubUpdater.Install"), QMessageBox::AcceptRole);
					message.addButton(QTStr("Cancel"), QMessageBox::RejectRole);
					message.exec();

					if (message.clickedButton() != install) {
						QFile::remove(scriptPath);
						if (!QDir(stagePath).removeRecursively())
							blog(LOG_WARNING, "Could not remove canceled update staging directory: %s",
							     QT_TO_UTF8(stagePath));
						forkUpdateInProgress = false;
						updateCheckFinished();
						return;
					}

					pendingForkUpdateScript = scriptPath;
					pendingForkUpdateStage = stagePath;
					pendingForkUpdateInstallRoot = installRoot.absolutePath();
					pendingForkUpdateVersion = version;
					close();
					if (!isClosing()) {
						pendingForkUpdateScript.clear();
						pendingForkUpdateStage.clear();
						pendingForkUpdateInstallRoot.clear();
						pendingForkUpdateVersion.clear();
						QFile::remove(scriptPath);
						QDir(stagePath).removeRecursively();
						forkUpdateInProgress = false;
						updateCheckFinished();
					}
				});
			prepare->start();
		});

	progress->show();
}

void OBSBasic::StartPendingForkUpdate()
{
	if (pendingForkUpdateScript.isEmpty())
		return;

	const QString powershell = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
	const QString logPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
					.filePath(QStringLiteral("OBS-Studio-update-%1.log").arg(pendingForkUpdateVersion));
	const QStringList arguments = {
		QStringLiteral("-NoProfile"),
		QStringLiteral("-NonInteractive"),
		QStringLiteral("-ExecutionPolicy"),
		QStringLiteral("Bypass"),
		QStringLiteral("-WindowStyle"),
		QStringLiteral("Hidden"),
		QStringLiteral("-File"),
		pendingForkUpdateScript,
		QStringLiteral("-Mode"),
		QStringLiteral("Apply"),
		QStringLiteral("-Archive"),
		QDir(pendingForkUpdateStage).filePath(QStringLiteral("release.zip")),
		QStringLiteral("-InstallRoot"),
		pendingForkUpdateInstallRoot,
		QStringLiteral("-StageRoot"),
		pendingForkUpdateStage,
		QStringLiteral("-Version"),
		pendingForkUpdateVersion,
		QStringLiteral("-LogPath"),
		logPath,
		QStringLiteral("-ProcessId"),
		QString::number(QCoreApplication::applicationPid()),
	};
	if (powershell.isEmpty() || !QProcess::startDetached(powershell, arguments, QString())) {
		blog(LOG_ERROR, "Could not start the fork update installer for version %s.",
		     QT_TO_UTF8(pendingForkUpdateVersion));
		QFile::remove(pendingForkUpdateScript);
		QDir(pendingForkUpdateStage).removeRecursively();
	}

	pendingForkUpdateScript.clear();
	pendingForkUpdateStage.clear();
	pendingForkUpdateInstallRoot.clear();
	pendingForkUpdateVersion.clear();
}
#endif

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
			QUrl assetUrl;
			QByteArray assetSha256;
			qint64 assetSize = 0;
			try {
				const nlohmann::json release = nlohmann::json::parse(text);
				if (release.value("draft", false) || release.value("prerelease", false))
					return;

				releaseTag = QString::fromStdString(release.at("tag_name").get<std::string>());
				releaseUrl = QString::fromStdString(release.at("html_url").get<std::string>());
				releaseNotes = QString::fromStdString(release.value("body", std::string()));

				const QString expectedAssetName =
					QStringLiteral("OBS-Studio-byDan4ik-%1-windows-x64-portable.zip").arg(releaseTag);
				for (const auto &asset : release.at("assets")) {
					if (asset.value("name", std::string()) != expectedAssetName.toStdString())
						continue;

					const QString digest = QString::fromStdString(asset.at("digest").get<std::string>());
					const QString expectedDigestPattern = QStringLiteral("^sha256:([0-9a-fA-F]{64})$");
					const QRegularExpression digestRegex(expectedDigestPattern);
					const QRegularExpressionMatch digestMatch = digestRegex.match(digest);
					if (!digestMatch.hasMatch())
						throw std::runtime_error("Release asset has no valid SHA-256 digest.");

					assetSha256 = digestMatch.captured(1).toLatin1().toLower();
					assetSize = asset.at("size").get<qint64>();
					assetUrl = QUrl(QString::fromStdString(asset.at("browser_download_url").get<std::string>()));
					break;
				}
			} catch (const nlohmann::json::exception &exception) {
				blog(LOG_WARNING, "Parsing GitHub release information failed: %s", exception.what());
				if (manualUpdate) {
					QMessageBox::warning(this, QTStr("GitHubUpdater.Error.Title"),
							     QTStr("GitHubUpdater.InvalidRelease"));
				}
				return;
			} catch (const std::exception &exception) {
				blog(LOG_WARNING, "Invalid GitHub release asset: %s", exception.what());
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

			const QString expectedAssetName =
				QStringLiteral("OBS-Studio-byDan4ik-%1-windows-x64-portable.zip").arg(releaseTag);
			const QString expectedAssetPath =
				QStringLiteral("/byDan4ik/obs-studio/releases/download/%1/%2")
					.arg(QString::fromUtf8(QUrl::toPercentEncoding(releaseTag)), expectedAssetName);
			if (assetUrl.scheme() != QStringLiteral("https") ||
			    assetUrl.host() != QStringLiteral("github.com") || assetUrl.path() != expectedAssetPath ||
			    assetSha256.size() != 64 || assetSize <= 0 || assetSize > 1024LL * 1024 * 1024) {
				blog(LOG_WARNING, "GitHub release is missing a valid portable x64 asset: %s",
				     QT_TO_UTF8(releaseTag));
				if (manualUpdate) {
					QMessageBox::warning(this, QTStr("GitHubUpdater.Error.Title"),
							     QTStr("GitHubUpdater.InvalidRelease"));
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
			QPushButton *installUpdate =
				message.addButton(QTStr("GitHubUpdater.Install"), QMessageBox::AcceptRole);
			QPushButton *openRelease = message.addButton(QTStr("GitHubUpdater.OpenRelease"), QMessageBox::AcceptRole);
			message.addButton(QTStr("Cancel"), QMessageBox::RejectRole);
			message.exec();

			if (message.clickedButton() == installUpdate) {
				DownloadForkRelease(assetUrl, releaseTag, assetSha256, assetSize);
			} else if (message.clickedButton() == openRelease && !QDesktopServices::openUrl(parsedReleaseUrl)) {
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
