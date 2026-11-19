#include "TerrainMesh.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

namespace MM
{
	namespace
	{
		class Reader
		{
		public:
			explicit Reader(std::span<const std::uint8_t> a_data) :
				_data(a_data)
			{}

			template <class T>
			bool Get(T& a_out)
			{
				if (_pos + sizeof(T) > _data.size()) {
					return false;
				}
				std::memcpy(&a_out, _data.data() + _pos, sizeof(T));
				_pos += sizeof(T);
				return true;
			}

			bool Skip(std::size_t a_bytes)
			{
				if (_pos + a_bytes > _data.size()) {
					return false;
				}
				_pos += a_bytes;
				return true;
			}

			bool String(std::size_t a_len, std::string& a_out)
			{
				if (_pos + a_len > _data.size()) {
					return false;
				}
				a_out.assign(reinterpret_cast<const char*>(_data.data() + _pos), a_len);
				_pos += a_len;
				return true;
			}

			bool ByteString(std::string& a_out)
			{
				std::uint8_t len = 0;
				return Get(len) && String(len, a_out);
			}

			bool SizedString(std::string& a_out)
			{
				std::uint32_t len = 0;
				return Get(len) && String(len, a_out);
			}

			[[nodiscard]] std::size_t Pos() const noexcept { return _pos; }
			void                      Seek(std::size_t a_pos) noexcept { _pos = std::min(a_pos, _data.size()); }
			[[nodiscard]] std::size_t Size() const noexcept { return _data.size(); }

		private:
			std::span<const std::uint8_t> _data;
			std::size_t                   _pos{ 0 };
		};

		struct Transform
		{
			float t[3]{ 0, 0, 0 };
			float r[3][3]{ { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
			float s{ 1.0f };

			[[nodiscard]] std::array<float, 3> Apply(const std::array<float, 3>& a_v) const noexcept
			{
				std::array<float, 3> o{};
				for (int i = 0; i < 3; ++i) {
					o[i] = t[i] + s * (r[i][0] * a_v[0] + r[i][1] * a_v[1] + r[i][2] * a_v[2]);
				}
				return o;
			}

			// this (parent) applied after a_child
			[[nodiscard]] Transform Compose(const Transform& a_child) const noexcept
			{
				Transform o;
				for (int i = 0; i < 3; ++i) {
					for (int j = 0; j < 3; ++j) {
						o.r[i][j] = r[i][0] * a_child.r[0][j] + r[i][1] * a_child.r[1][j] + r[i][2] * a_child.r[2][j];
					}
				}
				const auto tt = Apply({ a_child.t[0], a_child.t[1], a_child.t[2] });
				o.t[0] = tt[0];
				o.t[1] = tt[1];
				o.t[2] = tt[2];
				o.s = s * a_child.s;
				return o;
			}
		};

		struct Block
		{
			bool                     isNode{ false };
			bool                     isShape{ false };
			std::int32_t             name{ -1 };
			Transform                xf;
			std::vector<std::int32_t> children;
			BtrMesh                  mesh;
		};

		// NiObjectNET + NiAVObject part shared by nodes and shapes (SSE, user version 2 = 100).
		bool ReadAVObject(Reader& a_r, Block& a_b)
		{
			std::uint32_t numExtra = 0, flags = 0;
			std::int32_t  controller = 0, collision = 0;
			if (!a_r.Get(a_b.name) || !a_r.Get(numExtra) || !a_r.Skip(std::size_t(numExtra) * 4) || !a_r.Get(controller) || !a_r.Get(flags)) {
				return false;
			}
			for (float& v : a_b.xf.t) {
				if (!a_r.Get(v))
					return false;
			}
			for (auto& row : a_b.xf.r) {
				for (float& v : row) {
					if (!a_r.Get(v))
						return false;
				}
			}
			return a_r.Get(a_b.xf.s) && a_r.Get(collision);
		}

		bool ReadNode(Reader& a_r, Block& a_b)
		{
			std::uint32_t numChildren = 0;
			if (!ReadAVObject(a_r, a_b) || !a_r.Get(numChildren) || numChildren > 65536) {
				return false;
			}
			a_b.children.resize(numChildren);
			for (auto& c : a_b.children) {
				if (!a_r.Get(c))
					return false;
			}
			return true;
		}

		bool ReadTriShape(Reader& a_r, Block& a_b)
		{
			std::int32_t  skin = 0, shader = 0, alpha = 0;
			std::uint64_t desc = 0;
			std::uint16_t numTris = 0, numVerts = 0;
			std::uint32_t dataSize = 0;
			if (!ReadAVObject(a_r, a_b) || !a_r.Skip(16) || !a_r.Get(skin) || !a_r.Get(shader) || !a_r.Get(alpha) ||
				!a_r.Get(desc) || !a_r.Get(numTris) || !a_r.Get(numVerts) || !a_r.Get(dataSize)) {
				return false;
			}
			if (dataSize == 0) {
				return true;
			}
			const std::size_t stride = static_cast<std::size_t>(desc & 0xF) * 4;
			const bool        hasVertex = ((desc >> 44) & 0x1) != 0;
			if (!hasVertex || stride < 12 || stride * numVerts + std::size_t(numTris) * 6 != dataSize) {
				return false;
			}
			const std::size_t start = a_r.Pos();
			a_b.mesh.verts.resize(numVerts);
			for (std::size_t i = 0; i < numVerts; ++i) {
				a_r.Seek(start + i * stride);
				auto& v = a_b.mesh.verts[i];
				if (!a_r.Get(v[0]) || !a_r.Get(v[1]) || !a_r.Get(v[2]))
					return false;
			}
			a_r.Seek(start + stride * numVerts);
			a_b.mesh.tris.resize(numTris);
			for (auto& t : a_b.mesh.tris) {
				if (!a_r.Get(t[0]) || !a_r.Get(t[1]) || !a_r.Get(t[2]))
					return false;
				if (t[0] >= numVerts || t[1] >= numVerts || t[2] >= numVerts)
					return false;
			}
			return true;
		}

		bool IEquals(const std::string& a_a, const char* a_b)
		{
			const std::size_t n = std::strlen(a_b);
			if (a_a.size() != n)
				return false;
			for (std::size_t i = 0; i < n; ++i) {
				if (std::tolower(static_cast<unsigned char>(a_a[i])) != std::tolower(static_cast<unsigned char>(a_b[i])))
					return false;
			}
			return true;
		}

		bool EndsWith(const std::string& a_s, const char* a_suffix)
		{
			const std::size_t n = std::strlen(a_suffix);
			return a_s.size() >= n && a_s.compare(a_s.size() - n, n, a_suffix) == 0;
		}
	}

	bool ParseBtr(std::span<const std::uint8_t> a_data, BtrData& a_out, std::string& a_error)
	{
		a_out = {};
		Reader r(a_data);

		// Header line "Gamebryo File Format, Version 20.2.0.7\n"
		std::size_t eol = 0;
		while (eol < a_data.size() && eol < 64 && a_data[eol] != '\n')
			++eol;
		if (eol >= a_data.size() || a_data[eol] != '\n' || std::memcmp(a_data.data(), "Gamebryo File Format", 20) != 0) {
			a_error = "not a NIF file";
			return false;
		}
		r.Seek(eol + 1);

		std::uint32_t version = 0, userVersion = 0, numBlocks = 0, bsVersion = 0;
		std::uint8_t  endian = 0;
		if (!r.Get(version) || !r.Get(endian) || !r.Get(userVersion) || !r.Get(numBlocks) || !r.Get(bsVersion)) {
			a_error = "truncated header";
			return false;
		}
		if (version != 0x14020007 || endian != 1 || bsVersion != 100 || numBlocks > 100000) {
			a_error = "unsupported NIF version (need Skyrim SE, 20.2.0.7 / 100)";
			return false;
		}
		std::string tmp;
		if (!r.ByteString(tmp) || !r.ByteString(tmp) || !r.ByteString(tmp)) {  // author, process script, export script
			a_error = "truncated export info";
			return false;
		}

		std::uint16_t numTypes = 0;
		if (!r.Get(numTypes)) {
			a_error = "truncated block types";
			return false;
		}
		std::vector<std::string> types(numTypes);
		for (auto& t : types) {
			if (!r.SizedString(t)) {
				a_error = "truncated block types";
				return false;
			}
		}
		std::vector<std::uint16_t> typeIndex(numBlocks);
		for (auto& i : typeIndex) {
			if (!r.Get(i) || i >= numTypes) {
				a_error = "bad block type index";
				return false;
			}
		}
		std::vector<std::uint32_t> sizes(numBlocks);
		for (auto& s : sizes) {
			if (!r.Get(s)) {
				a_error = "truncated block sizes";
				return false;
			}
		}
		std::uint32_t numStrings = 0, maxLen = 0;
		if (!r.Get(numStrings) || !r.Get(maxLen) || numStrings > 100000) {
			a_error = "truncated string table";
			return false;
		}
		std::vector<std::string> strings(numStrings);
		for (auto& s : strings) {
			if (!r.SizedString(s)) {
				a_error = "truncated string table";
				return false;
			}
		}
		std::uint32_t numGroups = 0;
		if (!r.Get(numGroups) || !r.Skip(std::size_t(numGroups) * 4)) {
			a_error = "truncated groups";
			return false;
		}

		std::vector<Block> blocks(numBlocks);
		for (std::uint32_t i = 0; i < numBlocks; ++i) {
			const std::string& type = types[typeIndex[i]];
			a_out.blockTypes.push_back(type);
			const std::size_t start = r.Pos();
			if (start + sizes[i] > r.Size()) {
				a_error = "block runs past end of file";
				return false;
			}
			Reader br(a_data.subspan(start, sizes[i]));
			Block& b = blocks[i];
			if (EndsWith(type, "Node")) {
				b.isNode = ReadNode(br, b);
			} else if (type == "BSTriShape" || type == "BSMeshLODTriShape" || type == "BSSubIndexTriShape") {
				b.isShape = ReadTriShape(br, b);
				if (!b.isShape) {
					a_error = "bad BSTriShape block " + std::to_string(i);
					return false;
				}
			}
			r.Seek(start + sizes[i]);
		}

		// Walk the scene graph from block 0, carrying transforms and whether we are under a WATER node.
		auto nameOf = [&](const Block& a_b) -> std::string {
			return (a_b.name >= 0 && static_cast<std::uint32_t>(a_b.name) < strings.size()) ? strings[a_b.name] : std::string{};
		};
		std::vector<bool> visited(numBlocks, false);
		struct Item
		{
			std::int32_t idx;
			Transform    parent;
			bool         underWater;
		};
		std::vector<Item> stack;
		if (numBlocks > 0)
			stack.push_back({ 0, Transform{}, false });
		while (!stack.empty()) {
			const Item it = stack.back();
			stack.pop_back();
			if (it.idx < 0 || static_cast<std::uint32_t>(it.idx) >= numBlocks || visited[it.idx])
				continue;
			visited[it.idx] = true;
			Block&          b = blocks[it.idx];
			const Transform world = it.parent.Compose(b.xf);
			const std::string name = nameOf(b);
			if (b.isNode) {
				const bool water = it.underWater || IEquals(name, "WATER");
				for (auto c : b.children)
					stack.push_back({ c, world, water });
			} else if (b.isShape) {
				BtrMesh mesh = std::move(b.mesh);
				mesh.name = name;
				for (auto& v : mesh.verts)
					v = world.Apply(v);
				if (it.underWater)
					a_out.water.push_back(std::move(mesh));
				else if (IEquals(name, "land"))
					a_out.land.push_back(std::move(mesh));
			}
		}
		if (a_out.land.empty()) {
			a_error = "no land shape";
			return false;
		}
		return true;
	}

	std::vector<float> RasterizeHeights(const std::vector<BtrMesh>& a_meshes, int a_size, float a_tileUnits)
	{
		constexpr float    kEmpty = -std::numeric_limits<float>::infinity();
		std::vector<float> out(static_cast<std::size_t>(a_size) * a_size, kEmpty);
		const float        k = a_size / a_tileUnits;
		for (const auto& m : a_meshes) {
			for (const auto& t : m.tris) {
				float xs[3], ys[3], zs[3];
				for (int i = 0; i < 3; ++i) {
					const auto& v = m.verts[t[i]];
					xs[i] = v[0] * k;
					ys[i] = a_size - v[1] * k;  // row 0 = north
					zs[i] = v[2];
				}
				const float d = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2]);
				if (std::fabs(d) < 1e-9f)
					continue;
				const int x0 = std::max(static_cast<int>(std::floor(std::min({ xs[0], xs[1], xs[2] }))), 0);
				const int x1 = std::min(static_cast<int>(std::ceil(std::max({ xs[0], xs[1], xs[2] }))), a_size - 1);
				const int y0 = std::max(static_cast<int>(std::floor(std::min({ ys[0], ys[1], ys[2] }))), 0);
				const int y1 = std::min(static_cast<int>(std::ceil(std::max({ ys[0], ys[1], ys[2] }))), a_size - 1);
				for (int py = y0; py <= y1; ++py) {
					const float Y = py + 0.5f;
					for (int px = x0; px <= x1; ++px) {
						const float X = px + 0.5f;
						const float w0 = ((ys[1] - ys[2]) * (X - xs[2]) + (xs[2] - xs[1]) * (Y - ys[2])) / d;
						const float w1 = ((ys[2] - ys[0]) * (X - xs[2]) + (xs[0] - xs[2]) * (Y - ys[2])) / d;
						const float w2 = 1.0f - w0 - w1;
						if (w0 < -1e-6f || w1 < -1e-6f || w2 < -1e-6f)
							continue;
						const float z = w0 * zs[0] + w1 * zs[1] + w2 * zs[2];
						float&      dst = out[static_cast<std::size_t>(py) * a_size + px];
						dst = std::max(dst, z);
					}
				}
			}
		}
		return out;
	}

	std::vector<std::uint8_t> RasterizeWater(const BtrData& a_btr, int a_size, float a_tileUnits)
	{
		std::vector<std::uint8_t> mask(static_cast<std::size_t>(a_size) * a_size, 0);
		if (a_btr.water.empty())
			return mask;
		const auto land = RasterizeHeights(a_btr.land, a_size, a_tileUnits);
		const auto water = RasterizeHeights(a_btr.water, a_size, a_tileUnits);
		for (std::size_t i = 0; i < mask.size(); ++i) {
			if (std::isfinite(water[i]) && land[i] < water[i])
				mask[i] = 255;
		}
		return mask;
	}
}
