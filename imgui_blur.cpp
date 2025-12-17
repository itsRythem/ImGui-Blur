#include "imgui_blur.h"
#include "imgui_impl_dx11.h"

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

#include <cstdint>

static const char* g_vertex_src = R"(
struct VS_INPUT {
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

PS_INPUT main(VS_INPUT input) {
    PS_INPUT output;
    output.pos = float4(input.pos, 0.0f, 1.0f);
    output.uv = input.uv;
    return output;
}
)";

static const char* g_downsample_src = R"(
cbuffer BlurConstants : register(b0) {
    float2 half_pixel;
    float offset;
    float noise;
};

Texture2D input_texture : register(t0);
SamplerState input_sampler : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_Target {
    float2 uv = input.uv;
    
    float4 sum = input_texture.Sample(input_sampler, uv) * 4.0;
    sum += input_texture.Sample(input_sampler, uv - half_pixel * offset);
    sum += input_texture.Sample(input_sampler, uv + half_pixel * offset);
    sum += input_texture.Sample(input_sampler, uv + float2(half_pixel.x, -half_pixel.y) * offset);
    sum += input_texture.Sample(input_sampler, uv - float2(half_pixel.x, -half_pixel.y) * offset);
    return sum / 8.0;
}
)";

static const char* g_upsample_src = R"(
cbuffer BlurConstants : register(b0) {
    float2 half_pixel;
    float offset;
    float noise;
};

Texture2D input_texture : register(t0);
SamplerState input_sampler : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_Target {
    float2 uv = input.uv;
    
    float4 sum = input_texture.Sample(input_sampler, uv) * 4.0;
    sum += input_texture.Sample(input_sampler, uv - half_pixel * offset);
    sum += input_texture.Sample(input_sampler, uv + half_pixel * offset);
    sum += input_texture.Sample(input_sampler, uv + float2(half_pixel.x, -half_pixel.y) * offset);
    sum += input_texture.Sample(input_sampler, uv - float2(half_pixel.x, -half_pixel.y) * offset);
    return sum / 8.0;
}
)";

class Framebuffer {
public:
    ~Framebuffer() {
        if (tex != nullptr) { tex->Release(); tex = nullptr; }
        if (rtv != nullptr) { rtv->Release(); rtv = nullptr; }
        if (srv != nullptr) { srv->Release(); srv = nullptr; }
    }

    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int width = 0, height = 0;
};

class BlurConstants {
public:
    ImVec2 half_pixel;
    float offset;
    float noise;
};

class BlurParameters {
public:
    int iterations;
    float offset;
    float noise;
};

static ID3D11PixelShader* g_downsample = nullptr;
static ID3D11PixelShader* g_upsample = nullptr;
static ID3D11VertexShader* g_vertex = nullptr;
static ID3D11InputLayout* g_input_layout = nullptr;
static ID3D11Buffer* g_constant_buffer = nullptr;
static ID3D11Buffer* g_vertex_buffer = nullptr;
static ID3D11SamplerState* g_linear_sampler = nullptr;
static ID3D11SamplerState* g_mirror_sampler = nullptr;
static ID3D11RasterizerState* g_rasterizer_state = nullptr;
static ID3D11DepthStencilState* g_depth_stencil_state = nullptr;
static ImVector<Framebuffer> g_framebuffers{};
static int g_last_iterations = 0;
static int g_last_width = 0;
static int g_last_height = 0;

bool blur::setup(ID3D11Device* device, ID3D11DeviceContext* device_context) {
    ID3DBlob* shader_blob = nullptr;
    ID3DBlob* error_blob = nullptr;

    if (FAILED(D3DCompile(g_vertex_src, strlen(g_vertex_src), nullptr, nullptr, nullptr,
        "main", "vs_5_0", 0, 0, &shader_blob, &error_blob)))
        return false;

    if (FAILED(device->CreateVertexShader(shader_blob->GetBufferPointer(), shader_blob->GetBufferSize(),
        nullptr, &g_vertex)))
        return false;

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    if (FAILED(device->CreateInputLayout(layout, 2, shader_blob->GetBufferPointer(),
        shader_blob->GetBufferSize(), &g_input_layout)))
        return false;

    if (FAILED(D3DCompile(g_downsample_src, strlen(g_downsample_src), nullptr, nullptr, nullptr,
        "main", "ps_5_0", 0, 0, &shader_blob, &error_blob)))
        return false;

    if (FAILED(device->CreatePixelShader(shader_blob->GetBufferPointer(), shader_blob->GetBufferSize(),
        nullptr, &g_downsample)))
        return false;

    if (FAILED(D3DCompile(g_upsample_src, strlen(g_upsample_src), nullptr, nullptr, nullptr,
        "main", "ps_5_0", 0, 0, &shader_blob, &error_blob)))
        return false;

    if (FAILED(device->CreatePixelShader(shader_blob->GetBufferPointer(), shader_blob->GetBufferSize(),
        nullptr, &g_upsample)))
        return false;

    if (shader_blob) shader_blob->Release();
    if (error_blob) error_blob->Release();

    struct Vertex {
        float pos[2];
        float uv[2];
    };

    Vertex vertices[] = {
        { { -1.0f,  1.0f }, { 0.0f, 0.0f } },
        { {  1.0f,  1.0f }, { 1.0f, 0.0f } },
        { { -1.0f, -1.0f }, { 0.0f, 1.0f } },
        { {  1.0f, -1.0f }, { 1.0f, 1.0f } },
    };

    D3D11_BUFFER_DESC vb_desc = {};
    vb_desc.Usage = D3D11_USAGE_DEFAULT;
    vb_desc.ByteWidth = sizeof(vertices);
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vb_data = {};
    vb_data.pSysMem = vertices;

    if (FAILED(device->CreateBuffer(&vb_desc, &vb_data, &g_vertex_buffer)))
        return false;

    D3D11_BUFFER_DESC cb_desc = {};
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.ByteWidth = sizeof(BlurConstants);
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(device->CreateBuffer(&cb_desc, nullptr, &g_constant_buffer)))
        return false;

    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    if (FAILED(device->CreateSamplerState(&sampler_desc, &g_linear_sampler)))
        return false;

    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_MIRROR;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_MIRROR;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;

    if (FAILED(device->CreateSamplerState(&sampler_desc, &g_mirror_sampler)))
        return false;

    D3D11_RASTERIZER_DESC raster_desc = {};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.ScissorEnable = FALSE;
    raster_desc.DepthClipEnable = FALSE;

    if (FAILED(device->CreateRasterizerState(&raster_desc, &g_rasterizer_state)))
        return false;

    D3D11_DEPTH_STENCIL_DESC depth_desc = {};
    depth_desc.DepthEnable = FALSE;
    depth_desc.StencilEnable = FALSE;

    return SUCCEEDED(device->CreateDepthStencilState(&depth_desc, &g_depth_stencil_state));
}

void blur::destroy() {
    if (g_downsample) { g_downsample->Release(); g_downsample = nullptr; }
    if (g_upsample) { g_upsample->Release(); g_upsample = nullptr; }
    if (g_vertex) { g_vertex->Release(); g_vertex = nullptr; }
    if (g_input_layout) { g_input_layout->Release(); g_input_layout = nullptr; }
    if (g_constant_buffer) { g_constant_buffer->Release(); g_constant_buffer = nullptr; }
    if (g_vertex_buffer) { g_vertex_buffer->Release(); g_vertex_buffer = nullptr; }
    if (g_linear_sampler) { g_linear_sampler->Release(); g_linear_sampler = nullptr; }
    if (g_mirror_sampler) { g_mirror_sampler->Release(); g_mirror_sampler = nullptr; }
    if (g_rasterizer_state) { g_rasterizer_state->Release(); g_rasterizer_state = nullptr; }
    if (g_depth_stencil_state) { g_depth_stencil_state->Release(); g_depth_stencil_state = nullptr; }
    g_framebuffers.clear_destruct();
}

static void render_fullscreen_quad(ID3D11DeviceContext* device_context) {
    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    device_context->IASetVertexBuffers(0, 1, &g_vertex_buffer, &stride, &offset);
    device_context->IASetInputLayout(g_input_layout);
    device_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    device_context->Draw(4, 0);
}

static void render_shader_pass(ID3D11DeviceContext* device_context, const Framebuffer& framebuffer, ID3D11ShaderResourceView* input_srv, ID3D11PixelShader* shader, float offset, float noise) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    device_context->Map(g_constant_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);

    BlurConstants* constants = (BlurConstants*)mapped.pData;
    constants->half_pixel = ImVec2(1.0f / framebuffer.width, 1.0f / framebuffer.height);
    constants->offset = offset;
    constants->noise = noise;

    device_context->Unmap(g_constant_buffer, 0);

    float clear_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    device_context->ClearRenderTargetView(framebuffer.rtv, clear_color);
    device_context->OMSetRenderTargets(1, &framebuffer.rtv, nullptr);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = (float)framebuffer.width;
    viewport.Height = (float)framebuffer.height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    device_context->RSSetViewports(1, &viewport);

    device_context->PSSetShader(shader, nullptr, 0);
    device_context->PSSetConstantBuffers(0, 1, &g_constant_buffer);
    device_context->PSSetShaderResources(0, 1, &input_srv);

    device_context->PSSetSamplers(0, 1, &g_mirror_sampler);

    render_fullscreen_quad(device_context);

    ID3D11ShaderResourceView* null_srv[2] = { nullptr, nullptr };
    device_context->PSSetShaderResources(0, 2, null_srv);
}

static bool create_framebuffer(ID3D11Device* device, Framebuffer& framebuffer, int width, int height) {
    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = width;
    tex_desc.Height = height;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    
    framebuffer.width = width;
    framebuffer.height = height;

    if (FAILED(device->CreateTexture2D(&tex_desc, nullptr, &framebuffer.tex)))
        return false;

    if (FAILED(device->CreateRenderTargetView(framebuffer.tex, nullptr, &framebuffer.rtv)))
        return false;

    return SUCCEEDED(device->CreateShaderResourceView(framebuffer.tex, nullptr, &framebuffer.srv));
}

static void post_process_callback(const ImDrawList*, const ImDrawCmd* cmd) {
    BlurParameters* blur_parameters = reinterpret_cast<BlurParameters*>(cmd->UserCallbackData);
    if (!blur_parameters)
        return;

    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    ImGui_ImplDX11_RenderState* render_state = (ImGui_ImplDX11_RenderState*)platform_io.Renderer_RenderState;
    ID3D11Device* device = render_state->Device;
    ID3D11DeviceContext* device_context = render_state->DeviceContext;

    ID3D11RenderTargetView* screen_rtv = nullptr;
    device_context->OMGetRenderTargets(1, &screen_rtv, nullptr);

    ID3D11Texture2D* screen_tex = nullptr;
    screen_rtv->GetResource(reinterpret_cast<ID3D11Resource**>(&screen_tex));

    D3D11_TEXTURE2D_DESC tex_desc;
    screen_tex->GetDesc(&tex_desc);

    const int width = tex_desc.Width;
    const int height = tex_desc.Height;
    if (g_last_iterations != blur_parameters->iterations || g_last_width != width || g_last_height != height) {
        g_framebuffers.clear_destruct();
        g_framebuffers.resize(blur_parameters->iterations + 1);

        create_framebuffer(device, g_framebuffers[0], width, height);
        for (int i = 1; i <= blur_parameters->iterations; ++i) {
            create_framebuffer(
                device, g_framebuffers[i],
                width / (1 << i),
                height / (1 << i)
            );
        }

        g_last_iterations = blur_parameters->iterations;
        g_last_width = width;
        g_last_height = height;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = tex_desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    srv_desc.Texture2D.MostDetailedMip = 0;

    ID3D11ShaderResourceView* screen_srv = nullptr;
    device->CreateShaderResourceView(screen_tex, &srv_desc, &screen_srv);

    screen_tex->Release();
    screen_rtv->Release();

    D3D11_VIEWPORT old_viewport;
    UINT num_viewports = 1;
    device_context->RSGetViewports(&num_viewports, &old_viewport);

    ID3D11RenderTargetView* old_rtv = nullptr;
    ID3D11DepthStencilView* old_dsv = nullptr;
    device_context->OMGetRenderTargets(1, &old_rtv, &old_dsv);

    ID3D11RasterizerState* old_rasterizer_state = nullptr;
    device_context->RSGetState(&old_rasterizer_state);

    ID3D11DepthStencilState* old_depth_stencil_state;
    UINT old_stencil_ref;
    device_context->OMGetDepthStencilState(&old_depth_stencil_state, &old_stencil_ref);

    device_context->VSSetShader(g_vertex, nullptr, 0);
    device_context->RSSetState(g_rasterizer_state);
    device_context->OMSetDepthStencilState(g_depth_stencil_state, 0);

    render_shader_pass(device_context, g_framebuffers[1], screen_srv, g_downsample, blur_parameters->offset, blur_parameters->noise);
    
    for (int i = 1; i < blur_parameters->iterations; ++i)
        render_shader_pass(device_context, g_framebuffers[i + 1], g_framebuffers[i].srv, g_downsample, blur_parameters->offset, blur_parameters->noise);

    for (int i = blur_parameters->iterations; i > 1; --i)
        render_shader_pass(device_context, g_framebuffers[i - 1], g_framebuffers[i].srv, g_upsample, blur_parameters->offset, blur_parameters->noise);

    render_shader_pass(device_context, g_framebuffers[0], g_framebuffers[1].srv, g_upsample, blur_parameters->offset, blur_parameters->noise);

    device_context->RSSetViewports(1, &old_viewport);
    device_context->OMSetRenderTargets(1, &old_rtv, old_dsv);
    device_context->RSSetState(old_rasterizer_state);
    device_context->OMSetDepthStencilState(old_depth_stencil_state, old_stencil_ref);

    screen_srv->Release();
}

void blur::process(ImDrawList* draw_list, int iterations, float offset, float noise) {
    BlurParameters params = { iterations, offset, noise };
    draw_list->AddCallback(post_process_callback, IM_NEW(BlurParameters(params)));
    draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    draw_list->AddCallback(
        [](const ImDrawList*, const ImDrawCmd* cmd) {
            IM_DELETE((BlurParameters*)cmd->UserCallbackData);
        },
        nullptr
    );
}

void blur::render(ImDrawList* draw_list, const ImVec2 min, const ImVec2 max, ImU32 col, float rounding, ImDrawFlags draw_flags) {
    ImGuiIO& io = ImGui::GetIO();

    draw_list->AddImageRounded(blur::get_texture(), min, max,
        { min.x / io.DisplaySize.x, min.y / io.DisplaySize.y },
        { max.x / io.DisplaySize.x, max.y / io.DisplaySize.y },
        col, rounding, draw_flags);
}

ImTextureID blur::get_texture() {
    return (ImTextureID)(g_framebuffers.empty() ? nullptr : g_framebuffers[0].srv);
}

