struct Blit {
    width: u32,
    height: u32,
    current: u32,
    previous: u32,
    method: u32,
    out_width: u32,
    out_height: u32,
    nearest: u32,
    options: u32,
    scale: u32,
    parity: u32,
    history: u32,
}

@group(0) @binding(0) var<storage, read> pixels: array<u32>;
@group(0) @binding(1) var<uniform> blit: Blit;
@group(0) @binding(2) var<storage, read> overlay_words: array<u32>;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
}

@vertex
fn vs(@builtin(vertex_index) i: u32) -> VertexOut {
    let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
    var o: VertexOut;
    o.position = vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = vec2<f32>(p.x, 1.0 - p.y);
    return o;
}

fn raw_texel(base: u32, x: i32, y: i32) -> vec4<f32> {
    let cx = u32(clamp(x, 0, i32(blit.width) - 1));
    let cy = u32(clamp(y, 0, i32(blit.height) - 1));
    return unpack4x8unorm(pixels[base + cy * blit.width + cx]);
}

// Reconstruct in native scanline space before applying presentation scaling.
// At internal upscales, all subpixels in one guest scanline share parity.
fn bob_texel(base: u32, x: i32, y: i32, parity: u32) -> vec4<f32> {
    let stride = i32(max(blit.scale, 1u));
    let cy = clamp(y, 0, i32(blit.height) - 1);
    if ((u32(cy / stride) & 1u) == parity) {
        return raw_texel(base, x, cy);
    }
    let above = cy - stride;
    let below = cy + stride;
    if (above < 0) {
        return raw_texel(base, x, below);
    }
    if (below >= i32(blit.height)) {
        return raw_texel(base, x, above);
    }
    return (raw_texel(base, x, above) + raw_texel(base, x, below)) * 0.5;
}

fn texel(base: u32, x: i32, y: i32) -> vec4<f32> {
    let cy = clamp(y, 0, i32(blit.height) - 1);
    let current = raw_texel(base, x, cy);
    if (blit.method == 0u) {
        return current;
    }
    if (blit.method == 5u) {
        return (current + raw_texel(blit.previous, x, cy)) * 0.5;
    }
    let bob = bob_texel(base, x, cy, blit.parity);
    if (blit.method == 2u || blit.history == 0u) {
        return bob;
    }
    if (blit.method == 3u) {
        return (bob + bob_texel(blit.previous, x, cy, blit.parity ^ 1u)) * 0.5;
    }
    let stride = max(blit.scale, 1u);
    if ((u32(cy) / stride & 1u) == blit.parity) {
        return current;
    }
    let old = raw_texel(blit.previous, x, cy);
    if (blit.method == 1u) {
        return old;
    }
    // Motion-adaptive approximation: weave stable areas, interpolate
    // missing lines when the opposite field differs from the current bob.
    let delta = abs(old.rgb - bob.rgb);
    let motion = max(delta.r, max(delta.g, delta.b));
    return mix(old, bob, smoothstep(0.04, 0.12, motion));
}

fn sample(base: u32, uv: vec2<f32>) -> vec4<f32> {
    let size = vec2<f32>(f32(blit.width), f32(blit.height));
    if (blit.nearest != 0u) {
        let p = vec2<i32>(floor(uv * size));
        return texel(base, p.x, p.y);
    }
    let scale = max(floor(vec2<f32>(f32(blit.out_width), f32(blit.out_height)) / size), vec2<f32>(1.0));
    let t = uv * size;
    let d = fract(t) - 0.5;
    let region = 0.5 - 0.5 / scale;
    let p = floor(t) + (d - clamp(d, -region, region)) * scale;
    let origin = floor(p);
    let f = p - origin;
    let x = i32(origin.x);
    let y = i32(origin.y);
    let top = mix(texel(base, x, y), texel(base, x + 1, y), f.x);
    let bottom = mix(texel(base, x, y + 1), texel(base, x + 1, y + 1), f.x);
    return mix(top, bottom, f.y);
}

fn scene(uv: vec2<f32>) -> vec3<f32> {
    return sample(blit.current, uv).rgb;
}

fn luma(c: vec3<f32>) -> f32 { return dot(c, vec3<f32>(0.299, 0.587, 0.114)); }

fn edge_aa(uv: vec2<f32>, center: vec3<f32>) -> vec3<f32> {
    // Fast approximate edge AA, evaluated in image space. Flat regions return
    // unchanged, while edge direction controls subpixel reconstruction.
    let texel_step = 1.0 / vec2<f32>(f32(blit.width), f32(blit.height));
    let nw = luma(scene(uv + texel_step * vec2<f32>(-1.0, -1.0)));
    let ne = luma(scene(uv + texel_step * vec2<f32>(1.0, -1.0)));
    let sw = luma(scene(uv + texel_step * vec2<f32>(-1.0, 1.0)));
    let se = luma(scene(uv + texel_step * vec2<f32>(1.0, 1.0)));
    let m = luma(center);
    let low = min(m, min(min(nw, ne), min(sw, se)));
    let high = max(m, max(max(nw, ne), max(sw, se)));
    if (high - low < max(0.0312, high * 0.125)) { return center; }
    var direction = vec2<f32>(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    let reduce = max((nw + ne + sw + se) * 0.03125, 0.0078125);
    direction = clamp(direction / (min(abs(direction.x), abs(direction.y)) + reduce), vec2<f32>(-8.0), vec2<f32>(8.0)) * texel_step;
    let a = 0.5 * (scene(uv - direction / 6.0) + scene(uv + direction / 6.0));
    let b = a * 0.5 + 0.25 * (scene(uv - direction * 0.5) + scene(uv + direction * 0.5));
    let lb = luma(b);
    return select(b, a, lb < low || lb > high);
}

@fragment
fn fs(in: VertexOut) -> @location(0) vec4<f32> {
    // Native host overlay: fixed 32x8 glyph grid; never touches GS VRAM.
    // Only evaluate when F9 is enabled. Adapt scale to the presentation viewport.
    if ((blit.options & 2u) != 0u) {
        let pixel_scale = max(1u, min(3u, min(blit.out_width / 208u, blit.out_height / 80u)));
        let local = vec2<i32>(floor(in.uv * vec2<f32>(f32(blit.out_width), f32(blit.out_height)))) - vec2<i32>(8);
        if (all(local >= vec2<i32>(0))) {
            let p = vec2<u32>(local) / pixel_scale;
            if (p.x < 200u && p.y < 72u) {
                let dark = vec4<f32>(0.025, 0.035, 0.05, 1.0);
                if (p.x >= 4u && p.x < 196u && p.y >= 4u && p.y < 68u) {
                    let q = p - vec2<u32>(4u);
                    let cell = vec2<u32>(q.x / 6u, q.y / 8u);
                    let col = q.x % 6u;
                    let row = q.y % 8u;
                    if (col < 5u && row < 7u) {
                        let index = (cell.y * 32u + cell.x) * 2u;
                        let bits = overlay_words[index + select(0u, 1u, row == 6u)];
                        let shift = (row % 6u) * 5u + 4u - col;
                        if (((bits >> shift) & 1u) != 0u) {
                            return select(vec4<f32>(0.94, 0.96, 0.98, 1.0),
                                vec4<f32>(0.40, 0.85, 1.0, 1.0), cell.y == 0u);
                        }
                    }
                }
                return dark;
            }
        }
    }
    var c = scene(in.uv);
    if ((blit.options & 1u) != 0u) { c = edge_aa(in.uv, c); }
    return vec4<f32>(clamp(c, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
}
