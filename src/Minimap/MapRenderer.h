#pragma once

// D3D11 drawing for the minimap. Plain D3D11 (no game types) so the offline test tool runs the
// exact same code on its own device.

#include "Icons.h"
#include "MapMath.h"
#include "InteriorMap.h"

#include <d3d11.h>
#include <span>
#include <string>
#include <wrl/client.h>

namespace MM
{
	using Microsoft::WRL::ComPtr;

	struct TileDraw
	{
		TileId                    id;
		ID3D11ShaderResourceView* diffuse{ nullptr };
		ID3D11ShaderResourceView* normal{ nullptr };  // optional
		ID3D11ShaderResourceView* water{ nullptr };   // optional
	};

	struct ComposeParams
	{
		Vec2        center;           // world point at the target centre
		float       unitsPerPixel{ 32.0f };
		int         targetSize{ 512 };
		ShadeParams shade;
	};

	// Interior map: navmesh floor and walls, centred on the player like the tile compose.
	struct NavComposeParams
	{
		Vec2  center;                 // world point at the target centre
		float unitsPerPixel{ 16.0f };
		int   targetSize{ 512 };
		float playerZ{ 0 };
		float floorBelow{ 120.0f };   // player's floor: playerZ - below .. playerZ + above
		float floorAbove{ 220.0f };
		float otherFloorAlpha{ 0 };   // other floors: 0 = hidden
		float wallWidthPx{ 2.0f };
	};

	struct OverlayParams
	{
		float left{ 0 }, top{ 0 }, size{ 256 };  // minimap square in viewport px
		float viewportW{ 1920 }, viewportH{ 1080 };
		float mapAngle{ 0 };    // radians clockwise from north the map is turned to (0 = north up)
		float arrowAngle{ 0 };  // radians clockwise from screen up
		Vec2  offsetPx;         // player minus compose centre, in target px (x east, y north)
		float opacity{ 1 };
		float arrowSize{ 10 };  // 0 = no arrow here (the plugin draws it as the last icon)
		bool  round{ true };
		bool  hasMap{ true };
		bool  linearOut{ false };  // HUD target is sRGB-typed or float: write linear colour
		float outScale{ 1.0f };
	};

	struct IconPassParams
	{
		float viewportW{ 1920 }, viewportH{ 1080 };
		Vec2  centerPx;     // minimap centre, viewport px
		float sizePx{ 256 };
		bool  round{ true };
		float opacity{ 1 };
		bool  linearOut{ false };
		float outScale{ 1.0f };
	};

	// Everything the passes touch, saved before and restored after (same set ImGui's DX11 backend keeps).
	struct D3DStateBlock
	{
		static constexpr UINT kMaxInstances = 256;

		UINT                      scissorCount{ 0 }, viewportCount{ 0 };
		D3D11_RECT                scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		D3D11_VIEWPORT            viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		ID3D11RasterizerState*    rs{ nullptr };
		ID3D11BlendState*         blend{ nullptr };
		FLOAT                     blendFactor[4]{};
		UINT                      sampleMask{ 0 };
		ID3D11DepthStencilState*  dss{ nullptr };
		UINT                      stencilRef{ 0 };
		ID3D11ShaderResourceView* psSRV[3]{};
		ID3D11SamplerState*       psSampler{ nullptr };
		ID3D11PixelShader*        ps{ nullptr };
		ID3D11VertexShader*       vs{ nullptr };
		ID3D11GeometryShader*     gs{ nullptr };
		ID3D11HullShader*         hs{ nullptr };
		ID3D11DomainShader*       ds{ nullptr };
		UINT                      psInstCount{ 0 }, vsInstCount{ 0 }, gsInstCount{ 0 }, hsInstCount{ 0 }, dsInstCount{ 0 };
		ID3D11ClassInstance*      psInst[kMaxInstances]{}, *vsInst[kMaxInstances]{}, *gsInst[kMaxInstances]{}, *hsInst[kMaxInstances]{}, *dsInst[kMaxInstances]{};
		ID3D11Buffer*             vsCB{ nullptr };
		ID3D11Buffer*             psCB{ nullptr };
		D3D11_PRIMITIVE_TOPOLOGY  topology{};
		ID3D11Buffer*             ib{ nullptr };
		DXGI_FORMAT               ibFormat{};
		UINT                      ibOffset{ 0 };
		ID3D11Buffer*             vb{ nullptr };
		UINT                      vbStride{ 0 }, vbOffset{ 0 };
		ID3D11InputLayout*        layout{ nullptr };
		ID3D11RenderTargetView*   rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView*   dsv{ nullptr };

		void Backup(ID3D11DeviceContext* a_ctx);
		void Restore(ID3D11DeviceContext* a_ctx);  // re-binds and releases everything Backup took
		void BindTargets(ID3D11DeviceContext* a_ctx) const;  // only the saved render targets + viewports
	};

	class MapRenderer
	{
	public:
		bool Init(ID3D11Device* a_device, std::string& a_error);
		[[nodiscard]] bool Ready() const noexcept { return _psOverlay != nullptr; }

		// Draws the given tiles into the compose target (resized as needed). Leaves the target bound.
		void Compose(ID3D11DeviceContext* a_ctx, const ComposeParams& a_params, std::span<const TileDraw> a_tiles);

		// Interior map pipeline. Its failure leaves the rest working (interiors then show the plain radar disc).
		[[nodiscard]] bool               NavReady() const noexcept { return _psNav != nullptr; }
		[[nodiscard]] const std::string& NavError() const noexcept { return _navError; }

		// Uploads a navmesh (immutable buffers) unless this generation is already loaded. False = nothing to draw.
		bool SetNavMesh(const NavMeshData& a_mesh);
		[[nodiscard]] bool HasNavMesh(std::uint32_t a_generation) const noexcept { return _navTriCount != 0 && _navGen == a_generation; }

		// Draws the loaded navmesh into the compose target (resized as needed). Leaves the target bound.
		void ComposeNav(ID3D11DeviceContext* a_ctx, const NavComposeParams& a_params);

		// Draws the minimap into whatever render target is bound now.
		void DrawOverlay(ID3D11DeviceContext* a_ctx, const OverlayParams& a_params);

		// All icons in one instanced draw into whatever render target is bound now (call after DrawOverlay).
		void DrawIcons(ID3D11DeviceContext* a_ctx, const IconPassParams& a_params, std::span<const IconInstance> a_icons);

		static constexpr UINT kMaxIcons = 1024;

		[[nodiscard]] ID3D11Texture2D* AtlasTexture() const noexcept { return _atlas.Get(); }
		[[nodiscard]] ID3D11Texture2D* ComposeTexture() const noexcept { return _target.Get(); }
		[[nodiscard]] int              ComposeSize() const noexcept { return _targetSize; }

	private:
		bool EnsureTarget(int a_size);
		void SetCommonState(ID3D11DeviceContext* a_ctx, ID3D11VertexShader* a_vs, ID3D11PixelShader* a_ps, ID3D11BlendState* a_blend);

		ComPtr<ID3D11Device>             _device;
		bool InitIcons(ID3D11Device* a_device, std::string& a_error);
		bool InitNav(ID3D11Device* a_device, std::string& a_error);
		bool BeginCompose(ID3D11DeviceContext* a_ctx, int a_size, const float (&a_clear)[3]);

		ComPtr<ID3D11VertexShader>       _vsTile, _vsOverlay, _vsIcon;
		ComPtr<ID3D11PixelShader>        _psTile, _psOverlay, _psIcon;
		ComPtr<ID3D11Buffer>             _cbTile, _cbOverlay, _cbIcon;
		ComPtr<ID3D11InputLayout>        _iconLayout;
		ComPtr<ID3D11Buffer>             _iconVB;
		ComPtr<ID3D11Texture2D>          _atlas;
		ComPtr<ID3D11ShaderResourceView> _atlasSRV;
		ComPtr<ID3D11SamplerState>       _sampler;
		ComPtr<ID3D11BlendState>         _blendOpaque, _blendAlpha;
		ComPtr<ID3D11RasterizerState>    _raster;
		ComPtr<ID3D11DepthStencilState>  _depthOff;
		ComPtr<ID3D11Texture2D>          _target;
		ComPtr<ID3D11RenderTargetView>   _targetRTV;
		ComPtr<ID3D11ShaderResourceView> _targetSRV;
		int                              _targetSize{ 0 };

		ComPtr<ID3D11VertexShader>       _vsNav, _vsNavWall;
		ComPtr<ID3D11PixelShader>        _psNav;
		ComPtr<ID3D11InputLayout>        _navLayout, _navWallLayout;
		ComPtr<ID3D11Buffer>             _cbNav;
		ComPtr<ID3D11Buffer>             _navVB, _navIB, _navWallVB, _navWallIB;
		UINT                             _navTriCount{ 0 }, _navWallCount{ 0 };  // index counts
		std::uint32_t                    _navGen{ 0 };
		std::string                      _navError;
	};
}
