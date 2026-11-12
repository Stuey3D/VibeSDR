// gfx.h — the 2D drawing vibe-hhui needs besides the waterfall: filled / outlined rectangles, lines, circles and
// textured quads (text), batched into one GLES 3 draw per texture change. Pixel coordinates, (0,0) top-left.
#pragma once
#include <array>
#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace hh {

struct Rgba { float r, g, b, a; };
inline Rgba rgb(unsigned hex, float a = 1.f) { return {((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f, a}; }

inline GLuint compileShader(GLenum type, const char* src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, nullptr, log); std::fprintf(stderr, "vibe-hhui: shader: %s\n", log); }
    return s;
}
inline GLuint linkProgram(const char* vs, const char* fs) {
    const GLuint p = glCreateProgram();
    const GLuint v = compileShader(GL_VERTEX_SHADER, vs), f = compileShader(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v); glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, nullptr, log); std::fprintf(stderr, "vibe-hhui: link: %s\n", log); }
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

class Gfx {
public:
    void init(int w, int h) {
        w_ = w; h_ = h;
        prog_ = linkProgram(
            "#version 300 es\nlayout(location=0) in vec2 p; layout(location=1) in vec2 uv; layout(location=2) in vec4 c;\n"
            "uniform vec2 uView; out vec2 vUv; out vec4 vC;\n"
            "void main(){ vUv=uv; vC=c; gl_Position=vec4(p.x/uView.x*2.0-1.0, 1.0-p.y/uView.y*2.0, 0.0, 1.0); }\n",
            // ★ uv.x < 0 = a solid (no texture read) — so shapes never force a batch break (one draw a frame, text.h)
            "#version 300 es\nprecision mediump float; in vec2 vUv; in vec4 vC; uniform sampler2D uTex;\n"
            "out vec4 o; void main(){ vec4 t = vUv.x < -0.5 ? vec4(1.0) : texture(uTex, vUv); o = vC * t; }\n");
        uView_ = glGetUniformLocation(prog_, "uView");
        uTex_ = glGetUniformLocation(prog_, "uTex");
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void*)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(V), (void*)(4 * sizeof(float)));
        glBindVertexArray(0);
    }
    int width() const { return w_; }
    int height() const { return h_; }

    void rect(float x, float y, float w, float h, Rgba c) { use(0); quad(x, y, w, h, -1, -1, -1, -1, c); }
    void frame(float x, float y, float w, float h, float t, Rgba c) {
        rect(x, y, w, t, c); rect(x, y + h - t, w, t, c); rect(x, y + t, t, h - 2 * t, c); rect(x + w - t, y + t, t, h - 2 * t, c);
    }
    /** A line of thickness t (a quad along it). */
    void line(float x0, float y0, float x1, float y1, float t, Rgba c) {
        use(0);
        const float dx = x1 - x0, dy = y1 - y0, l = std::sqrt(dx * dx + dy * dy);
        if (l <= 0) return;
        const float nx = -dy / l * t * 0.5f, ny = dx / l * t * 0.5f;
        tri({x0 + nx, y0 + ny, -1, -1, c}, {x1 + nx, y1 + ny, -1, -1, c}, {x1 - nx, y1 - ny, -1, -1, c});
        tri({x0 + nx, y0 + ny, -1, -1, c}, {x1 - nx, y1 - ny, -1, -1, c}, {x0 - nx, y0 - ny, -1, -1, c});
    }
    /** A filled disc, or a ring of thickness `t` (t <= 0 = filled). */
    void circle(float cx, float cy, float r, float t, Rgba c) {
        use(0);
        /* ★ The unit circle once (it was 80 cos / sin a circle — 300 spots on the map are 600 circles a frame), and
         *  12 sides for a small one (r ≤ 8 px: 0.2 px from round at most, below what the panel shows). */
        struct U { float c[21], s[21]; };
        static const U u20 = [] { U u{}; for (int i = 0; i <= 20; i++) { const float a = (float)i / 20 * 6.2831853f; u.c[i] = std::cos(a); u.s[i] = std::sin(a); } return u; }();
        static const U u12 = [] { U u{}; for (int i = 0; i <= 12; i++) { const float a = (float)i / 12 * 6.2831853f; u.c[i] = std::cos(a); u.s[i] = std::sin(a); } return u; }();
        const bool small = r <= 8;
        const int n = small ? 12 : 20;
        const U& uc = small ? u12 : u20;
        for (int i = 0; i < n; i++) {
            const float c0 = uc.c[i], s0 = uc.s[i], c1 = uc.c[i + 1], s1 = uc.s[i + 1];
            if (t <= 0) tri({cx, cy, -1, -1, c}, {cx + c0 * r, cy + s0 * r, -1, -1, c}, {cx + c1 * r, cy + s1 * r, -1, -1, c});
            else {
                const float ri = r - t;
                tri({cx + c0 * r, cy + s0 * r, -1, -1, c}, {cx + c1 * r, cy + s1 * r, -1, -1, c}, {cx + c1 * ri, cy + s1 * ri, -1, -1, c});
                tri({cx + c0 * r, cy + s0 * r, -1, -1, c}, {cx + c1 * ri, cy + s1 * ri, -1, -1, c}, {cx + c0 * ri, cy + s0 * ri, -1, -1, c});
            }
        }
    }
    /** A filled rounded rectangle (corner radius r). */
    void rrect(float x, float y, float w, float h, float r, Rgba c) {
        use(0);
        r = std::max(0.f, std::min(r, std::min(w, h) * 0.5f));
        if (r < 0.5f) { quad(x, y, w, h, -1, -1, -1, -1, c); return; }
        quad(x + r, y, w - 2 * r, h, -1, -1, -1, -1, c);
        quad(x, y + r, r, h - 2 * r, -1, -1, -1, -1, c);
        quad(x + w - r, y + r, r, h - 2 * r, -1, -1, -1, -1, c);
        corner(x + r, y + r, r, 0, 0, c, 2); corner(x + w - r, y + r, r, 0, 0, c, 3);
        corner(x + w - r, y + h - r, r, 0, 0, c, 0); corner(x + r, y + h - r, r, 0, 0, c, 1);
    }
    /** A rounded outline of thickness t. */
    void rframe(float x, float y, float w, float h, float r, float t, Rgba c) {
        use(0);
        r = std::max(0.f, std::min(r, std::min(w, h) * 0.5f));
        if (r < t) { frame(x, y, w, h, t, c); return; }
        quad(x + r, y, w - 2 * r, t, -1, -1, -1, -1, c); quad(x + r, y + h - t, w - 2 * r, t, -1, -1, -1, -1, c);
        quad(x, y + r, t, h - 2 * r, -1, -1, -1, -1, c); quad(x + w - t, y + r, t, h - 2 * r, -1, -1, -1, -1, c);
        corner(x + r, y + r, r, r - t, 0, c, 2); corner(x + w - r, y + r, r, r - t, 0, c, 3);
        corner(x + w - r, y + h - r, r, r - t, 0, c, 0); corner(x + r, y + h - r, r, r - t, 0, c, 1);
    }
    /** A horizontal gradient through stops cols[i] at positions pos[i] (0..1). */
    void hgrad(float x, float y, float w, float h, const Rgba* cols, const float* pos, int n) {
        use(0);
        for (int i = 1; i < n; i++) {
            const float x0 = x + w * pos[i - 1], x1 = x + w * pos[i];
            tri({x0, y, -1, -1, cols[i - 1]}, {x1, y, -1, -1, cols[i]}, {x1, y + h, -1, -1, cols[i]});
            tri({x0, y, -1, -1, cols[i - 1]}, {x1, y + h, -1, -1, cols[i]}, {x0, y + h, -1, -1, cols[i - 1]});
        }
    }
    /** Clip everything after this to a rectangle (top-left coordinates) until unclip(). Flushes. */
    void clip(float x, float y, float w, float h) {
        flush();
        glEnable(GL_SCISSOR_TEST);
        glScissor((GLint)std::floor(x), (GLint)std::floor(h_ - (y + h)), (GLsizei)std::ceil(w), (GLsizei)std::ceil(h));
        clipOn_ = true; clipR_[0] = x; clipR_[1] = y; clipR_[2] = w; clipR_[3] = h;
    }
    void unclip() { flush(); glDisable(GL_SCISSOR_TEST); clipOn_ = false; }
    /** ★ A clip INSIDE whatever clip is on (intersected), undone by clipPop() — clip()/unclip() are not nested: a clip
     *  inside a list's clip would otherwise replace it, and its unclip() would open the whole screen to the next rows. */
    void clipPush(float x, float y, float w, float h) {
        stack_.push_back({clipOn_ ? 1.f : 0.f, clipR_[0], clipR_[1], clipR_[2], clipR_[3]});
        if (clipOn_) {
            const float x0 = std::max(x, clipR_[0]), y0 = std::max(y, clipR_[1]);
            const float x1 = std::min(x + w, clipR_[0] + clipR_[2]), y1 = std::min(y + h, clipR_[1] + clipR_[3]);
            x = x0; y = y0; w = std::max(0.f, x1 - x0); h = std::max(0.f, y1 - y0);
        }
        clip(x, y, w, h);
    }
    void clipPop() {
        if (stack_.empty()) { unclip(); return; }
        const auto p = stack_.back(); stack_.pop_back();
        if (p[0] > 0.5f) clip(p[1], p[2], p[3], p[4]); else unclip();
    }

    /** A filled triangle. */
    void triangle(float x0, float y0, float x1, float y1, float x2, float y2, Rgba c) { use(0); tri({x0, y0, -1, -1, c}, {x1, y1, -1, -1, c}, {x2, y2, -1, -1, c}); }
    /** A textured quad from part of a texture (the glyph atlas). */
    void imageUV(GLuint tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, Rgba c) { use(tex); quad(x, y, w, h, u0, v0, u1, v1, c); }
    /** Batches drawn since the last call (the perf line). */
    int takeFlushes() { const int n = flushes_; flushes_ = 0; return n; }
    /** A textured quad (text). */
    void image(GLuint tex, float x, float y, float w, float h, Rgba c = {1, 1, 1, 1}) { use(tex); quad(x, y, w, h, 0, 0, 1, 1, c); }

    /** A polyline strip filled down to `baseY` (the spectrum trace's fill), then the line on top. */
    void traceFill(const std::vector<float>& xs, const std::vector<float>& ys, float baseY, Rgba fill) {
        use(0);
        for (size_t i = 1; i < xs.size(); i++) {
            tri({xs[i - 1], ys[i - 1], -1, -1, fill}, {xs[i], ys[i], -1, -1, fill}, {xs[i], baseY, -1, -1, fill});
            tri({xs[i - 1], ys[i - 1], -1, -1, fill}, {xs[i], baseY, -1, -1, fill}, {xs[i - 1], baseY, -1, -1, fill});
        }
    }

    void flush() {
        if (verts_.empty()) return;
        glUseProgram(prog_);
        glUniform2f(uView_, (float)w_, (float)h_);
        glUniform1i(uTex_, 0);
        if (tex_) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_); }
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts_.size() * sizeof(V)), verts_.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)verts_.size());
        flushes_++;
        glBindVertexArray(0);
        verts_.clear();
    }

private:
    struct V { float x, y, u, v; Rgba c; };
    int w_ = 640, h_ = 480;
    bool clipOn_ = false; float clipR_[4] = {0, 0, 0, 0};
    std::vector<std::array<float, 5>> stack_;   // ★ clipPush / clipPop
    GLuint prog_ = 0, vao_ = 0, vbo_ = 0, tex_ = 0;
    GLint uView_ = -1, uTex_ = -1;
    std::vector<V> verts_;
    int flushes_ = 0;

    void use(GLuint tex) { if (tex == 0) return; if (tex != tex_) { flush(); tex_ = tex; } }
    /** A quarter disc / ring at (cx, cy): q 0 = bottom-right, 1 = bottom-left, 2 = top-left, 3 = top-right. */
    void corner(float cx, float cy, float r, float ri, float, Rgba c, int q) {
        const int n = 6;
        const float a0 = q * 1.5707963f;
        for (int i = 0; i < n; i++) {
            const float t0 = a0 + i * 1.5707963f / n, t1 = a0 + (i + 1) * 1.5707963f / n;
            const float c0 = std::cos(t0), s0 = std::sin(t0), c1 = std::cos(t1), s1 = std::sin(t1);
            if (ri <= 0) tri({cx, cy, -1, -1, c}, {cx + c0 * r, cy + s0 * r, -1, -1, c}, {cx + c1 * r, cy + s1 * r, -1, -1, c});
            else {
                tri({cx + c0 * r, cy + s0 * r, -1, -1, c}, {cx + c1 * r, cy + s1 * r, -1, -1, c}, {cx + c1 * ri, cy + s1 * ri, -1, -1, c});
                tri({cx + c0 * r, cy + s0 * r, -1, -1, c}, {cx + c1 * ri, cy + s1 * ri, -1, -1, c}, {cx + c0 * ri, cy + s0 * ri, -1, -1, c});
            }
        }
    }
    void tri(V a, V b, V c) { verts_.push_back(a); verts_.push_back(b); verts_.push_back(c); }
    void quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, Rgba c) {
        tri({x, y, u0, v0, c}, {x + w, y, u1, v0, c}, {x + w, y + h, u1, v1, c});
        tri({x, y, u0, v0, c}, {x + w, y + h, u1, v1, c}, {x, y + h, u0, v1, c});
    }
};

}  // namespace hh
