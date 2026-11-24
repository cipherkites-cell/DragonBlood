// DragonBloodAudio — 龙血棋的音频插件
//
// 为什么需要它：Magelight 的页面（Ultralight）没有媒体栈，放不了任何声音；
// Papyrus 也没有任何接口能按文件播放、能改「Music」音频类别音量
// （Skyrim 的 Game 脚本只有 GetGameSetting*，setter 是《辐射4》才有的）。
// 所以自定义音效、有旋律的对局 BGM、静音本体音乐这三件事只能在插件里做：
//   · 音频是磁盘上的文件（Data\Sound\DragonBlood\<名>.wav），不需要 esp 记录
//   · BGM 起停靠 BSSoundHandle 句柄
//   · 静音本体音乐是改 Music 音频类别（SNCT）的类别音量，进牌桌 0、离开还原
//
// 对外接口是 Papyrus 脚本 DragonBloodAudio（见 Scripts/Source/DragonBloodAudio.psc）。

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <string>

namespace logger = SKSE::log;

namespace
{
	// 与引擎默认 GetSoundHandle 用的位组合一致（Magelight 的 PlayUISound 也是这套）
	constexpr std::uint32_t kSoundFlags = 0x1A;

	// BGM 文件首尾各 0.45 秒做淡入淡出；提前 460ms 续播，两个 handle 短暂重叠即为无缝
	constexpr std::uint32_t kLoopLeadMs = 460;

	std::string LowerCopy(std::string a_text)
	{
		std::transform(a_text.begin(), a_text.end(), a_text.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return a_text;
	}

	bool        Contains(const std::string& a_hay, const std::string& a_needle)
	{
		return a_hay.find(a_needle) != std::string::npos;
	}

	struct Entry
	{
		RE::BSSoundHandle                     handle;
		std::string                           path;
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

	bool StartSound(Entry& a_entry)
	{
		auto* mgr = RE::BSAudioManager::GetSingleton();
		if (!mgr) {
			logger::error("BSAudioManager 拿不到");
			return false;
		}

		RE::BSResource::ID id;
		id.GenerateFromPath(a_entry.path.c_str());

		a_entry.handle = {};
		mgr->GetSoundHandleByFile(a_entry.handle, id, kSoundFlags, 0);
		a_entry.handle.assumeSuccess = true;  // 加载是异步的，先假定成功，Play 会把请求排进队列
		a_entry.handle.SetVolume(a_entry.volume);

		const bool played = a_entry.handle.Play();
		a_entry.started = std::chrono::steady_clock::now();
		if (a_entry.durationMs == 0) {
			a_entry.durationMs = a_entry.handle.GetDuration();
		}
		return played;
	}

	RE::BGSSoundCategory* FindMusicCategory()
	{
		auto* handler = RE::TESDataHandler::GetSingleton();
		if (!handler) {
			return nullptr;
		}

		RE::BGSSoundCategory* best = nullptr;
		for (auto* form : handler->GetFormArray<RE::BGSSoundCategory>()) {
			if (!form) {
				continue;
			}
			const std::string editor = form->GetFormEditorID() ? form->GetFormEditorID() : "";
			const std::string name = form->GetFullName() ? form->GetFullName() : "";
			logger::info("SNCT {:08X} editor='{}' name='{}' menu={} vol={}",
				form->GetFormID(), editor, name, form->IsMenuCategory() ? 1 : 0, form->GetCategoryVolume());

			const std::string editorLow = LowerCopy(editor);
			const std::string nameLow = LowerCopy(name);

			// 先看名字/EDID 里带 music 的；国/俄/欧版没有英文 EDID 时靠名字兜底
			if (Contains(nameLow, "music") || Contains(nameLow, u8"音乐") || Contains(nameLow, u8"音樂") ||
				Contains(editorLow, "music")) {
				best = form;
				if (Contains(editorLow, "music") && Contains(nameLow, "music")) {
					break;  // 两项都对上，基本就是它
				}
			}
		}

		if (!best) {
			logger::warn("没找到 Music 音频类别，本体音乐不会被静音（DiagCategories 可以打出全部类别）");
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
		entry.path = "Sound\\DragonBlood\\" + name;
		entry.volume = std::clamp(a_volume, 0.0f, 2.0f);
		entry.loop = a_loop;

		if (!StartSound(entry)) {
			logger::warn("播放失败: {}", entry.path);
			return -1;
		}
		logger::info("播放 {} id={} loop={}", entry.path, id, entry.loop ? 1 : 0);
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
		it->second.handle.FadeOutAndRelease(150);
		g_playing.erase(it);
	}

	void StopAll(RE::StaticFunctionTag*)
	{
		std::lock_guard lock(g_lock);
		for (auto& [id, entry] : g_playing) {
			entry.handle.FadeOutAndRelease(150);
		}
		g_playing.clear();
	}

	// 由 Papyrus 每 0.25 秒调一次：处理 BGM 的循环续播、清掉已经播完的一次性音效
	void Tick(RE::StaticFunctionTag*)
	{
		std::lock_guard lock(g_lock);
		const auto now = std::chrono::steady_clock::now();

		for (auto it = g_playing.begin(); it != g_playing.end();) {
			Entry& entry = it->second;

			if (!entry.loop) {
				if (!entry.handle.IsPlaying()) {
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
				++it;  // 还没算出来，下个 tick 再说
				continue;
			}

			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.started).count();
			if (elapsed + static_cast<long long>(kLoopLeadMs) >= static_cast<long long>(entry.durationMs)) {
				// 用新 handle 接上，旧的自然淡出尾音
				Entry fresh = entry;
				fresh.handle = {};
				StartSound(fresh);
				entry.handle = fresh.handle;
				entry.started = fresh.started;
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
				g_musicCategory->SetCategoryVolume(0.0f);
				g_musicMuted = true;
				logger::info("本体音乐已静音 (原音量 {})", g_savedMusicVolume);
			}
		} else if (g_musicMuted) {
			g_musicCategory->SetCategoryVolume(g_savedMusicVolume > 0.0f ? g_savedMusicVolume : 1.0f);
			g_musicMuted = false;
			logger::info("本体音乐音量还原为 {}", g_savedMusicVolume);
		}
	}

	bool IsReady(RE::StaticFunctionTag*)
	{
		return true;  // 能调到这个函数就说明 dll 已经注册好了
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

SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	logger::info("DragonBloodAudio 载入 (Skyrim {})", REL::Module::get().version().string());

	if (auto* papyrus = SKSE::GetPapyrusInterface(); papyrus && !papyrus->Register(RegisterFunctions)) {
		logger::error("DragonBloodAudio: Papyrus 注册失败");
		return false;
	}
	return true;
}
