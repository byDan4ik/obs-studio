#include "OBSBasicControls.hpp"
#include "OBSBasic.hpp"
#include "qt-wrappers.hpp"

#include "moc_OBSBasicControls.cpp"

#include <QMessageBox>

#include <cwchar>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
constexpr wchar_t SoundSchemesKey[] = L"AppEvents\\Schemes";
constexpr wchar_t DwmKey[] = L"Software\\Microsoft\\Windows\\DWM";
constexpr wchar_t ExplorerAdvancedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

bool ReadSoundScheme(std::wstring &scheme)
{
	wchar_t value[128] = {};
	DWORD size = sizeof(value);
	const LSTATUS status =
		RegGetValueW(HKEY_CURRENT_USER, SoundSchemesKey, nullptr, RRF_RT_REG_SZ, nullptr, value, &size);
	if (status != ERROR_SUCCESS)
		return false;

	scheme.assign(value);
	return true;
}

bool WriteSoundScheme(const wchar_t *scheme)
{
	HKEY key = nullptr;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, SoundSchemesKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
			    nullptr) != ERROR_SUCCESS) {
		return false;
	}

	const DWORD size = static_cast<DWORD>((std::wcslen(scheme) + 1) * sizeof(wchar_t));
	const LSTATUS status = RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE *>(scheme), size);
	RegCloseKey(key);
	return status == ERROR_SUCCESS;
}

bool CopyDefaultSoundEventValues(HKEY eventKey, HKEY currentKey)
{
	HKEY defaultKey = nullptr;
	if (RegOpenKeyExW(eventKey, L".Default", 0, KEY_QUERY_VALUE, &defaultKey) != ERROR_SUCCESS)
		return false;

	DWORD type = 0;
	DWORD size = 0;
	LSTATUS status = RegQueryValueExW(defaultKey, nullptr, nullptr, &type, nullptr, &size);
	if (status == ERROR_FILE_NOT_FOUND || (status == ERROR_SUCCESS && size == 0)) {
		RegCloseKey(defaultKey);
		const wchar_t emptyValue[] = L"";
		return RegSetValueExW(currentKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE *>(emptyValue),
				      sizeof(emptyValue)) == ERROR_SUCCESS;
	}

	if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
		RegCloseKey(defaultKey);
		return false;
	}

	std::vector<BYTE> value(size);
	status = RegQueryValueExW(defaultKey, nullptr, nullptr, &type, value.data(), &size);
	RegCloseKey(defaultKey);
	if (status != ERROR_SUCCESS)
		return false;

	return RegSetValueExW(currentKey, nullptr, 0, type, value.data(), size) == ERROR_SUCCESS;
}

bool ApplyBuiltInSoundScheme(HKEY key, bool noSounds)
{
	DWORD subKeyCount = 0;
	DWORD maxSubKeyLength = 0;
	if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subKeyCount, &maxSubKeyLength, nullptr, nullptr,
			     nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
		return false;
	}

	std::vector<wchar_t> subKeyName(maxSubKeyLength + 1);
	for (DWORD index = 0; index < subKeyCount; index++) {
		DWORD nameLength = static_cast<DWORD>(subKeyName.size());
		const LSTATUS enumStatus =
			RegEnumKeyExW(key, index, subKeyName.data(), &nameLength, nullptr, nullptr, nullptr, nullptr);
		if (enumStatus != ERROR_SUCCESS)
			return false;

		if (std::wcscmp(subKeyName.data(), L".Current") == 0) {
			HKEY currentKey = nullptr;
			if (RegOpenKeyExW(key, L".Current", 0, KEY_SET_VALUE, &currentKey) != ERROR_SUCCESS)
				return false;

			bool updated = false;
			if (noSounds) {
				const wchar_t emptyValue[] = L"";
				updated = RegSetValueExW(currentKey, nullptr, 0, REG_SZ,
							 reinterpret_cast<const BYTE *>(emptyValue), sizeof(emptyValue)) ==
					  ERROR_SUCCESS;
			} else {
				updated = CopyDefaultSoundEventValues(key, currentKey);
			}

			RegCloseKey(currentKey);
			if (!updated)
				return false;
			continue;
		}

		HKEY childKey = nullptr;
		if (RegOpenKeyExW(key, subKeyName.data(), 0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE,
				  &childKey) != ERROR_SUCCESS) {
			return false;
		}

		const bool updated = ApplyBuiltInSoundScheme(childKey, noSounds);
		RegCloseKey(childKey);
		if (!updated)
			return false;
	}

	return true;
}

bool WriteDwordSetting(const wchar_t *keyPath, const wchar_t *name, DWORD value)
{
	HKEY key = nullptr;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
	    ERROR_SUCCESS) {
		return false;
	}

	const LSTATUS status =
		RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
	RegCloseKey(key);
	return status == ERROR_SUCCESS;
}

void NotifyWindowsSettings(const wchar_t *area)
{
	DWORD_PTR result = 0;
	SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(area),
			    SMTO_ABORTIFHUNG, 1000, &result);
}
#endif

} // namespace

OBSBasicControls::OBSBasicControls(OBSBasic *main) : QFrame(nullptr), ui(new Ui::OBSBasicControls)
{
	/* Create UI elements */
	ui->setupUi(this);
	ui->modeSwitch->setVisible(false);

#ifdef _WIN32
	std::wstring soundScheme;
	if (ReadSoundScheme(soundScheme))
		windowsSystemMuted = soundScheme == L".None";
	else
		blog(LOG_WARNING, "Could not read the Windows sound scheme from the registry");
	connect(ui->mwButton, &QPushButton::clicked, this, &OBSBasicControls::ToggleWindowsSoundMode);
	UpdateWindowsSoundButtonState();
#else
	ui->mwButton->setVisible(false);
#endif

	streamButtonMenu.reset(new QMenu());
	startStreamAction = streamButtonMenu->addAction(QTStr("Basic.Main.StartStreaming"));
	stopStreamAction = streamButtonMenu->addAction(QTStr("Basic.Main.StopStreaming"));
	QAction *forceStopStreamAction = streamButtonMenu->addAction(QTStr("Basic.Main.ForceStopStreaming"));

	/* Transfer buttons signals as OBSBasicControls signals */
	connect(
		ui->streamButton, &QPushButton::clicked, this, [this]() { emit this->StreamButtonClicked(); },
		Qt::DirectConnection);
	connect(
		ui->broadcastButton, &QPushButton::clicked, this, [this]() { emit this->BroadcastButtonClicked(); },
		Qt::DirectConnection);
	connect(
		ui->recordButton, &QPushButton::clicked, this, [this]() { emit this->RecordButtonClicked(); },
		Qt::DirectConnection);
	connect(
		ui->pauseRecordButton, &QPushButton::clicked, this, [this]() { emit this->PauseRecordButtonClicked(); },
		Qt::DirectConnection);
	connect(
		ui->replayBufferButton, &QPushButton::clicked, this,
		[this]() { emit this->ReplayBufferButtonClicked(); }, Qt::DirectConnection);
	connect(
		ui->saveReplayButton, &QPushButton::clicked, this,
		[this]() { emit this->SaveReplayBufferButtonClicked(); }, Qt::DirectConnection);
	connect(
		ui->virtualCamButton, &QPushButton::clicked, this, [this]() { emit this->VirtualCamButtonClicked(); },
		Qt::DirectConnection);
	connect(
		ui->virtualCamConfigButton, &QPushButton::clicked, this,
		[this]() { emit this->VirtualCamConfigButtonClicked(); }, Qt::DirectConnection);
	connect(
		ui->modeSwitch, &QPushButton::clicked, this, [this]() { emit this->StudioModeButtonClicked(); },
		Qt::DirectConnection);
	connect(
		ui->settingsButton, &QPushButton::clicked, this, [this]() { emit this->SettingsButtonClicked(); },
		Qt::DirectConnection);

	/* Transfer menu actions signals as OBSBasicControls signals */
	connect(
		startStreamAction.get(), &QAction::triggered, this,
		[this]() { emit this->StartStreamMenuActionClicked(); }, Qt::DirectConnection);
	connect(
		stopStreamAction.get(), &QAction::triggered, this,
		[this]() { emit this->StopStreamMenuActionClicked(); }, Qt::DirectConnection);
	connect(
		forceStopStreamAction, &QAction::triggered, this,
		[this]() { emit this->ForceStopStreamMenuActionClicked(); }, Qt::DirectConnection);

	/* Set up default visibility */
	ui->broadcastButton->setVisible(false);
	ui->pauseRecordButton->setVisible(false);
	ui->replayBufferButton->setVisible(false);
	ui->saveReplayButton->setVisible(false);
	ui->virtualCamButton->setVisible(false);
	ui->virtualCamConfigButton->setVisible(false);

	/* Set up state update connections */
	connect(main, &OBSBasic::StreamingPreparing, this, &OBSBasicControls::StreamingPreparing);
	connect(main, &OBSBasic::StreamingStarting, this, &OBSBasicControls::StreamingStarting);
	connect(main, &OBSBasic::StreamingStarted, this, &OBSBasicControls::StreamingStarted);
	connect(main, &OBSBasic::StreamingStopping, this, &OBSBasicControls::StreamingStopping);
	connect(main, &OBSBasic::StreamingStopped, this, &OBSBasicControls::StreamingStopped);

	connect(main, &OBSBasic::BroadcastStreamReady, this, &OBSBasicControls::BroadcastStreamReady);
	connect(main, &OBSBasic::BroadcastStreamActive, this, &OBSBasicControls::BroadcastStreamActive);
	connect(main, &OBSBasic::BroadcastStreamStarted, this, &OBSBasicControls::BroadcastStreamStarted);

	connect(main, &OBSBasic::RecordingStarted, this, &OBSBasicControls::RecordingStarted);
	connect(main, &OBSBasic::RecordingPaused, this, &OBSBasicControls::RecordingPaused);
	connect(main, &OBSBasic::RecordingUnpaused, this, &OBSBasicControls::RecordingUnpaused);
	connect(main, &OBSBasic::RecordingStopping, this, &OBSBasicControls::RecordingStopping);
	connect(main, &OBSBasic::RecordingStopped, this, &OBSBasicControls::RecordingStopped);

	connect(main, &OBSBasic::ReplayBufStarted, this, &OBSBasicControls::ReplayBufferStarted);
	connect(main, &OBSBasic::ReplayBufStopping, this, &OBSBasicControls::ReplayBufferStopping);
	connect(main, &OBSBasic::ReplayBufStopped, this, &OBSBasicControls::ReplayBufferStopped);

	connect(main, &OBSBasic::VirtualCamStarted, this, &OBSBasicControls::VirtualCamStarted);
	connect(main, &OBSBasic::VirtualCamStopped, this, &OBSBasicControls::VirtualCamStopped);

	connect(main, &OBSBasic::PreviewProgramModeChanged, this, &OBSBasicControls::UpdateStudioModeState);

	/* Set up enablement connection */
	connect(main, &OBSBasic::BroadcastFlowEnabled, this, &OBSBasicControls::EnableBroadcastFlow);
	connect(main, &OBSBasic::ReplayBufEnabled, this, &OBSBasicControls::EnableReplayBufferButtons);
	connect(main, &OBSBasic::VirtualCamEnabled, this, &OBSBasicControls::EnableVirtualCamButtons);
}

void OBSBasicControls::ToggleWindowsSoundMode()
{
#ifdef _WIN32
	HKEY soundEventsKey = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"AppEvents\\Schemes\\Apps", 0,
			  KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE, &soundEventsKey) != ERROR_SUCCESS) {
		QMessageBox::warning(this, QTStr("Basic.Main.WindowsEffects"),
				     QTStr("Basic.Main.WindowsEffectsWriteError"));
		return;
	}

	if (!windowsSystemMuted) {
		const bool eventsCleared = ApplyBuiltInSoundScheme(soundEventsKey, true);
		RegCloseKey(soundEventsKey);
		if (!eventsCleared || !WriteSoundScheme(L".None")) {
			QMessageBox::warning(this, QTStr("Basic.Main.WindowsEffects"),
					     QTStr("Basic.Main.WindowsEffectsWriteError"));
			return;
		}

		windowsSystemMuted = true;

		const bool aeroPeekUpdated = WriteDwordSetting(DwmKey, L"EnableAeroPeek", 0);
		const bool previewDesktopUpdated =
			WriteDwordSetting(ExplorerAdvancedKey, L"DisablePreviewDesktop", 1);
		const bool peekUpdated = aeroPeekUpdated && previewDesktopUpdated;
		NotifyWindowsSettings(L"AppEvents");
		NotifyWindowsSettings(L"Software\\Microsoft\\Windows\\DWM");
		NotifyWindowsSettings(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced");

		UpdateWindowsSoundButtonState();
		if (!peekUpdated) {
			QMessageBox::warning(this, QTStr("Basic.Main.WindowsEffects"),
					     QTStr("Basic.Main.WindowsEffectsWriteError"));
		}
	} else {
		const bool eventsRestored = ApplyBuiltInSoundScheme(soundEventsKey, false);
		RegCloseKey(soundEventsKey);
		if (!eventsRestored || !WriteSoundScheme(L".Default")) {
			QMessageBox::warning(this, QTStr("Basic.Main.WindowsEffects"),
					     QTStr("Basic.Main.WindowsEffectsWriteError"));
			return;
		}

		windowsSystemMuted = false;
		const bool aeroPeekRestored =
			WriteDwordSetting(DwmKey, L"EnableAeroPeek", 1);
		const bool previewDesktopRestored =
			WriteDwordSetting(ExplorerAdvancedKey, L"DisablePreviewDesktop", 0);
		const bool peekRestored = aeroPeekRestored && previewDesktopRestored;
		NotifyWindowsSettings(L"AppEvents");
		NotifyWindowsSettings(L"Software\\Microsoft\\Windows\\DWM");
		NotifyWindowsSettings(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced");

		UpdateWindowsSoundButtonState();
		if (!peekRestored) {
			QMessageBox::warning(this, QTStr("Basic.Main.WindowsEffects"),
					     QTStr("Basic.Main.WindowsEffectsWriteError"));
		}
	}
#endif
}

void OBSBasicControls::UpdateWindowsSoundButtonState()
{
	setClasses(ui->mwButton, windowsSystemMuted ? "state-active" : "");
	ui->mwButton->setToolTip(QTStr(windowsSystemMuted ? "Basic.Main.WindowsEffectsMuted"
							 : "Basic.Main.WindowsEffects"));
}

void OBSBasicControls::StreamingPreparing()
{
	ui->streamButton->setEnabled(false);
	ui->streamButton->setText(QTStr("Basic.Main.PreparingStream"));
}

void OBSBasicControls::StreamingStarting(bool broadcastAutoStart)
{
	ui->streamButton->setText(QTStr("Basic.Main.Connecting"));

	if (!broadcastAutoStart) {
		// well, we need to disable button while stream is not active
		ui->broadcastButton->setEnabled(false);

		ui->broadcastButton->setText(QTStr("Basic.Main.StartBroadcast"));

		ui->broadcastButton->setProperty("broadcastState", "ready");
		ui->broadcastButton->style()->unpolish(ui->broadcastButton);
		ui->broadcastButton->style()->polish(ui->broadcastButton);
	}
}

void OBSBasicControls::StreamingStarted(bool withDelay)
{
	ui->streamButton->setEnabled(true);
	setClasses(ui->streamButton, "state-active");
	ui->streamButton->setText(QTStr("Basic.Main.StopStreaming"));

	if (withDelay) {
		ui->streamButton->setMenu(streamButtonMenu.get());
		startStreamAction->setVisible(false);
		stopStreamAction->setVisible(true);
	}
}

void OBSBasicControls::StreamingStopping()
{
	ui->streamButton->setText(QTStr("Basic.Main.StoppingStreaming"));
}

void OBSBasicControls::StreamingStopped(bool withDelay)
{
	ui->streamButton->setEnabled(true);
	setClasses(ui->streamButton, "");
	ui->streamButton->setText(QTStr("Basic.Main.StartStreaming"));

	if (withDelay) {
		if (!ui->streamButton->menu()) {
			ui->streamButton->setMenu(streamButtonMenu.get());
		}

		startStreamAction->setVisible(true);
		stopStreamAction->setVisible(false);
	} else {
		ui->streamButton->setMenu(nullptr);
	}
}

void OBSBasicControls::BroadcastStreamReady(bool ready)
{
	setClasses(ui->broadcastButton, ready ? "state-active" : "");
}

void OBSBasicControls::BroadcastStreamActive()
{
	ui->broadcastButton->setEnabled(true);
}

void OBSBasicControls::BroadcastStreamStarted(bool autoStop)
{
	ui->broadcastButton->setText(QTStr(autoStop ? "Basic.Main.AutoStopEnabled" : "Basic.Main.StopBroadcast"));
	if (autoStop) {
		ui->broadcastButton->setEnabled(false);
	}

	ui->broadcastButton->setProperty("broadcastState", "active");
	ui->broadcastButton->style()->unpolish(ui->broadcastButton);
	ui->broadcastButton->style()->polish(ui->broadcastButton);
}

void OBSBasicControls::RecordingStarted(bool pausable)
{
	setClasses(ui->recordButton, "state-active");
	ui->recordButton->setText(QTStr("Basic.Main.StopRecording"));

	if (pausable) {
		ui->pauseRecordButton->setVisible(pausable);
		RecordingUnpaused();
	}
}

void OBSBasicControls::RecordingPaused()
{
	QString text = QTStr("Basic.Main.UnpauseRecording");

	setClasses(ui->pauseRecordButton, "icon-media-pause state-active");
	ui->pauseRecordButton->setAccessibleName(text);
	ui->pauseRecordButton->setToolTip(text);

	ui->saveReplayButton->setEnabled(false);
}

void OBSBasicControls::RecordingUnpaused()
{
	QString text = QTStr("Basic.Main.PauseRecording");

	setClasses(ui->pauseRecordButton, "icon-media-pause");
	ui->pauseRecordButton->setAccessibleName(text);
	ui->pauseRecordButton->setToolTip(text);

	ui->saveReplayButton->setEnabled(true);
}

void OBSBasicControls::RecordingStopping()
{
	ui->recordButton->setText(QTStr("Basic.Main.StoppingRecording"));
}

void OBSBasicControls::RecordingStopped()
{
	setClasses(ui->recordButton, "");
	ui->recordButton->setText(QTStr("Basic.Main.StartRecording"));

	ui->pauseRecordButton->setVisible(false);
}

void OBSBasicControls::ReplayBufferStarted()
{
	setClasses(ui->replayBufferButton, "state-active");
	ui->replayBufferButton->setText(QTStr("Basic.Main.StopReplayBuffer"));

	ui->saveReplayButton->setVisible(true);
}

void OBSBasicControls::ReplayBufferStopping()
{
	ui->replayBufferButton->setText(QTStr("Basic.Main.StoppingReplayBuffer"));
}

void OBSBasicControls::ReplayBufferStopped()
{
	setClasses(ui->replayBufferButton, "");
	ui->replayBufferButton->setText(QTStr("Basic.Main.StartReplayBuffer"));

	ui->saveReplayButton->setVisible(false);
}

void OBSBasicControls::VirtualCamStarted()
{
	setClasses(ui->virtualCamButton, "state-active");
	ui->virtualCamButton->setText(QTStr("Basic.Main.StopVirtualCam"));
}

void OBSBasicControls::VirtualCamStopped()
{
	setClasses(ui->virtualCamButton, "");
	ui->virtualCamButton->setText(QTStr("Basic.Main.StartVirtualCam"));
}

void OBSBasicControls::UpdateStudioModeState(bool enabled)
{
	setClasses(ui->modeSwitch, enabled ? "state-active" : "");
}

void OBSBasicControls::EnableBroadcastFlow(bool enabled)
{
	ui->broadcastButton->setVisible(enabled);
	ui->broadcastButton->setEnabled(enabled);

	ui->broadcastButton->setText(QTStr("Basic.Main.SetupBroadcast"));

	ui->broadcastButton->setProperty("broadcastState", "idle");
	ui->broadcastButton->style()->unpolish(ui->broadcastButton);
	ui->broadcastButton->style()->polish(ui->broadcastButton);
}

void OBSBasicControls::EnableReplayBufferButtons(bool enabled)
{
	ui->replayBufferButton->setVisible(enabled);
}

void OBSBasicControls::EnableVirtualCamButtons()
{
	ui->virtualCamButton->setVisible(true);
	ui->virtualCamConfigButton->setVisible(true);
}
