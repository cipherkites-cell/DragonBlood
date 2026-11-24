// DragonBlood.dll — 龙血棋的音频插件（只做音频）
//
// 为什么必须有它：Magelight 的页面（Ultralight）没有媒体栈，放不了任何声音；
// Papyrus 也没有按文件播放、改「Music」类别音量的接口。
//
// 对外只实现 DragonBloodAudio.psc 里的 7 个原生函数，签名与 v28 的 pex 完全一致：
//   Int Play(String name, Float vol, Bool loop)
//   Stop(Int id) / StopAll() / Tick() / MusicMute(Bool) / IsReady() / DiagCategories()
// 脚本自己决定何时开 BGM、何时静音本体音乐；插件不碰界面、不碰视图、不碰 UI 模式。
//
// 关键点（照 Skygwent 的做法，同一套引擎）：
//   · Music 类别 = BGSDefaultObjectManager 的 kMusicSoundCategory
//   · GetSoundHandleByFile 之后必须 IsValid()，否则就是静默失败
//   · 路径 Sound\ / Data\Sound\ / sound\ 三种根，成功一次就记住

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace logger = SKSE::log;

namespace
{
	constexpr std::uint32_t kSoundFlags = 0x1A;
	constexpr std::uint32_t kSoundPriority = 128;
	constexpr std::uint32_t kLoopLeadMs = 460;

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

	void MusicMute(bool a_mute)
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

	void StopClip(int a_id)
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

	void StopAllClips()
	{
		std::lock_guard lock(g_lock);
		for (auto& pair : g_playing) {
			if (pair.second.handle.IsValid()) {
				pair.second.handle.FadeOutAndRelease(150);
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

	// ---------- Papyrus 原生函数 ----------

	int Play(RE::StaticFunctionTag*, std::string a_name, float a_volume, bool a_loop)
	{
		StartTicking();
		return PlayClip(a_name, a_volume, a_loop);
	}

	void Stop(RE::StaticFunctionTag*, int a_id)
	{
		StopClip(a_id);
	}

	void StopAll(RE::StaticFunctionTag*)
	{
		StopAllClips();
	}

	void Tick(RE::StaticFunctionTag*)
	{
		TickOnce();
	}

	void MusicMute(RE::StaticFunctionTag*, bool a_mute)
	{
		MusicMute(a_mute);
	}

	bool IsReady(RE::StaticFunctionTag*)
	{
		return RE::BSAudioManager::GetSingleton() != nullptr;
	}

	std::string DiagCategories(RE::StaticFunctionTag*)
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
		a_vm->RegisterFunction("Play", "DragonBloodAudio", Play);
		a_vm->RegisterFunction("Stop", "DragonBloodAudio", Stop);
		a_vm->RegisterFunction("StopAll", "DragonBloodAudio", StopAll);
		a_vm->RegisterFunction("Tick", "DragonBloodAudio", Tick);
		a_vm->RegisterFunction("MusicMute", "DragonBloodAudio", MusicMute);
		a_vm->RegisterFunction("IsReady", "DragonBloodAudio", IsReady);
		a_vm->RegisterFunction("DiagCategories", "DragonBloodAudio", DiagCategories);
		logger::info("DragonBloodAudio: 7 个 Papyrus 函数已注册");
		return true;
	}
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	logger::info("DragonBloodAudio 载入");

	auto* papyrus = SKSE::GetPapyrusInterface();
	if (!papyrus || !papyrus->Register(RegisterFunctions)) {
		logger::error("DragonBloodAudio: Papyrus 注册失败");
		return false;
	}
	return true;
}
