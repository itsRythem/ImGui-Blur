#pragma once

#include "imgui.h"

#include <d3d11.h>

namespace blur {

	bool setup(ID3D11Device* device, ID3D11DeviceContext* device_context);
	void destroy();

	void process(ImDrawList* draw_list, int iterations = 2, float offset = 2.0f, float noise = 0.0f);
	void render(ImDrawList* draw_list, const ImVec2 min, const ImVec2 max, ImU32 col, float rounding = 0.0f, ImDrawFlags draw_flags = 0);

	ImTextureID get_texture();

}