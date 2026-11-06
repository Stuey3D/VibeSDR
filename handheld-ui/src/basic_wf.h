// basic_wf.h — ★ STAND-IN until the port of the app's waterfall (src/wf/, the app's shader, row placement, zoom fan,
// tune ease and fixed output clock) lands. A plain ring of rows on the GPU with the gqrx-style palette, so the rest of
// vibe-hhui can be built and tried on the device. It is NOT the app's behaviour (no per-row placement: a zoom or tune
// shifts nothing) and must not ship as the waterfall.
#pragma once
#include "gfx.h"

#include <vector>

namespace hh {

class BasicWaterfall {
public:
    void init() {
        prog_ = linkProgram(
            "#version 300 es\nlayout(location=0) in vec2 p; out vec2 uv; uniform vec4 uRect; uniform vec2 uView;\n"
            "void main(){ uv = p; vec2 q = uRect.xy + p * uRect.zw; gl_Position = vec4(q.x/uView.x*2.0-1.0, 1.0-q.y/uView.y*2.0, 0.0, 1.0); }\n",
            "#version 300 es\nprecision highp float; in vec2 uv; out vec4 o; uniform sampler2D uRing; uniform sampler2D uLut;\n"
            "uniform float uHead; uniform float uRows; uniform float uLo; uniform float uHi;\n"
            "void main(){ float row = mod(uHead - 1.0 - floor(uv.y * uRows), uRows);\n"
            "  float v = texture(uRing, vec2(uv.x, (row + 0.5) / uRows)).r * 255.0;\n"
            "  if (v < 0.5) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
            "  float db = v - 256.0; float t = clamp((db - uLo) / (uHi - uLo), 0.0, 1.0);\n"
            "  o = texture(uLut, vec2(t, 0.5)); }\n");
        const float q[] = {0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1};
        glGenVertexArrays(1, &vao_); glGenBuffers(1, &vbo_);
        glBindVertexArray(vao_); glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
        glGenTextures(1, &ring_);
        glBindTexture(GL_TEXTURE_2D, ring_);
        std::vector<uint8_t> z((size_t)kBins * kRows, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kBins, kRows, 0, GL_RED, GL_UNSIGNED_BYTE, z.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        // the gqrx-style ramp: black → blue → cyan → yellow → red → white
        const unsigned stops[] = {0x000000, 0x00007f, 0x0040ff, 0x00ffff, 0xffff00, 0xff3000, 0xffffff};
        std::vector<uint8_t> lut(256 * 4);
        for (int i = 0; i < 256; i++) {
            const float f = i / 255.f * 6.f;
            const int k = (int)f < 6 ? (int)f : 5;
            const float u = f - k;
            for (int c = 0; c < 3; c++) {
                const int a = (stops[k] >> (16 - 8 * c)) & 255, b = (stops[k + 1] >> (16 - 8 * c)) & 255;
                lut[(size_t)i * 4 + c] = (uint8_t)(a + (b - a) * u);
            }
            lut[(size_t)i * 4 + 3] = 255;
        }
        glGenTextures(1, &lut_);
        glBindTexture(GL_TEXTURE_2D, lut_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, lut.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    }
    /** One row, low → high (resampled to the ring's width). */
    void push(const uint8_t* bins, int n) {
        row_.resize(kBins);
        for (int i = 0; i < kBins; i++) row_[(size_t)i] = bins[(size_t)((int64_t)i * n / kBins)];
        glBindTexture(GL_TEXTURE_2D, ring_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, head_ % kRows, kBins, 1, GL_RED, GL_UNSIGNED_BYTE, row_.data());
        head_++;
    }
    void draw(int viewW, int viewH, float x, float y, float w, float h, float loDb, float hiDb) {
        glUseProgram(prog_);
        glUniform4f(glGetUniformLocation(prog_, "uRect"), x, y, w, h);
        glUniform2f(glGetUniformLocation(prog_, "uView"), (float)viewW, (float)viewH);
        glUniform1f(glGetUniformLocation(prog_, "uHead"), (float)(head_ % kRows));
        glUniform1f(glGetUniformLocation(prog_, "uRows"), (float)kRows);
        glUniform1f(glGetUniformLocation(prog_, "uLo"), loDb);
        glUniform1f(glGetUniformLocation(prog_, "uHi"), hiDb);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, ring_);
        glUniform1i(glGetUniformLocation(prog_, "uRing"), 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, lut_);
        glUniform1i(glGetUniformLocation(prog_, "uLut"), 1);
        glActiveTexture(GL_TEXTURE0);
        glDisable(GL_BLEND);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
    }
private:
    static constexpr int kBins = 1024, kRows = 256;
    GLuint prog_ = 0, vao_ = 0, vbo_ = 0, ring_ = 0, lut_ = 0;
    uint64_t head_ = 0;
    std::vector<uint8_t> row_;
};

}  // namespace hh
