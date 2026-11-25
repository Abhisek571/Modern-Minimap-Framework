#include "Bc7Encode.h"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <DirectXTex.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <utility>

namespace PhotoBaker
{
	namespace
	{
		namespace fs = std::filesystem;
		using Microsoft::WRL::ComPtr;

		// DirectXTex flags per drop-down entry. The GPU encoder honours the same two BC7 flags.
		struct CompressFlags
		{
			TileFormat                  format;
			DirectX::TEX_COMPRESS_FLAGS flags;
		};
		constexpr CompressFlags kCompressFlags[]{
			{ TileFormat::kBc7Max, DirectX::TEX_COMPRESS_BC7_USE_3SUBSETS },
			{ TileFormat::kBc7, DirectX::TEX_COMPRESS_DEFAULT },
			{ TileFormat::kBc7Quick, DirectX::TEX_COMPRESS_BC7_QUICK },
		};

		DirectX::TEX_COMPRESS_FLAGS FlagsFor(TileFormat a_format)
		{
			for (const auto& entry : kCompressFlags) {
				if (entry.format == a_format)
					return entry.flags;
			}
			return DirectX::TEX_COMPRESS_DEFAULT;
		}

		// Box filtering of the raw values, without WIC and without an sRGB round trip: the same averaging the plugin
		// uses for its BC1 mips, so BC7 tiles match them apart from the compression.
		constexpr auto kFilter = DirectX::TEX_FILTER_BOX | DirectX::TEX_FILTER_FORCE_NON_WIC;

		// The tiles are stored as UNORM with the sRGB-encoded values written as they are, like the plugin's BC1 tiles.
		constexpr DXGI_FORMAT kTileFormat = DXGI_FORMAT_BC7_UNORM;

		std::uint64_t WriteTime(const fs::path& a_path)
		{
			WIN32_FILE_ATTRIBUTE_DATA data{};
			if (!GetFileAttributesExW(a_path.c_str(), GetFileExInfoStandard, &data))
				return 0;
			return (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
		}

		// Up to a_limit bytes from the start of a file (0 = the whole file).
		bool ReadBytes(const fs::path& a_path, std::size_t a_limit, std::vector<std::uint8_t>& a_out)
		{
			std::ifstream in(a_path, std::ios::binary);
			if (!in)
				return false;
			std::size_t size = a_limit;
			if (size == 0) {
				in.seekg(0, std::ios::end);
				size = static_cast<std::size_t>(in.tellg());
				in.seekg(0);
			}
			a_out.resize(size);
			in.read(reinterpret_cast<char*>(a_out.data()), static_cast<std::streamsize>(size));
			a_out.resize(static_cast<std::size_t>(in.gcount()));
			return true;
		}

		std::vector<fs::path> ListMasters(const fs::path& a_folder)
		{
			std::vector<fs::path> masters;
			std::error_code       ec;
			for (fs::directory_iterator it(a_folder, ec), end; !ec && it != end; it.increment(ec)) {
				if (it->is_regular_file(ec) && _wcsicmp(it->path().extension().c_str(), L".bmp") == 0)
					masters.push_back(it->path());
			}
			return masters;
		}

		ComPtr<ID3D11Device> MakeGpuDevice()
		{
			ComPtr<ID3D11Device> device;
			constexpr D3D_FEATURE_LEVEL kLevels[]{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
			if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, kLevels, static_cast<UINT>(std::size(kLevels)), D3D11_SDK_VERSION,
					device.GetAddressOf(), nullptr, nullptr)))
				return nullptr;
			return device;
		}

		std::wstring HrText(HRESULT a_hr) { return std::format(L"error 0x{:08X}", static_cast<std::uint32_t>(a_hr)); }

		// Master .bmp -> toned RGBA image.
		HRESULT LoadMaster(const fs::path& a_path, const EncodeJob& a_job, DirectX::ScratchImage& a_out)
		{
			std::vector<std::uint8_t> file;
			if (!ReadBytes(a_path, 0, file))
				return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
			int                       width = 0, height = 0;
			std::vector<std::uint8_t> rgba;
			if (!DecodeBmp24(file, width, height, rgba))
				return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
			ToneMasterRgba(a_job.tone, a_job.recolour, rgba);
			const HRESULT hr = a_out.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, static_cast<std::size_t>(width), static_cast<std::size_t>(height), 1, 1);
			if (FAILED(hr))
				return hr;
			const DirectX::Image* image = a_out.GetImage(0, 0, 0);
			for (int y = 0; y < height; ++y)
				std::memcpy(image->pixels + image->rowPitch * static_cast<std::size_t>(y), rgba.data() + static_cast<std::size_t>(y) * width * 4,
					static_cast<std::size_t>(width) * 4);
			return S_OK;
		}

		HRESULT ShrinkTo(DirectX::ScratchImage& a_image, int a_side)
		{
			const auto& meta = a_image.GetMetadata();
			if (static_cast<int>(meta.width) <= a_side)
				return S_OK;
			DirectX::ScratchImage smaller;
			const HRESULT hr = DirectX::Resize(*a_image.GetImage(0, 0, 0), static_cast<std::size_t>(a_side), static_cast<std::size_t>(a_side), kFilter, smaller);
			if (SUCCEEDED(hr))
				a_image = std::move(smaller);
			return hr;
		}

		HRESULT Compress(ID3D11Device* a_gpu, const DirectX::ScratchImage& a_mips, TileFormat a_format, DirectX::ScratchImage& a_out)
		{
			const auto flags = FlagsFor(a_format);
			if (a_gpu && SUCCEEDED(DirectX::Compress(a_gpu, a_mips.GetImages(), a_mips.GetImageCount(), a_mips.GetMetadata(), kTileFormat, flags, 1.0f, a_out)))
				return S_OK;
			return DirectX::Compress(a_mips.GetImages(), a_mips.GetImageCount(), a_mips.GetMetadata(), kTileFormat, flags | DirectX::TEX_COMPRESS_PARALLEL,
				DirectX::TEX_THRESHOLD_DEFAULT, a_out);
		}

		// Through a temp file renamed over the target, so a stopped run never leaves a half-written tile that looks done.
		HRESULT SaveTile(const DirectX::ScratchImage& a_tile, const fs::path& a_dest)
		{
			const auto    temp = fs::path(a_dest).concat(L".tmp");
			const HRESULT hr = DirectX::SaveToDDSFile(a_tile.GetImages(), a_tile.GetImageCount(), a_tile.GetMetadata(), DirectX::DDS_FLAGS_NONE, temp.c_str());
			if (FAILED(hr))
				return hr;
			if (!MoveFileExW(temp.c_str(), a_dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				const auto error = GetLastError();
				DeleteFileW(temp.c_str());
				return HRESULT_FROM_WIN32(error);
			}
			return S_OK;
		}

		enum class TileResult
		{
			kEncoded,
			kSkipped,
			kFailed
		};

		// Side the tile should have, or 0 when the master's header is unreadable.
		int WantedSide(const fs::path& a_master, int a_maxSide)
		{
			std::vector<std::uint8_t> head;
			if (!ReadBytes(a_master, kBmpHeadBytes, head))
				return 0;
			const int width = BmpWidth(head);
			return width > 0 ? EncodedSide(width, a_maxSide) : 0;
		}

		// A tile is out of date when its master or the colour settings changed after it was written.
		bool UpToDate(const fs::path& a_master, const fs::path& a_dest, int a_side, std::uint64_t a_colourTime)
		{
			std::vector<std::uint8_t> head;
			if (!ReadBytes(a_dest, kDdsHeadBytes, head))
				return false;
			return ShouldSkipEncode(head, a_side, WriteTime(a_dest), std::max(WriteTime(a_master), a_colourTime));
		}

		TileResult EncodeTile(ID3D11Device* a_gpu, const fs::path& a_master, const fs::path& a_dest, const EncodeJob& a_job, std::wstring& a_why)
		{
			const int side = WantedSide(a_master, a_job.maxSide);
			if (side == 0) {
				a_why = L"not a 24-bit master image";
				return TileResult::kFailed;
			}
			if (UpToDate(a_master, a_dest, side, a_job.colourTime))
				return TileResult::kSkipped;

			DirectX::ScratchImage image, mips, tile;
			HRESULT               hr = LoadMaster(a_master, a_job, image);
			if (SUCCEEDED(hr))
				hr = ShrinkTo(image, side);
			if (SUCCEEDED(hr))
				hr = DirectX::GenerateMipMaps(*image.GetImage(0, 0, 0), kFilter, 0, mips);  // 0 = the full chain down to 1x1
			if (SUCCEEDED(hr))
				hr = Compress(a_gpu, mips, a_job.format, tile);
			if (SUCCEEDED(hr))
				hr = SaveTile(tile, a_dest);
			if (FAILED(hr)) {
				a_why = HrText(hr);
				return TileResult::kFailed;
			}
			return TileResult::kEncoded;
		}
	}

	void Bc7Encoder::Start(const EncodeJob& a_job)
	{
		Stop();
		_stop = false;
		_finished = false;
		_gpu = false;
		_total = -1;
		_encoded = 0;
		_skipped = 0;
		_failed = 0;
		{
			std::scoped_lock lock(_failuresLock);
			_failures.clear();
		}
		_thread = std::thread(&Bc7Encoder::Run, this, a_job);
	}

	void Bc7Encoder::Stop()
	{
		_stop = true;
		if (_thread.joinable())
			_thread.join();
	}

	EncodeCounts Bc7Encoder::Counts() const
	{
		EncodeCounts counts;
		counts.total = _total;
		counts.encoded = _encoded;
		counts.skipped = _skipped;
		counts.failed = _failed;
		counts.done = counts.encoded + counts.skipped + counts.failed;
		counts.gpu = _gpu;
		counts.finished = _finished;
		counts.stopped = _finished && counts.total >= 0 && counts.done < counts.total;
		return counts;
	}

	std::vector<std::wstring> Bc7Encoder::TakeFailures()
	{
		std::scoped_lock lock(_failuresLock);
		return std::exchange(_failures, {});
	}

	void Bc7Encoder::AddFailure(std::wstring a_line)
	{
		std::scoped_lock lock(_failuresLock);
		_failures.push_back(std::move(a_line));
	}

	void Bc7Encoder::Run(EncodeJob a_job)
	{
		const auto masters = ListMasters(a_job.masters);
		_total = static_cast<int>(masters.size());
		std::error_code ec;
		fs::create_directories(a_job.dest, ec);

		const auto gpu = MakeGpuDevice();  // made and used on this thread only
		_gpu = gpu != nullptr;

		for (const auto& master : masters) {
			if (_stop)
				break;
			const auto   dest = a_job.dest / fs::path(master.stem()).concat(L".dds");
			std::wstring why;
			switch (EncodeTile(gpu.Get(), master, dest, a_job, why)) {
			case TileResult::kEncoded:
				++_encoded;
				break;
			case TileResult::kSkipped:
				++_skipped;
				break;
			case TileResult::kFailed:
				AddFailure(master.filename().wstring() + L": " + why);
				++_failed;
				break;
			}
		}
		_finished = true;
	}
}
