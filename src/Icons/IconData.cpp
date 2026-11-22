#include "PCH.h"

#include "IconData.h"

namespace MM::IconData
{
	namespace
	{
		// Timer intervals (seconds).
		constexpr double kMarkerInterval = 2.0;
		constexpr double kQuestInterval = 0.25;
		constexpr double kActorInterval = 0.2;
		constexpr double kDoorInterval = 0.5;  // only rebuilds when a door cell attached/detached or the player changed cell

		// Icon sizes in px at 1080p, before the INI scale.
		constexpr float kSizeCity = 16.0f;
		constexpr float kSizeSettlement = 14.0f;
		constexpr float kSizeDungeon = 13.0f;
		constexpr float kSizeOther = 11.0f;
		constexpr float kSizeQuest = 17.0f;
		constexpr float kSizeActor = 7.0f;
		constexpr float kSizeDead = 8.0f;
		constexpr float kSizeDoor = 11.0f;
		constexpr float kSizeCustom = 15.0f;

		std::atomic<bool> flushRequested{ true };
		std::atomic<bool> doorsDirty{ true };

		struct Space
		{
			RE::TESObjectCELL*  interior{ nullptr };
			RE::TESWorldSpace*  world{ nullptr };
			friend bool operator==(const Space&, const Space&) = default;
		};

		Space SpaceOf(const RE::TESObjectREFR* a_ref)
		{
			auto* cell = a_ref ? a_ref->GetParentCell() : nullptr;
			if (!cell)
				return {};
			if (cell->IsInteriorCell())
				return { cell, nullptr };
			return { nullptr, a_ref->GetWorldspace() };
		}

		bool Valid(const Space& a_s) { return a_s.interior || a_s.world; }

		std::uint32_t WithAlpha(std::uint32_t a_rgba, float a_k)
		{
			const auto a = static_cast<std::uint32_t>(std::lround(static_cast<float>(a_rgba >> 24) * std::clamp(a_k, 0.0f, 1.0f)));
			return (a_rgba & 0x00FFFFFF) | (a << 24);
		}

		struct ActorSample
		{
			RE::FormID id{ 0 };
			Vec2       prev, cur;
			IconShape  shape{ IconShape::kCircle };
			std::uint32_t rgba{ 0 };
			float      size{ kSizeActor };
		};

		// Main thread only.
		struct State
		{
			Schedule                 schedule;
			int                      jobMarkers{ -1 }, jobQuest{ -1 }, jobActors{ -1 }, jobDoors{ -1 };
			std::uint32_t            settingsGen{ 0 };
			Space                    playerSpace;
			RE::TESWorldSpace*       mapWorld{ nullptr };
			std::vector<IconSource>  markers;
			std::vector<IconSource>  doors;
			std::vector<IconSource>  custom;
			std::vector<IconSource>  quests;
			std::vector<ActorSample> actors;
			double                   actorTime{ 0.0 };
			bool                     anyIcons{ false };

			State()
			{
				jobMarkers = schedule.Add(kMarkerInterval);
				jobQuest = schedule.Add(kQuestInterval);
				jobActors = schedule.Add(kActorInterval);
				jobDoors = schedule.Add(kDoorInterval);
			}

			void Clear()
			{
				markers.clear();
				doors.clear();
				custom.clear();
				quests.clear();
				actors.clear();
			}
		};

		State& S()
		{
			static State* state = new State;  // left alive at exit on purpose
			return *state;
		}

		void GatherMarkers(State& a_s, const Settings::Values& a_cfg, RE::PlayerCharacter* a_player)
		{
			a_s.markers.clear();
			const auto& ic = a_cfg.icons;
			if (!ic.mapMarkers || !a_s.mapWorld)
				return;
			for (const auto& handle : a_player->GetPlayerRuntimeData().currentMapMarkers) {
				const auto ref = handle.get();
				if (!ref || ref->IsDisabled() || ref->IsMarkedForDeletion())
					continue;
				const auto* extra = ref->extraList.GetByType<RE::ExtraMapMarker>();
				const auto* data = extra ? extra->mapData : nullptr;
				if (!data || !data->flags.all(RE::MapMarkerData::Flag::kVisible))
					continue;
				const bool discovered = data->flags.all(RE::MapMarkerData::Flag::kCanTravelTo);
				if (!discovered && !ic.undiscovered)
					continue;
				if (MapRootWorld(ref->GetWorldspace()) != a_s.mapWorld)
					continue;
				const auto cls = ClassifyMapMarker(static_cast<int>(data->type.underlying()));
				IconSource src;
				const auto pos = ref->GetPosition();
				src.world = { pos.x, pos.y };
				src.shape = ShapeForMarkerClass(cls);
				switch (cls) {
				case MarkerClass::kCity:
					src.rgba = ic.colors.city;
					src.sizePx = kSizeCity;
					break;
				case MarkerClass::kSettlement:
					src.rgba = ic.colors.settlement;
					src.sizePx = kSizeSettlement;
					break;
				case MarkerClass::kDungeon:
					src.rgba = ic.colors.dungeon;
					src.sizePx = kSizeDungeon;
					break;
				default:
					src.rgba = ic.colors.other;
					src.sizePx = kSizeOther;
					break;
				}
				if (!discovered)
					src.rgba = WithAlpha(src.rgba, 0.5f);
				a_s.markers.push_back(src);
			}
		}

		void GatherCustomMarker(State& a_s, const Settings::Values& a_cfg, RE::PlayerCharacter* a_player)
		{
			a_s.custom.clear();
			if (!a_cfg.icons.customMarker || !a_s.mapWorld)
				return;
			const auto ref = a_player->GetInfoRuntimeData().playerMapMarker.get();
			if (!ref || ref->IsDisabled() || MapRootWorld(ref->GetWorldspace()) != a_s.mapWorld)
				return;
			const auto pos = ref->GetPosition();
			IconSource src;
			src.world = { pos.x, pos.y };
			src.shape = IconShape::kPin;
			src.rgba = a_cfg.icons.colors.custom;
			src.sizePx = kSizeCustom;
			a_s.custom.push_back(src);
		}

		// Where to point for a quest target: the target itself when it shares our space, else the first door
		// on the path the game keeps for the compass (TESQuestTarget::teleportPath), else nothing.
		bool QuestPoint(const State& a_s, RE::TESObjectREFR* a_target, const RE::TESQuestTarget* a_qt, Vec2& a_out)
		{
			const Space ts = SpaceOf(a_target);
			if (Valid(ts) && ts == a_s.playerSpace) {
				const auto p = a_target->GetPosition();
				a_out = { p.x, p.y };
				return true;
			}
			for (const auto& link : a_qt->teleportPath.teleportRefs) {
				if (link.ref && SpaceOf(link.ref) == a_s.playerSpace) {
					const auto p = link.ref->GetPosition();
					a_out = { p.x, p.y };
					return true;
				}
			}
			// City worldspace <-> parent: same coordinates, so point straight at it.
			if (ts.world && a_s.playerSpace.world && MapRootWorld(ts.world) == a_s.mapWorld) {
				const auto p = a_target->GetPosition();
				a_out = { p.x, p.y };
				return true;
			}
			return false;
		}

		void GatherQuests(State& a_s, const Settings::Values& a_cfg, RE::PlayerCharacter* a_player)
		{
			a_s.quests.clear();
			if (!a_cfg.icons.quest)
				return;
			// The whole pass runs under the game's lock: the game rewrites each target's teleportPath (the compass
			// door path, read by QuestPoint) under this same lock, so reading it after releasing the lock races with
			// that rewrite. BSSpinLock is recursive, so GetTargetRef locking it again on this thread is fine.
			RE::BSSpinLockGuard guard(a_player->GetQuestTargetsLock());
			std::vector<std::pair<RE::TESQuest*, RE::TESQuestTarget*>> pairs;
			for (const auto& entry : a_player->GetQuestTargets()) {
				// Same rule as the compass: only quests tracked in the journal. The list also holds
				// untracked and hidden quests.
				if (!entry.first || !entry.second || !entry.first->IsActive())
					continue;
				for (auto* target : *entry.second) {
					if (target)
						pairs.emplace_back(entry.first, target);
				}
			}
			std::vector<RE::FormID> seen;
			for (const auto& [quest, target] : pairs) {
				RE::ObjectRefHandle handle;
				target->GetTargetRef(handle, true, quest);
				const auto ref = handle.get();
				if (!ref || ref.get() == a_player)
					continue;
				Vec2 point;
				if (!QuestPoint(a_s, ref.get(), target, point))
					continue;
				const auto id = ref->GetFormID();
				if (std::ranges::find(seen, id) != seen.end())
					continue;
				seen.push_back(id);
				IconSource src;
				src.world = point;
				src.shape = IconShape::kStar;
				src.rgba = a_cfg.icons.colors.quest;
				src.sizePx = kSizeQuest;
				src.clampToRim = true;
				a_s.quests.push_back(src);
			}
		}

		void GatherActors(State& a_s, double a_now, const Settings::Values& a_cfg, RE::PlayerCharacter* a_player, float a_radius)
		{
			const auto& ic = a_cfg.icons;
			std::vector<ActorSample> next;
			auto*                    lists = RE::ProcessLists::GetSingleton();
			if (lists && (ic.hostile || ic.follower || ic.neutral || ic.dead)) {
				const auto  pp = a_player->GetPosition();
				const float reach = a_radius * 1.5f;
				for (const auto& handle : lists->highActorHandles) {
					const auto actor = handle.get();
					if (!actor || actor.get() == a_player || actor->IsDisabled() || !actor->Is3DLoaded())
						continue;
					const auto p = actor->GetPosition();
					const float dx = p.x - pp.x, dy = p.y - pp.y;
					if (dx * dx + dy * dy > reach * reach)
						continue;
					ActorSample a;
					if (actor->IsDead()) {
						if (!ic.dead)
							continue;
						a.shape = IconShape::kCross;
						a.rgba = ic.colors.dead;
						a.size = kSizeDead;
					} else if (actor->IsPlayerTeammate()) {
						if (!ic.follower)
							continue;
						a.rgba = ic.colors.follower;
					} else if (actor->IsHostileToActor(a_player)) {
						if (!ic.hostile)
							continue;
						a.rgba = ic.colors.hostile;
					} else {
						if (!ic.neutral)
							continue;
						a.rgba = ic.colors.neutral;
					}
					a.id = actor->GetFormID();
					a.cur = { p.x, p.y };
					a.prev = a.cur;
					// Smooth motion: move from the last sample's position to this one over the next interval.
					for (const auto& old : a_s.actors) {
						if (old.id == a.id) {
							const float jx = a.cur.x - old.cur.x, jy = a.cur.y - old.cur.y;
							if (jx * jx + jy * jy < 2000.0f * 2000.0f)
								a.prev = old.cur;
							break;
						}
					}
					next.push_back(a);
				}
			}
			a_s.actors = std::move(next);
			a_s.actorTime = a_now;
		}

		void AddDoorsOfCell(State& a_s, const RE::TESObjectCELL* a_cell, std::uint32_t a_rgba)
		{
			if (!a_cell)
				return;
			a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				const auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
				if (base && base->Is(RE::FormType::Door) && !a_ref->IsDisabled() && a_ref->extraList.HasType<RE::ExtraTeleport>()) {
					const auto p = a_ref->GetPosition();
					IconSource src;
					src.world = { p.x, p.y };
					src.shape = IconShape::kDoor;
					src.rgba = a_rgba;
					src.sizePx = kSizeDoor;
					a_s.doors.push_back(src);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}

		void GatherDoors(State& a_s, const Settings::Values& a_cfg, RE::PlayerCharacter* a_player)
		{
			a_s.doors.clear();
			const auto mode = a_cfg.icons.doors;
			const bool interior = a_s.playerSpace.interior != nullptr;
			if (mode == Settings::DoorMode::kOff || (mode == Settings::DoorMode::kInteriors && !interior))
				return;
			const auto rgba = a_cfg.icons.colors.door;
			if (interior) {
				AddDoorsOfCell(a_s, a_player->GetParentCell(), rgba);
				return;
			}
			const auto* tes = RE::TES::GetSingleton();
			const auto* grid = tes ? tes->gridCells : nullptr;
			if (!grid)
				return;
			for (std::uint32_t x = 0; x < grid->length; ++x) {
				for (std::uint32_t y = 0; y < grid->length; ++y) {
					const auto* cell = grid->GetCell(x, y);
					if (cell && cell->IsAttached())
						AddDoorsOfCell(a_s, cell, rgba);
				}
			}
		}

		class EventSink :
			public RE::BSTEventSink<RE::TESCellAttachDetachEvent>,
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static EventSink* GetSingleton()
			{
				static EventSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent* a_event, RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
			{
				if (a_event && a_event->reference) {
					const auto* base = a_event->reference->GetBaseObject();
					if (base && base->Is(RE::FormType::Door))
						doorsDirty.store(true, std::memory_order_relaxed);
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->opening && a_event->menuName == RE::LoadingMenu::MENU_NAME)
					RequestFlush();
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	RE::TESWorldSpace* MapRootWorld(RE::TESWorldSpace* a_world)
	{
		// Bounded: a mod that makes two worldspaces each other's parent must not hang the draw call.
		for (int depth = 0; depth < 8 && a_world && a_world->parentWorld && a_world->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLODData); ++depth)
			a_world = a_world->parentWorld;
		return a_world;
	}

	void RegisterEvents()
	{
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton())
			holder->AddEventSink<RE::TESCellAttachDetachEvent>(EventSink::GetSingleton());
		if (auto* ui = RE::UI::GetSingleton())
			ui->AddEventSink<RE::MenuOpenCloseEvent>(EventSink::GetSingleton());
		logger::info("icon event sinks registered");
	}

	void RequestFlush() { flushRequested.store(true, std::memory_order_relaxed); }

	void Update(double a_now, const Settings::Values& a_cfg, RE::PlayerCharacter* a_player, float a_radius)
	{
		auto& s = S();
		if (!a_player || !a_cfg.icons.enabled) {
			if (s.anyIcons) {
				s.Clear();
				s.anyIcons = false;
			}
			return;
		}
		s.anyIcons = true;

		if (flushRequested.exchange(false, std::memory_order_relaxed)) {
			s.Clear();
			s.schedule.ForceAll();
			doorsDirty.store(true, std::memory_order_relaxed);
		}
		if (const auto gen = Settings::Generation(); gen != s.settingsGen) {
			s.settingsGen = gen;
			s.schedule.ForceAll();
			doorsDirty.store(true, std::memory_order_relaxed);
		}

		// Cheap per-frame check: the player moved to another cell / worldspace.
		const Space space = SpaceOf(a_player);
		if (space != s.playerSpace) {
			const bool worldChanged = space.world != s.playerSpace.world;
			s.playerSpace = space;
			s.mapWorld = space.world ? MapRootWorld(space.world) : nullptr;
			doorsDirty.store(true, std::memory_order_relaxed);
			s.schedule.Force(s.jobQuest);
			if (worldChanged || space.interior)
				s.schedule.Force(s.jobMarkers);
			s.actors.clear();
		}

		const auto due = s.schedule.Tick(a_now);
		if (due & (1u << s.jobMarkers))
			GatherMarkers(s, a_cfg, a_player);
		if (due & (1u << s.jobQuest)) {
			GatherQuests(s, a_cfg, a_player);
			GatherCustomMarker(s, a_cfg, a_player);
		}
		if (due & (1u << s.jobActors))
			GatherActors(s, a_now, a_cfg, a_player, a_radius);
		if ((due & (1u << s.jobDoors)) && doorsDirty.exchange(false, std::memory_order_relaxed))
			GatherDoors(s, a_cfg, a_player);
	}

	void Collect(double a_now, std::vector<IconSource>& a_out)
	{
		const auto& s = S();
		if (!s.anyIcons)
			return;
		a_out.insert(a_out.end(), s.markers.begin(), s.markers.end());
		a_out.insert(a_out.end(), s.doors.begin(), s.doors.end());
		a_out.insert(a_out.end(), s.custom.begin(), s.custom.end());
		const float t = static_cast<float>(std::clamp((a_now - s.actorTime) / kActorInterval, 0.0, 1.0));
		for (const auto& a : s.actors) {
			IconSource src;
			src.world = { a.prev.x + (a.cur.x - a.prev.x) * t, a.prev.y + (a.cur.y - a.prev.y) * t };
			src.shape = a.shape;
			src.rgba = a.rgba;
			src.sizePx = a.size;
			a_out.push_back(src);
		}
		a_out.insert(a_out.end(), s.quests.begin(), s.quests.end());
	}

	Counts GetCounts()
	{
		const auto& s = S();
		return { s.markers.size(), s.doors.size(), s.actors.size(), s.quests.size() };
	}
}
