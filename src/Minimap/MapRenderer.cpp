#include "MapRenderer.h"

#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <d3dcompiler.h>

namespace MM
{
	namespace
	{
		struct TileCB
		{
			float rect[4];
			float light[4];
			float shade[4];
			float grade[4];
			float water[4];
		};

		struct OverlayCB
		{
			float rect[4];
			float geo[4];
			float rot[4];
			float style[4];
			float out[4];
			float look[4];
			float tint[4];
		};

		struct IconCB
		{
			float view[4];
			float mask[4];
			float out[4];
		};

		struct NavCB
		{
			float view[4];
			float band[4];
			float color[4];
		};

		struct NavWallVertex
		{
			float p[3];
			float q[3];
			float side;
		};

		template <class T>
		void SafeRelease(T*& a_p)
		{
			if (a_p) {
				a_p->Release();
				a_p = nullptr;
			}
		}

		bool CompileShader(const char* a_entry, const char* a_target, ComPtr<ID3DBlob>& a_out, std::string& a_error)
		{
			ComPtr<ID3DBlob> errors;
			const HRESULT    hr = D3DCompile(Shaders::kSource, sizeof(Shaders::kSource) - 1, "ModernMinimap", nullptr, nullptr, a_entry, a_target,
				   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, a_out.ReleaseAndGetAddressOf(), errors.GetAddressOf());
			if (FAILED(hr)) {
				a_error = std::string("shader ") + a_entry + ": " + (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
				return false;
			}
			return true;
		}

		template <class T>
		void Upload(ID3D11DeviceContext* a_ctx, ID3D11Buffer* a_buffer, const T& a_data)
		{
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(a_ctx->Map(a_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				std::memcpy(mapped.pData, &a_data, sizeof(T));
				a_ctx->Unmap(a_buffer, 0);
			}
		}
	}

	void D3DStateBlock::Backup(ID3D11DeviceContext* a_ctx)
	{
		scissorCount = viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		a_ctx->RSGetScissorRects(&scissorCount, scissors);
		a_ctx->RSGetViewports(&viewportCount, viewports);
		a_ctx->RSGetState(&rs);
		a_ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
		a_ctx->OMGetDepthStencilState(&dss, &stencilRef);
		a_ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
		a_ctx->PSGetShaderResources(0, 3, psSRV);
		a_ctx->PSGetSamplers(0, 1, &psSampler);
		psInstCount = vsInstCount = gsInstCount = hsInstCount = dsInstCount = kMaxInstances;
		a_ctx->PSGetShader(&ps, psInst, &psInstCount);
		a_ctx->VSGetShader(&vs, vsInst, &vsInstCount);
		a_ctx->GSGetShader(&gs, gsInst, &gsInstCount);
		a_ctx->HSGetShader(&hs, hsInst, &hsInstCount);
		a_ctx->DSGetShader(&ds, dsInst, &dsInstCount);
		a_ctx->VSGetConstantBuffers(0, 1, &vsCB);
		a_ctx->PSGetConstantBuffers(0, 1, &psCB);
		a_ctx->IAGetPrimitiveTopology(&topology);
		a_ctx->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
		a_ctx->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
		a_ctx->IAGetInputLayout(&layout);
	}

	void D3DStateBlock::BindTargets(ID3D11DeviceContext* a_ctx) const
	{
		a_ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
		a_ctx->RSSetViewports(viewportCount, viewports);
	}

	void D3DStateBlock::Restore(ID3D11DeviceContext* a_ctx)
	{
		a_ctx->RSSetScissorRects(scissorCount, scissors);
		a_ctx->RSSetViewports(viewportCount, viewports);
		a_ctx->RSSetState(rs);
		a_ctx->OMSetBlendState(blend, blendFactor, sampleMask);
		a_ctx->OMSetDepthStencilState(dss, stencilRef);
		a_ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
		a_ctx->PSSetShaderResources(0, 3, psSRV);
		a_ctx->PSSetSamplers(0, 1, &psSampler);
		a_ctx->PSSetShader(ps, psInst, psInstCount);
		a_ctx->VSSetShader(vs, vsInst, vsInstCount);
		a_ctx->GSSetShader(gs, gsInst, gsInstCount);
		a_ctx->HSSetShader(hs, hsInst, hsInstCount);
		a_ctx->DSSetShader(ds, dsInst, dsInstCount);
		a_ctx->VSSetConstantBuffers(0, 1, &vsCB);
		a_ctx->PSSetConstantBuffers(0, 1, &psCB);
		a_ctx->IASetPrimitiveTopology(topology);
		a_ctx->IASetIndexBuffer(ib, ibFormat, ibOffset);
		a_ctx->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
		a_ctx->IASetInputLayout(layout);

		SafeRelease(rs);
		SafeRelease(blend);
		SafeRelease(dss);
		for (auto*& v : rtv)
			SafeRelease(v);
		SafeRelease(dsv);
		for (auto*& v : psSRV)
			SafeRelease(v);
		SafeRelease(psSampler);
		SafeRelease(ps);
		SafeRelease(vs);
		SafeRelease(gs);
		SafeRelease(hs);
		SafeRelease(ds);
		for (UINT i = 0; i < psInstCount; ++i)
			SafeRelease(psInst[i]);
		for (UINT i = 0; i < vsInstCount; ++i)
			SafeRelease(vsInst[i]);
		for (UINT i = 0; i < gsInstCount; ++i)
			SafeRelease(gsInst[i]);
		for (UINT i = 0; i < hsInstCount; ++i)
			SafeRelease(hsInst[i]);
		for (UINT i = 0; i < dsInstCount; ++i)
			SafeRelease(dsInst[i]);
		SafeRelease(vsCB);
		SafeRelease(psCB);
		SafeRelease(ib);
		SafeRelease(vb);
		SafeRelease(layout);
	}

	bool MapRenderer::Init(ID3D11Device* a_device, std::string& a_error)
	{
		_device = a_device;
		ComPtr<ID3DBlob> vsTile, psTile, vsOverlay, psOverlay;
		if (!CompileShader("VSTile", "vs_5_0", vsTile, a_error) || !CompileShader("PSTile", "ps_5_0", psTile, a_error) ||
			!CompileShader("VSOverlay", "vs_5_0", vsOverlay, a_error) || !CompileShader("PSOverlay", "ps_5_0", psOverlay, a_error)) {
			return false;
		}
		auto ok = [&](HRESULT a_hr, const char* a_what) {
			if (FAILED(a_hr))
				a_error = std::string("create ") + a_what + " failed";
			return SUCCEEDED(a_hr);
		};
		if (!ok(a_device->CreateVertexShader(vsTile->GetBufferPointer(), vsTile->GetBufferSize(), nullptr, &_vsTile), "VSTile") ||
			!ok(a_device->CreatePixelShader(psTile->GetBufferPointer(), psTile->GetBufferSize(), nullptr, &_psTile), "PSTile") ||
			!ok(a_device->CreateVertexShader(vsOverlay->GetBufferPointer(), vsOverlay->GetBufferSize(), nullptr, &_vsOverlay), "VSOverlay")) {
			return false;
		}

		D3D11_BUFFER_DESC cb{};
		cb.Usage = D3D11_USAGE_DYNAMIC;
		cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		cb.ByteWidth = sizeof(TileCB);
		if (!ok(a_device->CreateBuffer(&cb, nullptr, &_cbTile), "tile constants"))
			return false;
		cb.ByteWidth = sizeof(OverlayCB);
		if (!ok(a_device->CreateBuffer(&cb, nullptr, &_cbOverlay), "overlay constants"))
			return false;

		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
		if (!ok(a_device->CreateSamplerState(&sd, &_sampler), "sampler"))
			return false;

		D3D11_BLEND_DESC bd{};
		bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (!ok(a_device->CreateBlendState(&bd, &_blendOpaque), "opaque blend"))
			return false;
		auto& rt = bd.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
		rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		rt.BlendOp = D3D11_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D11_BLEND_ONE;
		rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		if (!ok(a_device->CreateBlendState(&bd, &_blendAlpha), "alpha blend"))
			return false;

		D3D11_RASTERIZER_DESC rd{};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		if (!ok(a_device->CreateRasterizerState(&rd, &_raster), "rasterizer"))
			return false;

		D3D11_DEPTH_STENCIL_DESC dd{};
		dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
		dd.StencilEnable = FALSE;
		dd.DepthEnable = FALSE;
		if (!ok(a_device->CreateDepthStencilState(&dd, &_depthOff), "depth state"))
			return false;

		if (!InitIcons(a_device, a_error))
			return false;

		// Optional: without it interiors fall back to the radar disc.
		if (!InitNav(a_device, _navError))
			_psNav.Reset();

		// Overlay pixel shader last: Ready() keys off it.
		return ok(a_device->CreatePixelShader(psOverlay->GetBufferPointer(), psOverlay->GetBufferSize(), nullptr, &_psOverlay), "PSOverlay");
	}

	bool MapRenderer::InitIcons(ID3D11Device* a_device, std::string& a_error)
	{
		ComPtr<ID3DBlob> vs, ps;
		if (!CompileShader("VSIcon", "vs_5_0", vs, a_error) || !CompileShader("PSIcon", "ps_5_0", ps, a_error))
			return false;
		auto ok = [&](HRESULT a_hr, const char* a_what) {
			if (FAILED(a_hr))
				a_error = std::string("create ") + a_what + " failed";
			return SUCCEEDED(a_hr);
		};
		if (!ok(a_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &_vsIcon), "VSIcon") ||
			!ok(a_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &_psIcon), "PSIcon"))
			return false;

		const D3D11_INPUT_ELEMENT_DESC layout[]{
			{ "INST", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(IconInstance, x), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
			{ "INST", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(IconInstance, clip), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
			{ "INST", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(IconInstance, uv), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
			{ "INST", 3, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(IconInstance, rgba), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
		};
		if (!ok(a_device->CreateInputLayout(layout, static_cast<UINT>(std::size(layout)), vs->GetBufferPointer(), vs->GetBufferSize(), &_iconLayout), "icon layout"))
			return false;

		D3D11_BUFFER_DESC bd{};
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		bd.ByteWidth = sizeof(IconInstance) * kMaxIcons;
		if (!ok(a_device->CreateBuffer(&bd, nullptr, &_iconVB), "icon buffer"))
			return false;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.ByteWidth = sizeof(IconCB);
		if (!ok(a_device->CreateBuffer(&bd, nullptr, &_cbIcon), "icon constants"))
			return false;

		const AtlasImage img = BuildIconAtlas();
		D3D11_TEXTURE2D_DESC td{};
		td.Width = td.Height = static_cast<UINT>(img.size);
		td.MipLevels = static_cast<UINT>(img.mips.size());
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_IMMUTABLE;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		std::vector<D3D11_SUBRESOURCE_DATA> init;
		for (std::size_t i = 0; i < img.mips.size(); ++i)
			init.push_back({ img.mips[i].data(), static_cast<UINT>((img.size >> i) * 4), 0 });
		return ok(a_device->CreateTexture2D(&td, init.data(), &_atlas), "icon atlas") &&
		       ok(a_device->CreateShaderResourceView(_atlas.Get(), nullptr, &_atlasSRV), "icon atlas view");
	}

	bool MapRenderer::InitNav(ID3D11Device* a_device, std::string& a_error)
	{
		ComPtr<ID3DBlob> vs, vsWall, ps;
		if (!CompileShader("VSNav", "vs_5_0", vs, a_error) || !CompileShader("VSNavWall", "vs_5_0", vsWall, a_error) || !CompileShader("PSNav", "ps_5_0", ps, a_error))
			return false;
		auto ok = [&](HRESULT a_hr, const char* a_what) {
			if (FAILED(a_hr))
				a_error = std::string("create ") + a_what + " failed";
			return SUCCEEDED(a_hr);
		};
		const D3D11_INPUT_ELEMENT_DESC layout[]{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		const D3D11_INPUT_ELEMENT_DESC wallLayout[]{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(NavWallVertex, p), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(NavWallVertex, q), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, offsetof(NavWallVertex, side), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		D3D11_BUFFER_DESC cb{};
		cb.Usage = D3D11_USAGE_DYNAMIC;
		cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		cb.ByteWidth = sizeof(NavCB);
		return ok(a_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &_vsNav), "VSNav") &&
		       ok(a_device->CreateVertexShader(vsWall->GetBufferPointer(), vsWall->GetBufferSize(), nullptr, &_vsNavWall), "VSNavWall") &&
		       ok(a_device->CreateInputLayout(layout, static_cast<UINT>(std::size(layout)), vs->GetBufferPointer(), vs->GetBufferSize(), &_navLayout), "nav layout") &&
		       ok(a_device->CreateInputLayout(wallLayout, static_cast<UINT>(std::size(wallLayout)), vsWall->GetBufferPointer(), vsWall->GetBufferSize(), &_navWallLayout),
				   "nav wall layout") &&
		       ok(a_device->CreateBuffer(&cb, nullptr, &_cbNav), "nav constants") &&
		       ok(a_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &_psNav), "PSNav");  // last: NavReady() keys off it
	}

	bool MapRenderer::SetNavMesh(const NavMeshData& a_mesh)
	{
		if (!NavReady())
			return false;
		if (a_mesh.generation == _navGen)
			return _navTriCount != 0;
		_navGen = a_mesh.generation;
		_navVB.Reset();
		_navIB.Reset();
		_navWallVB.Reset();
		_navWallIB.Reset();
		_navTriCount = _navWallCount = 0;
		if (a_mesh.Empty())
			return false;

		auto make = [&](const void* a_data, std::size_t a_bytes, UINT a_bind, ComPtr<ID3D11Buffer>& a_out) {
			if (a_bytes == 0 || a_bytes > 0x7FFFFFFF)
				return false;
			D3D11_BUFFER_DESC bd{};
			bd.Usage = D3D11_USAGE_IMMUTABLE;
			bd.BindFlags = a_bind;
			bd.ByteWidth = static_cast<UINT>(a_bytes);
			const D3D11_SUBRESOURCE_DATA init{ a_data, 0, 0 };
			return SUCCEEDED(_device->CreateBuffer(&bd, &init, &a_out));
		};
		if (!make(a_mesh.xyz.data(), a_mesh.xyz.size() * sizeof(float), D3D11_BIND_VERTEX_BUFFER, _navVB) ||
			!make(a_mesh.tris.data(), a_mesh.tris.size() * sizeof(std::uint32_t), D3D11_BIND_INDEX_BUFFER, _navIB)) {
			_navVB.Reset();
			_navIB.Reset();
			return false;
		}
		_navTriCount = static_cast<UINT>(a_mesh.tris.size());

		// Walls: each edge becomes a quad (4 corners, 2 triangles) widened to the line width in the vertex shader.
		const std::size_t edges = a_mesh.walls.size() / 2;
		if (edges != 0) {
			std::vector<NavWallVertex> verts;
			std::vector<std::uint32_t> idx;
			verts.reserve(edges * 4);
			idx.reserve(edges * 6);
			const float* xyz = a_mesh.xyz.data();
			for (std::size_t e = 0; e < edges; ++e) {
				const float* a = xyz + static_cast<std::size_t>(a_mesh.walls[e * 2]) * 3;
				const float* b = xyz + static_cast<std::size_t>(a_mesh.walls[e * 2 + 1]) * 3;
				const auto   base = static_cast<std::uint32_t>(verts.size());
				// The corner at b with side -1 lies on the same side as the corner at a with side +1.
				verts.push_back({ { a[0], a[1], a[2] }, { b[0], b[1], b[2] }, 1.0f });
				verts.push_back({ { a[0], a[1], a[2] }, { b[0], b[1], b[2] }, -1.0f });
				verts.push_back({ { b[0], b[1], b[2] }, { a[0], a[1], a[2] }, -1.0f });
				verts.push_back({ { b[0], b[1], b[2] }, { a[0], a[1], a[2] }, 1.0f });
				for (const std::uint32_t i : { 0u, 1u, 2u, 1u, 3u, 2u })
					idx.push_back(base + i);
			}
			if (make(verts.data(), verts.size() * sizeof(NavWallVertex), D3D11_BIND_VERTEX_BUFFER, _navWallVB) &&
				make(idx.data(), idx.size() * sizeof(std::uint32_t), D3D11_BIND_INDEX_BUFFER, _navWallIB))
				_navWallCount = static_cast<UINT>(idx.size());
		}
		return true;
	}

	bool MapRenderer::BeginCompose(ID3D11DeviceContext* a_ctx, int a_size, const float (&a_clear)[3])
	{
		if (!Ready() || !EnsureTarget(a_size))
			return false;
		ID3D11ShaderResourceView* nullSRV[3]{};
		a_ctx->PSSetShaderResources(0, 3, nullSRV);  // the target may still be bound as an input from the last overlay draw
		a_ctx->OMSetRenderTargets(1, _targetRTV.GetAddressOf(), nullptr);
		const D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(_targetSize), static_cast<float>(_targetSize), 0, 1 };
		a_ctx->RSSetViewports(1, &vp);
		const float clear[4]{ a_clear[0], a_clear[1], a_clear[2], 1.0f };
		a_ctx->ClearRenderTargetView(_targetRTV.Get(), clear);
		return true;
	}

	void MapRenderer::ComposeNav(ID3D11DeviceContext* a_ctx, const NavComposeParams& a_p)
	{
		if (!NavReady() || !BeginCompose(a_ctx, a_p.targetSize, kNavClear) || _navTriCount == 0)
			return;

		const float size = static_cast<float>(_targetSize);
		const float ndcPerUnit = 2.0f / (a_p.unitsPerPixel * size);
		const float halfWidthNdc = a_p.wallWidthPx / size;  // (width / 2) px * (2 / size) NDC per px
		NavCB       cb{ { a_p.center.x, a_p.center.y, ndcPerUnit, halfWidthNdc }, { a_p.playerZ - a_p.floorBelow, a_p.playerZ + a_p.floorAbove, 0, a_p.otherFloorAlpha }, {} };

		SetCommonState(a_ctx, _vsNav.Get(), _psNav.Get(), _blendAlpha.Get());
		a_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		a_ctx->VSSetConstantBuffers(0, 1, _cbNav.GetAddressOf());
		a_ctx->PSSetConstantBuffers(0, 1, _cbNav.GetAddressOf());

		// Other floors first (faint, underneath), then the player's floor on top. Each: floor triangles, then walls.
		const UINT offset = 0;
		for (int pass = a_p.otherFloorAlpha > 0.0f ? 1 : 0; pass >= 0; --pass) {
			cb.band[2] = static_cast<float>(pass);
			std::memcpy(cb.color, kNavFloor, sizeof kNavFloor);
			Upload(a_ctx, _cbNav.Get(), cb);
			const UINT stride = sizeof(float) * 3;
			a_ctx->VSSetShader(_vsNav.Get(), nullptr, 0);
			a_ctx->IASetInputLayout(_navLayout.Get());
			a_ctx->IASetVertexBuffers(0, 1, _navVB.GetAddressOf(), &stride, &offset);
			a_ctx->IASetIndexBuffer(_navIB.Get(), DXGI_FORMAT_R32_UINT, 0);
			a_ctx->DrawIndexed(_navTriCount, 0, 0);

			if (_navWallCount == 0)
				continue;
			std::memcpy(cb.color, kNavWall, sizeof kNavWall);
			Upload(a_ctx, _cbNav.Get(), cb);
			const UINT wallStride = sizeof(NavWallVertex);
			a_ctx->VSSetShader(_vsNavWall.Get(), nullptr, 0);
			a_ctx->IASetInputLayout(_navWallLayout.Get());
			a_ctx->IASetVertexBuffers(0, 1, _navWallVB.GetAddressOf(), &wallStride, &offset);
			a_ctx->IASetIndexBuffer(_navWallIB.Get(), DXGI_FORMAT_R32_UINT, 0);
			a_ctx->DrawIndexed(_navWallCount, 0, 0);
		}
	}

	bool MapRenderer::EnsureTarget(int a_size)
	{
		if (_target && _targetSize == a_size)
			return true;
		_target.Reset();
		_targetRTV.Reset();
		_targetSRV.Reset();
		_targetSize = 0;
		D3D11_TEXTURE2D_DESC td{};
		td.Width = td.Height = static_cast<UINT>(a_size);
		td.MipLevels = td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(_device->CreateTexture2D(&td, nullptr, &_target)) || FAILED(_device->CreateRenderTargetView(_target.Get(), nullptr, &_targetRTV)) ||
			FAILED(_device->CreateShaderResourceView(_target.Get(), nullptr, &_targetSRV))) {
			_target.Reset();
			_targetRTV.Reset();
			_targetSRV.Reset();
			return false;
		}
		_targetSize = a_size;
		return true;
	}

	void MapRenderer::SetCommonState(ID3D11DeviceContext* a_ctx, ID3D11VertexShader* a_vs, ID3D11PixelShader* a_ps, ID3D11BlendState* a_blend)
	{
		const float factor[4]{ 0, 0, 0, 0 };
		a_ctx->IASetInputLayout(nullptr);
		a_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		ID3D11Buffer* nullVB = nullptr;
		const UINT    zero = 0;
		a_ctx->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
		a_ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		a_ctx->VSSetShader(a_vs, nullptr, 0);
		a_ctx->PSSetShader(a_ps, nullptr, 0);
		a_ctx->GSSetShader(nullptr, nullptr, 0);
		a_ctx->HSSetShader(nullptr, nullptr, 0);
		a_ctx->DSSetShader(nullptr, nullptr, 0);
		a_ctx->PSSetSamplers(0, 1, _sampler.GetAddressOf());
		a_ctx->RSSetState(_raster.Get());
		a_ctx->OMSetBlendState(a_blend, factor, 0xFFFFFFFF);
		a_ctx->OMSetDepthStencilState(_depthOff.Get(), 0);
	}

	void MapRenderer::Compose(ID3D11DeviceContext* a_ctx, const ComposeParams& a_params, std::span<const TileDraw> a_tiles)
	{
		if (!BeginCompose(a_ctx, a_params.targetSize, kNavClear))
			return;
		ID3D11ShaderResourceView* nullSRV[3]{};

		SetCommonState(a_ctx, _vsTile.Get(), _psTile.Get(), _blendOpaque.Get());
		a_ctx->VSSetConstantBuffers(0, 1, _cbTile.GetAddressOf());
		a_ctx->PSSetConstantBuffers(0, 1, _cbTile.GetAddressOf());

		const auto& s = a_params.shade;
		float       lx = s.lightX, ly = s.lightY, lz = s.lightZ;
		const float len = std::sqrt(lx * lx + ly * ly + lz * lz);
		if (len > 1e-6f) {
			lx /= len;
			ly /= len;
			lz /= len;
		} else {
			lx = ly = 0.0f;
			lz = 1.0f;
		}
		const float invLz = lz > 0.05f ? 1.0f / lz : 20.0f;

		const float size = static_cast<float>(_targetSize);
		for (const auto& t : a_tiles) {
			if (!t.diffuse)
				continue;
			const Rect r = TileWorldRect(t.id);
			const Vec2 tl = WorldToTarget({ r.minX, r.maxY }, a_params.center, a_params.unitsPerPixel, size);
			const Vec2 br = WorldToTarget({ r.maxX, r.minY }, a_params.center, a_params.unitsPerPixel, size);
			if (br.x <= 0 || br.y <= 0 || tl.x >= size || tl.y >= size)
				continue;
			const bool hasWater = s.water && t.water;
			const bool baked = t.id.lvl == kBakedLevel;  // finished picture: draw as is (shade 1, no grading)
			TileCB     cb{
                { tl.x / size * 2 - 1, 1 - tl.y / size * 2, br.x / size * 2 - 1, 1 - br.y / size * 2 },
                { lx, ly, lz, invLz },
                { s.ambient, s.diffuse, s.shadeMin, s.shadeMax },
                { s.brightness, s.saturation, t.normal ? 1.0f : 0.0f, hasWater ? 1.0f : 0.0f },
                { s.waterR, s.waterG, s.waterB, s.waterAlpha }
			};
			if (baked) {
				const float shade[4]{ 1.0f, 0.0f, 0.0f, 2.0f };  // s = 1
				const float grade[4]{ 1.0f, 1.0f, 0.0f, 0.0f };  // brightness 1, saturation 1, no normal, no water
				std::memcpy(cb.shade, shade, sizeof shade);
				std::memcpy(cb.grade, grade, sizeof grade);
			}
			Upload(a_ctx, _cbTile.Get(), cb);
			ID3D11ShaderResourceView* srvs[3]{ t.diffuse, t.normal, hasWater ? t.water : nullptr };
			a_ctx->PSSetShaderResources(0, 3, srvs);
			a_ctx->Draw(4, 0);
		}
		a_ctx->PSSetShaderResources(0, 3, nullSRV);
	}

	void MapRenderer::DrawOverlay(ID3D11DeviceContext* a_ctx, const OverlayParams& a_p)
	{
		if (!Ready())
			return;
		const bool hasMap = a_p.hasMap && _targetSRV;
		SetCommonState(a_ctx, _vsOverlay.Get(), _psOverlay.Get(), _blendAlpha.Get());
		a_ctx->VSSetConstantBuffers(0, 1, _cbOverlay.GetAddressOf());
		a_ctx->PSSetConstantBuffers(0, 1, _cbOverlay.GetAddressOf());

		const float W = a_p.viewportW, H = a_p.viewportH;
		OverlayCB   cb{
            { a_p.left / W * 2 - 1, 1 - a_p.top / H * 2, (a_p.left + a_p.size) / W * 2 - 1, 1 - (a_p.top + a_p.size) / H * 2 },
            { a_p.size, static_cast<float>(_targetSize), a_p.offsetPx.x, a_p.offsetPx.y },
            { std::cos(a_p.mapAngle), std::sin(a_p.mapAngle), std::cos(a_p.arrowAngle), std::sin(a_p.arrowAngle) },
            { a_p.opacity, a_p.round ? 1.0f : 0.0f, hasMap ? 1.0f : 0.0f, a_p.arrowSize },
            { a_p.linearOut ? 1.0f : 0.0f, a_p.outScale, 0.0f, 0.0f },
            { a_p.look.saturation, a_p.look.contrast, 0.0f, 0.0f },
            { a_p.look.tintR, a_p.look.tintG, a_p.look.tintB, 0.0f }
		};
		Upload(a_ctx, _cbOverlay.Get(), cb);
		ID3D11ShaderResourceView* srv = hasMap ? _targetSRV.Get() : nullptr;
		a_ctx->PSSetShaderResources(0, 1, &srv);
		a_ctx->Draw(4, 0);
		ID3D11ShaderResourceView* nullSRV = nullptr;
		a_ctx->PSSetShaderResources(0, 1, &nullSRV);
	}

	void MapRenderer::DrawIcons(ID3D11DeviceContext* a_ctx, const IconPassParams& a_p, std::span<const IconInstance> a_icons)
	{
		if (!Ready() || a_icons.empty())
			return;
		const auto count = static_cast<UINT>(std::min<std::size_t>(a_icons.size(), kMaxIcons));
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_ctx->Map(_iconVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return;
		std::memcpy(mapped.pData, a_icons.data(), sizeof(IconInstance) * count);
		// Over capacity: the last instance (the player arrow, drawn on top) replaces the last slot instead of being dropped.
		if (a_icons.size() > count)
			std::memcpy(static_cast<IconInstance*>(mapped.pData) + (count - 1), &a_icons.back(), sizeof(IconInstance));
		a_ctx->Unmap(_iconVB.Get(), 0);

		SetCommonState(a_ctx, _vsIcon.Get(), _psIcon.Get(), _blendAlpha.Get());
		a_ctx->IASetInputLayout(_iconLayout.Get());
		const UINT stride = sizeof(IconInstance), offset = 0;
		a_ctx->IASetVertexBuffers(0, 1, _iconVB.GetAddressOf(), &stride, &offset);
		a_ctx->VSSetConstantBuffers(0, 1, _cbIcon.GetAddressOf());
		a_ctx->PSSetConstantBuffers(0, 1, _cbIcon.GetAddressOf());
		const IconCB cb{ { a_p.viewportW, a_p.viewportH, a_p.opacity, a_p.round ? 1.0f : 0.0f }, { a_p.centerPx.x, a_p.centerPx.y, a_p.sizePx * 0.5f, 0.0f },
			{ a_p.linearOut ? 1.0f : 0.0f, a_p.outScale, 0.0f, 0.0f } };
		Upload(a_ctx, _cbIcon.Get(), cb);
		a_ctx->PSSetShaderResources(0, 1, _atlasSRV.GetAddressOf());
		a_ctx->DrawInstanced(4, count, 0, 0);
		ID3D11ShaderResourceView* nullSRV = nullptr;
		a_ctx->PSSetShaderResources(0, 1, &nullSRV);
	}
}
