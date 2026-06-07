#include "gp32_win64_video.h"
#include "gp32emu/video_effects.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef COBJMACROS
#define COBJMACROS 1
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GP32_LCD_W 320u
#define GP32_LCD_H 240u
#define GP32_RAW_LCD_W 240u
#define GP32_RAW_LCD_H 320u

typedef HRESULT (WINAPI *gp32_d3dcompile_fn)(LPCVOID src_data, SIZE_T src_size, LPCSTR source_name,
                                            const D3D_SHADER_MACRO *defines, ID3DInclude *include,
                                            LPCSTR entrypoint, LPCSTR target, UINT flags1, UINT flags2,
                                            ID3DBlob **code, ID3DBlob **error_msgs);

typedef struct gp32_d3d_vertex { float x, y, u, v; } gp32_d3d_vertex_t;

struct gp32_win64_video {
    HWND hwnd;
    unsigned window_w;
    unsigned window_h;
    unsigned scale;
    int integer_scaling;
    int keep_aspect;
    int fullscreen;
    int requested_d3d11;
    int d3d11_available;
    char backend_name[32];
    BITMAPINFO bmi;
    HDC gdi_mem_dc;
    HBITMAP gdi_bitmap;
    HGDIOBJ gdi_old_bitmap;
    void *gdi_bits;
    unsigned gdi_w;
    unsigned gdi_h;
    uint32_t *scratch;
    uint32_t *effect_pixels;
    size_t scratch_cap;
    size_t effect_cap;
    gp32_video_effects_t effects;
    HMODULE d3dcompiler;
    gp32_d3dcompile_fn d3d_compile;
    ID3D11Device *d3d_device;
    ID3D11DeviceContext *d3d_context;
    IDXGISwapChain *swap_chain;
    ID3D11RenderTargetView *rtv;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *input_layout;
    ID3D11Buffer *vertex_buffer;
    ID3D11Texture2D *lcd_texture;
    ID3D11ShaderResourceView *lcd_srv;
    ID3D11SamplerState *sampler;
};

static int str_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static uint32_t force_rgb(uint32_t p) {
    return p & 0x00ffffffu;
}


static const uint32_t *finalize_frame(gp32_win64_video_t *v) {
    if (!v || !v->scratch) return NULL;
    uint32_t *out = v->scratch;
    if (gp32_video_effects_active(&v->effects)) {
        const size_t need = (size_t)GP32_LCD_W * GP32_LCD_H;
        if (v->effect_cap < need) {
            uint32_t *p = (uint32_t *)realloc(v->effect_pixels, need * sizeof(uint32_t));
            if (!p) return NULL;
            v->effect_pixels = p;
            v->effect_cap = need;
        }
        if (!gp32_video_effects_process_320x240(&v->effects, v->scratch, v->effect_pixels)) return NULL;
        out = v->effect_pixels;
    }
    for (unsigned i = 0; i < GP32_LCD_W * GP32_LCD_H; ++i) out[i] = 0xff000000u | (out[i] & 0x00ffffffu);
    return out;
}

static const uint32_t *stage_frame(gp32_win64_video_t *v, const gp32_framebuffer_desc_t *fb) {
    if (!v || !fb || !fb->pixels_rgba8888 || fb->width == 0 || fb->height == 0 || fb->stride_pixels < fb->width) return NULL;

    /*
     * The core exposes the S3C2400 LCD framebuffer in controller/native
     * portrait order for BIOS-driven software: 240x320.  The GP32 display
     * is physically landscape 320x240, and the SDL frontends rotate this
     * buffer 90 degrees counter-clockwise before presentation.  The first
     * Win64 frontend version assumed the API already returned 320x240 and
     * rejected the real 240x320 framebuffer, so both GDI and D3D11 presented
     * only their black clear/background.
     */
    size_t need = (size_t)GP32_LCD_W * GP32_LCD_H;
    if (v->scratch_cap < need) {
        uint32_t *p = (uint32_t *)realloc(v->scratch, need * sizeof(uint32_t));
        if (!p) return NULL;
        v->scratch = p;
        v->scratch_cap = need;
    }

    const uint32_t *src = fb->pixels_rgba8888;
    const uint32_t stride = fb->stride_pixels;

    if (fb->width == GP32_LCD_W && fb->height == GP32_LCD_H) {
        for (unsigned y = 0; y < GP32_LCD_H; ++y) {
            const uint32_t *row = src + (size_t)y * stride;
            uint32_t *dst = v->scratch + (size_t)y * GP32_LCD_W;
            for (unsigned x = 0; x < GP32_LCD_W; ++x) dst[x] = force_rgb(row[x]);
        }
        return finalize_frame(v);
    }

    if (fb->width == GP32_RAW_LCD_W && fb->height == GP32_RAW_LCD_H) {
        for (unsigned y = 0; y < GP32_LCD_H; ++y) {
            uint32_t sx = GP32_RAW_LCD_W - 1u - y;
            uint32_t *dst = v->scratch + (size_t)y * GP32_LCD_W;
            for (unsigned x = 0; x < GP32_LCD_W; ++x) dst[x] = force_rgb(src[(size_t)x * stride + sx]);
        }
        return finalize_frame(v);
    }

    /* Generic fallback: scale/crop the source into the fixed GP32 landscape
       presenter instead of failing to black if a game uses unusual LCD regs. */
    for (unsigned y = 0; y < GP32_LCD_H; ++y) {
        uint32_t sy = (uint32_t)(((uint64_t)y * fb->height) / GP32_LCD_H);
        if (sy >= fb->height) sy = fb->height - 1u;
        uint32_t *dst = v->scratch + (size_t)y * GP32_LCD_W;
        const uint32_t *row = src + (size_t)sy * stride;
        for (unsigned x = 0; x < GP32_LCD_W; ++x) {
            uint32_t sx = (uint32_t)(((uint64_t)x * fb->width) / GP32_LCD_W);
            if (sx >= fb->width) sx = fb->width - 1u;
            dst[x] = force_rgb(row[sx]);
        }
    }
    return finalize_frame(v);
}

static void calc_dest(const gp32_win64_video_t *v, RECT *out) {
    RECT r;
    r.left = 0;
    r.top = 0;
    r.right = (LONG)(v ? v->window_w : GP32_LCD_W);
    r.bottom = (LONG)(v ? v->window_h : GP32_LCD_H);
    if (!v) { *out = r; return; }

    unsigned ww = v->window_w ? v->window_w : 1u;
    unsigned wh = v->window_h ? v->window_h : 1u;
    unsigned dw = ww;
    unsigned dh = wh;

    if (v->integer_scaling && !v->fullscreen) {
        unsigned sx = ww / GP32_LCD_W;
        unsigned sy = wh / GP32_LCD_H;
        unsigned scale = sx < sy ? sx : sy;
        if (!scale) scale = 1u;
        dw = GP32_LCD_W * scale;
        dh = GP32_LCD_H * scale;
    } else if (v->keep_aspect) {
        /* Prefer the GP32/Bios output exactly as a 4:3 image that uses the
           entire available vertical height.  On normal widescreen fullscreen
           desktops this produces pillarboxing, not vertical letterboxing.
           If the window is narrower than 4:3, fall back to fitting width so
           the image remains visible without cropping. */
        uint64_t width_for_full_height = ((uint64_t)wh * GP32_LCD_W + (GP32_LCD_H / 2u)) / GP32_LCD_H;
        if (width_for_full_height <= ww) {
            dw = (unsigned)width_for_full_height;
            dh = wh;
        } else {
            dw = ww;
            dh = (unsigned)(((uint64_t)ww * GP32_LCD_H + (GP32_LCD_W / 2u)) / GP32_LCD_W);
            if (dh > wh) dh = wh;
        }
        if (!dw) dw = 1u;
        if (!dh) dh = 1u;
    } else {
        *out = r;
        return;
    }

    r.left = (LONG)((ww > dw) ? ((ww - dw) / 2u) : 0u);
    r.top = (LONG)((wh > dh) ? ((wh - dh) / 2u) : 0u);
    r.right = r.left + (LONG)dw;
    r.bottom = r.top + (LONG)dh;
    *out = r;
}

static const char *gp32_d3d_shader_source =
    "Texture2D lcdTex : register(t0);\n"
    "SamplerState lcdSampler : register(s0);\n"
    "struct VSIn { float2 pos : POSITION; float2 tex : TEXCOORD0; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float2 tex : TEXCOORD0; };\n"
    "VSOut vs_main(VSIn input) { VSOut output; output.pos = float4(input.pos, 0.0f, 1.0f); output.tex = input.tex; return output; }\n"
    "float4 ps_main(VSOut input) : SV_Target { return lcdTex.Sample(lcdSampler, input.tex); }\n";

static int d3d11_load_compiler(gp32_win64_video_t *v) {
    static const char *dlls[] = { "d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll", "d3dcompiler_42.dll" };
    for (size_t i = 0; i < sizeof(dlls) / sizeof(dlls[0]); ++i) {
        HMODULE mod = LoadLibraryA(dlls[i]);
        if (!mod) continue;
        v->d3d_compile = (gp32_d3dcompile_fn)GetProcAddress(mod, "D3DCompile");
        if (v->d3d_compile) { v->d3dcompiler = mod; return 1; }
        FreeLibrary(mod);
    }
    return 0;
}

static void d3d11_release_pipeline(gp32_win64_video_t *v) {
    if (!v) return;
    if (v->d3d_context) {
        ID3D11ShaderResourceView *null_srv = NULL;
        ID3D11DeviceContext_PSSetShaderResources(v->d3d_context, 0, 1, &null_srv);
        ID3D11DeviceContext_ClearState(v->d3d_context);
    }
    if (v->sampler) ID3D11SamplerState_Release(v->sampler);
    if (v->lcd_srv) ID3D11ShaderResourceView_Release(v->lcd_srv);
    if (v->lcd_texture) ID3D11Texture2D_Release(v->lcd_texture);
    if (v->vertex_buffer) ID3D11Buffer_Release(v->vertex_buffer);
    if (v->input_layout) ID3D11InputLayout_Release(v->input_layout);
    if (v->ps) ID3D11PixelShader_Release(v->ps);
    if (v->vs) ID3D11VertexShader_Release(v->vs);
    v->sampler = NULL; v->lcd_srv = NULL; v->lcd_texture = NULL; v->vertex_buffer = NULL; v->input_layout = NULL; v->ps = NULL; v->vs = NULL;
}

static int d3d11_compile_shader(gp32_win64_video_t *v, const char *entry, const char *target, ID3DBlob **out_blob) {
    ID3DBlob *blob = NULL, *errors = NULL;
    if (!v || !v->d3d_compile || !out_blob) return 0;
    *out_blob = NULL;
    HRESULT hr = v->d3d_compile(gp32_d3d_shader_source, strlen(gp32_d3d_shader_source), "gp32_win64_video.hlsl", NULL, NULL, entry, target, 0, 0, &blob, &errors);
    if (errors) { const char *msg = (const char *)ID3D10Blob_GetBufferPointer(errors); if (msg && *msg) OutputDebugStringA(msg); ID3D10Blob_Release(errors); }
    if (FAILED(hr) || !blob) { if (blob) ID3D10Blob_Release(blob); return 0; }
    *out_blob = blob;
    return 1;
}

static int d3d11_create_rtv(gp32_win64_video_t *v) {
    ID3D11Texture2D *back = NULL;
    HRESULT hr = IDXGISwapChain_GetBuffer(v->swap_chain, 0, &IID_ID3D11Texture2D, (void **)&back);
    if (FAILED(hr) || !back) return 0;
    hr = ID3D11Device_CreateRenderTargetView(v->d3d_device, (ID3D11Resource *)back, NULL, &v->rtv);
    ID3D11Texture2D_Release(back);
    return SUCCEEDED(hr) && v->rtv;
}

static int d3d11_create_pipeline(gp32_win64_video_t *v) {
    ID3DBlob *vs_blob = NULL, *ps_blob = NULL;
    if (!d3d11_load_compiler(v)) return 0;
    if (!d3d11_compile_shader(v, "vs_main", "vs_4_0", &vs_blob)) return 0;
    if (!d3d11_compile_shader(v, "ps_main", "ps_4_0", &ps_blob)) { ID3D10Blob_Release(vs_blob); return 0; }
    HRESULT hr = ID3D11Device_CreateVertexShader(v->d3d_device, ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), NULL, &v->vs);
    if (FAILED(hr)) goto fail;
    hr = ID3D11Device_CreatePixelShader(v->d3d_device, ID3D10Blob_GetBufferPointer(ps_blob), ID3D10Blob_GetBufferSize(ps_blob), NULL, &v->ps);
    if (FAILED(hr)) goto fail;
    D3D11_INPUT_ELEMENT_DESC il[2]; memset(il, 0, sizeof(il));
    il[0].SemanticName = "POSITION"; il[0].Format = DXGI_FORMAT_R32G32_FLOAT; il[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    il[1].SemanticName = "TEXCOORD"; il[1].Format = DXGI_FORMAT_R32G32_FLOAT; il[1].AlignedByteOffset = 8; il[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    hr = ID3D11Device_CreateInputLayout(v->d3d_device, il, 2, ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), &v->input_layout);
    if (FAILED(hr)) goto fail;
    D3D11_BUFFER_DESC vb_desc; memset(&vb_desc, 0, sizeof(vb_desc));
    vb_desc.ByteWidth = (UINT)(sizeof(gp32_d3d_vertex_t) * 4u); vb_desc.Usage = D3D11_USAGE_DYNAMIC; vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER; vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(v->d3d_device, &vb_desc, NULL, &v->vertex_buffer);
    if (FAILED(hr)) goto fail;
    D3D11_TEXTURE2D_DESC tex_desc; memset(&tex_desc, 0, sizeof(tex_desc));
    tex_desc.Width = GP32_LCD_W; tex_desc.Height = GP32_LCD_H; tex_desc.MipLevels = 1; tex_desc.ArraySize = 1; tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; tex_desc.SampleDesc.Count = 1; tex_desc.Usage = D3D11_USAGE_DYNAMIC; tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateTexture2D(v->d3d_device, &tex_desc, NULL, &v->lcd_texture);
    if (FAILED(hr)) goto fail;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc; memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = tex_desc.Format; srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srv_desc.Texture2D.MipLevels = 1;
    hr = ID3D11Device_CreateShaderResourceView(v->d3d_device, (ID3D11Resource *)v->lcd_texture, &srv_desc, &v->lcd_srv);
    if (FAILED(hr)) goto fail;
    D3D11_SAMPLER_DESC samp_desc; memset(&samp_desc, 0, sizeof(samp_desc));
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP; samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP; samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; samp_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = ID3D11Device_CreateSamplerState(v->d3d_device, &samp_desc, &v->sampler);
    if (FAILED(hr)) goto fail;
    ID3D10Blob_Release(vs_blob); ID3D10Blob_Release(ps_blob); return 1;
fail:
    if (vs_blob) ID3D10Blob_Release(vs_blob); if (ps_blob) ID3D10Blob_Release(ps_blob); d3d11_release_pipeline(v); return 0;
}

static int d3d11_init(gp32_win64_video_t *v) {
    RECT rc; GetClientRect(v->hwnd, &rc);
    DXGI_SWAP_CHAIN_DESC sd; memset(&sd, 0, sizeof(sd));
    sd.BufferCount = 2; sd.BufferDesc.Width = (UINT)((rc.right > rc.left) ? rc.right - rc.left : 1); sd.BufferDesc.Height = (UINT)((rc.bottom > rc.top) ? rc.bottom - rc.top : 1); sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = v->hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 }, got;
    D3D_DRIVER_TYPE drivers[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
    HRESULT hr = E_FAIL;
    for (size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]); ++i) {
        hr = D3D11CreateDeviceAndSwapChain(NULL, drivers[i], NULL, 0, levels, (UINT)(sizeof(levels)/sizeof(levels[0])), D3D11_SDK_VERSION, &sd, &v->swap_chain, &v->d3d_device, &got, &v->d3d_context);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) return 0;
    if (!d3d11_create_rtv(v) || !d3d11_create_pipeline(v)) return 0;
    v->d3d11_available = 1;
    return 1;
}

static void d3d11_destroy(gp32_win64_video_t *v) {
    if (!v) return;
    d3d11_release_pipeline(v);
    if (v->rtv) ID3D11RenderTargetView_Release(v->rtv);
    if (v->swap_chain) IDXGISwapChain_Release(v->swap_chain);
    if (v->d3d_context) ID3D11DeviceContext_Release(v->d3d_context);
    if (v->d3d_device) ID3D11Device_Release(v->d3d_device);
    if (v->d3dcompiler) FreeLibrary(v->d3dcompiler);
    v->rtv = NULL; v->swap_chain = NULL; v->d3d_context = NULL; v->d3d_device = NULL; v->d3dcompiler = NULL; v->d3d_compile = NULL; v->d3d11_available = 0;
}

static void d3d11_resize(gp32_win64_video_t *v, unsigned w, unsigned h) {
    if (!v || !v->swap_chain) return;
    if (v->rtv) { ID3D11RenderTargetView_Release(v->rtv); v->rtv = NULL; }
    if (SUCCEEDED(IDXGISwapChain_ResizeBuffers(v->swap_chain, 0, (UINT)(w ? w : 1u), (UINT)(h ? h : 1u), DXGI_FORMAT_UNKNOWN, 0))) (void)d3d11_create_rtv(v);
}

static int d3d11_update_texture(gp32_win64_video_t *v, const uint32_t *fb) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ID3D11DeviceContext_Map(v->d3d_context, (ID3D11Resource *)v->lcd_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return 0;
    for (unsigned y = 0; y < GP32_LCD_H; ++y) memcpy((uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch, fb + (size_t)y * GP32_LCD_W, GP32_LCD_W * sizeof(uint32_t));
    ID3D11DeviceContext_Unmap(v->d3d_context, (ID3D11Resource *)v->lcd_texture, 0);
    return 1;
}

static int d3d11_update_vertices(gp32_win64_video_t *v) {
    RECT dst; calc_dest(v, &dst);
    float ww = (float)(v->window_w ? v->window_w : 1u), wh = (float)(v->window_h ? v->window_h : 1u);
    float l = ((float)dst.left / ww) * 2.0f - 1.0f, r = ((float)dst.right / ww) * 2.0f - 1.0f;
    float t = 1.0f - ((float)dst.top / wh) * 2.0f, b = 1.0f - ((float)dst.bottom / wh) * 2.0f;
    gp32_d3d_vertex_t verts[4] = {{l,t,0,0},{r,t,1,0},{l,b,0,1},{r,b,1,1}};
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ID3D11DeviceContext_Map(v->d3d_context, (ID3D11Resource *)v->vertex_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return 0;
    memcpy(mapped.pData, verts, sizeof(verts));
    ID3D11DeviceContext_Unmap(v->d3d_context, (ID3D11Resource *)v->vertex_buffer, 0);
    return 1;
}

static int d3d11_present(gp32_win64_video_t *v, const uint32_t *fb) {
    if (!v || !v->d3d11_available || !fb) return 0;
    if (!d3d11_update_texture(v, fb) || !d3d11_update_vertices(v)) return 0;
    FLOAT clear[4] = {0,0,0,1};
    D3D11_VIEWPORT vp; memset(&vp, 0, sizeof(vp));
    vp.Width = (float)(v->window_w ? v->window_w : 1u); vp.Height = (float)(v->window_h ? v->window_h : 1u); vp.MaxDepth = 1.0f;
    UINT stride = sizeof(gp32_d3d_vertex_t), offset = 0;
    ID3D11ShaderResourceView *null_srv = NULL;
    ID3D11DeviceContext_OMSetRenderTargets(v->d3d_context, 1, &v->rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(v->d3d_context, 1, &vp);
    ID3D11DeviceContext_ClearRenderTargetView(v->d3d_context, v->rtv, clear);
    ID3D11DeviceContext_IASetInputLayout(v->d3d_context, v->input_layout);
    ID3D11DeviceContext_IASetVertexBuffers(v->d3d_context, 0, 1, &v->vertex_buffer, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(v->d3d_context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(v->d3d_context, v->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(v->d3d_context, v->ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(v->d3d_context, 0, 1, &v->lcd_srv);
    ID3D11DeviceContext_PSSetSamplers(v->d3d_context, 0, 1, &v->sampler);
    ID3D11DeviceContext_Draw(v->d3d_context, 4, 0);
    ID3D11DeviceContext_PSSetShaderResources(v->d3d_context, 0, 1, &null_srv);
    return SUCCEEDED(IDXGISwapChain_Present(v->swap_chain, 0, 0));
}

static void gdi_destroy_backbuffer(gp32_win64_video_t *v) {
    if (!v) return;
    if (v->gdi_mem_dc && v->gdi_old_bitmap) SelectObject(v->gdi_mem_dc, v->gdi_old_bitmap);
    if (v->gdi_bitmap) DeleteObject(v->gdi_bitmap);
    if (v->gdi_mem_dc) DeleteDC(v->gdi_mem_dc);
    v->gdi_mem_dc = NULL; v->gdi_bitmap = NULL; v->gdi_old_bitmap = NULL; v->gdi_bits = NULL; v->gdi_w = v->gdi_h = 0;
}

static int gdi_ensure_backbuffer(gp32_win64_video_t *v, unsigned w, unsigned h) {
    if (!v || !v->hwnd) return 0;
    if (!w) w = 1; if (!h) h = 1;
    if (v->gdi_mem_dc && v->gdi_bitmap && v->gdi_w == w && v->gdi_h == h) return 1;
    gdi_destroy_backbuffer(v);
    HDC wnd_dc = GetDC(v->hwnd); if (!wnd_dc) return 0;
    HDC mem_dc = CreateCompatibleDC(wnd_dc);
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = (LONG)w; bi.bmiHeader.biHeight = -(LONG)h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bitmap = CreateDIBSection(wnd_dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(v->hwnd, wnd_dc);
    if (!mem_dc || !bitmap || !bits) { if (bitmap) DeleteObject(bitmap); if (mem_dc) DeleteDC(mem_dc); return 0; }
    v->gdi_mem_dc = mem_dc; v->gdi_bitmap = bitmap; v->gdi_old_bitmap = SelectObject(mem_dc, bitmap); v->gdi_bits = bits; v->gdi_w = w; v->gdi_h = h;
    return 1;
}

static int gdi_present(gp32_win64_video_t *v, const uint32_t *fb) {
    RECT client; GetClientRect(v->hwnd, &client);
    if (client.right <= client.left || client.bottom <= client.top) return 0;
    unsigned w = (unsigned)(client.right - client.left), h = (unsigned)(client.bottom - client.top);
    if (!gdi_ensure_backbuffer(v, w, h)) return -1;
    RECT mem_rc = {0,0,(LONG)v->gdi_w,(LONG)v->gdi_h};
    FillRect(v->gdi_mem_dc, &mem_rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    RECT dst; calc_dest(v, &dst);
    SetStretchBltMode(v->gdi_mem_dc, COLORONCOLOR);
    StretchDIBits(v->gdi_mem_dc, dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top, 0, 0, (int)GP32_LCD_W, (int)GP32_LCD_H, fb, &v->bmi, DIB_RGB_COLORS, SRCCOPY);
    HDC dc = GetDC(v->hwnd); if (!dc) return -1;
    BitBlt(dc, 0, 0, (int)v->gdi_w, (int)v->gdi_h, v->gdi_mem_dc, 0, 0, SRCCOPY);
    ReleaseDC(v->hwnd, dc);
    return 0;
}

gp32_win64_video_t *gp32_win64_video_create(HWND hwnd, unsigned scale, int integer_scaling, int keep_aspect, int lcd_persistence, int frame_interpolation, const char *backend) {
    gp32_win64_video_t *v = (gp32_win64_video_t *)calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->hwnd = hwnd; v->scale = scale ? scale : 2u; v->integer_scaling = integer_scaling != 0; v->keep_aspect = keep_aspect != 0; v->requested_d3d11 = !backend || str_eq(backend, "d3d11");
    if (!gp32_video_effects_init(&v->effects)) { free(v); return NULL; }
    gp32_video_effects_set(&v->effects, lcd_persistence, frame_interpolation);
    memset(&v->bmi, 0, sizeof(v->bmi));
    v->bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); v->bmi.bmiHeader.biWidth = (LONG)GP32_LCD_W; v->bmi.bmiHeader.biHeight = -(LONG)GP32_LCD_H; v->bmi.bmiHeader.biPlanes = 1; v->bmi.bmiHeader.biBitCount = 32; v->bmi.bmiHeader.biCompression = BI_RGB;
    strcpy(v->backend_name, "gdi");
    if (v->requested_d3d11 && d3d11_init(v)) strcpy(v->backend_name, "d3d11");
    RECT rc; GetClientRect(hwnd, &rc); gp32_win64_video_resize(v, (unsigned)(rc.right - rc.left), (unsigned)(rc.bottom - rc.top));
    return v;
}

void gp32_win64_video_destroy(gp32_win64_video_t *v) {
    if (!v) return;
    d3d11_destroy(v); gdi_destroy_backbuffer(v); gp32_video_effects_shutdown(&v->effects); free(v->scratch); free(v->effect_pixels); free(v);
}

void gp32_win64_video_resize(gp32_win64_video_t *v, unsigned width, unsigned height) {
    if (!v) return;
    v->window_w = width ? width : 1u; v->window_h = height ? height : 1u;
    if (v->d3d11_available) d3d11_resize(v, v->window_w, v->window_h); else gdi_destroy_backbuffer(v);
}

int gp32_win64_video_present(gp32_win64_video_t *v, const gp32_framebuffer_desc_t *fb) {
    const uint32_t *pixels = stage_frame(v, fb);
    if (!pixels) return -1;
    if (v->d3d11_available && d3d11_present(v, pixels)) return 0;
    return gdi_present(v, pixels);
}

void gp32_win64_video_set_integer_scaling(gp32_win64_video_t *v, int enabled) { if (v) v->integer_scaling = enabled != 0; }
int gp32_win64_video_integer_scaling(const gp32_win64_video_t *v) { return v ? v->integer_scaling : 0; }
void gp32_win64_video_set_keep_aspect(gp32_win64_video_t *v, int enabled) { if (v) v->keep_aspect = enabled != 0; }
int gp32_win64_video_keep_aspect(const gp32_win64_video_t *v) { return v ? v->keep_aspect : 0; }
void gp32_win64_video_set_fullscreen(gp32_win64_video_t *v, int enabled) { if (v) v->fullscreen = enabled != 0; }
void gp32_win64_video_set_lcd_persistence(gp32_win64_video_t *v, int enabled) { if (v) gp32_video_effects_set(&v->effects, enabled, gp32_video_effects_frame_interpolation(&v->effects)); }
int gp32_win64_video_lcd_persistence(const gp32_win64_video_t *v) { return v ? gp32_video_effects_lcd_persistence(&v->effects) : 0; }
void gp32_win64_video_set_frame_interpolation(gp32_win64_video_t *v, int enabled) { if (v) gp32_video_effects_set(&v->effects, gp32_video_effects_lcd_persistence(&v->effects), enabled); }
int gp32_win64_video_frame_interpolation(const gp32_win64_video_t *v) { return v ? gp32_video_effects_frame_interpolation(&v->effects) : 0; }
const char *gp32_win64_video_active_backend(const gp32_win64_video_t *v) { return v ? v->backend_name : "none"; }
