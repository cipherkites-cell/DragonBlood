// DragonBlood.dll — 音频 + 关桌兜底（页面 Ultralight 没有媒体栈，Papyrus 也没有按文件播放、
// 改「Music」类别音量的接口）。
//
// 通道划分（Magelight 的监听槽按名字只存一份，后注册者覆盖先注册者）：
//   · DLL 注册：sfx / bgm / bgmStop / duelLeave，回调直接在本插件内播放与关桌。
//   · ready / duelWin / duelLose / duelDraw 仍由主片段脚本自己 RegisterListener，
//     这三个结算通道 DLL 不碰，避免覆盖脚本的结算监听。
//
// 关键约束（读 Magelight 0cbe86 源码确认）：
//   · 绝不在本 DLL 里 RegisterMod 接管 "DragonBlood"：接管后该 mod 变成 plugin-owned，
//     Papyrus 对它的 RequestUIMode / ReleaseUIMode / ShowView 全部静默失效或被拒，
//     第二次进桌会报 "UI mode refused"，关桌也退不掉。
//   · 关桌释放不按硬编码 mod id，而是取运行时 GetUIModeOwner() 的真实属主再
//     ReleaseUIMode(owner)；宿主校验只比对 s_uiOwner == mod，ABI 不带调用者身份，
//     插件以他人属主身份释放不要求自己是属主。
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
	std::atomic<bool>        g_attachPending{ true };   // 每次进 UI 模式都要重注册通道（页面会重载）

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

	// 按文件名查是否已有同一条循环音在播：首进牌桌时 Papyrus StartAudio 已经起过
	// 一条 duel_bgm，页面再发 bgm 不能重复叠第二条。
	bool HasLoopingClip(const std::string& a_file)
	{
		std::lock_guard lock(g_audioLock);
		for (const auto& pair : g_playing) {
			if (pair.second.loop && pair.second.file.find(a_file) != std::string::npos) {
				return true;
			}
		}
		return false;
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

		// manifest 里 loadOnShow=true：每次显示视图都会重载页面，DOM 是新的，宿主要重新下发
		// 一次通道 shim 页面才连得上。所以不能在"视图 id 没变"时提前返回——否则第二次进桌
		// 一次都不重注册，页面的 sfx/bgm/duelLeave 全部落空（无音效、无 BGM、关不掉桌）。
		// 只在 mode 从关到开的边沿重注册一次，不是每帧都做。
		if (!g_attachPending.exchange(false)) {
			return;
		}

		// 音频通道 + duelLeave 关桌兜底；ready / duelWin / duelLose / duelDraw 留给主片段脚本。
		// 宿主的监听槽按名字存一份，后注册者覆盖先注册者，两档不能抢同一个名字。
		static std::string s_names[4];
		static const char* const kChannels[4] = { "sfx", "bgm", "bgmStop", "duelLeave" };

		g_view.store(view);
		int ok = 0;
		for (int i = 0; i < 4; ++i) {
			s_names[i] = kChannels[i];
			if (g_api->RegisterJSListenerEx(view, kChannels[i], OnJsCall, &s_names[i]) == ml::Result::Ok) {
				++ok;
			}
		}
		logger::info("视图已接管 view={}，通道注册 {}/4", view, ok);
		if (ok == 0) {
			// 视图还没就绪（重建中）：把标记放回去，下一跳再试，不能就这么丢掉一次进桌。
			g_attachPending.store(true);
			return;
		}

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
			if (g_bgm == 0 && !HasLoopingClip("duel_bgm.wav")) {
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
		if (name == "duelLeave") {
			logger::info("页面请求离开牌桌");
			StopAllClips();
			g_bgm = 0;
			MusicMuteImpl(false);

			const ml::ViewId view = g_view.load();
			const ml::ModId owner = g_api->GetUIModeOwner();
			if (owner == 0) {
				logger::warn("离开牌桌：当前没有 UI 模式属主，无法释放模式");
			} else {
				const ml::Result res = g_api->ReleaseUIMode(owner);
				logger::info("离开牌桌：属主 mod={}，ReleaseUIMode 结果={}",
					static_cast<unsigned>(owner), static_cast<int>(res));
			}
			if (view && g_api->IsViewValid(view)) {
				g_api->ShowView(view, false);
			}
			return;
		}

		// ready / duelWin / duelLose / duelDraw 仍由主片段脚本自己监听结算。
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
			g_view.store(0);
			logger::info("UI 模式已退出：音频已复位");
		}
		if (!g_wasActive.load() && active) {
			// 新的一次进桌：页面会重新加载，通道要重注册一遍。
			g_attachPending.store(true);
			logger::info("UI 模式已进入：准备重注册页面通道");
		}
		g_wasActive.store(active);
	}

	// 游戏线程
	void Pump()
	{
		// 先判边沿再挂通道：同一 tick 里进入模式就能注册上，不等下一个 250ms 心跳，
		// 免得页面刚显示时那一声 sfx('/ready') 落在空窗里。
		WatchUiMode();
		AttachViewIfPossible();
		DrainCalls();
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
	logger::info("DragonBlood 载入：后台线程已启动（音频 + duelLeave 关桌兜底，无 mod 接管）  [build=closefix-r5]");
	return true;
}
