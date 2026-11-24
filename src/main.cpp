// DragonBloodAudio — 龙血棋的音频插件
//
// 为什么需要它：Magelight 的页面（Ultralight）没有媒体栈，放不了任何声音；
// Papyrus 也没有任何接口能按文件播放、能改「Music」音频类别音量。
// 所以自定义音效、有旋律的对局 BGM、静音本体音乐这三件事只能在插件里做。
//
// 音频播放与「静音本体音乐」这两段是照 Skygwent.dll 的源码逐条对齐的（它用同一套
// Magelight 宿主 + 同一条音频路径，在 1.5.97 上已验证可用）：
//   · 取 Music 音频类别：BGSDefaultObjectManager::GetObject<BGSSoundCategory>(kMusicSoundCategory)，
//     不是遍历 SNCT 按名字猜
//   · 取句柄：GetSoundHandleByFile(handle, id, 0x1A, 128)，然后必须查 handle.IsValid()，
//     不能自己把 assumeSuccess 置 true 假装成功
//   · 文件路径根目录要探测（Sound\FX\ 与 Data\Sound\FX\ 两种写法在实际机器上都能出现）
//
// 对外接口是 Papyrus 脚本 DragonBloodAudio（见 Scripts/Source/DragonBloodAudio.psc）。

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <cctype>
#include <cstdint>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <string>

namespace logger = SKSE::log;

namespace
{
	constexpr std::uint32_t kSoundFlags = 0x1A;
	constexpr std::uint32_t kSoundPriority = 128;   // Skygwent 用的就是 128
	constexpr std::uint32_t kLoopLeadMs = 460;      // BGM 提前 460ms 续播

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

	struct Entry
	{
		RE::BSSoundHandle                     handle;
		std::string                           file;   // 相对路径，不含根
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

	// 第一次成功播放后记住可用的根目录，后续不再试探
	std::string g_soundRoot;

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
			return false;   // 文件不存在或引擎没加载到
		}

		handle.SetVolume(std::clamp(a_volume, 0.0f, 1.0f));
		if (!handle.Play()) {
			return false;
		}

		a_out = handle;
		return true;
	}

	// a_file 形如 "DragonBlood\\duel_bgm.wav"
	bool PlayFile(const std::string& a_file, float a_volume, RE::BSSoundHandle& a_out)
	{
		if (!g_soundRoot.empty()) {
			if (TryPlay(g_soundRoot + a_file, a_volume, a_out)) {
				return true;
			}
			g_soundRoot.clear();   // 之前记的根失效了，重新探测
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

	// 本体音乐类别：先走默认对象管理器（Skygwent 的做法），失败再退到遍历 SNCT 按名字找
	RE::BGSSoundCategory* FindMusicCategory()
	{
		if (auto* defaults = RE::BGSDefaultObjectManager::GetSingleton()) {
			if (auto* cat = defaults->GetObject<RE::BGSSoundCategory>(RE::DEFAULT_OBJECT::kMusicSoundCategory)) {
				logger::info("Music 音频类别 = {:08X} (default object)", cat->GetFormID());
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
			if (Contains(editorLow, "music") || Contains(nameLow, "music") ||
				Contains(nameLow, "音乐") || Contains(nameLow, "音樂")) {
				logger::info("SNCT 候选 {:08X} editor='{}' name='{}' vol={:.2f}",
					form->GetFormID(), editor, name, form->GetCategoryVolume());
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

	// ---------------- Papyrus 原生函数 ----------------

	int Play(RE::StaticFunctionTag*, std::string a_name, float a_volume, bool a_loop)
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

	void Stop(RE::StaticFunctionTag*, int a_id)
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

	void StopAll(RE::StaticFunctionTag*)
	{
		std::lock_guard lock(g_lock);
		for (auto& [id, entry] : g_playing) {
			if (entry.handle.IsValid()) {
				entry.handle.FadeOutAndRelease(150);
			}
		}
		g_playing.clear();
	}

	void Tick(RE::StaticFunctionTag*)
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
				++it;   // 时长还没算出来，下个 tick 再说
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
					it = g_playing.erase(it);   // 续播失败就别留死句柄
					continue;
				}
			}
			++it;
		}
	}

	void MusicMute(RE::StaticFunctionTag*, bool a_mute)
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

	bool IsReady(RE::StaticFunctionTag*)
	{
		return true;
	}

	std::string DiagCategories(RE::StaticFunctionTag*)
	{
		auto* handler = RE::TESDataHandler::GetSingleton();
		if (!handler) {
			return "TESDataHandler 不存在";
		}

		std::string out;
		for (auto* form : handler->GetFormArray<RE::BGSSoundCategory>()) {
			if (!form) {
				continue;
			}
			out += fmt::format("{:08X} [{}] {} vol={:.2f}\n",
				form->GetFormID(),
				form->GetFormEditorID() ? form->GetFormEditorID() : "",
				form->GetFullName() ? form->GetFullName() : "",
				form->GetCategoryVolume());
		}
		return out;
	}

	bool RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm)
	{
		a_vm->RegisterFunction("Play", "DragonBloodAudio", Play);
		a_vm->RegisterFunction("Stop", "DragonBloodAudio", Stop);
		a_vm->RegisterFunction("StopAll", "DragonBloodAudio", StopAll);
		a_vm->RegisterFunction("Tick", "DragonBloodAudio", Tick);
		a_vm->RegisterFunction("MusicMute", "DragonBloodAudio", MusicMute);
		a_vm->RegisterFunction("DiagCategories", "DragonBloodAudio", DiagCategories);
		a_vm->RegisterFunction("IsReady", "DragonBloodAudio", IsReady);
		logger::info("DragonBloodAudio: Papyrus 函数已注册");
		return true;
	}
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	logger::info("DragonBloodAudio 载入 (Skyrim {})", REL::Module::get().version().string());

	if (auto* papyrus = SKSE::GetPapyrusInterface(); papyrus && !papyrus->Register(RegisterFunctions)) {
		logger::error("DragonBloodAudio: Papyrus 注册失败");
		return false;
	}
	return true;
}
