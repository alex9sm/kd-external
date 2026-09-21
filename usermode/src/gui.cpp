#include "gui.h"

#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <windows.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")

static ID3D11Device *g_device = nullptr;
static ID3D11DeviceContext *g_context = nullptr;
static IDXGISwapChain *g_swap_chain = nullptr;
static ID3D11RenderTargetView *g_rtv = nullptr;
static HWND g_hwnd = nullptr;
static int g_width = 0;
static int g_height = 0;

struct find_ctx {
	HWND candidates[16];
	int count;
};

static BOOL CALLBACK enum_cb(HWND hwnd, LPARAM lp) {
	char title[256];
	if (!GetWindowTextA(hwnd, title, sizeof(title))) return TRUE;
	if (strcmp(title, "Discord Overlay") != 0) return TRUE;

	auto* ctx = reinterpret_cast<find_ctx*>(lp);
	if (ctx->count < 16)
		ctx->candidates[ctx->count++] = hwnd;
	return TRUE;
}

static bool find_discord_overlay() {
	find_ctx ctx = {};
	EnumWindows(enum_cb, reinterpret_cast<LPARAM>(&ctx));
	printf("found %d window(s) titled \"Discord Overlay\"\n", ctx.count);

	if (ctx.count == 0) return false;

	if (ctx.count == 1) {
		g_hwnd = ctx.candidates[0];
	} else {
		HWND topmost = nullptr;
		int topmost_count = 0;
		for (int i = 0; i < ctx.count; i++) {
			LONG ex = GetWindowLong(ctx.candidates[i], GWL_EXSTYLE);
			if (ex & WS_EX_TOPMOST) {
				topmost = ctx.candidates[i];
				topmost_count++;
			}
		}
		if (topmost_count != 1) return false;
		g_hwnd = topmost;
	}

	RECT rc = {};
	if (!GetClientRect(g_hwnd, &rc)) return false;
	g_width = rc.right - rc.left;
	g_height = rc.bottom - rc.top;
	return true;
}

static bool create_device() {
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 1;
	sd.BufferDesc.Width = g_width;
	sd.BufferDesc.Height = g_height;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 0;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = g_hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	D3D_FEATURE_LEVEL fl;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(
			nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
			nullptr, 0, D3D11_SDK_VERSION,
			&sd, &g_swap_chain, &g_device, &fl, &g_context);
	if (FAILED(hr)) {
		printf("D3D11CreateDeviceAndSwapChain failed: 0x%08lX\n", hr);
		return false;
	}
	puts("d3d11 device created");

	ID3D11Texture2D* back_buf = nullptr;
	g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buf));
	g_device->CreateRenderTargetView(back_buf, nullptr, &g_rtv);
	back_buf->Release();
	return true;
}

static bool world_to_screen(const vec3& world, const view_matrix_t& vm,
							int screen_w, int screen_h, vec2& out) {
	float w = vm.m[3][0]*world.x + vm.m[3][1]*world.y + vm.m[3][2]*world.z + vm.m[3][3];
	if (w < 0.1f) return false;

	float x = vm.m[0][0]*world.x + vm.m[0][1]*world.y + vm.m[0][2]*world.z + vm.m[0][3];
	float y = vm.m[1][0]*world.x + vm.m[1][1]*world.y + vm.m[1][2]*world.z + vm.m[1][3];

	out.x = (screen_w / 2.0f) + (x / w) * (screen_w / 2.0f);
	out.y = (screen_h / 2.0f) - (y / w) * (screen_h / 2.0f);
	return true;
}

bool overlay::init() {
	if (!find_discord_overlay()) return false;
	if (!create_device()) return false;

	ImGui::CreateContext();
	ImGui_ImplWin32_Init(g_hwnd);
	ImGui_ImplDX11_Init(g_device, g_context);
	return true;
}

void overlay::run(SharedState& state) {
	MSG msg = {};
	while (state.running) {
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				state.running = false;
				return;
			}
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		std::vector<EnemyData> enemies;
		view_matrix_t vm;
		{
			std::lock_guard<std::mutex> lock(state.mtx);
			enemies = state.enemies;
			vm = state.view_matrix;
		}

		ImDrawList* draw = ImGui::GetBackgroundDrawList();

		char dbg[64];
		snprintf(dbg, sizeof(dbg), "enemies: %d", (int)enemies.size());
		draw->AddText(ImVec2(10, 10), IM_COL32(0, 255, 0, 255), dbg);

		for (const auto& e : enemies) {
			vec2 feet_scr, head_scr;
			if (!world_to_screen(e.feet, vm, g_width, g_height, feet_scr)) continue;
			if (!world_to_screen(e.head, vm, g_width, g_height, head_scr)) continue;

			float box_h = feet_scr.y - head_scr.y;
			float box_w = box_h * 0.45f;

			ImVec2 tl(head_scr.x - box_w / 2, head_scr.y);
			ImVec2 br(head_scr.x + box_w / 2, feet_scr.y);
			draw->AddRect(tl, br, IM_COL32(255, 255, 255, 255), 0.0f, 0, 1.5f);
		}

		ImGui::Render();

		const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
		g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
		g_context->ClearRenderTargetView(g_rtv, clear);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_swap_chain->Present(1, 0);
	}
}

void overlay::shutdown() {
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	if (g_rtv)        g_rtv->Release();
	if (g_swap_chain) g_swap_chain->Release();
	if (g_context)    g_context->Release();
	if (g_device)     g_device->Release();
}
