#include "MainWindow.h"
#include "DiscordRichPresence.h"
#include "ConsoleLog/ConsoleLines.h"
#include "Networking/GithubAPI.h"
#include "ConsoleLog/NetworkStatus.h"
#include "Platform/Platform.h"
#include "ImGui_TF2BotDetector.h"
#include "Actions/ActionGenerators.h"
#include "BaseTextures.h"
#include "Log.h"
#include "TextureManager.h"
#include "Util/PathUtils.h"
#include "Version.h"

#include <imgui_desktop/ScopeGuards.h>
#include <imgui_desktop/ImGuiHelpers.h>
#include <imgui.h>
#include <libzippp/libzippp.h>
#include <misc/cpp/imgui_stdlib.h>
#include <mh/math/interpolation.hpp>
#include <mh/text/case_insensitive_string.hpp>
#include <mh/text/fmtstr.hpp>
#include <mh/text/string_insertion.hpp>
#include <mh/text/stringops.hpp>
#include <srcon/async_client.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>

using namespace tf2_bot_detector;
using namespace std::chrono_literals;
using namespace std::string_literals;
using namespace std::string_view_literals;

MainWindow::MainWindow() :
	ImGuiDesktop::Window(800, 600, mh::fmtstr<128>("TF2 Bot Detector v{}", VERSION).c_str()),
	m_WorldState(IWorldState::Create(m_Settings)),
	m_ActionManager(IRCONActionManager::Create(m_Settings, GetWorld())),
	m_TextureManager(CreateTextureManager()),
	m_BaseTextures(IBaseTextures::Create(*m_TextureManager))
{
	m_TextureManager = CreateTextureManager();

	ILogManager::GetInstance().CleanupLogFiles();

	GetWorld().AddConsoleLineListener(this);
	GetWorld().AddWorldEventListener(this);

	DebugLog("Debug Info:"s
		<< "\n\tSteam dir:         " << m_Settings.GetSteamDir()
		<< "\n\tTF dir:            " << m_Settings.GetTFDir()
		<< "\n\tSteamID:           " << m_Settings.GetLocalSteamID()
		<< "\n\tVersion:           " << VERSION
		<< "\n\tIs CI Build:       " << std::boolalpha << (TF2BD_IS_CI_COMPILE ? true : false)
		<< "\n\tCompile Timestamp: " << __TIMESTAMP__
		<< "\n\tOpenGL Version:    " << GetGLContextVersion()

		<< "\n\tIs Debug Build:    "
#ifdef _DEBUG
		<< true
#else
		<< false
#endif

#ifdef _MSC_FULL_VER
		<< "\n\t-D _MSC_FULL_VER:  " << _MSC_FULL_VER
#endif
#if _M_X64
		<< "\n\t-D _M_X64:         " << _M_X64
#endif
#if _MT
		<< "\n\t-D _MT:            " << _MT
#endif
	);

	m_OpenTime = clock_t::now();

	GetActionManager().AddPeriodicActionGenerator<StatusUpdateActionGenerator>();
	GetActionManager().AddPeriodicActionGenerator<ConfigActionGenerator>();
	GetActionManager().AddPeriodicActionGenerator<LobbyDebugActionGenerator>();
	//m_ActionManager.AddPiggybackAction<GenericCommandAction>("net_status");
}

MainWindow::~MainWindow()
{
}

void MainWindow::OnDrawColorPicker(const char* name, std::array<float, 4>& color)
{
	if (ImGui::ColorEdit4(name, color.data(), ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreview))
		m_Settings.SaveFile();
}

void MainWindow::OnDrawColorPickers(const char* id, const std::initializer_list<ColorPicker>& pickers)
{
	ImGui::HorizontalScrollBox(id, [&]
		{
			for (const auto& picker : pickers)
			{
				OnDrawColorPicker(picker.m_Name, picker.m_Color);
				ImGui::SameLine();
			}
		});
}

void MainWindow::OnDrawChat()
{
	OnDrawColorPickers("ChatColorPickers",
		{
			{ "You", m_Settings.m_Theme.m_Colors.m_ChatLogYouFG },
			{ "Enemies", m_Settings.m_Theme.m_Colors.m_ChatLogEnemyTeamFG },
			{ "Friendlies", m_Settings.m_Theme.m_Colors.m_ChatLogFriendlyTeamFG },
		});

	ImGui::AutoScrollBox("##fileContents", { 0, 0 }, [&]()
		{
			if (!m_MainState)
				return;

			ImGui::PushTextWrapPos();

			const IConsoleLine::PrintArgs args{ m_Settings };
			for (auto it = m_MainState->m_PrintingLines.rbegin(); it != m_MainState->m_PrintingLines.rend(); ++it)
			{
				assert(*it);
				(*it)->Print(args);
			}

			ImGui::PopTextWrapPos();
		});
}

void MainWindow::OnDrawAppLog()
{
	ImGui::AutoScrollBox("AppLog", { 0, 0 }, [&]()
		{
			ImGui::PushTextWrapPos();

			const void* lastLogMsg = nullptr;
			for (const LogMessage& msg : ILogManager::GetInstance().GetVisibleMsgs())
			{
				const std::tm timestamp = ToTM(msg.m_Timestamp);

				ImGuiDesktop::ScopeGuards::ID id(&msg);

				ImGui::BeginGroup();
				ImGui::TextColored({ 0.25f, 1.0f, 0.25f, 0.25f }, "[%02i:%02i:%02i]",
					timestamp.tm_hour, timestamp.tm_min, timestamp.tm_sec);

				ImGui::SameLine();
				ImGui::TextFmt({ msg.m_Color.r, msg.m_Color.g, msg.m_Color.b, msg.m_Color.a }, msg.m_Text);
				ImGui::EndGroup();

				if (auto scope = ImGui::BeginPopupContextItemScope("AppLogContextMenu"))
				{
					if (ImGui::MenuItem("Copy"))
						ImGui::SetClipboardText(msg.m_Text.c_str());
				}

				lastLogMsg = &msg;
			}

			if (m_LastLogMessage != lastLogMsg)
			{
				m_LastLogMessage = lastLogMsg;
				QueueUpdate();
			}

			ImGui::PopTextWrapPos();
		});
}

void MainWindow::OnDrawSettingsPopup()
{
	static constexpr char POPUP_NAME[] = "Settings##Popup";

	static bool s_Open = false;
	if (m_SettingsPopupOpen)
	{
		m_SettingsPopupOpen = false;
		ImGui::OpenPopup(POPUP_NAME);
		s_Open = true;
	}

	ImGui::SetNextWindowSize({ 400, 400 }, ImGuiCond_Once);
	if (ImGui::BeginPopupModal(POPUP_NAME, &s_Open, ImGuiWindowFlags_HorizontalScrollbar))
	{
		if (ImGui::TreeNode("Autodetected Settings Overrides"))
		{
			// Steam dir
			if (InputTextSteamDirOverride("Steam directory", m_Settings.m_SteamDirOverride, true))
				m_Settings.SaveFile();

			// TF game dir override
			if (InputTextTFDirOverride("tf directory", m_Settings.m_TFDirOverride, FindTFDir(m_Settings.GetSteamDir()), true))
				m_Settings.SaveFile();

			// Local steamid
			if (InputTextSteamIDOverride("My Steam ID", m_Settings.m_LocalSteamIDOverride, true))
				m_Settings.SaveFile();

			ImGui::TreePop();
		}

		if (ImGui::TreeNode("Logging"))
		{
#ifdef TF2BD_ENABLE_DISCORD_INTEGRATION
			if (ImGui::Checkbox("Discord Rich Presence", &m_Settings.m_Logging.m_DiscordRichPresence))
				m_Settings.SaveFile();
#endif
			if (ImGui::Checkbox("RCON Packets", &m_Settings.m_Logging.m_RCONPackets))
				m_Settings.SaveFile();

			ImGui::TreePop();
		}

		if (ImGui::TreeNode("Moderation"))
		{
			// Auto temp mute
			{
				if (ImGui::Checkbox("Auto temp mute", &m_Settings.m_AutoTempMute))
					m_Settings.SaveFile();
				ImGui::SetHoverTooltip("Automatically, temporarily mute ingame chat messages if we think someone else in the server is running the tool.");
			}

			// Auto votekick delay
			{
				if (ImGui::SliderFloat("Auto votekick delay", &m_Settings.m_AutoVotekickDelay, 0, 30, "%1.1f seconds"))
					m_Settings.SaveFile();
				ImGui::SetHoverTooltip("Delay between a player being registered as fully connected and us expecting them to be ready to vote on an issue.\n\n"
					"This is needed because players can't vote until they have joined a team and picked a class. If we call a vote before enough people are ready, it might fail.");
			}

			// Send warnings for connecting cheaters
			{
				if (ImGui::Checkbox("Chat message warnings for connecting cheaters", &m_Settings.m_AutoChatWarningsConnecting))
					m_Settings.SaveFile();

				ImGui::SetHoverTooltip("Automatically sends a chat message if a cheater has joined the lobby,"
					" but is not yet in the game. Only has an effect if \"Enable Chat Warnings\""
					" is enabled (upper left of main window).\n"
					"\n"
					"Looks like: \"Heads up! There are N known cheaters joining the other team! Names unknown until they fully join.\"");
			}

			ImGui::TreePop();
		}

		if (ImGui::TreeNode("Performance"))
		{
			// Sleep when unfocused
			{
				if (ImGui::Checkbox("Sleep when unfocused", &m_Settings.m_SleepWhenUnfocused))
					m_Settings.SaveFile();
				ImGui::SetHoverTooltip("Slows program refresh rate when not focused to reduce CPU/GPU usage.");
			}

			ImGui::TreePop();
		}

		if (ImGui::TreeNode("Service Integrations"))
		{
			if (ImGui::Checkbox("Discord integrations", &m_Settings.m_Discord.m_EnableRichPresence))
				m_Settings.SaveFile();

#ifdef _DEBUG
			if (ImGui::Checkbox("Lazy Load API Data", &m_Settings.m_LazyLoadAPIData))
				m_Settings.SaveFile();
			ImGui::SetHoverTooltip("If enabled, waits until data is actually needed by the UI before requesting it, saving system resources. Otherwise, instantly loads all data from integration APIs as soon as a player joins the server.");
#endif

			if (bool allowInternet = m_Settings.m_AllowInternetUsage.value_or(false);
				ImGui::Checkbox("Allow internet connectivity", &allowInternet))
			{
				m_Settings.m_AllowInternetUsage = allowInternet;
				m_Settings.SaveFile();
			}

			ImGui::EnabledSwitch(m_Settings.m_AllowInternetUsage.value_or(false), [&](bool enabled)
				{
					ImGui::NewLine();
					if (std::string key = m_Settings.GetSteamAPIKey();
						InputTextSteamAPIKey("Steam API Key", key, true))
					{
						m_Settings.SetSteamAPIKey(key);
						m_Settings.SaveFile();
					}
					ImGui::NewLine();

					if (auto mode = enabled ? m_Settings.m_ProgramUpdateCheckMode : ProgramUpdateCheckMode::Disabled;
						Combo("Automatic update checking", mode))
					{
						m_Settings.m_ProgramUpdateCheckMode = mode;
						m_Settings.SaveFile();
					}
				}, "Requires \"Allow internet connectivity\"");

			ImGui::TreePop();
		}

		ImGui::NewLine();

		if (AutoLaunchTF2Checkbox(m_Settings.m_AutoLaunchTF2))
			m_Settings.SaveFile();

		ImGui::EndPopup();
	}
}

void MainWindow::OnDrawUpdateCheckPopup()
{
	static constexpr char POPUP_NAME[] = "Check for Updates##Popup";

	static bool s_Open = false;
	if (m_UpdateCheckPopupOpen)
	{
		m_UpdateCheckPopupOpen = false;
		ImGui::OpenPopup(POPUP_NAME);
		s_Open = true;
	}

	ImGui::SetNextWindowSize({ 500, 300 }, ImGuiCond_Appearing);
	if (ImGui::BeginPopupModal(POPUP_NAME, &s_Open, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::PushTextWrapPos();
		ImGui::TextFmt("You have chosen to disable internet connectivity for TF2 Bot Detector. You can still manually check for updates below.");
		ImGui::TextFmt({ 1, 1, 0, 1 }, "Reminder: if you use antivirus software, connecting to the internet may trigger warnings.");

		ImGui::EnabledSwitch(!m_UpdateInfo.valid(), [&]
			{
				if (ImGui::Button("Check for updates"))
					GetUpdateInfo();
			});

		ImGui::NewLine();

		if (mh::is_future_ready(m_UpdateInfo))
		{
			auto& updateInfo = m_UpdateInfo.get();

			if (updateInfo.IsUpToDate())
			{
				ImGui::TextFmt({ 0.1f, 1, 0.1f, 1 }, "You are already running the latest version of TF2 Bot Detector.");
			}
			else if (updateInfo.IsPreviewAvailable())
			{
				ImGui::TextFmt("There is a new preview version available.");
				if (ImGui::Button("View on Github"))
					Shell::OpenURL(updateInfo.m_Preview->m_URL);
			}
			else if (updateInfo.IsReleaseAvailable())
			{
				ImGui::TextFmt("There is a new stable version available.");
				if (ImGui::Button("View on Github"))
					Shell::OpenURL(updateInfo.m_Stable->m_URL);
			}
			else if (updateInfo.IsError())
			{
				ImGui::TextFmt({ 1, 0, 0, 1 }, "There was an error checking for updates.");
			}
		}
		else if (m_UpdateInfo.valid())
		{
			ImGui::TextFmt("Checking for updates...");
		}
		else
		{
			ImGui::TextFmt("Press \"Check for updates\" to check Github for a newer version.");
		}

		ImGui::EndPopup();
	}
}

void MainWindow::OpenUpdateCheckPopup()
{
	m_NotifyOnUpdateAvailable = false;
	m_UpdateCheckPopupOpen = true;
}

void MainWindow::OnDrawUpdateAvailablePopup()
{
	static constexpr char POPUP_NAME[] = "Update Available##Popup";

	static bool s_Open = false;
	if (m_UpdateAvailablePopupOpen)
	{
		m_UpdateAvailablePopupOpen = false;
		ImGui::OpenPopup(POPUP_NAME);
		s_Open = true;
	}

	if (ImGui::BeginPopupModal(POPUP_NAME, &s_Open, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextFmt("There is a new{} version of TF2 Bot Detector available for download.",
			(m_UpdateInfo.get().IsPreviewAvailable() ? " preview" : ""));

		if (ImGui::Button("View on Github"))
			Shell::OpenURL(m_UpdateInfo.get().GetURL());

		ImGui::EndPopup();
	}
}

void MainWindow::OpenUpdateAvailablePopup()
{
	m_NotifyOnUpdateAvailable = false;
	m_UpdateAvailablePopupOpen = true;
}

void MainWindow::OnDrawAboutPopup()
{
	static constexpr char POPUP_NAME[] = "About##Popup";

	static bool s_Open = false;
	if (m_AboutPopupOpen)
	{
		m_AboutPopupOpen = false;
		ImGui::OpenPopup(POPUP_NAME);
		s_Open = true;
	}

	ImGui::SetNextWindowSize({ 600, 450 }, ImGuiCond_Appearing);
	if (ImGui::BeginPopupModal(POPUP_NAME, &s_Open))
	{
		ImGui::PushTextWrapPos();

		ImGui::TextFmt("TF2 Bot Detector v\"{}\n"
			"\n"
			"Automatically detects and votekicks cheaters in Team Fortress 2 Casual.\n"
			"\n"
			"This program is free, open source software licensed under the MIT license. Full license text"
			" for this program and its dependencies can be found in the licenses subfolder next to this"
			" executable.", VERSION);

		ImGui::NewLine();
		ImGui::Separator();
		ImGui::NewLine();

		ImGui::TextFmt("Credits");
		ImGui::Spacing();
		if (ImGui::TreeNode("Code/concept by Matt \"pazer\" Haynie"))
		{
			if (ImGui::Selectable("GitHub - PazerOP", false, ImGuiSelectableFlags_DontClosePopups))
				Shell::OpenURL("https://github.com/PazerOP");
			if (ImGui::Selectable("Twitter - @PazerFromSilver", false, ImGuiSelectableFlags_DontClosePopups))
				Shell::OpenURL("https://twitter.com/PazerFromSilver");

			ImGui::TreePop();
		}
		if (ImGui::TreeNode("Artwork/icon by S-Purple"))
		{
			if (ImGui::Selectable("Twitter (NSFW)", false, ImGuiSelectableFlags_DontClosePopups))
				Shell::OpenURL("https://twitter.com/spurpleheart");

			ImGui::TreePop();
		}
		if (ImGui::TreeNode("Documentation/moderation by Nicholas \"ClusterConsultant\" Flamel"))
		{
			if (ImGui::Selectable("GitHub - ClusterConsultant", false, ImGuiSelectableFlags_DontClosePopups))
				Shell::OpenURL("https://github.com/ClusterConsultant");

			ImGui::TreePop();
		}
		if (ImGui::TreeNode("Other Attributions"))
		{
			ImGui::TextFmt("\"Game Ban\" icon made by Freepik from www.flaticon.com");
			ImGui::TreePop();
		}

		ImGui::NewLine();
		ImGui::Separator();
		ImGui::NewLine();

		if (m_MainState)
		{
			if (const auto sponsors = GetSponsorsList().GetSponsors(); !sponsors.empty())
			{
				ImGui::TextFmt("Sponsors\n"
					"Huge thanks to the people sponsoring this project via GitHub Sponsors:");

				ImGui::NewLine();

				for (const auto& sponsor : sponsors)
				{
					ImGui::Bullet();
					ImGui::TextFmt(sponsor.m_Name);

					if (!sponsor.m_Message.empty())
					{
						ImGui::SameLineNoPad();
						ImGui::TextFmt(" - {}", sponsor.m_Message);
					}
				}

				ImGui::NewLine();
			}
		}

		ImGui::TextFmt("If you're feeling generous, you can make a small donation to help support my work.");
		if (ImGui::Button("GitHub Sponsors"))
			Shell::OpenURL("https://github.com/sponsors/PazerOP");

		ImGui::EndPopup();
	}
}

void MainWindow::GenerateDebugReport()
{
	Log("Generating debug_report.zip...");
	{
		using namespace libzippp;
		ZipArchive archive("debug_report.zip");
		archive.open(ZipArchive::NEW);

		if (!archive.addFile("console.log", (m_Settings.GetTFDir() / "console.log").string()))
		{
			LogError("Failed to add console.log to debug report");
		}

		for (const auto& entry : std::filesystem::recursive_directory_iterator("logs"))
		{
			if (!entry.is_regular_file())
				continue;

			const auto& path = entry.path();

			if (archive.addFile(path.string(), path.string()))
				Log("Added file to debug report: {}", path);
			else
				Log("Failed to add file to debug report: {}", path);
		}

		if (auto err = archive.close(); err != LIBZIPPP_OK)
		{
			LogError("Failed to close debug report zip archive: close() returned {}", err);
			return;
		}
	}
	Log("Finished generating debug_report.zip.");
	Shell::ExploreToAndSelect("debug_report.zip");
}

void MainWindow::OnDrawServerStats()
{
	ImGui::PlotLines("Edicts", [&](int idx)
		{
			return m_EdictUsageSamples[idx].m_UsedEdicts;
		}, (int)m_EdictUsageSamples.size(), 0, nullptr, 0, 2048);

	if (!m_EdictUsageSamples.empty())
	{
		ImGui::SameLine(0, 4);

		auto& lastSample = m_EdictUsageSamples.back();
		const float percent = float(lastSample.m_UsedEdicts) / lastSample.m_MaxEdicts;
		ImGui::ProgressBar(percent, { -1, 0 },
			mh::pfstr<64>("%i (%1.0f%%)", lastSample.m_UsedEdicts, percent * 100).c_str());

		ImGui::SetHoverTooltip("{} of {} ({:1.1f}%)", lastSample.m_UsedEdicts, lastSample.m_MaxEdicts, percent * 100);
	}

	if (!m_ServerPingSamples.empty())
	{
		ImGui::PlotLines(mh::fmtstr<64>("Average ping: {}", m_ServerPingSamples.back().m_Ping).c_str(),
			[&](int idx)
			{
				return m_ServerPingSamples[idx].m_Ping;
			}, (int)m_ServerPingSamples.size(), 0, nullptr, 0);
	}

	//OnDrawNetGraph();
}

void MainWindow::OnDraw()
{
	OnDrawSettingsPopup();
	OnDrawUpdateAvailablePopup();
	OnDrawUpdateCheckPopup();
	OnDrawAboutPopup();

	{
		ISetupFlowPage::DrawState ds;
		ds.m_ActionManager = &GetActionManager();
		ds.m_Settings = &m_Settings;

		if (m_SetupFlow.OnDraw(m_Settings, ds))
			return;
	}

	if (!m_MainState)
		return;

	ImGui::Columns(2, "MainWindowSplit");

	ImGui::HorizontalScrollBox("SettingsScroller", [&]
		{
			ImGui::Checkbox("Pause", &m_Paused); ImGui::SameLine();

			auto& settings = m_Settings;
			const auto ModerationCheckbox = [&settings](const char* name, bool& value, const char* tooltip)
			{
				{
					ImGuiDesktop::ScopeGuards::TextColor text({ 1, 0.5f, 0, 1 }, !value);
					if (ImGui::Checkbox(name, &value))
						settings.SaveFile();
				}

				const char* orangeReason = "";
				if (!value)
					orangeReason = "\n\nThis label is orange to highlight the fact that it is currently disabled.";

				ImGui::SameLine();
				ImGui::SetHoverTooltip("{}{}", tooltip, orangeReason);
			};

			ModerationCheckbox("Enable Chat Warnings", m_Settings.m_AutoChatWarnings, "Enables chat message warnings about cheaters.");
			ModerationCheckbox("Enable Auto Votekick", m_Settings.m_AutoVotekick, "Automatically votekicks cheaters on your team.");
			ModerationCheckbox("Enable Auto-mark", m_Settings.m_AutoMark, "Automatically marks players matching the detection rules.");

			ImGui::Checkbox("Show Commands", &m_Settings.m_Unsaved.m_DebugShowCommands); ImGui::SameLine();
			ImGui::SetHoverTooltip("Prints out all game commands to the log.");
		});

	ImGui::Value("Time (Compensated)", to_seconds<float>(GetCurrentTimestampCompensated() - m_OpenTime));

#ifdef _DEBUG
	{
		auto leader = GetModLogic().GetBotLeader();
		ImGui::Value("Bot Leader", leader ? mh::fmtstr<128>("{}", *leader).view() : ""sv);

		ImGui::TextFmt("Is vote in progress:");
		ImGui::SameLine();
		if (GetWorld().IsVoteInProgress())
			ImGui::TextFmt({ 1, 1, 0, 1 }, "YES");
		else
			ImGui::TextFmt({ 0, 1, 0, 1 }, "NO");

		ImGui::TextFmt("FPS: {:1.1f}", GetFPS());

		ImGui::Value("Texture Count", m_TextureManager->GetActiveTextureCount());
	}
#endif

	ImGui::Value("Blacklisted user count", GetModLogic().GetBlacklistedPlayerCount());
	ImGui::Value("Rule count", GetModLogic().GetRuleCount());

	if (m_MainState)
	{
		auto& world = GetWorld();
		const auto parsedLineCount = m_ParsedLineCount;
		const auto parseProgress = m_MainState->m_Parser.GetParseProgress();

		if (parseProgress < 0.95f)
		{
			ImGui::ProgressBar(parseProgress, { 0, 0 }, mh::pfstr<64>("%1.2f %%", parseProgress * 100).c_str());
			ImGui::SameLine(0, 4);
		}

		ImGui::Value("Parsed line count", parsedLineCount);
	}

	//OnDrawServerStats();
	OnDrawChat();

	ImGui::NextColumn();

	OnDrawScoreboard();
	OnDrawAppLog();
	ImGui::NextColumn();
}

void MainWindow::OnEndFrame()
{
	m_TextureManager->EndFrame();
}

void MainWindow::OnDrawMenuBar()
{
	const bool isInSetupFlow = m_SetupFlow.ShouldDraw();

	if (ImGui::BeginMenu("File"))
	{
		if (!isInSetupFlow)
		{
			if (ImGui::MenuItem("Reload Playerlists/Rules"))
				GetModLogic().ReloadConfigFiles();
			if (ImGui::MenuItem("Reload Settings"))
				m_Settings.LoadFile();
		}

		if (ImGui::MenuItem("Generate Debug Report"))
			GenerateDebugReport();

		ImGui::Separator();

		if (ImGui::MenuItem("Exit", "Alt+F4"))
			SetShouldClose(true);

		ImGui::EndMenu();
	}

#ifdef _DEBUG
	if (ImGui::BeginMenu("Debug"))
	{
		ImGui::Separator();

		if (ImGui::MenuItem("Crash"))
		{
			struct Test
			{
				int i;
			};

			Test* testPtr = nullptr;
			testPtr->i = 42;
		}
		ImGui::EndMenu();
	}

#ifdef _DEBUG
	static bool s_ImGuiDemoWindow = false;
#endif
	if (ImGui::BeginMenu("Window"))
	{
#ifdef _DEBUG
		ImGui::MenuItem("ImGui Demo Window", nullptr, &s_ImGuiDemoWindow);
#endif
		ImGui::EndMenu();
	}

#ifdef _DEBUG
	if (s_ImGuiDemoWindow)
		ImGui::ShowDemoWindow(&s_ImGuiDemoWindow);
#endif
#endif

	if (!isInSetupFlow)
	{
		if (ImGui::MenuItem("Settings"))
			OpenSettingsPopup();
	}

	if (ImGui::BeginMenu("Help"))
	{
		if (ImGui::MenuItem("Open GitHub"))
			Shell::OpenURL("https://github.com/PazerOP/tf2_bot_detector");
		if (ImGui::MenuItem("Open Discord"))
			Shell::OpenURL("https://discord.gg/W8ZSh3Z");

		ImGui::Separator();

		static const mh::fmtstr<128> VERSION_STRING_LABEL("Version: {}", VERSION_STRING);
		ImGui::MenuItem(VERSION_STRING_LABEL.c_str(), nullptr, false, false);

		if (m_Settings.m_AllowInternetUsage.value_or(false))
		{
			auto newVersion = GetUpdateInfo();
			if (!newVersion)
			{
				ImGui::MenuItem("Checking for new version...", nullptr, nullptr, false);
			}
			else if (newVersion->IsUpToDate())
			{
				ImGui::MenuItem("Up to date!", nullptr, nullptr, false);
			}
			else if (newVersion->IsReleaseAvailable())
			{
				ImGuiDesktop::ScopeGuards::TextColor green({ 0, 1, 0, 1 });
				if (ImGui::MenuItem("A new version is available"))
					Shell::OpenURL(newVersion->m_Stable->m_URL);
			}
			else if (newVersion->IsPreviewAvailable())
			{
				if (ImGui::MenuItem("A new preview is available"))
					Shell::OpenURL(newVersion->m_Preview->m_URL);
			}
			else
			{
				assert(newVersion->IsError());
				ImGui::MenuItem("Error occurred checking for new version.", nullptr, nullptr, false);
			}
		}
		else
		{
			if (ImGui::MenuItem("Check for updates..."))
				OpenUpdateCheckPopup();
		}

		ImGui::Separator();

		if (ImGui::MenuItem("About TF2 Bot Detector"))
			OpenAboutPopup();

		ImGui::EndMenu();
	}
}

GithubAPI::NewVersionResult* MainWindow::GetUpdateInfo()
{
	if (!m_UpdateInfo.valid())
	{
		if (auto client = m_Settings.GetHTTPClient())
			m_UpdateInfo = GithubAPI::CheckForNewVersion(*client);
		else
			return nullptr;
	}

	if (mh::is_future_ready(m_UpdateInfo))
		return const_cast<GithubAPI::NewVersionResult*>(&m_UpdateInfo.get());

	return nullptr;
}

void MainWindow::HandleUpdateCheck()
{
	if (!m_NotifyOnUpdateAvailable)
		return;

	if (!m_Settings.m_AllowInternetUsage.value_or(false))
		return;

	const bool checkPreviews = m_Settings.m_ProgramUpdateCheckMode == ProgramUpdateCheckMode::Previews;
	const bool checkReleases = checkPreviews || m_Settings.m_ProgramUpdateCheckMode == ProgramUpdateCheckMode::Releases;
	if (!checkPreviews && !checkReleases)
		return;

	auto result = GetUpdateInfo();
	if (!result)
		return;

	if ((result->IsPreviewAvailable() && checkPreviews) ||
		(result->IsReleaseAvailable() && checkReleases))
	{
		OpenUpdateAvailablePopup();
	}
}

void MainWindow::PostSetupFlowState::OnUpdateDiscord()
{
#ifdef TF2BD_ENABLE_DISCORD_INTEGRATION
	const auto curTime = clock_t::now();
	if (!m_DRPManager && m_Parent->m_Settings.m_Discord.m_EnableRichPresence)
	{
		m_DRPManager = IDRPManager::Create(m_Parent->m_Settings, m_Parent->GetWorld());
	}
	else if (m_DRPManager && !m_Parent->m_Settings.m_Discord.m_EnableRichPresence)
	{
		m_DRPManager.reset();
	}

	if (m_DRPManager)
		m_DRPManager->Update();
#endif
}

void MainWindow::OnUpdate()
{
	if (m_Paused)
		return;

	GetWorld().Update();

	HandleUpdateCheck();

	if (m_Settings.m_Unsaved.m_RCONClient)
		m_Settings.m_Unsaved.m_RCONClient->set_logging(m_Settings.m_Logging.m_RCONPackets);

	if (m_SetupFlow.OnUpdate(m_Settings))
	{
		m_MainState.reset();
	}
	else
	{
		if (!m_MainState)
			m_MainState.emplace(*this);

		m_MainState->m_Parser.Update();
		GetModLogic().Update();

		m_MainState->OnUpdateDiscord();
	}

	GetActionManager().Update();
}

void MainWindow::OnConsoleLogChunkParsed(IWorldState& world, bool consoleLinesUpdated)
{
	assert(&world == &GetWorld());

	if (consoleLinesUpdated)
		UpdateServerPing(GetCurrentTimestampCompensated());
}

bool MainWindow::IsSleepingEnabled() const
{
	return m_Settings.m_SleepWhenUnfocused && !HasFocus();
}

bool MainWindow::IsTimeEven() const
{
	const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(clock_t::now() - m_OpenTime);
	return !(seconds.count() % 2);
}

float MainWindow::TimeSine(float interval, float min, float max) const
{
	const auto elapsed = (clock_t::now() - m_OpenTime) % std::chrono::duration_cast<clock_t::duration>(std::chrono::duration<float>(interval));
	const auto progress = std::chrono::duration<float>(elapsed).count() / interval;
	return mh::remap(std::sin(progress * 6.28318530717958647693f), -1.0f, 1.0f, min, max);
}

void MainWindow::OnConsoleLineParsed(IWorldState& world, IConsoleLine& parsed)
{
	m_ParsedLineCount++;

	if (parsed.ShouldPrint() && m_MainState)
	{
		while (m_MainState->m_PrintingLines.size() > m_MainState->MAX_PRINTING_LINES)
			m_MainState->m_PrintingLines.pop_back();

		m_MainState->m_PrintingLines.push_front(parsed.shared_from_this());
	}

	switch (parsed.GetType())
	{
	case ConsoleLineType::LobbyChanged:
	{
		auto& lobbyChangedLine = static_cast<const LobbyChangedLine&>(parsed);
		const LobbyChangeType changeType = lobbyChangedLine.GetChangeType();

		if (changeType == LobbyChangeType::Created || changeType == LobbyChangeType::Updated)
			GetActionManager().QueueAction<LobbyUpdateAction>();

		break;
	}
	case ConsoleLineType::EdictUsage:
	{
		auto& usageLine = static_cast<const EdictUsageLine&>(parsed);
		m_EdictUsageSamples.push_back({ usageLine.GetTimestamp(), usageLine.GetUsedEdicts(), usageLine.GetTotalEdicts() });

		while (m_EdictUsageSamples.front().m_Timestamp < (usageLine.GetTimestamp() - 5min))
			m_EdictUsageSamples.erase(m_EdictUsageSamples.begin());

		break;
	}
	}
}

void MainWindow::OnConsoleLineUnparsed(IWorldState& world, const std::string_view& text)
{
	m_ParsedLineCount++;
}

cppcoro::generator<IPlayer&> MainWindow::PostSetupFlowState::GeneratePlayerPrintData()
{
	IPlayer* printData[33]{};
	auto begin = std::begin(printData);
	auto end = std::end(printData);
	assert(begin <= end);
	auto& world = m_Parent->m_WorldState;
	assert(static_cast<size_t>(end - begin) >= world->GetApproxLobbyMemberCount());

	std::fill(begin, end, nullptr);

	{
		auto* current = begin;
		for (IPlayer& member : world->GetLobbyMembers())
		{
			*current = &member;
			current++;
		}

		if (current == begin)
		{
			// We seem to have either an empty lobby or we're playing on a community server.
			// Just find the most recent status updates.
			for (IPlayer& playerData : world->GetPlayers())
			{
				if (playerData.GetLastStatusUpdateTime() >= (world->GetLastStatusUpdateTime() - 15s))
				{
					*current = &playerData;
					current++;

					if (current >= end)
						break; // This might happen, but we're not in a lobby so everything has to be approximate
				}
			}
		}

		end = current;
	}

	std::sort(begin, end, [](const IPlayer* lhs, const IPlayer* rhs) -> bool
		{
			assert(lhs);
			assert(rhs);
			//if (!lhs && !rhs)
			//	return false;
			//if (auto result = !!rhs <=> !!lhs; !std::is_eq(result))
			//	return result < 0;

			// Intentionally reversed, we want descending kill order
			if (auto killsResult = rhs->GetScores().m_Kills <=> lhs->GetScores().m_Kills; !std::is_eq(killsResult))
				return std::is_lt(killsResult);

			if (auto deathsResult = lhs->GetScores().m_Deaths <=> rhs->GetScores().m_Deaths; !std::is_eq(deathsResult))
				return std::is_lt(deathsResult);

			// Sort by ascending userid
			{
				auto luid = lhs->GetUserID();
				auto ruid = rhs->GetUserID();
				if (luid && ruid)
				{
					if (auto result = *luid <=> *ruid; !std::is_eq(result))
						return std::is_lt(result);
				}
			}

			return false;
		});

	for (auto it = begin; it != end; ++it)
		co_yield **it;
}

void MainWindow::UpdateServerPing(time_point_t timestamp)
{
	if ((timestamp - m_LastServerPingSample) <= 7s)
		return;

	float totalPing = 0;
	uint16_t samples = 0;

	for (IPlayer& player : GetWorld().GetPlayers())
	{
		if (player.GetLastStatusUpdateTime() < (timestamp - 20s))
			continue;

		auto& data = player.GetOrCreateData<PlayerExtraData>(player);
		totalPing += data.GetAveragePing();
		samples++;
	}

	m_ServerPingSamples.push_back({ timestamp, uint16_t(totalPing / samples) });
	m_LastServerPingSample = timestamp;

	while ((timestamp - m_ServerPingSamples.front().m_Timestamp) > 5min)
		m_ServerPingSamples.erase(m_ServerPingSamples.begin());
}

time_point_t MainWindow::GetLastStatusUpdateTime() const
{
	return GetWorld().GetLastStatusUpdateTime();
}

float MainWindow::PlayerExtraData::GetAveragePing() const
{
	//throw std::runtime_error("TODO");
#if 1
	unsigned totalPing = m_Parent->GetPing();
	unsigned samples = 1;

	for (const auto& entry : m_PingHistory)
	{
		totalPing += entry.m_Ping;
		samples++;
	}

	return totalPing / float(samples);
#endif
}

time_point_t MainWindow::GetCurrentTimestampCompensated() const
{
	return GetWorld().GetCurrentTime();
}

std::shared_ptr<ITexture> MainWindow::TryGetAvatarTexture(IPlayer& player)
{
	struct PlayerAvatarData
	{
		std::shared_future<Bitmap> m_Bitmap;
		std::shared_ptr<ITexture> m_Texture;
		bool m_PrintedErrorMessage = false;
	};

	auto& avatarData = player.GetOrCreateData<PlayerAvatarData>();

	if (mh::is_future_ready(avatarData.m_Bitmap))
	{
		try
		{
			avatarData.m_Texture = m_TextureManager->CreateTexture(avatarData.m_Bitmap.get());
			avatarData.m_Bitmap = {};
		}
		catch (const std::exception& e)
		{
			if (!avatarData.m_PrintedErrorMessage)
			{
				LogWarning("Failed to create avatar texture from bitmap: "s << typeid(e).name() << ": " << e.what());
				avatarData.m_PrintedErrorMessage = true;
			}
		}
	}
	else if (avatarData.m_Texture)
		return avatarData.m_Texture;
	else if (!avatarData.m_Bitmap.valid())
	{
		if (auto summary = player.GetPlayerSummary())
			avatarData.m_Bitmap = summary->GetAvatarBitmap(m_Settings.GetHTTPClient());
	}

	return nullptr;
}

MainWindow::PostSetupFlowState::PostSetupFlowState(MainWindow& window) :
	m_Parent(&window),
	m_ModeratorLogic(IModeratorLogic::Create(window.GetWorld(), window.m_Settings, window.GetActionManager())),
	m_SponsorsList(window.m_Settings),
	m_Parser(window.GetWorld(), window.m_Settings, window.m_Settings.GetTFDir() / "console.log")
{
#ifdef TF2BD_ENABLE_DISCORD_INTEGRATION
	m_DRPManager = IDRPManager::Create(window.m_Settings, window.GetWorld());
#endif
}