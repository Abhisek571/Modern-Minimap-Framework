#include "PCH.h"

#include "FogState.h"

namespace MM::FogState
{
	namespace
	{
		constexpr std::uint32_t kUniqueId = 'MMFW';
		constexpr std::uint32_t kRecord = 'FOGW';
		constexpr double        kRevealInterval = 1.0;  // seconds between "where is the player" checks

		// The store is touched by the main thread (Update) and the save / load callbacks.
		std::mutex  storeMutex;
		Fog::Store  store;          // guarded by storeMutex
		bool        storeChanged{ true };  // loaded or reverted: rebuild the mask; guarded by storeMutex

		struct Tick
		{
			Schedule                    schedule;
			int                         jobReveal{ -1 };
			std::uint32_t               worldId{ 0 };
			int                         originX{ 0 }, originY{ 0 };
			std::shared_ptr<const Mask> mask;
			std::uint32_t               generation{ 0 };

			Tick() { jobReveal = schedule.Add(kRevealInterval); }
		};

		Tick& T()
		{
			static Tick* tick = new Tick;  // left alive at exit on purpose
			return *tick;
		}

		void OnSave(SKSE::SerializationInterface* a_intfc)
		{
			std::vector<std::uint8_t> data;
			{
				std::lock_guard lock(storeMutex);
				data = store.Serialize();
			}
			if (!a_intfc->OpenRecord(kRecord, Fog::Store::kVersion) || !a_intfc->WriteRecordData(data.data(), static_cast<std::uint32_t>(data.size())))
				logger::error("fog of war: could not write the co-save record");
		}

		void OnLoad(SKSE::SerializationInterface* a_intfc)
		{
			std::uint32_t type = 0, version = 0, length = 0;
			while (a_intfc->GetNextRecordInfo(type, version, length)) {
				if (type != kRecord)
					continue;
				if (version != Fog::Store::kVersion) {
					logger::warn("fog of war: co-save record version {} not known, starting unexplored", version);
					continue;
				}
				std::vector<std::uint8_t> data(length);
				if (length != 0 && a_intfc->ReadRecordData(data.data(), length) != length) {
					logger::error("fog of war: co-save record cut short, starting unexplored");
					continue;
				}
				// A worldspace from a plugin whose load order index moved still resolves; a removed one is dropped.
				auto resolve = [a_intfc](std::uint32_t a_old, std::uint32_t& a_new) { return a_intfc->ResolveFormID(a_old, a_new); };
				std::lock_guard lock(storeMutex);
				if (!store.Deserialize(data, resolve)) {
					store.Clear();
					logger::error("fog of war: co-save record broken, starting unexplored");
				}
				storeChanged = true;
				logger::info("fog of war: loaded {} worldspaces", store.WorldCount());
			}
		}

		void OnRevert(SKSE::SerializationInterface*)
		{
			std::lock_guard lock(storeMutex);
			store.Clear();
			storeChanged = true;
		}
	}

	void RegisterSerialization()
	{
		auto* intfc = SKSE::GetSerializationInterface();
		if (!intfc) {
			logger::warn("fog of war: no SKSE serialization interface; exploration is not saved");
			return;
		}
		intfc->SetUniqueID(kUniqueId);
		intfc->SetSaveCallback(OnSave);
		intfc->SetLoadCallback(OnLoad);
		intfc->SetRevertCallback(OnRevert);
	}

	std::shared_ptr<const Mask> Update(double a_now, const Settings::Values& a_cfg, RE::PlayerCharacter* a_player, RE::TESWorldSpace* a_mapWorld)
	{
		auto& t = T();
		if (!a_cfg.fogOfWar || !a_mapWorld || !a_player)
			return nullptr;
		const auto pos = a_player->GetPosition();
		const Vec2 player{ pos.x, pos.y };
		const auto worldId = a_mapWorld->GetFormID();
		int        originX = 0, originY = 0;
		Fog::MaskOrigin(player, originX, originY);
		const bool moved = worldId != t.worldId || originX != t.originX || originY != t.originY;
		if (worldId != t.worldId)
			t.schedule.Force(t.jobReveal);  // reveal where we arrived right away

		std::lock_guard lock(storeMutex);
		if (storeChanged)
			t.schedule.Force(t.jobReveal);  // after a load: the player's surroundings count at once
		bool dirty = moved || storeChanged || !t.mask;
		if (t.schedule.Tick(a_now) & (1u << t.jobReveal))
			dirty = store.World(worldId).RevealAround(player, Fog::kRevealRadius) > 0 || dirty;
		if (!dirty)
			return t.mask;

		// A bit flipped or the window moved: one new mask (64 KB), at most about once a second while exploring.
		auto mask = std::make_shared<Mask>();
		if (const auto* map = store.Find(worldId))
			map->BuildMask(originX, originY, Fog::kMaskSize, mask->pixels);
		else
			mask->pixels.assign(static_cast<std::size_t>(Fog::kMaskSize) * Fog::kMaskSize, 0);
		mask->world = Fog::MaskRect(originX, originY);
		mask->generation = ++t.generation == 0 ? ++t.generation : t.generation;
		t.mask = std::move(mask);
		t.worldId = worldId;
		t.originX = originX;
		t.originY = originY;
		storeChanged = false;
		return t.mask;
	}
}
