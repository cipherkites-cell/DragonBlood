// DragonBlood.dll — 龙血棋的 SKSE 插件（音频 + 关牌桌）
//
// 为什么需要它：
//   · Magelight 的页面（Ultralight）没有媒体栈，放不了任何声音；
//     Papyrus 也没有任何按文件播放、改「Music」类别音量的接口。
//   · 页面按钮要关闭牌桌，而 Magelight 宿主没有 close 通道，
//     脚本那侧的 RegisterListener → ModEvent 桥在实际游戏里不可靠。
//
// 做法完全照 Skygwent.dll（同一套 Magelight 宿主，1.5.97 实测可用）：
//   · 插件自己 RegisterMod("DragonBlood")，把脚本创建的那个 Mod 接管过来
//     （API 规定同 modId = ADOPT），于是插件就是这个 Mod 的所有者，
//     可以对它的 view 直接 ShowView / ReleaseUIMode / RegisterJSListenerEx。
//   · 页面事件（duelLeave / sfx / bgm）由插件直连 C++ 回调，不经过 Papyrus。
//   · 音频：BGSDefaultObjectManager 拿 Music 类别静音；BSAudioManager
//     GetSoundHandleByFile(0x1A, 128) 取句柄，必须 handle.IsValid() 才播。
//
// 不依赖任何 Papyrus 原生函数，也不依赖脚本增加调用：
// 脚本仍旧是 v6 那份（对话选项正常），插件在 DomReady / UIModeEntered 时自己接管。

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include "MagelightUI_API.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

// <Windows.h> 把 GetObject 重定义成 GetObjectW，会盖掉 BGSDefaultObjectManager::GetObject
#ifdef GetObject
#	undef GetObject
#endif

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace logger = SKSE::log;
namespace ML = MAGELIGHT_API;

namespace
{
	constexpr std::uint32_t kSoundFlags = 0x1A;
	constexpr std::uint32_t kSoundPriority = 128;
	constexpr std::uint32_t kLoopLeadMs = 460;

	constexpr const char* kModId = "DragonBlood";
	constexpr const char* kViewName = "board";

	// ---------------- Magelight ----------------

	const ML::MagelightApi4* g_ui{ nullptr };
	ML::ModId                g_mod{ 0 };
	ML::ViewId               g_view{ 0 };
	bool                     g_viewReady{ false };
	bool                     g_boardOpen{ false };
	int                      g_bgmId{ -1 };

	// ---------------- 音频 ----------------

	struct Entry
	{
		RE::BSSoundHandle                     handle;
		std::string                           file;
		float                                 volume{ 1.0f };
		bool                                  loop{ false };
		std::uint64_t                         durationMs{ 0 };
		std::chrono::steady_clock::time_point started;
	};

	std::mutex           g_lock;
	std::map<int, Entry> g_playing;
	int                  g_nextId{ 1 };

	RE::BGSSoundCategory* g_musicCategory{ nullptr };
	float                 g_savedMusicVolume{ 1.0f };
	bool                  g_musicMuted{ false };
	std::string           g_soundRoot;
	std::atomic<bool>     g_ticking{ false };

	std::string LowerCopy(std::string a_text)
	{
		std::transform(a_text.begin(), a_text.end(), a_text.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return a_text;
	}

	bool Contains(const std::string& a_hay, const std::string& a_needle)
	{
		return a_hay.find(a_needle) != std::string::npos;
	}

	bool TryPlay(const std::string& a_path, float a_volume, RE::BSSoundHandle& a_out)
	{
		auto* mgr = RE::BSAudioManager::GetSingleton();
		if (!mgr) {
			logger::error("BSAudioManager 拿不到");
			return false;
		}

		RE::BSResource::ID id;
		id.GenerateFromPath(a_path.c_str());

		RE::BSSoundHandle handle;
		mgr->GetSoundHandleByFile(handle, id, kSoundFlags, kSoundPriority);
		if (!handle.IsValid()) {
			return false;
		}

		handle.SetVolume(std::clamp(a_volume, 0.0f, 1.0f));
		if (!handle.Play()) {
			return false;
		}

		a_out = handle;
		return true;
	}

	bool PlayFile(const std::string& a_file, float a_volume, RE::BSSoundHandle& a_out)
	{
		if (!g_soundRoot.empty()) {
			if (TryPlay(g_soundRoot + a_file, a_volume, a_out)) {
				return true;
			}
			g_soundRoot.clear();
		}
		for (const char* root : { "Sound\\", "Data\\Sound\\", "sound\\" }) {
			if (TryPlay(std::string(root) + a_file, a_volume, a_out)) {
				g_soundRoot = root;
				logger::info("音频根目录确定为 '{}'", root);
				return true;
			}
		}
		logger::warn("播放失败（文件不存在或引擎读不到）: Sound\\{}", a_file);
		return false;
	}

	bool StartSound(Entry& a_entry)
	{
		if (!PlayFile(a_entry.file, a_entry.volume, a_entry.handle)) {
			return false;
		}
		a_entry.started = std::chrono::steady_clock::now();
		if (a_entry.durationMs == 0) {
			a_entry.durationMs = a_entry.handle.GetDuration();
		}
		logger::info("开始播放 {} (vol={:.2f} loop={})", a_entry.file, a_entry.volume, a_entry.loop ? 1 : 0);
		return true;
	}

	RE::BGSSoundCategory* FindMusicCategory()
	{
		if (auto* defaults = RE::BGSDefaultObjectManager::GetSingleton()) {
			RE::BGSSoundCategory* cat = defaults->GetObject<RE::BGSSoundCategory>(RE::DEFAULT_OBJECT::kMusicSoundCategory);
			if (cat) {
				logger::info("Music 音频类别 = {:08X} (default object)", static_cast<unsigned>(cat->GetFormID()));
				return cat;
			}
			logger::warn("默认对象里没有 kMusicSoundCategory");
		}

		auto* handler = RE::TESDataHandler::GetSingleton();
		if (!handler) {
			logger::warn("TESDataHandler 拿不到");
			return nullptr;
		}

		RE::BGSSoundCategory* best = nullptr;
		for (auto* form : handler->GetFormArray<RE::BGSSoundCategory>()) {
			if (!form) {
				continue;
			}
			const std::string editor = form->GetFormEditorID() ? form->GetFormEditorID() : "";
			const std::string name = form->GetFullName() ? form->GetFullName() : "";
			const std::string editorLow = LowerCopy(editor);
			const std::string nameLow = LowerCopy(name);
			if (Contains(editorLow, "music") || Contains(nameLow, "music")) {
				best = form;
				if (Contains(editorLow, "music") && Contains(nameLow, "music")) {
					break;
				}
			}
		}
		if (!best) {
			logger::warn("没找到 Music 音频类别，本体音乐不会被静音");
		}
		return best;
	}

	void MusicMute(bool a_mute)
	{
		if (!g_musicCategory) {
			g_musicCategory = FindMusicCategory();
		}
		if (!g_musicCategory) {
			return;
		}
		if (a_mute) {
			if (!g_musicMuted) {
				g_savedMusicVolume = g_musicCategory->GetCategoryVolume();
				static_cast<void>(g_musicCategory->SetCategoryVolume(0.0f));
				g_musicMuted = true;
				logger::info("本体音乐已静音（原音量 {:.2f}）", g_savedMusicVolume);
			}
		} else if (g_musicMuted) {
			static_cast<void>(g_musicCategory->SetCategoryVolume(g_savedMusicVolume));
			g_musicMuted = false;
			logger::info("本体音乐音量还原为 {:.2f}", g_savedMusicVolume);
		}
	}

	int Play(const std::string& a_name, float a_volume, bool a_loop)
	{
		if (a_name.empty()) {
			return -1;
		}
		std::string name = a_name;
		if (name.find('.') == std::string::npos) {
			name += ".wav";
		}

		std::lock_guard lock(g_lock);
		int id = g_nextId++;

		Entry entry;
		entry.file = "DragonBlood\\" + name;
		entry.volume = std::clamp(a_volume, 0.0f, 1.0f);
		entry.loop = a_loop;
		if (!StartSound(entry)) {
			return -1;
		}
		g_playing.emplace(id, std::move(entry));
		return id;
	}

	void Stop(int a_id)
	{
		std::lock_guard lock(g_lock);
		auto it = g_playing.find(a_id);
		if (it == g_playing.end()) {
			return;
		}
		if (it->second.handle.IsValid()) {
			it->second.handle.FadeOutAndRelease(150);
		}
		g_playing.erase(it);
	}

	void StopAll()
	{
		std::lock_guard lock(g_lock);
		for (auto& [id, entry] : g_playing) {
			(void)id;
			if (entry.handle.IsValid()) {
				entry.handle.FadeOutAndRelease(150);
			}
		}
		g_playing.clear();
	}

	void TickOnce()
	{
		std::lock_guard lock(g_lock);
		const auto now = std::chrono::steady_clock::now();
		for (auto it = g_playing.begin(); it != g_playing.end();) {
			Entry& entry = it->second;
			if (!entry.loop) {
				if (entry.handle.IsValid() && !entry.handle.IsPlaying()) {
					it = g_playing.erase(it);
					continue;
				}
				++it;
				continue;
			}
			if (entry.durationMs == 0) {
				entry.durationMs = entry.handle.GetDuration();
			}
			if (entry.durationMs == 0) {
				++it;
				continue;
			}
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.started).count();
			if (elapsed + static_cast<long long>(kLoopLeadMs) >= static_cast<long long>(entry.durationMs)) {
				Entry fresh = entry;
				fresh.handle = {};
				if (StartSound(fresh)) {
					entry.handle = fresh.handle;
					entry.started = fresh.started;
				} else {
					it = g_playing.erase(it);
					continue;
				}
			}
			++it;
		}
	}

	// 循环续播用游戏线程计时器，不依赖 Papyrus 的 OnUpdate
	void StartTicking()
	{
		bool expected = false;
		if (!g_ticking.compare_exchange_strong(expected, true)) {
			return;
		}
		std::thread([]() {
			while (g_ticking.load()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				auto* tasks = SKSE::GetTaskInterface();
				if (!tasks) {
					break;
				}
				tasks->AddTask([]() {
					if (g_ticking.load()) {
						TickOnce();
					}
				});
			}
		}).detach();
	}

	// ---------------- 游戏线程 ----------------

	void RunOnGameThread(std::function<void()> a_task)
	{
		if (g_ui) {
			auto* box = new std::function<void()>(std::move(a_task));
			const auto run = [](void* a_user) {
				std::unique_ptr<std::function<void()>> task(static_cast<std::function<void()>*>(a_user));
				(*task)();
			};
			if (g_ui->PostGameTask(run, box) == ML::Result::Ok) {
				return;
			}
			a_task = std::move(*box);
			delete box;
		}
		if (auto* tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask(std::move(a_task));
		}
	}

	void StartTableAudio()
	{
		if (g_boardOpen) {
			return;
		}
		g_boardOpen = true;
		MusicMute(true);
		g_bgmId = Play("duel_bgm", 0.85f, true);
		Play("board_open", 1.0f, false);
		StartTicking();
		logger::info("牌桌音频启动 (bgmId={})", g_bgmId);
	}

	void StopTableAudio()
	{
		if (!g_boardOpen) {
			return;
		}
		g_boardOpen = false;
		Play("board_close", 1.0f, false);
		StopAll();
		g_bgmId = -1;
		MusicMute(false);
		logger::info("牌桌音频停止");
	}

	// 关牌桌：完全照 Skygwent 的 Close()，只是视图不销毁（脚本 v6 会复用 FindView）
	void CloseBoard()
	{
		StopTableAudio();
		if (!g_ui) {
			return;
		}
		if (g_mod) {
			g_ui->CancelUIModeRequest(g_mod);
			const auto r = g_ui->ReleaseUIMode(g_mod);
			logger::info("ReleaseUIMode -> {}", static_cast<int>(r));
		}
		if (g_view) {
			g_ui->ShowView(g_view, false);
		}
		if (!g_mod) {
			logger::warn("关牌桌时 mod 还没有注册（脚本可能还没跑过）");
		}
	}

	// ---------------- 页面调用 ----------------

	void Listen(const char* a_name, void (*a_call)(const char*))
	{
		if (!g_ui || !g_view) {
			return;
		}
		const auto result = g_ui->RegisterJSListenerEx(g_view, a_name,
			[](ML::ViewId, const char* a_argument, void* a_user) {
				reinterpret_cast<void (*)(const char*)>(a_user)(a_argument);
			},
			reinterpret_cast<void*>(a_call));
		if (result != ML::Result::Ok) {
			logger::error("监听页面事件 '{}' 失败 (Magelight result {})", a_name, static_cast<int>(result));
		} else {
			logger::info("已监听页面事件 '{}'", a_name);
		}
	}

	void OnPageSfx(const char* a_name)
	{
		const std::string name = a_name ? a_name : "";
		if (name.empty() || name.size() > 64) {
			return;
		}
		RunOnGameThread([name]() { Play(name, 1.0f, false); });
	}

	void OnPageBgm(const char*)
	{
		RunOnGameThread([]() {
			if (g_bgmId == -1) {
				g_bgmId = Play("duel_bgm", 0.85f, true);
			}
		});
	}

	void OnPageBgmStop(const char*)
	{
		RunOnGameThread([]() {
			StopTableAudio();
		});
	}

	void OnPageLeave(const char*)
	{
		logger::info("页面请求离开牌桌");
		RunOnGameThread([]() { CloseBoard(); });
	}

	void HookPage()
	{
		if (!g_ui || !g_view) {
			return;
		}
		Listen("duelLeave", OnPageLeave);
		Listen("sfx", OnPageSfx);
		Listen("bgm", OnPageBgm);
		Listen("bgmStop", OnPageBgmStop);
		// 页面据此把自定义音频当可用，sfx/bgm 才会发给插件而不是退回宿主内置音
		g_ui->InteropCall(g_view, "__audioReady", "1");
	}

	void OnUIEvent(const ML::EventData* a_event, void*)
	{
		if (!a_event) {
			return;
		}
		const std::string_view detail = a_event->detail ? a_event->detail : "";
		switch (a_event->type) {
		case ML::Event::ViewDomReady:
			if (!g_view) {
				g_view = a_event->view;
				logger::info("牌桌页面加载完成 (view {})", g_view);
			}
			g_viewReady = true;
			HookPage();
			break;
		case ML::Event::ViewReloaded:
			if (a_event->view == g_view) {
				g_viewReady = true;
				HookPage();
			}
			break;
		case ML::Event::ViewLoadFailed:
			logger::error("牌桌页面没加载起来: {}", detail);
			break;
		case ML::Event::UIModeEntered:
			g_view = a_event->view;
			g_ui->ShowView(g_view, true);
			RunOnGameThread([]() { StartTableAudio(); });
			if (g_viewReady) {
				HookPage();
			}
			logger::info("UI 模式已进入 (view {})", g_view);
			break;
		case ML::Event::UIModeExited:
			logger::info("UI 模式已退出");
			RunOnGameThread([]() { StopTableAudio(); });
			break;
		case ML::Event::UIModeRefused:
			logger::warn("UI 模式被拒绝: {}", detail);
			break;
		case ML::Event::ConsoleMessage:
			if (a_event->x >= 2) {
				logger::warn("页面脚本错误: {}", std::string(detail).substr(0, 400));
			}
			break;
		case ML::Event::RenderDead:
			logger::error("Magelight 渲染已停用: {}", detail);
			RunOnGameThread([]() { StopTableAudio(); });
			break;
		default:
			break;
		}
	}

	void SetupMagelight()
	{
		g_ui = ML::RequestApi4();
		if (!g_ui) {
			logger::critical("找不到 Magelight UI，音频与关牌桌都无法工作");
			return;
		}

		ML::ModDesc desc{};
		desc.size = sizeof(desc);
		desc.modId = kModId;
		desc.displayName = "Dragon's Blood";
		desc.modVersion = ML::PackVersion(1, 0, 0);
		desc.minHostVersion = ML::PackVersion(1, 9, 0);
		desc.callbackThread = ML::CallbackThread::GameThread;
		desc.onEvent = OnUIEvent;

		const auto result = g_ui->RegisterMod(&desc, &g_mod);
		if (result != ML::Result::Ok) {
			logger::error("Magelight 拒绝 DragonBlood (result {})", static_cast<int>(result));
			g_ui = nullptr;
			g_mod = 0;
			return;
		}

		logger::info("Magelight {} 已接管 mod '{}' (modId {})", g_ui->hostVersion ? g_ui->hostVersion : "?", kModId, g_mod);

		// 脚本可能已经（或即将）用同名 mod 创建 view；先找一次，找不到就在 DomReady 事件里拿到
		if (g_ui->FindView(g_mod, kViewName, &g_view) == ML::Result::Ok && g_view) {
			logger::info("已找到脚本创建的牌桌视图 view {}", g_view);
			HookPage();
		}
	}
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	logger::info("DragonBlood 载入 (Skyrim {})", REL::Module::get().version().string());
	SetupMagelight();
	return true;
}
