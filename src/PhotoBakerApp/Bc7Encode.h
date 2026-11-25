#pragma once

// BC7 encoding of the plugin's lossless master tiles (DirectXTex), on a background thread.
// Every master .bmp in a folder becomes a BC7 .dds with a full mip chain in the output folder. A tile whose .dds is
// already BC7, the wanted size and newer than its master is skipped, so a stopped or crashed run continues where it
// left off. Uses the GPU (DirectCompute) when a Direct3D 11 device can be made, the CPU otherwise.
// The masters are untoned: each one gets the plugin's tone (tone.txt) and the optional recolour before encoding.

#include "MasterTone.h"
#include "TileFormats.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace PhotoBaker
{
	struct EncodeCounts
	{
		int  total{ -1 };   // masters found (-1 = still listing)
		int  done{ 0 };     // encoded + skipped + failed
		int  encoded{ 0 };
		int  skipped{ 0 };  // already BC7 and up to date
		int  failed{ 0 };
		bool gpu{ false };
		bool finished{ false };
		bool stopped{ false };  // Stop() before every tile was handled
	};

	struct EncodeJob
	{
		std::filesystem::path masters;  // folder of <key>.bmp
		std::filesystem::path dest;     // textures\ModernMinimap\<world> of the output mod
		TileFormat            format{ TileFormat::kBc7 };
		int                   maxSide{ 0 };  // 0 = the master's own size; kHalfSetSide = a half-size set from 1024 masters
		MM::BakeImage::Tone   tone;          // from the masters' tone.txt; the masters themselves are untoned
		Recolour              recolour;      // brightness and saturation from "Encode existing masters"
		std::uint64_t         colourTime{ 0 };  // FILETIME of the last colour change: older tiles are encoded again
	};

	class Bc7Encoder
	{
	public:
		Bc7Encoder() = default;
		~Bc7Encoder() { Stop(); }
		Bc7Encoder(const Bc7Encoder&) = delete;
		Bc7Encoder& operator=(const Bc7Encoder&) = delete;

		void Start(const EncodeJob& a_job);  // a_job.format must be one of the BC7 formats
		void Stop();                         // asks the worker to stop after its current tile and waits for it
		void RequestStop() { _stop = true; }  // the same without waiting; Counts().finished turns true when it has stopped
		[[nodiscard]] bool                      Running() const { return _thread.joinable() && !_finished; }
		[[nodiscard]] EncodeCounts              Counts() const;
		[[nodiscard]] std::vector<std::wstring> TakeFailures();  // one line per failed file since the last call

	private:
		void Run(EncodeJob a_job);
		void AddFailure(std::wstring a_line);

		std::thread               _thread;
		std::atomic<bool>         _stop{ false };
		std::atomic<bool>         _finished{ false };
		std::atomic<bool>         _gpu{ false };
		std::atomic<int>          _total{ -1 };
		std::atomic<int>          _encoded{ 0 };
		std::atomic<int>          _skipped{ 0 };
		std::atomic<int>          _failed{ 0 };
		std::mutex                _failuresLock;
		std::vector<std::wstring> _failures;
	};
}
