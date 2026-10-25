// The window's output through Direct3D 11 (docs/technical/gpu-renderer.md, step 3a): the CPU-drawn frame goes to a
// texture and the GPU scales it into a flip-model swap chain, letterboxed, with vsync. Windows only.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace amg {

struct Scene;  // gfx.hpp

class D3DPresenter {
public:
    // nullptr when no device can be made (the caller falls back to the software path).
    static std::unique_ptr<D3DPresenter> create(HWND window, int frame_w, int frame_h);
    ~D3DPresenter();

    // `bgr` is frame_w x frame_h, 24-bit, rows top-down, as the software path keeps it. Draws it into the window
    // and presents (blocks until the next vertical blank). Safe to call from any one thread at a time.
    void present(const uint8_t* bgr);
    // The scene after the view transform (Graphics::view_scene), drawn on the GPU and presented. The textures of its
    // images are uploaded the first time they are seen and kept until the image is freed.
    void present_scene(const Scene& viewed);
    // The scene texture as 24-bit RGB rows (what Graphics::render_scene returns), for comparing the two back ends.
    bool read_scene(std::vector<uint8_t>& rgb);
    // The same at whatever render scale it has now: `w` x `h` pixels (test aid).
    bool read_scene_scaled(std::vector<uint8_t>& rgb, int& w, int& h);
    // Bytes of textures uploaded since the last call (what a scene change costs).
    size_t take_uploaded_bytes();
    // Draws the last frame again: after an expose or a resize.
    void redraw();
    // How the frame is scaled to the window: 0 smooth (crisp at whole multiples), 1 sharp bilinear, 2 pixels (nearest).
    void set_filter(int mode);
    // How the frame sits in the window: 0 fit (bars), 1 fill (crops), 2 stretch, 3 whole multiples of the frame.
    void set_aspect(int mode);
    // The scene is drawn at this many times the frame, 1 to 4 (0: follows the window). Takes effect on the next frame.
    void set_render_scale(int k);
    void resize(int client_w, int client_h);

    struct Impl;

private:
    D3DPresenter();
    std::unique_ptr<Impl> impl;
};

}  // namespace amg
