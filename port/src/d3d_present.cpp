#define NOMINMAX
#include "d3d_present.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "gfx.hpp"

using Microsoft::WRL::ComPtr;

namespace amg {

namespace {

const double PI = 3.14159265358979323846;

// One triangle that covers the viewport; the picture is placed by the viewport and the scissor, so neither
// letterboxing nor a sprite needs geometry.
const char* SHADER = R"(
struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VOut vs(uint id : SV_VertexID) {
    VOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = p;
    return o;
}

// Presenting a finished frame: the window's letterbox.
Texture2D tex : register(t0);
SamplerState smp : register(s0);
// The player's brightness (a factor) and gamma (the exponent is 1 / gamma), applied last by every presenting shader.
cbuffer Tone : register(b1) { float4 tone; };
float3 finish(float3 c) { return pow(saturate(c * tone.x), tone.y); }
float4 ps(VOut i) : SV_Target { return float4(finish(tex.Sample(smp, i.uv).rgb), 1); }

// The same with "sharp bilinear": a texel keeps its own colour except along its edges, where the linear sampler blends it
// into the next one over about one screen pixel, so a scaled picture is neither blurry nor blocky. The scale (screen pixels
// per texel) comes from the derivative of the coordinates.
float4 ps_sharp(VOut i) : SV_Target {
    float2 size;
    tex.GetDimensions(size.x, size.y);
    float2 scale = 1.0 / (fwidth(i.uv) * size);
    float2 at = i.uv * size;
    float2 base = floor(at);
    float2 off = at - base - 0.5;
    float2 range = 0.5 - 0.5 / max(scale, 1.0);
    float2 f = (off - clamp(off, -range, range)) * max(scale, 1.0) + 0.5;
    return float4(finish(tex.Sample(smp, (base + f) / size).rgb), 1);
}

// Bicubic (Catmull-Rom): sixteen texels around the point, weighted by a cubic that goes through the texels, so the edges
// stay crisper than with the linear sampler without the blocks of the nearest one.
float4 catmull(float t) {
    float t2 = t * t, t3 = t2 * t;
    return float4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}
float4 ps_bicubic(VOut i) : SV_Target {
    float2 size;
    tex.GetDimensions(size.x, size.y);
    float2 at = i.uv * size - 0.5;
    float2 base = floor(at);
    float2 f = at - base;
    float4 wx = catmull(f.x), wy = catmull(f.y);
    float3 sum = 0;
    [unroll] for (int j = 0; j < 4; ++j)
        [unroll] for (int k = 0; k < 4; ++k) {
            int2 p = clamp(int2(base) + int2(k - 1, j - 1), int2(0, 0), int2(size) - 1);
            sum += tex.Load(int3(p, 0)).rgb * (wx[k] * wy[j]);
        }
    return float4(finish(sum), 1);
}

// Lanczos with two lobes: the same sixteen texels, the weights of sinc(x) * sinc(x / 2), normalised.
float lanczos2(float d) {
    d = abs(d);
    if (d < 0.0001) return 1.0;
    if (d >= 2.0) return 0.0;
    float a = 3.14159265 * d;
    return 2.0 * sin(a) * sin(a * 0.5) / (a * a);
}
float4 ps_lanczos(VOut i) : SV_Target {
    float2 size;
    tex.GetDimensions(size.x, size.y);
    float2 at = i.uv * size - 0.5;
    float2 base = floor(at);
    float2 f = at - base;
    float3 sum = 0;
    float total = 0;
    [unroll] for (int j = 0; j < 4; ++j)
        [unroll] for (int k = 0; k < 4; ++k) {
            float w = lanczos2(float(k - 1) - f.x) * lanczos2(float(j - 1) - f.y);
            int2 p = clamp(int2(base) + int2(k - 1, j - 1), int2(0, 0), int2(size) - 1);
            sum += tex.Load(int3(p, 0)).rgb * w;
            total += w;
        }
    return float4(finish(sum / total), 1);
}

// Sharpen: the linear sampler plus a contrast-adaptive sharpening pass (the idea of AMD's CAS): the four neighbours one
// screen pixel away, a weight that is smaller where the local contrast is already high, so edges gain bite without halos.
float4 ps_cas(VOut i) : SV_Target {
    float2 d = fwidth(i.uv);
    float3 c = tex.Sample(smp, i.uv).rgb;
    float3 n = tex.Sample(smp, i.uv + float2(0, -d.y)).rgb, s = tex.Sample(smp, i.uv + float2(0, d.y)).rgb;
    float3 e = tex.Sample(smp, i.uv + float2(d.x, 0)).rgb, w = tex.Sample(smp, i.uv + float2(-d.x, 0)).rgb;
    float3 mn = min(c, min(min(n, s), min(e, w))), mx = max(c, max(max(n, s), max(e, w)));
    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 0.0001)));
    float3 k = amp * -0.125;
    return float4(finish((k * (n + s + e + w) + c) / (1.0 + 4.0 * k)), 1);
}

// "Fit, blurred edges": under the picture, the same frame enlarged to cover the whole window and blurred, darkened. `tone.zw` is
// the part of the frame the window shows when it covers the window (the other two numbers of the constant buffer are the
// brightness and gamma).
float4 ps_blur(VOut i) : SV_Target {
    float2 uv = (i.uv - 0.5) * tone.zw + 0.5;
    float3 sum = 0;
    [unroll] for (int k = 0; k < 40; ++k) {
        float a = float(k) * 2.39996323;                       // the golden angle: an even spread of taps over a disc
        float r = 0.07 * sqrt((float(k) + 0.5) / 40.0);
        sum += tex.Sample(smp, uv + float2(cos(a), sin(a)) * r).rgb;
    }
    return float4(finish(sum / 40.0 * 0.55), 1);
}

// Drawing a DrawItem (the arithmetic of `draw()` in gfx.cpp, smooth path): the pixel's position is mapped back into
// the sprite's rectangle, four texels are read at the clamped coordinates and mixed bilinearly.
cbuffer Item : register(b0) {
    float4 geo;    // centre_x, centre_y, rw, rh
    float4 axes;   // ux, uy, vx, vy
    float4 origin; // rx, ry, plain (1: a rectangle of colour `light`), 0
    int4 lim;      // lo_u, hi_u, lo_v, hi_v
    float4 light;  // r, g, b, alpha
    float4 flake;  // snow: fx, fy, size, k
};
Texture2D sprite : register(t1);

float4 ps_item(VOut i) : SV_Target {
    float dx = i.pos.x - geo.x, dy = i.pos.y - geo.y;
    float u = geo.z * 0.5 + dx * axes.x + dy * axes.y;
    float v = geo.w * 0.5 + dx * axes.z + dy * axes.w;
    if (!(u >= 0 && u < geo.z && v >= 0 && v < geo.w)) discard;
    if (origin.z > 0.5) return float4(saturate(light.rgb), saturate(light.a));
    float su = origin.x + u - 0.5, sv = origin.y + v - 0.5;
    float fu = frac(su), fv = frac(sv);
    int u0 = (int)floor(su), v0 = (int)floor(sv);
    int ua = clamp(u0, lim.x, lim.y), ub = clamp(u0 + 1, lim.x, lim.y);
    int va = clamp(v0, lim.z, lim.w), vb = clamp(v0 + 1, lim.z, lim.w);
    float4 t00 = sprite.Load(int3(ua, va, 0)), t10 = sprite.Load(int3(ub, va, 0));
    float4 t01 = sprite.Load(int3(ua, vb, 0)), t11 = sprite.Load(int3(ub, vb, 0));
    float4 t = lerp(lerp(t00, t10, fu), lerp(t01, t11, fu), fv);
    // 0x80 in the texture's alpha is opaque.
    float a = saturate(t.a * (255.0 / 128.0) * saturate(light.a));
    return float4(saturate(t.rgb * light.rgb), a);
}

)";

struct ItemConstants {
    float geo[4], axes[4], origin[4];
    int lim[4];
    float light[4];
};

}  // namespace

struct D3DPresenter::Impl {
    std::mutex lock;
    HWND window = nullptr;
    int frame_w = 0, frame_h = 0, back_w = 0, back_h = 0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11Texture2D> frame;
    ComPtr<ID3D11ShaderResourceView> frame_view;
    ComPtr<ID3D11Texture2D> scene_tex;  // the scene drawn on the GPU (stage 1)
    ComPtr<ID3D11ShaderResourceView> scene_view;
    ComPtr<ID3D11RenderTargetView> scene_target;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps, ps_sharp, ps_item, ps_bicubic, ps_lanczos, ps_cas, ps_blur;
    std::atomic<int> aspect{0};        // 0 fit, 1 fill (crops), 2 stretch, 3 whole multiples of the frame, 4 fit over a blurred copy
    std::atomic<int> render_scale{0};  // the scene texture is this many times the frame (1..4); 0 follows the window
    int scale = 1;                      // the one the scene texture has now
    std::atomic<bool> vsync{true};
    std::atomic<float> brightness{1.0f}, gamma{1.0f};  // the player's picture settings (tone_cb)
    ComPtr<ID3D11Buffer> tone_cb;
    std::atomic<int> filter{0};  // how the frame is scaled to the window: 0 smooth, 1 sharp bilinear, 2 pixels, 3 bicubic, 4 Lanczos, 5 sharpen
    ComPtr<ID3D11SamplerState> point, linear;
    ComPtr<ID3D11Buffer> item_cb;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend_normal, blend_add;
    std::vector<uint8_t> bgra;
    ID3D11ShaderResourceView* source = nullptr;  // what the letterbox pass draws: the software frame or the scene

    struct Cached {
        std::weak_ptr<Image> image;
        ComPtr<ID3D11ShaderResourceView> view;
    };
    // Images are never changed once they are in a scene (a changed picture is a new Image, e.g. NaviObject::step), so the
    // pointer is the key; the weak pointer tells a freed image from a new one at the same address.
    std::unordered_map<const Image*, Cached> textures;
    size_t uploaded = 0;  // bytes uploaded since the counter was read

    bool make_target() {
        ComPtr<ID3D11Texture2D> buffer;
        if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return false;
        D3D11_TEXTURE2D_DESC d;
        buffer->GetDesc(&d);
        back_w = int(d.Width);
        back_h = int(d.Height);
        return SUCCEEDED(device->CreateRenderTargetView(buffer.Get(), nullptr, &target));
    }

    ID3D11ShaderResourceView* texture_of(const ImagePtr& image) {
        auto it = textures.find(image.get());
        if (it != textures.end()) {
            if (it->second.image.lock() == image) return it->second.view.Get();
            textures.erase(it);
        }
        if (image->w <= 0 || image->h <= 0 || image->px.size() < size_t(image->w) * size_t(image->h) * 4) return nullptr;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = UINT(image->w);
        td.Height = UINT(image->h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {image->px.data(), UINT(image->w) * 4, 0};
        ComPtr<ID3D11Texture2D> t;
        if (FAILED(device->CreateTexture2D(&td, &init, &t))) return nullptr;
        Cached c;
        c.image = image;
        if (FAILED(device->CreateShaderResourceView(t.Get(), nullptr, &c.view))) return nullptr;
        uploaded += image->px.size();
        return (textures[image.get()] = std::move(c)).view.Get();
    }

    void forget_freed() {
        for (auto it = textures.begin(); it != textures.end();) it = it->second.image.expired() ? textures.erase(it) : std::next(it);
    }

    // Largest picture with the frame's aspect ratio, centred (the same rule as the software path's letterbox).
    void draw(bool wait) {
        if (!source || back_w <= 0 || back_h <= 0) return;
        int w = back_w, h = back_h;
        const int fit = aspect.load();
        if (fit == 2) {
            // stretch: the whole window
        } else if (fit == 1) {  // fill: the larger of the two scales, the overflow is cropped
            if (w * frame_h < h * frame_w) w = h * frame_w / frame_h;
            else h = w * frame_h / frame_w;
        } else {
            if (w * frame_h > h * frame_w) w = h * frame_w / frame_h;
            else h = w * frame_h / frame_w;
            if (fit == 3 && w >= frame_w && h >= frame_h) {  // whole multiples: the biggest that fits
                const int k = std::min(w / frame_w, h / frame_h);
                w = k * frame_w;
                h = k * frame_h;
            }
        }
        D3D11_VIEWPORT vp = {float((back_w - w) / 2), float((back_h - h) / 2), float(w), float(h), 0.0f, 1.0f};
        const bool bars = fit == 4 && (w < back_w || h < back_h);
        const float black[4] = {0, 0, 0, 1};
        context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
        context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context->RSSetState(nullptr);
        context->ClearRenderTargetView(target.Get(), black);
        context->RSSetViewports(1, &vp);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        const int mode = filter.load();
        ID3D11PixelShader* shaders[] = {ps.Get(), ps_sharp.Get(), ps.Get(), ps_bicubic.Get(), ps_lanczos.Get(), ps_cas.Get()};
        context->PSSetShaderResources(0, 1, &source);
        auto set_tone = [&](float cover_x, float cover_y) {
            D3D11_MAPPED_SUBRESOURCE map;
            if (SUCCEEDED(context->Map(tone_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                const float tone[4] = {brightness.load(), 1.0f / std::max(0.1f, gamma.load()), cover_x, cover_y};
                std::memcpy(map.pData, tone, sizeof tone);
                context->Unmap(tone_cb.Get(), 0);
            }
            context->PSSetConstantBuffers(1, 1, tone_cb.GetAddressOf());
        };
        context->PSSetSamplers(0, 1, linear.GetAddressOf());
        if (bars) {  // the blurred frame over the whole window first, the picture over it
            D3D11_VIEWPORT whole = {0.0f, 0.0f, float(back_w), float(back_h), 0.0f, 1.0f};
            context->RSSetViewports(1, &whole);
            const float shape = float(frame_w) / float(frame_h), window = float(back_w) / float(back_h);
            set_tone(window < shape ? window / shape : 1.0f, window > shape ? shape / window : 1.0f);
            context->PSSetShader(ps_blur.Get(), nullptr, 0);
            context->Draw(3, 0);
            context->RSSetViewports(1, &vp);
        }
        set_tone(1.0f, 1.0f);
        context->PSSetShader(shaders[mode >= 0 && mode < 6 ? mode : 0], nullptr, 0);
        // Smooth is bilinear at every size, whole multiples included; only Pixels takes the nearest texel.
        ID3D11SamplerState* sampler = mode == 2 ? point.Get() : linear.Get();
        context->PSSetSamplers(0, 1, &sampler);
        context->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(0, 1, &none);  // the scene texture is a render target again next frame
        swap->Present(wait && vsync.load() ? 1 : 0, 0);
    }

    void set_item(const ItemConstants& c) {
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(context->Map(item_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            std::memcpy(map.pData, &c, sizeof c);
            context->Unmap(item_cb.Get(), 0);
        }
    }

    void scissor(int x0, int y0, int x1, int y1) {
        D3D11_RECT r = {x0, y0, x1, y1};
        context->RSSetScissorRects(1, &r);
    }

    // The scene texture at `k` times the frame (render scale): the sprites are sampled at that resolution.
    bool make_scene_texture(int k) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = UINT(frame_w * k);
        td.Height = UINT(frame_h * k);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D11RenderTargetView> rt;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &tex)) ||
            FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &view)) ||
            FAILED(device->CreateRenderTargetView(tex.Get(), nullptr, &rt)))
            return false;
        scene_tex = tex;
        scene_view = view;
        scene_target = rt;
        scale = k;
        return true;
    }

    // The scene (already through the view transform) onto the scene texture. The placement mirrors `draw()` in gfx.cpp.
    void draw_scene(const Scene& scene) {
        int want = render_scale.load();
        if (want <= 0) want = std::clamp((back_h + frame_h - 1) / frame_h, 1, 4);  // auto: about the window's own resolution
        if (want != scale && make_scene_texture(want)) source = scene_view.Get();
        const double S = scale;
        const float black[4] = {0, 0, 0, 1};
        context->OMSetRenderTargets(1, scene_target.GetAddressOf(), nullptr);
        context->ClearRenderTargetView(scene_target.Get(), black);
        D3D11_VIEWPORT vp = {0, 0, float(frame_w * scale), float(frame_h * scale), 0.0f, 1.0f};
        context->RSSetViewports(1, &vp);
        context->RSSetState(raster.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, item_cb.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, item_cb.GetAddressOf());
        for (const DrawItem& obj : scene.items) {
            double alpha = std::min(obj.alpha, 1.0);
            if (alpha <= 0) continue;
            double rx = obj.rect[0], ry = obj.rect[1], rw = obj.rect[2], rh = obj.rect[3];
            double sx = obj.scale[0], sy = obj.scale[1];
            double w = rw * std::fabs(sx), h = rh * std::fabs(sy);
            if (w <= 0 || h <= 0) continue;
            double angle = obj.angle * PI;
            double cos_a = std::cos(angle), sin_a = -std::sin(angle);
            double centre_x = SCREEN_W / 2.0 + obj.centre[0], centre_y = SCREEN_H / 2.0 + obj.centre[1];
            double asx = std::fabs(sx), asy = std::fabs(sy);
            double half_w, half_h, ux, uy, vx, vy;
            if (obj.scale_on_screen) {
                half_w = (std::fabs(rw * cos_a) + std::fabs(rh * sin_a)) / 2 * asx;
                half_h = (std::fabs(rw * sin_a) + std::fabs(rh * cos_a)) / 2 * asy;
                ux = cos_a / asx;
                uy = sin_a / asy;
                vx = -sin_a / asx;
                vy = cos_a / asy;
            } else {
                half_w = (std::fabs(w * cos_a) + std::fabs(h * sin_a)) / 2;
                half_h = (std::fabs(w * sin_a) + std::fabs(h * cos_a)) / 2;
                ux = cos_a * rw / w;
                uy = sin_a * rw / w;
                vx = -sin_a * rh / h;
                vy = cos_a * rh / h;
            }
            auto clampi = [](double v, int lo, int hi) {
                double f = std::floor(v + 0.5);
                if (f < lo) return lo;
                if (f > hi) return hi;
                return int(f);
            };
            int x0 = clampi(centre_x - half_w, 0, SCREEN_W), x1 = clampi(centre_x + half_w, 0, SCREEN_W);
            int y0 = clampi(centre_y - half_h, 0, SCREEN_H), y1 = clampi(centre_y + half_h, 0, SCREEN_H);
            if (obj.clipped) {
                x0 = std::max(x0, int(obj.clip[0]));
                x1 = std::min(x1, int(obj.clip[2]));
                y0 = std::max(y0, int(obj.clip[1]));
                y1 = std::min(y1, int(obj.clip[3]));
            }
            if (x0 >= x1 || y0 >= y1) continue;

            ItemConstants c = {};
            c.geo[0] = float(centre_x * S);  // output pixels; the mapping back to the sprite shrinks by the same factor
            c.geo[1] = float(centre_y * S);
            c.geo[2] = float(rw);
            c.geo[3] = float(rh);
            c.axes[0] = float(ux / S);
            c.axes[1] = float(uy / S);
            c.axes[2] = float(vx / S);
            c.axes[3] = float(vy / S);
            c.origin[0] = float(rx);
            c.origin[1] = float(ry);
            c.light[0] = float(obj.light[0]);
            c.light[1] = float(obj.light[1]);
            c.light[2] = float(obj.light[2]);
            c.light[3] = float(alpha);
            ID3D11ShaderResourceView* view = nullptr;
            if (!obj.image) {
                c.origin[2] = 1;
            } else {
                view = texture_of(obj.image);
                if (!view) continue;
                c.lim[0] = int(rx);
                c.lim[1] = std::min(int(rx + rw) - 1, obj.image->w - 1);
                c.lim[2] = int(ry);
                c.lim[3] = std::min(int(ry + rh) - 1, obj.image->h - 1);
            }
            set_item(c);
            scissor(x0 * scale, y0 * scale, x1 * scale, y1 * scale);
            context->OMSetBlendState(obj.additive ? blend_add.Get() : blend_normal.Get(), nullptr, 0xFFFFFFFF);
            context->PSSetShader(ps_item.Get(), nullptr, 0);
            context->PSSetShaderResources(1, 1, &view);
            context->Draw(3, 0);
        }
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(1, 1, &none);
    }
};

D3DPresenter::D3DPresenter() : impl(new Impl) {}
D3DPresenter::~D3DPresenter() = default;

std::unique_ptr<D3DPresenter> D3DPresenter::create(HWND window, int frame_w, int frame_h) {
    std::unique_ptr<D3DPresenter> p(new D3DPresenter);
    Impl& m = *p->impl;
    m.window = window;
    m.frame_w = frame_w;
    m.frame_h = frame_h;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3, D3D11_SDK_VERSION,
                                   &m.device, &got, &m.context);
    if (FAILED(hr)) return nullptr;
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(m.device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        return nullptr;
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    if (FAILED(factory->CreateSwapChainForHwnd(m.device.Get(), window, &sd, nullptr, nullptr, &m.swap))) return nullptr;
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);  // Alt+Enter is the window's own full screen
    if (!m.make_target()) return nullptr;

    ComPtr<ID3DBlob> code, errors;
    auto compile = [&](const char* entry, const char* profile) {
        code.Reset();
        errors.Reset();
        return SUCCEEDED(D3DCompile(SHADER, std::strlen(SHADER), nullptr, nullptr, nullptr, entry, profile, 0, 0, &code, &errors));
    };
    if (!compile("vs", "vs_4_0") || FAILED(m.device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m.vs)))
        return nullptr;
    auto make_ps = [&](const char* entry, ComPtr<ID3D11PixelShader>& out) {
        if (!compile(entry, "ps_4_0")) {
            if (errors) std::fprintf(stderr, "shader %s: %s\n", entry, static_cast<const char*>(errors->GetBufferPointer()));
            return false;
        }
        return SUCCEEDED(m.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &out));
    };
    if (!make_ps("ps", m.ps) || !make_ps("ps_sharp", m.ps_sharp) || !make_ps("ps_item", m.ps_item)) return nullptr;
    // The extra filters are optional: if one does not compile on this driver the window falls back to smooth.
    if (!make_ps("ps_bicubic", m.ps_bicubic)) m.ps_bicubic = m.ps;
    if (!make_ps("ps_lanczos", m.ps_lanczos)) m.ps_lanczos = m.ps;
    if (!make_ps("ps_cas", m.ps_cas)) m.ps_cas = m.ps;
    if (!make_ps("ps_blur", m.ps_blur)) m.ps_blur = m.ps;

    // The software frame (a dynamic texture) and the scene (a render target), both the size of the game's frame.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = UINT(frame_w);
    td.Height = UINT(frame_h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m.device->CreateTexture2D(&td, nullptr, &m.frame)) ||
        FAILED(m.device->CreateShaderResourceView(m.frame.Get(), nullptr, &m.frame_view)))
        return nullptr;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.CPUAccessFlags = 0;
    if (FAILED(m.device->CreateTexture2D(&td, nullptr, &m.scene_tex)) ||
        FAILED(m.device->CreateShaderResourceView(m.scene_tex.Get(), nullptr, &m.scene_view)) ||
        FAILED(m.device->CreateRenderTargetView(m.scene_tex.Get(), nullptr, &m.scene_target)))
        return nullptr;

    D3D11_SAMPLER_DESC sm = {};
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sm.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(m.device->CreateSamplerState(&sm, &m.point))) return nullptr;
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (FAILED(m.device->CreateSamplerState(&sm, &m.linear))) return nullptr;

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(ItemConstants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m.device->CreateBuffer(&bd, nullptr, &m.item_cb))) return nullptr;
    bd.ByteWidth = 16;
    if (FAILED(m.device->CreateBuffer(&bd, nullptr, &m.tone_cb))) return nullptr;
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = TRUE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(m.device->CreateRasterizerState(&rd, &m.raster))) return nullptr;
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(m.device->CreateBlendState(&bl, &m.blend_normal))) return nullptr;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;  // additive: the light balls' glow
    if (FAILED(m.device->CreateBlendState(&bl, &m.blend_add))) return nullptr;
    m.bgra.assign(size_t(frame_w) * size_t(frame_h) * 4, 255);
    return p;
}

void D3DPresenter::present(const uint8_t* bgr) {
    Impl& m = *impl;
    std::lock_guard<std::mutex> lk(m.lock);
    const size_t pixels = size_t(m.frame_w) * size_t(m.frame_h);
    for (size_t i = 0; i < pixels; ++i) {
        m.bgra[i * 4] = bgr[i * 3];
        m.bgra[i * 4 + 1] = bgr[i * 3 + 1];
        m.bgra[i * 4 + 2] = bgr[i * 3 + 2];
    }
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(m.context->Map(m.frame.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        for (int y = 0; y < m.frame_h; ++y)
            std::memcpy(static_cast<uint8_t*>(map.pData) + size_t(y) * map.RowPitch,
                        m.bgra.data() + size_t(y) * size_t(m.frame_w) * 4, size_t(m.frame_w) * 4);
        m.context->Unmap(m.frame.Get(), 0);
        m.source = m.frame_view.Get();
    }
    m.draw(true);
}

void D3DPresenter::present_scene(const Scene& viewed) {
    Impl& m = *impl;
    std::lock_guard<std::mutex> lk(m.lock);
    m.draw_scene(viewed);
    m.forget_freed();
    m.source = m.scene_view.Get();
    m.draw(true);
}

bool D3DPresenter::read_scene(std::vector<uint8_t>& rgb) {
    Impl& m = *impl;
    std::lock_guard<std::mutex> lk(m.lock);
    if (m.scale != 1) return false;  // the comparison with the software frame is made at AMG_RENDER=1
    D3D11_TEXTURE2D_DESC td;
    m.scene_tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(m.device->CreateTexture2D(&td, nullptr, &staging))) return false;
    m.context->CopyResource(staging.Get(), m.scene_tex.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    rgb.resize(size_t(m.frame_w) * size_t(m.frame_h) * 3);
    for (int y = 0; y < m.frame_h; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch;  // B8G8R8A8
        uint8_t* dst = rgb.data() + size_t(y) * size_t(m.frame_w) * 3;
        for (int x = 0; x < m.frame_w; ++x) {
            dst[x * 3] = src[x * 4 + 2];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4];
        }
    }
    m.context->Unmap(staging.Get(), 0);
    return true;
}

bool D3DPresenter::read_scene_scaled(std::vector<uint8_t>& rgb, int& w, int& h) {
    Impl& m = *impl;
    std::lock_guard<std::mutex> lk(m.lock);
    D3D11_TEXTURE2D_DESC td;
    m.scene_tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(m.device->CreateTexture2D(&td, nullptr, &staging))) return false;
    m.context->CopyResource(staging.Get(), m.scene_tex.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    w = int(td.Width);
    h = int(td.Height);
    rgb.resize(size_t(w) * size_t(h) * 3);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch;
        uint8_t* dst = rgb.data() + size_t(y) * size_t(w) * 3;
        for (int x = 0; x < w; ++x) {
            dst[x * 3] = src[x * 4 + 2];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4];
        }
    }
    m.context->Unmap(staging.Get(), 0);
    return true;
}

size_t D3DPresenter::take_uploaded_bytes() {
    std::lock_guard<std::mutex> lk(impl->lock);
    size_t n = impl->uploaded;
    impl->uploaded = 0;
    return n;
}

void D3DPresenter::set_aspect(int mode) {
    impl->aspect = mode;
    redraw();
}

void D3DPresenter::set_vsync(bool on) { impl->vsync = on; }

void D3DPresenter::set_tone(float brightness, float gamma) {
    impl->brightness = brightness;
    impl->gamma = gamma;
    redraw();
}

void D3DPresenter::set_render_scale(int k) { impl->render_scale = k; }

void D3DPresenter::set_filter(int mode) {
    impl->filter = mode;
    redraw();
}

void D3DPresenter::redraw() {
    std::lock_guard<std::mutex> lk(impl->lock);
    impl->draw(false);
}

void D3DPresenter::resize(int client_w, int client_h) {
    Impl& m = *impl;
    if (client_w <= 0 || client_h <= 0) return;  // minimised
    std::lock_guard<std::mutex> lk(m.lock);
    m.context->OMSetRenderTargets(0, nullptr, nullptr);
    m.target.Reset();
    if (SUCCEEDED(m.swap->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0))) m.make_target();
    m.draw(false);
}

}  // namespace amg
