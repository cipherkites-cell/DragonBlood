// DragonBlood.dll — 只做音频（页面 Ultralight 没有媒体栈，Papyrus 也没有按文件播放、
// 改「Music」类别音量的接口）。
//
// 通道划分（与 Papyrus 持久脚本 DragonBlood_DuelQuest 错开，避免抢占同一个监听槽）：
//   · DLL 只注册音频通道：sfx / bgm / bgmStop，回调里直接在本插件内播放。
//   · ready / duelWin / duelLose / duelDraw / duelLeave 由持久任务脚本自己
//     Magelight.RegisterListener，ModEvent 直接进脚本，DLL 不再转发。
//
// 关键约束（读 Magelight 源码确认）：
//   · 绝不在本 DLL 里 RegisterMod 接管 "DragonBlood"：接管后该 mod 变成 plugin-owned，
//     Papyrus 对它的 RequestUIMode / ReleaseUIMode / ShowView 全部静默失效或被拒，
//     第二次进桌会报 "UI mode refused"，关桌也退不掉。
//   · UI 模式的进入 / 释放归属主脚本管；DLL 只做跨视图可寻址的监听注册与音频播放。
//   · Magelight.dll 比本插件后加载，API 不能在加载阶段取，必须运行中惰性查找。

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "MagelightUI_API.h"

// windows.h 由上面的头带进来，会把 GetObject 变成 GetObjectA 这类宏
#undef GetObject
#undef GetObjectA
#undef GetObjectW

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace logger = SKSE::log;
namespace ml = MAGELIGHT_API;

namespace
{
	constexpr std::uint32_t kSoundFlags = 0x1A;
	constexpr std::uint32_t kSoundPriority = 128;
	constexpr std::uint32_t kLoopLeadMs = 460;
	constexpr std::uint32_t kTickMs = 250;

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

	std::mutex           g_audioLock;
	std::map<int, Entry> g_playing;
	int                  g_nextId{ 1 };
	int                  g_bgm{ 0 };

	RE::BGSSoundCategory* g_musicCategory{ nullptr };
	float                 g_savedMusicVolume{ 1.0f };
	bool                  g_musicMuted{ false };
	std::string           g_soundRoot;

	// ---------------- 页面通道 ----------------

	const ml::MagelightApi4* g_api{ nullptr };
	std::atomic<ml::ViewId>  g_view{ 0 };
	std::atomic<bool>        g_wasActive{ false };

	struct JsCall
	{
		std::string name;
		std::string arg;
	};

	std::mutex           g_callLock;
	std::vector<JsCall>  g_calls;

	std::atomic<bool> g_workerUp{ false };

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
		if (!g_soundRoot.empty() && TryPlay(g_soundRoot + a_file, a_volume, a_out)) {
			return true;
		}
		g_soundRoot.clear();
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
		logger::info("开始播放 {} (vol={:.2f}, loop={}, dur={}ms)",
			a_entry.file, a_entry.volume, a_entry.loop ? 1 : 0, a_entry.durationMs);
		return true;
	}

	RE::BGSSoundCategory* GetMusicCategory()
	{
		if (g_musicCategory) {
			return g_musicCategory;
		}
		auto* defaults = RE::BGSDefaultObjectManager::GetSingleton();
		if (!defaults) {
			logger::warn("BGSDefaultObjectManager 拿不到，本体音乐无法静音");
			return nullptr;
		}
		auto* cat = defaults->GetObject<RE::BGSSoundCategory>(RE::DEFAULT_OBJECT::kMusicSoundCategory);
		if (!cat) {
			logger::warn("默认对象里没有 kMusicSoundCategory，本体音乐无法静音");
			return nullptr;
		}
		g_musicCategory = cat;
		logger::info("Music 音频类别 = {:08X}，当前音量 {:.2f}",
			static_cast<unsigned>(cat->GetFormID()), cat->GetCategoryVolume());
		return cat;
	}

	void MusicMuteImpl(bool a_mute)
	{
		auto* cat = GetMusicCategory();
		if (!cat) {
			return;
		}
		if (a_mute) {
			if (!g_musicMuted) {
				g_savedMusicVolume = cat->GetCategoryVolume();
				cat->SetCategoryVolume(0.0f);
				g_musicMuted = true;
				logger::info("本体音乐已静音（原音量 {:.2f}）", g_savedMusicVolume);
			}
		} else if (g_musicMuted) {
			cat->SetCategoryVolume(g_savedMusicVolume);
			g_musicMuted = false;
			logger::info("本体音乐音量还原为 {:.2f}", g_savedMusicVolume);
		}
	}

	int PlayClip(const std::string& a_name, float a_volume, bool a_loop)
	{
		if (a_name.empty()) {
			return -1;
		}
		std::string name = a_name;
		if (name.find('.') == std::string::npos) {
			name += ".wav";
		}

		std::lock_guard lock(g_audioLock);
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

	void StopClip(int a_id)
	{
		std::lock_guard lock(g_audioLock);
		auto it = g_playing.find(a_id);
		if (it == g_playing.end()) {
			return;
		}
		if (it->second.handle.IsValid()) {
			it->second.handle.FadeOutAndRelease(150);
		}
		g_playing.erase(it);
	}

	void StopAllClips()
	{
		std::lock_guard lock(g_audioLock);
		for (auto& pair : g_playing) {
			if (pair.second.handle.IsValid()) {
				pair.second.handle.FadeOutAndRelease(150);
			}
		}
		g_playing.clear();
	}

	void TickOnce()
	{
		std::lock_guard lock(g_audioLock);
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
				Entry fresh;
				fresh.file = entry.file;
				fresh.volume = entry.volume;
				fresh.loop = entry.loop;
				fresh.durationMs = entry.durationMs;
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

	// ---------------- 页面音频通道 ----------------

	// 渲染线程：只把名字和参数抄进队列，绝不碰 RE 状态、不调 SKSE AddTask
	void OnJsCall(ml::ViewId, const char* a_arg, void* a_user)
	{
		if (!a_user) {
			return;
		}
		const std::string* name = static_cast<const std::string*>(a_user);
		logger::info("收到页面调用 '{}'，参数 '{}'", *name, a_arg ? a_arg : "");
		std::lock_guard lock(g_callLock);
		JsCall call;
		call.name = *name;
		call.arg = a_arg ? a_arg : "";
		g_calls.push_back(std::move(call));
	}

	void AttachViewIfPossible()
	{
		if (!g_api) {
			g_api = ml::RequestApi4();
			if (!g_api) {
				return;
			}
			logger::info("Magelight UI API v4 已取到（host {}）", g_api->hostVersion ? g_api->hostVersion : "?");
		}

		const ml::ModId owner = g_api->GetUIModeOwner();
		if (!owner) {
			return;
		}

		ml::ViewId view = 0;
		if (g_api->FindView(owner, "board", &view) != ml::Result::Ok || !view) {
			return;
		}
		if (view == g_view.load()) {
			return;
		}

		// 只注册音频通道；ready / duelWin / duelLose / duelDraw / duelLeave 留给持久任务脚本。
		// 宿主的监听槽按名字存一份，后注册者覆盖先注册者，两档不能抢同一个名字。
		static std::string s_names[3];
		static const char* const kChannels[3] = { "sfx", "bgm", "bgmStop" };

		g_view.store(view);
		int ok = 0;
		for (int i = 0; i < 3; ++i) {
			s_names[i] = kChannels[i];
			if (g_api->RegisterJSListenerEx(view, kChannels[i], OnJsCall, &s_names[i]) == ml::Result::Ok) {
				++ok;
			}
		}
		logger::info("视图已接管 view={}，音频通道注册 {}/3", view, ok);

		// 直接告诉页面"音频就绪"：页面的 sfx() 只有 audioReady 为真才发。
		if (ok > 0) {
			g_api->InteropCall(view, "__audioReady", "1");
		}
	}

	void StopBgm()
	{
		if (g_bgm != 0) {
			StopClip(g_bgm);
			g_bgm = 0;
		}
	}

	void HandleJsCall(const JsCall& a_call)
	{
		const std::string& name = a_call.name;

		if (name == "sfx") {
			logger::info("页面请求音效 '{}'", a_call.arg);
			if (!a_call.arg.empty()) {
				PlayClip(a_call.arg, 1.0f, false);
			}
			return;
		}
		if (name == "bgm") {
			logger::info("页面请求 BGM");
			if (g_bgm == 0) {
				MusicMuteImpl(true);
				g_bgm = PlayClip("duel_bgm", 0.85f, true);
			}
			return;
		}
		if (name == "bgmStop") {
			logger::info("页面请求停止 BGM");
			StopBgm();
			MusicMuteImpl(false);
			return;
		}

		// 其余通道（ready / duelWin / duelLose / duelDraw / duelLeave）由持久任务脚本自己监听。
	}

	void DrainCalls()
	{
		std::vector<JsCall> local;
		{
			std::lock_guard lock(g_callLock);
			if (g_calls.empty()) {
				return;
			}
			local.swap(g_calls);
		}
		for (const JsCall& call : local) {
			HandleJsCall(call);
		}
	}

	// 监听 UI 模式的退出边沿：Esc / 宿主热键 / 脚本释放 / 读档收回模式时，
	// 页面未必来得及发 bgmStop，这里兜底把 BGM、音效清掉并还原本体音乐音量。
	void WatchUiMode()
	{
		if (!g_api) {
			return;
		}
		const bool active = g_api->IsUIModeActive() != 0;
		if (g_wasActive.load() && !active) {
			StopAllClips();
			g_bgm = 0;
			MusicMuteImpl(false);
			logger::info("UI 模式已退出：音频已复位");
		}
		g_wasActive.store(active);
	}

	// 游戏线程
	void Pump()
	{
		AttachViewIfPossible();
		DrainCalls();
		WatchUiMode();
		TickOnce();
	}

	void StartWorker()
	{
		bool expected = false;
		if (!g_workerUp.compare_exchange_strong(expected, true)) {
			return;
		}
		std::thread([]() {
			while (g_workerUp.load()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
				auto* tasks = SKSE::GetTaskInterface();
				if (!tasks) {
					break;
				}
				tasks->AddTask([]() {
					if (g_workerUp.load()) {
						Pump();
					}
				});
			}
		}).detach();
	}

	// ---------------- Papyrus 原生函数 ----------------

	int PapPlay(RE::StaticFunctionTag*, std::string a_name, float a_volume, bool a_loop)
	{
		return PlayClip(a_name, a_volume, a_loop);
	}

	void PapStop(RE::StaticFunctionTag*, int a_id)
	{
		StopClip(a_id);
	}

	void PapStopAll(RE::StaticFunctionTag*)
	{
		StopAllClips();
	}

	void PapTick(RE::StaticFunctionTag*)
	{
		TickOnce();
	}

	void PapMusicMute(RE::StaticFunctionTag*, bool a_mute)
	{
		MusicMuteImpl(a_mute);
	}

	bool PapIsReady(RE::StaticFunctionTag*)
	{
		return RE::BSAudioManager::GetSingleton() != nullptr;
	}

	std::string PapDiagCategories(RE::StaticFunctionTag*)
	{
		auto* cat = GetMusicCategory();
		if (!cat) {
			return "未找到 Music 类别";
		}
		char buf[160];
		std::snprintf(buf, sizeof(buf), "%08X 类别音量=%.2f 静音中=%d\n",
			static_cast<unsigned>(cat->GetFormID()),
			cat->GetCategoryVolume(),
			g_musicMuted ? 1 : 0);
		return std::string(buf);
	}

	bool RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm)
	{
		a_vm->RegisterFunction("Play", "DragonBloodAudio", PapPlay);
		a_vm->RegisterFunction("Stop", "DragonBloodAudio", PapStop);
		a_vm->RegisterFunction("StopAll", "DragonBloodAudio", PapStopAll);
		a_vm->RegisterFunction("Tick", "DragonBloodAudio", PapTick);
		a_vm->RegisterFunction("MusicMute", "DragonBloodAudio", PapMusicMute);
		a_vm->RegisterFunction("IsReady", "DragonBloodAudio", PapIsReady);
		a_vm->RegisterFunction("DiagCategories", "DragonBloodAudio", PapDiagCategories);
		logger::info("DragonBloodAudio: 7 个 Papyrus 函数已注册");
		return true;
	}
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	auto* papyrus = SKSE::GetPapyrusInterface();
	if (papyrus && papyrus->Register(RegisterFunctions)) {
		logger::info("DragonBlood 载入：Papyrus 函数已注册（音频可用）");
	} else {
		logger::error("DragonBlood 载入：Papyrus 注册失败，音频不可用");
	}

	StartWorker();
	logger::info("DragonBlood 载入：后台线程已启动（仅音频通道，UI 归属主脚本）  [build=closefix-r3]");
	return true;
}
