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
		constexpr double kNearbyInterval = 0.5;  // path trail and nearby undiscovered places

		// Icon sizes in px at 1080p, before the INI scale.
		constexpr float kSizeCity = 16.0f;
		constexpr float kSizeSettlement = 14.0f;
		constexpr float kSizeDungeon = 13.0f;
		constexpr float kSizeOther = 11.0f;
		constexpr float kSizeQuest = 17.0f;
		constexpr float kSizeActor = 7.0f;
		constexpr float kSizeDead = 11.0f;
		constexpr float kSizeDoor = 11.0f;
		constexpr float kSizeCustom = 15.0f;
		constexpr float kSizeHorse = 14.0f;
		constexpr float kSizeTrail = 4.0f;
		constexpr float kSizeNearby = 18.0f;  // '?' glyph
		constexpr float kSizeLabel = 17.0f;   // quest distance glyphs (the ink is about half the glyph cell)
		constexpr float kSizeService = 11.0f;
		constexpr float kSizeDragon = 11.0f;

		constexpr float kTrailSpacing = 256.0f;  // world units between trail points

		std::atomic<bool> flushRequested{ true };

		// Deaths reported by TESDeathEvent, handed to the actor timer. Capped like the log they feed.
		std::mutex                       deathMutex;
		std::vector<DeadBodyLog::Entry>  pendingDeaths;  // guarded by deathMutex
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

		// What an actor is, worked out once when it first shows up in the actor sample (cached by FormID).
		struct ActorTraits
		{
			ServiceKind service{ ServiceKind::kNone };
			bool        guard{ false };
			bool        dragon{ false };
		};

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
			std::vector<IconSource>  questLabels;    // distance glyphs, drawn over the stars
			std::vector<IconSource>  horse;
			std::vector<Vec2>        hiddenMarkers;  // not yet seen map markers of this map (positions only)
			std::vector<IconSource>  nearby;         // '?' for the hidden markers close to the player
			TrailBuffer              trailPoints;
			std::vector<IconSource>  trail;
			int                      jobNearby{ -1 };
			std::unordered_map<RE::FormID, ActorTraits>          traits;
			DeadBodyLog                                          deadBodies;
			std::unordered_map<RE::FormID, const RE::BGSLocation*> markerLocations;  // map marker ref -> its location
			bool                                                 markerLocationsBuilt{ false };
			double                   actorTime{ 0.0 };
			bool                     anyIcons{ false };

			State()
			{
				jobMarkers = schedule.Add(kMarkerInterval);
				jobQuest = schedule.Add(kQuestInterval);
				jobActors = schedule.Add(kActorInterval);
				jobDoors = schedule.Add(kDoorInterval);
				jobNearby = schedule.Add(kNearbyInterval);
			}

			void Clear()
			{
				markers.clear();
				doors.clear();
				custom.clear();
				quests.clear();
				actors.clear();
				questLabels.clear();
				horse.clear();
				hiddenMarkers.clear();
				nearby.clear();
				trailPoints.Clear();
				trail.clear();
				traits.clear();
				deadBodies.Clear();
				{
					std::lock_guard lock(deathMutex);
					pendingDeaths.clear();
				}
				markerLocations.clear();
				markerLocationsBuilt = false;
			}
		};

		State& S()
		{
			static State* state = new State;  // left alive at exit on purpose
			return *state;
		}

		// Each location's world marker (LCTN MNAM), reversed once per game load so a marker finds its location.
		void BuildMarkerLocations(State& a_s)
		{
			a_s.markerLocationsBuilt = true;
			a_s.markerLocations.clear();
			auto* data = RE::TESDataHandler::GetSingleton();
			if (!data)
				return;
			for (const auto* loc : data->GetFormArray<RE::BGSLocation>()) {
				if (!loc)
					continue;
				if (const auto marker = loc->worldLocMarker.get())
					a_s.markerLocations.emplace(marker->GetFormID(), loc);
			}
		}

		bool MarkerCleared(const State& a_s, const RE::TESObjectREFR* a_marker)
		{
			const auto it = a_s.markerLocations.find(a_marker->GetFormID());
			return it != a_s.markerLocations.end() && it->second->IsCleared();
		}

		void GatherMarkers(State& a_s, const Settings::Values& a_cfg, RE::PlayerCharacter* a_player)
		{
			a_s.markers.clear();
			a_s.hiddenMarkers.clear();
			const auto& ic = a_cfg.icons;
			if ((!ic.mapMarkers && !ic.undiscoveredNearby) || !a_s.mapWorld)
				return;
			if (ic.dimCleared && !a_s.markerLocationsBuilt)
				BuildMarkerLocations(a_s);
			for (const auto& handle : a_player->GetPlayerRuntimeData().currentMapMarkers) {
				const auto ref = handle.get();
				if (!ref || ref->IsDisabled() || ref->IsMarkedForDeletion())
					continue;
				const auto* extra = ref->extraList.GetByType<RE::ExtraMapMarker>();
				const auto* data = extra ? extra->mapData : nullptr;
				if (!data || MapRootWorld(ref->GetWorldspace()) != a_s.mapWorld)
					continue;
				const auto pos = ref->GetPosition();
				// Not seen yet: only its position is kept, for the nearby '?' (no name, no type).
				if (!data->flags.all(RE::MapMarkerData::Flag::kVisible)) {
					if (ic.undiscoveredNearby)
						a_s.hiddenMarkers.push_back({ pos.x, pos.y });
					continue;
				}
				if (!ic.mapMarkers)
					continue;
				const bool discovered = data->flags.all(RE::MapMarkerData::Flag::kCanTravelTo);
				if (!discovered && !ic.undiscovered)
					continue;
				const auto cls = ClassifyMapMarker(static_cast<int>(data->type.underlying()));
				IconSource src;
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
				if (!discovered || (ic.dimCleared && MarkerCleared(a_s, ref.get())))
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
		bool QuestPoint(const State& a_s, RE::TESObjectREFR* a_target, const RE::TESQuestTarget* a_qt, Vec2& a_out, bool& a_atTarget)
		{
			a_atTarget = true;
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
					a_atTarget = false;
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
			a_s.questLabels.clear();
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
				bool atTarget = true;
				if (!QuestPoint(a_s, ref.get(), target, point, atTarget))
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
				if (a_cfg.icons.questDistance) {
					// Straight to the target when it shares our space; in another cell, to the door the star points at.
					const auto  pp = a_player->GetPosition();
					const float dist = atTarget ? pp.GetDistance(ref->GetPosition()) : std::hypot(point.x - pp.x, point.y - pp.y);
					AppendTextLabel(a_s.questLabels, FormatDistance(dist), point, kSizeQuest, kSizeLabel, true, a_cfg.icons.colors.questDistance,
						a_cfg.icons.colors.textOutline);
				}
			}
		}

		// VendorItem* keywords in a vendor's buy/sell list -> VendorGoods bits. Keywords keep their editor IDs at runtime.
		std::uint32_t VendorGoodsOf(const RE::BGSListForm* a_list)
		{
			struct KeywordGoods
			{
				const RE::BGSKeyword* keyword;
				std::uint32_t         goods;
			};
			static const auto table = [] {
				static constexpr std::pair<const char*, std::uint32_t> kNames[]{
					{ "VendorItemWeapon", kGoodsWeaponsArmor },
					{ "VendorItemArmor", kGoodsWeaponsArmor },
					{ "VendorItemPotion", kGoodsPotions },
					{ "VendorItemIngredient", kGoodsPotions },
					{ "VendorItemPoison", kGoodsPotions },
					{ "VendorItemSpellTome", kGoodsSpells },
					{ "VendorItemScroll", kGoodsSpells },
					{ "VendorItemStaff", kGoodsSpells },
					{ "VendorItemFood", kGoodsFood },
					{ "VendorItemFoodRaw", kGoodsFood },
				};
				std::vector<KeywordGoods> found;
				for (const auto& [name, goods] : kNames) {
					if (const auto* kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(name))
						found.push_back({ kw, goods });
				}
				logger::info("service icons: {} of {} vendor keywords found", found.size(), std::size(kNames));
				return found;
			}();
			if (!a_list)
				return 0;
			std::uint32_t goods = 0;
			for (const auto* form : a_list->forms) {
				for (const auto& entry : table) {
					if (form == entry.keyword)
						goods |= entry.goods;
				}
			}
			return goods;
		}

		ServiceKind ServiceOf(RE::Actor* a_actor)
		{
			ServiceKind kind = ServiceKind::kNone;
			a_actor->VisitFactions([&](RE::TESFaction* a_faction, std::int8_t a_rank) {
				if (!a_faction || a_rank < 0 || !a_faction->IsVendor())
					return false;
				const auto& vendor = a_faction->vendorData;
				kind = ClassifyVendor(vendor.vendorValues.notBuySell, VendorGoodsOf(vendor.vendorSellBuyList));
				return kind != ServiceKind::kNone;  // stop at the first vendor faction that says what they sell
			});
			return kind;
		}

		const ActorTraits& TraitsOf(State& a_s, RE::Actor* a_actor)
		{
			const auto id = a_actor->GetFormID();
			if (const auto it = a_s.traits.find(id); it != a_s.traits.end())
				return it->second;
			ActorTraits traits;
			traits.guard = a_actor->IsGuard();
			const auto* race = a_actor->GetRace();
			traits.dragon = race && race->HasKeywordString("ActorTypeDragon");
			traits.service = ServiceOf(a_actor);
			return a_s.traits.emplace(id, traits).first->second;
		}

		bool KilledByPlayer(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
		{
			return a_actor->GetActorRuntimeData().myKiller.get().get() == a_player;
		}

		bool IsCorpse(const RE::Actor* a_actor);

		// The player's last ridden mount; its info block sits elsewhere in VR.
		RE::ActorPtr LastRiddenMount(RE::PlayerCharacter* a_player)
		{
			if (REL::Module::IsVR()) {
				const auto* info = a_player->GetVRInfoRuntimeData();
				return info ? info->lastRiddenMount.get() : RE::ActorPtr{};
			}
			return a_player->GetInfoRuntimeData().lastRiddenMount.get();
		}

		void GatherHorse(State& a_s, const Settings::Values& a_cfg, RE::PlayerCharacter* a_player)
		{
			a_s.horse.clear();
			if (!a_cfg.icons.horse || a_player->IsOnMount())
				return;
			const auto horse = LastRiddenMount(a_player);
			if (!horse || IsCorpse(horse.get()) || horse->IsDisabled() || SpaceOf(horse.get()) != a_s.playerSpace)
				return;
			const auto p = horse->GetPosition();
			IconSource src;
			src.world = { p.x, p.y };
			src.shape = IconShape::kHorse;
			src.rgba = a_cfg.icons.colors.horse;
			src.sizePx = kSizeHorse;
			src.clampToRim = true;
			a_s.horse.push_back(src);
		}

		// Game setting iMapMarkerVisibleDistance (12500 in Skyrim.esm): how far the compass shows map markers. Read once.
		float UndiscoveredRadius(const Settings::Values& a_cfg)
		{
			static const int gameRadius = [] {
				auto*       settings = RE::GameSettingCollection::GetSingleton();
				auto*       setting = settings ? settings->GetSetting("iMapMarkerVisibleDistance") : nullptr;
				const int   value = setting ? setting->GetInteger() : 0;
				if (value > 0)
					logger::info("nearby undiscovered places: radius {} (iMapMarkerVisibleDistance)", value);
				else
					logger::info("nearby undiscovered places: game setting missing, using fUndiscoveredRadius");
				return value;
			}();
			return gameRadius > 0 ? static_cast<float>(gameRadius) : a_cfg.icons.undiscoveredRadius;
		}

		void GatherNearbyUndiscovered(State& a_s, const Settings::Values& a_cfg, RE::PlayerCharacter* a_player)
		{
			a_s.nearby.clear();
			IconShape question;
			if (!a_cfg.icons.undiscoveredNearby || a_s.hiddenMarkers.empty() || !GlyphShape('?', question))
				return;
			const auto  pp = a_player->GetPosition();
			const float radius = UndiscoveredRadius(a_cfg);
			for (const auto& m : a_s.hiddenMarkers) {
				const float dx = m.x - pp.x, dy = m.y - pp.y;
				if (dx * dx + dy * dy > radius * radius)
					continue;
				IconSource src;
				src.world = m;
				src.shape = question;
				src.rgba = a_cfg.icons.colors.nearby;
				src.sizePx = kSizeNearby;
				a_s.nearby.push_back(src);
			}
		}

		void GatherTrail(State& a_s, const Settings::Values& a_cfg, RE::PlayerCharacter* a_player)
		{
			if (!a_cfg.icons.trail) {
				a_s.trailPoints.Clear();
				a_s.trail.clear();
				return;
			}
			const auto pp = a_player->GetPosition();
			const bool added = a_s.trailPoints.Offer({ pp.x, pp.y }, kTrailSpacing);
			if (!added && a_s.trail.size() == static_cast<std::size_t>(a_s.trailPoints.Size()))
				return;  // nothing new to draw
			a_s.trail.clear();
			const int  count = a_s.trailPoints.Size();
			const auto rgba = a_cfg.icons.colors.trail;
			for (int i = 0; i < count; ++i) {
				IconSource src;
				src.world = a_s.trailPoints.At(i);
				src.shape = IconShape::kCircle;
				src.rgba = WithAlpha(rgba, TrailBuffer::AgeAlpha(i, count));
				src.sizePx = kSizeTrail;
				a_s.trail.push_back(src);
			}
		}

		// Dead or dying, read from the actor's life state. Actor::IsDead() (a virtual with an "essential" flag) is not
		// trusted alone for this: a body counts when either says so.
		bool IsCorpse(const RE::Actor* a_actor)
		{
			const auto* state = a_actor->AsActorState();
			const auto  life = state ? state->GetLifeState() : RE::ACTOR_LIFE_STATE::kAlive;
			return life == RE::ACTOR_LIFE_STATE::kDead || life == RE::ACTOR_LIFE_STATE::kDying || a_actor->IsDead();
		}

		// Why a logged body leaves the list, or nullptr to keep it.
		const char* DeadBodyDropReason(const State& a_s, const RE::Actor* a_actor)
		{
			if (!a_actor)
				return "no longer exists";
			if (a_actor->IsDeleted() || a_actor->IsMarkedForDeletion())
				return "deleted";
			if (!a_actor->Is3DLoaded())
				return "unloaded";
			if (!IsCorpse(a_actor))
				return "alive again";
			if (SpaceOf(a_actor) != a_s.playerSpace)
				return "in another space";
			return nullptr;
		}

		bool WithinReach(const RE::NiPoint3& a_p, const RE::NiPoint3& a_player, float a_reach)
		{
			const float dx = a_p.x - a_player.x, dy = a_p.y - a_player.y;
			return dx * dx + dy * dy <= a_reach * a_reach;
		}

		ActorSample DeadCross(const RE::Actor* a_actor, const Settings::Values& a_cfg)
		{
			const auto  p = a_actor->GetPosition();
			ActorSample a;
			a.id = a_actor->GetFormID();
			a.cur = a.prev = { p.x, p.y };
			a.shape = IconShape::kCross;
			a.rgba = a_cfg.icons.colors.dead;
			a.size = kSizeDead;
			return a;
		}

		// Bodies from the death events: the dead often leave the high process list, so the list alone misses them.
		// Entries that unloaded, were deleted, came back to life or are in another space are dropped here.
		void AddLoggedDeadBodies(State& a_s, const Settings::Values& a_cfg, const RE::NiPoint3& a_pp, float a_reach, std::vector<RE::FormID>& a_seen,
			std::vector<ActorSample>& a_out)
		{
			{
				std::lock_guard lock(deathMutex);
				for (const auto& death : pendingDeaths) {
					if (a_s.deadBodies.Add(death.formId, death.killedByPlayer))
						logger::debug("dead bodies: added {:08X} (killed by player: {}), {} listed", death.formId, death.killedByPlayer, a_s.deadBodies.Entries().size());
				}
				pendingDeaths.clear();
			}
			a_s.deadBodies.RemoveIf([&](const DeadBodyLog::Entry& a_entry) {
				const char* reason = DeadBodyDropReason(a_s, RE::TESForm::LookupByID<RE::Actor>(a_entry.formId));
				if (reason)
					logger::debug("dead bodies: dropped {:08X}: {}", a_entry.formId, reason);
				return reason != nullptr;
			});
			if (a_cfg.icons.dead == DeadMode::kOff)
				return;
			for (const auto& entry : a_s.deadBodies.Entries()) {
				if (!ShowsDeadBody(a_cfg.icons.dead, entry.killedByPlayer) || std::ranges::find(a_seen, entry.formId) != a_seen.end())
					continue;
				const auto* actor = RE::TESForm::LookupByID<RE::Actor>(entry.formId);
				if (!actor || actor->IsDisabled() || !WithinReach(actor->GetPosition(), a_pp, a_reach))
					continue;
				a_seen.push_back(entry.formId);
				a_out.push_back(DeadCross(actor, a_cfg));
			}
		}

		// Debug log only: dead actors seen in each process list, once when the counts change.
		void LogListedDeadCounts(int a_high, int a_middleHigh)
		{
			static int lastHigh = -1, lastMiddleHigh = -1;
			if (a_high == lastHigh && a_middleHigh == lastMiddleHigh)
				return;
			lastHigh = a_high;
			lastMiddleHigh = a_middleHigh;
			logger::debug("dead bodies: {} dead in the high process list, {} in middle-high (in reach)", a_high, a_middleHigh);
		}

		// iDead = 2: bodies still in the process lists we can read cheaply (middle-high holds many of the recently dead).
		// Returns how many dead actors in reach the list held.
		int AddListedDeadBodies(const RE::BSTArray<RE::ActorHandle>& a_handles, const Settings::Values& a_cfg, RE::PlayerCharacter* a_player,
			const RE::NiPoint3& a_pp, float a_reach, std::vector<RE::FormID>& a_seen, std::vector<ActorSample>& a_out)
		{
			int found = 0;
			for (const auto& handle : a_handles) {
				const auto actor = handle.get();
				if (!actor || actor.get() == a_player || actor->IsDisabled() || !actor->Is3DLoaded() || !IsCorpse(actor.get()))
					continue;
				if (!WithinReach(actor->GetPosition(), a_pp, a_reach))
					continue;
				++found;
				if (std::ranges::find(a_seen, actor->GetFormID()) != a_seen.end())
					continue;
				a_seen.push_back(actor->GetFormID());
				a_out.push_back(DeadCross(actor.get(), a_cfg));
			}
			return found;
		}

		void GatherActors(State& a_s, double a_now, const Settings::Values& a_cfg, RE::PlayerCharacter* a_player, float a_radius)
		{
			const auto& ic = a_cfg.icons;
			std::vector<ActorSample> next;
			std::vector<RE::FormID>  deadSeen;
			int                      deadInHigh = 0;
			const auto               pp = a_player->GetPosition();
			const float              reach = a_radius * 1.5f;
			auto*                    lists = RE::ProcessLists::GetSingleton();
			if (lists) {  // always: dragons are shown whatever the other actor options say
				for (const auto& handle : lists->highActorHandles) {
					const auto actor = handle.get();
					if (!actor || actor.get() == a_player || actor->IsDisabled() || !actor->Is3DLoaded())
						continue;
					const auto p = actor->GetPosition();
					if (!WithinReach(p, pp, reach))
						continue;
					ActorSample a;
					if (IsCorpse(actor.get())) {
						++deadInHigh;
						// The killer is only looked up when the mode needs it; the death log below covers the rest.
						if (!ShowsDeadBody(ic.dead, ic.dead == DeadMode::kPlayerKills && KilledByPlayer(actor.get(), a_player)))
							continue;
						deadSeen.push_back(actor->GetFormID());
						next.push_back(DeadCross(actor.get(), a_cfg));
						continue;
					} else if (const auto& traits = TraitsOf(a_s, actor.get()); traits.dragon) {
						a.rgba = ic.colors.dragon;
						a.size = kSizeDragon;
					} else if (actor->IsPlayerTeammate()) {
						if (!ic.follower)
							continue;
						a.rgba = ic.colors.follower;
					} else if (CountsAsEnemy(actor->IsHostileToActor(a_player), actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kConfidence))) {
						if (!ic.hostile)
							continue;
						a.rgba = ic.colors.hostile;
					} else if (ic.services && traits.service != ServiceKind::kNone) {
						a.shape = ShapeForService(traits.service);
						a.rgba = ic.colors.service;
						a.size = kSizeService;
					} else {
						if (!ic.neutral)
							continue;
						a.rgba = traits.guard ? ic.colors.guard : ic.colors.neutral;
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
				if (ic.dead == DeadMode::kAll)
					LogListedDeadCounts(deadInHigh, AddListedDeadBodies(lists->middleHighActorHandles, a_cfg, a_player, pp, reach, deadSeen, next));
			}
			AddLoggedDeadBodies(a_s, a_cfg, pp, reach, deadSeen, next);
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
			public RE::BSTEventSink<RE::TESDeathEvent>,
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

			// Sent twice per death (dying, then dead); DeadBodyLog merges the two.
			RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override
			{
				if (!a_event || !a_event->actorDying)
					return RE::BSEventNotifyControl::kContinue;
				const auto* player = RE::PlayerCharacter::GetSingleton();
				const bool  byPlayer = player && a_event->actorKiller.get() == player;
				logger::debug("dead bodies: death event for {:08X} (form type {}), dead {}, killer {:08X}", a_event->actorDying->GetFormID(),
					static_cast<int>(a_event->actorDying->GetFormType()), a_event->dead, a_event->actorKiller ? a_event->actorKiller->GetFormID() : 0u);
				if (!a_event->actorDying->Is(RE::FormType::ActorCharacter))
					return RE::BSEventNotifyControl::kContinue;
				std::lock_guard lock(deathMutex);
				if (pendingDeaths.size() < static_cast<std::size_t>(DeadBodyLog::kCapacity))
					pendingDeaths.push_back({ a_event->actorDying->GetFormID(), byPlayer });
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
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESCellAttachDetachEvent>(EventSink::GetSingleton());
			holder->AddEventSink<RE::TESDeathEvent>(EventSink::GetSingleton());
			logger::info("death event sink registered (dead body crosses)");
		}
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
			s.trailPoints.Clear();  // a new cell or worldspace starts a new trail
			s.trail.clear();
			s.schedule.Force(s.jobNearby);
		}

		const auto due = s.schedule.Tick(a_now);
		if (due & (1u << s.jobMarkers))
			GatherMarkers(s, a_cfg, a_player);
		if (due & (1u << s.jobQuest)) {
			GatherQuests(s, a_cfg, a_player);
			GatherCustomMarker(s, a_cfg, a_player);
		}
		if (due & (1u << s.jobActors)) {
			GatherActors(s, a_now, a_cfg, a_player, a_radius);
			GatherHorse(s, a_cfg, a_player);
		}
		if (due & (1u << s.jobNearby)) {
			GatherTrail(s, a_cfg, a_player);
			GatherNearbyUndiscovered(s, a_cfg, a_player);
		}
		if ((due & (1u << s.jobDoors)) && doorsDirty.exchange(false, std::memory_order_relaxed))
			GatherDoors(s, a_cfg, a_player);
	}

	void Collect(double a_now, std::vector<IconSource>& a_out)
	{
		const auto& s = S();
		if (!s.anyIcons)
			return;
		a_out.insert(a_out.end(), s.trail.begin(), s.trail.end());
		a_out.insert(a_out.end(), s.markers.begin(), s.markers.end());
		a_out.insert(a_out.end(), s.nearby.begin(), s.nearby.end());
		a_out.insert(a_out.end(), s.doors.begin(), s.doors.end());
		a_out.insert(a_out.end(), s.custom.begin(), s.custom.end());
		a_out.insert(a_out.end(), s.horse.begin(), s.horse.end());
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
		a_out.insert(a_out.end(), s.questLabels.begin(), s.questLabels.end());
	}

	Counts GetCounts()
	{
		const auto& s = S();
		return { s.markers.size(), s.doors.size(), s.actors.size(), s.quests.size() };
	}
}
