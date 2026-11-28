// Written by vulkan/tools/crtgen.py from dx/bfi_crt_gen.h -- do not edit by hand.
#define CRT_SLOTS 85
[[vk::binding(8, 0)]] cbuffer CrtParams { float4 gCrtP[85]; };
#define CRT_TIMER (gCrtP[0].x)
#define FRAMECOUNT ((uint)gCrtP[1].x)
#define crt_b_scanline_attack (gCrtP[2].x)
#define crt_b_scanline_max (gCrtP[3].x)
#define crt_b_scanline_min (gCrtP[4].x)
#define crt_bb_bright (gCrtP[5].x)
#define crt_bb_dark (gCrtP[6].x)
#define crt_bb_mode ((int)gCrtP[7].x)
#define crt_beam_corner_spread (gCrtP[8].x)
#define crt_beam_h_bloom (gCrtP[9].x)
#define crt_beam_max_sigma (gCrtP[10].x)
#define crt_beam_min_sigma (gCrtP[11].x)
#define crt_beam_shape (gCrtP[12].x)
#define crt_brightness (gCrtP[13].x)
#define crt_burnin_orbit_period (gCrtP[14].x)
#define crt_burnin_orbit_radius (gCrtP[15].x)
#define crt_burnin_phase_amp (gCrtP[16].x)
#define crt_burnin_phase_period (gCrtP[17].x)
#define crt_colour_temp (gCrtP[18].x)
#define crt_contrast (gCrtP[19].x)
#define crt_corner_border (gCrtP[20].x)
#define crt_corner_intensity (gCrtP[21].x)
#define crt_corner_shadow (gCrtP[22].x)
#define crt_corner_size (gCrtP[23].x)
#define crt_decay_frames ((int)gCrtP[24].x)
#define crt_g_scanline_attack (gCrtP[25].x)
#define crt_g_scanline_max (gCrtP[26].x)
#define crt_g_scanline_min (gCrtP[27].x)
#define crt_gamma_in (gCrtP[28].x)
#define crt_gamma_out (gCrtP[29].x)
#define crt_gamut_expand_ceiling (gCrtP[30].x)
#define crt_gamut_expand_method ((int)gCrtP[31].x)
#define crt_gamut_expand_neutral (gCrtP[32].x)
#define crt_gamut_expand_skin (gCrtP[33].x)
#define crt_gamut_expand_strength (gCrtP[34].x)
#define crt_glow_strength (gCrtP[35].x)
#define crt_glow_wide_strength (gCrtP[36].x)
#define crt_grain_animate (gCrtP[37].x > 0.5)
#define crt_grain_colour (gCrtP[38].x > 0.5)
#define crt_grain_emulsion (gCrtP[39].x > 0.5)
#define crt_grain_intensity (gCrtP[40].x)
#define crt_grain_shadows (gCrtP[41].x)
#define crt_grain_size (gCrtP[42].x)
#define crt_halation_strength (gCrtP[43].x)
#define crt_interlace_strength (gCrtP[44].x)
#define crt_luma_gate_curve (gCrtP[45].x)
#define crt_luma_gate_threshold (gCrtP[46].x)
#define crt_mask_boost (gCrtP[47].x)
#define crt_mask_dither (gCrtP[48].x)
#define crt_mask_offset_x ((int)gCrtP[49].x)
#define crt_mask_offset_y ((int)gCrtP[50].x)
#define crt_mask_strength (gCrtP[51].x)
#define crt_mask_type ((int)gCrtP[52].x)
#define crt_noise_floor (gCrtP[53].x)
#define crt_noise_floor_scale (gCrtP[54].x)
#define crt_phosphor_colour (gCrtP[55].xyz)
#define crt_phosphor_dot (gCrtP[56].x)
#define crt_phosphor_sharpness (gCrtP[57].x)
#define crt_r_scanline_attack (gCrtP[58].x)
#define crt_r_scanline_max (gCrtP[59].x)
#define crt_r_scanline_min (gCrtP[60].x)
#define crt_saturation (gCrtP[61].x)
#define crt_scanline_roll_drift (gCrtP[62].x)
#define crt_scanline_roll_strength (gCrtP[63].x)
#define crt_scanline_sigma (gCrtP[64].x)
#define crt_scanline_ss_coherence (gCrtP[65].x)
#define crt_scanline_ss_strength (gCrtP[66].x)
#define crt_scanline_strength (gCrtP[67].x)
#define crt_scanline_width (gCrtP[68].x)
#define crt_sharpen_clamp (gCrtP[69].x)
#define crt_sharpen_strength (gCrtP[70].x)
#define crt_slot_mask_strength (gCrtP[71].x)
#define crt_soften_strength (gCrtP[72].x)
#define crt_soop_hdr10_peak_nits (gCrtP[73].x)
#define crt_spot_size (gCrtP[74].x)
#define crt_timer (gCrtP[75].x)
#define crt_triad_width (gCrtP[76].x)
#define crt_vignette_hdr_strength (gCrtP[77].x)
#define crt_vignette_hdr_threshold (gCrtP[78].x)
#define crt_vignette_power (gCrtP[79].x)
#define crt_vignette_shape ((int)gCrtP[80].x)
#define crt_vignette_strength (gCrtP[81].x)
#define crt_vignette_v_power (gCrtP[82].x)
#define BUFFER_WIDTH (gCrtP[83].x)
#define BUFFER_HEIGHT (gCrtP[83].y)
#define CRT_OVERLAY_CHECK (gCrtP[83].z > 0.5)
static const float4x4 kCubicBezier = float4x4(
     1.0f,  0.0f,  0.0f,  0.0f,
    -3.0f,  3.0f,  0.0f,  0.0f,
     3.0f, -6.0f,  3.0f,  0.0f,
    -1.0f,  3.0f, -3.0f,  1.0f);

float Bezier(float t, float4 cp)
{
    float4 tv = float4(1.0, t, t*t, t*t*t);
    return dot(tv, mul(kCubicBezier, cp));
}
float gauss(float x, float sigma)
{
    return exp(-(x*x) / (2.0*sigma*sigma));
}

// Fast erf approximation (Abramowitz & Stegun 7.1.26, max error 1.5e-7)
float crt_erf(float x)
{
    float t = 1.0 / (1.0 + 0.3275911 * abs(x));
    float p = t * (0.254829592
              + t * (-0.284496736
              + t * (1.421413741
              + t * (-1.453152027
              + t *  1.061405429))));
    return sign(x) * (1.0 - p * exp(-(x*x)));
}

// Analytically integrate Gaussian over pixel footprint [f-hw, f+hw].
// Eliminates stairstepping at any subpixel position.
float gauss_integral(float f, float hw, float sigma)
{
    // Guarded to match gen_gauss_integral's quadrature path. Reachable sigma
    // never gets near zero today (slider minimum 0.05, scaled by factors that
    // stay above ~0.12), but the two paths should defend identically.
    float s = max(sigma * 1.41421356, 0.0001); // sigma * sqrt(2)
    return 0.5 * (crt_erf((f + hw) / s) - crt_erf((f - hw) / s));
}

// Generalized Gaussian integral over pixel footprint [f-hw, f+hw].
// gen_gauss(x, sigma, n) = exp(-(|x|/(sigma*sqrt2))^n), normalised so that
// n=2 is exactly the standard Gaussian used by the erf path.
// For n=2: takes the fast erf path (exact, and matches the quadrature form).
// For n!=2: 16-point Gauss-Legendre quadrature (8 symmetric pairs).
// 4-point GL fails badly when sigma << hw (e.g. sigma=0.05, tight beam)
// because the narrow spike is missed by coarse nodes -- 85% error at n=8.
// 16-point GL gives < 3% error even at sigma=0.05, n=8.
// Shape n>2 produces flatter scanline plateau and steeper dark gap transition,
// matching real well-focused CRT beam cross-sections.
float gen_gauss_integral(float f, float hw, float sigma, float n)
{
    if (abs(n - 2.0) < 0.05)
    {
        // n=2: exact erf path (fast)
        return gauss_integral(f, hw, sigma);
    }

    // Match the n==2 erf path exactly. That path integrates the standard
    // normal exp(-x^2/(2*sigma^2)); writing the generalized form as
    // exp(-(|x|/(sigma*sqrt2))^n) makes n=2 reduce to precisely that, so the
    // two branches agree at the threshold instead of the quadrature kernel
    // being narrower by sqrt(2) (which caused a brightness/width step of up
    // to -28% when Beam Shape crossed 2.05).
    float inv_sigma = 1.0 / max(sigma * 1.41421356, 0.0001);
    float s = 1.0 / (sigma * sqrt(2.0 * 3.14159265)); // normalisation

    // 16-point GL nodes and weights (8 symmetric pairs on [-1,1])
    static const float nd[8] = {
        0.0950125098, 0.2816035508, 0.4580167777, 0.6178762444,
        0.7554044084, 0.8656312024, 0.9445750231, 0.9894009350
    };
    static const float wt[8] = {
        0.1894506105, 0.1826034150, 0.1691565194, 0.1495959889,
        0.1246289863, 0.0951585117, 0.0622535239, 0.0271524594
    };

    float v = 0.0;
    [unroll] for (int i = 0; i < 8; i++)
    {
        float tp = f + hw * nd[i];
        float tn = f - hw * nd[i];
        v += wt[i] * (exp(-pow(max(abs(tp) * inv_sigma, 0.0), n))
                    + exp(-pow(max(abs(tn) * inv_sigma, 0.0), n)));
    }

    return v * hw * s;
}

// Per-scanline brightness variation.
// Spatially correlated low-frequency hash -- adjacent rows similar, not random.
// Models HV supply ripple + phosphor coating unevenness in real CRTs.
// Standalone integer hash -- used by crt_scanline_roll independently of ENABLE_GRAIN
uint crt_row_hash(uint x)
{
    x ^= x >> 16u;
    x *= 0x45D9F3Bu;
    x ^= x >> 16u;
    return x;
}

float crt_scanline_roll(float row)
{
    // Per-row hash with slight spatial correlation to adjacent rows.
    // Models real CRT HV supply ripple -- each scanline independently varies,
    // with neighbours similar but not identical (short-range correlation).
    // Much higher frequency than sine waves -- matches actual tube behaviour.
    float r = floor(row);

    uint  seed0 = crt_row_hash(uint(r));
    uint  seed1 = crt_row_hash(uint(r + 1.0));
    uint  seedm = crt_row_hash(uint(r - 1.0));

    float v0 = float(seed0 & 0xFFFFu) / 32767.5 - 1.0;
    float v1 = float(seed1 & 0xFFFFu) / 32767.5 - 1.0;
    float vm = float(seedm & 0xFFFFu) / 32767.5 - 1.0;

    // Weighted average: 50% current row, 25% each neighbour
    return v0 * 0.5 + (v1 + vm) * 0.25;
}
#define CRT_REFERENCE_WIDTH 3840.0

// ============================================================
// Multi-type CRT mask
// 0: Aperture Grille (horizontal RGB stripes)
// 1: Diagonal Aperture Grille (staggered rows, better for QD-OLED)
// 2: Slot Mask (aperture grille + alternating dark rows)
// 3: Trinitron (wider green, narrower R/B -- real Trinitron proportions)
// ============================================================

float3 crt_mask_apply(float2 fc, float tw_ref, float strength, float sharp,
                      float3 phcol, int mask_type, float slot_dark,
                      int offset_x, int offset_y, float2 fc_clean, float pixel_luma)
{
    float tw     = tw_ref * (float(BUFFER_WIDTH) / CRT_REFERENCE_WIDTH);
    // Use fc_clean (without burn-in step offsets) for fwidth to prevent
    // discontinuities from integer triad steps from widening the AA edge
    float fw     = fwidth(fc_clean.x / tw);
    float edge   = max(fw, 1.0 / max(sharp, 0.01));

    float3 mask  = 1.0;

    if (mask_type == 0)
    {
        // -- Aperture Grille: standard horizontal RGB stripes --
        float t = frac(fc.x / tw);
        float r = smoothstep(0.0,   edge, t) * smoothstep(0.333, 0.333-edge, t);
        float g = smoothstep(0.333, 0.333+edge, t) * smoothstep(0.667, 0.667-edge, t);
        float b = smoothstep(0.667, 0.667+edge, t) * smoothstep(1.0,   1.0-edge, t);
        mask = float3(r, g, b) * phcol;
    }
    else if (mask_type == 1)
    {
        // -- Diagonal Aperture Grille: rows offset by 1 triad/2 --
        // Each row is shifted by half a triad relative to its neighbour.
        // This distributes the phosphor structure diagonally, reducing
        // sensitivity to QD-OLED triangular subpixel alignment.
        float row_offset = floor(fc.y) * (tw * 0.5);
        float t = frac((fc.x + row_offset) / tw);
        float r = smoothstep(0.0,   edge, t) * smoothstep(0.333, 0.333-edge, t);
        float g = smoothstep(0.333, 0.333+edge, t) * smoothstep(0.667, 0.667-edge, t);
        float b = smoothstep(0.667, 0.667+edge, t) * smoothstep(1.0,   1.0-edge, t);
        mask = float3(r, g, b) * phcol;
    }
    else if (mask_type == 2)
    {
        // -- Slot Mask: aperture grille + alternating dark rows --
        // Mimics shadow mask CRTs where horizontal slots separate scanlines.
        float t = frac(fc.x / tw);
        float r = smoothstep(0.0,   edge, t) * smoothstep(0.333, 0.333-edge, t);
        float g = smoothstep(0.333, 0.333+edge, t) * smoothstep(0.667, 0.667-edge, t);
        float b = smoothstep(0.667, 0.667+edge, t) * smoothstep(1.0,   1.0-edge, t);
        mask = float3(r, g, b) * phcol;
        // Slot rows: fwidth-based AA on the vertical row boundary.
        // Smoothstep replaces the hard binary step for anti-aliased slot edges.
        float  fw_y    = fwidth(fc.y * 0.5);
        float  row_t   = frac(floor(fc.y * 0.5) * 0.5); // 0 or 0.5
        float  slot_aa = smoothstep(0.0, fw_y, row_t) * smoothstep(0.5, 0.5 - fw_y, row_t);
        float  lane    = 1.0 - slot_dark * (1.0 - slot_aa);
        mask *= lane;
    }
    else if (mask_type == 3)
    {
        // -- Trinitron: wider green, narrower R/B --
        // Real Sony Trinitron tubes had green phosphors ~40% wider than R/B.
        // Proportions: R=0-0.25, G=0.25-0.75, B=0.75-1.0 (green gets 50% of triad)
        float t    = frac(fc.x / tw);
        float fedge = edge * 0.7; // sharper edges for Trinitron-style
        float r = smoothstep(0.0,  fedge, t) * smoothstep(0.25, 0.25-fedge, t);
        float g = smoothstep(0.25, 0.25+fedge, t) * smoothstep(0.75, 0.75-fedge, t);
        float b = smoothstep(0.75, 0.75+fedge, t) * smoothstep(1.0,  1.0-fedge,  t);
        mask = float3(r, g, b) * phcol;
    }
    else if (mask_type == 4)
    {
        // -- QD-OLED Delta: scaled checkerboard based on physical A95L subpixel layout --
        // triad_width=2.0 = 1:1 physical pixel mapping (native subpixel size)
        // triad_width=4.0 = each virtual phosphor covers 2 physical pixels (coarser, more visible)
        // triad_width=6.0 = 3 physical pixels per phosphor, very visible structure
        // The 2x2 checkerboard tile scales with tw so triad width is meaningful.
        // Tile pattern:
        //   [G][R]   (even virtual row)
        //   [B][G]   (odd virtual row)

        // Scale fc by tw/2 so the 2x2 tile covers tw pixels
        // At tw=2: 1 pixel per cell. At tw=4: 2 pixels per cell.
        float  cell_size = tw * 0.5; // pixels per subpixel cell
        float2 fc_off    = float2(floor(abs(fc))) + float2(float(crt_mask_offset_x),
                                                            float(crt_mask_offset_y));

        // Determine which 2x2 virtual tile position we're in using uint arithmetic
        // Converting to uint before modulo guarantees exact 0/1 with no float error
        // at any resolution or cell size
        // Small epsilon prevents boundary rounding errors at non-integer cell sizes
        // (e.g. triad 4.0 at 4K with DSR where fc coordinates may fall just below
        // an integer boundary due to floating point precision)
        uint2 tile_idx = uint2(floor(fc_off / cell_size + 0.001));
        float cx       = float(tile_idx.x & 1u); // exactly 0 or 1
        float cy       = float(tile_idx.y & 1u);

        // Subpixel identity:
        // (cx=0, cy=0)=Green  (cx=1, cy=0)=Red
        // (cx=0, cy=1)=Blue   (cx=1, cy=1)=Green
        float is_green = (1.0 - cx) * (1.0 - cy) + cx * cy;
        float is_red   = cx * (1.0 - cy);
        float is_blue  = (1.0 - cx) * cy;

        // Position within the current cell, -1..1
        // Used for the rounded square phosphor shape
        float2 cell_pos = (frac(fc_off / cell_size) * 2.0 - 1.0);

        // Phosphor sizes scale with cell_size to maintain consistent fill ratio
        float fill_scale = 1.0 - 0.08 / max(cell_size, 0.5);

        // Green phosphor: larger rounded square (~60% of cell at native size)
        float g_size   = 0.72 + (0.88 - 0.72) * saturate((cell_size - 1.0) / 3.0);
        g_size        *= fill_scale;
        float g_soft   = max(sharp * 0.1, 0.15) / max(cell_size * 0.5, 1.0);
        float g_shape  = smoothstep(g_size, g_size - g_soft,
                         max(abs(cell_pos.x), abs(cell_pos.y)));

        // Red/Blue phosphors: smaller rounded square (~40% of cell at native size)
        float rb_size  = 0.52 + (0.76 - 0.52) * saturate((cell_size - 1.0) / 3.0);
        rb_size       *= fill_scale;
        float rb_soft  = max(sharp * 0.1, 0.15) / max(cell_size * 0.5, 1.0);
        float rb_shape = smoothstep(rb_size, rb_size - rb_soft,
                         max(abs(cell_pos.x), abs(cell_pos.y)));

        mask = float3(is_red * rb_shape,
                      is_green * g_shape,
                      is_blue * rb_shape) * phcol;
    }
    else if (mask_type == 5)
    {
        // -- QD-OLED Luminance Gate: mask strength inversely proportional to pixel luminance --
        // The QD-OLED colour pattern is applied at full strength in dark areas and
        // reduced toward passthrough in bright areas. Bright pixels (highlights) are
        // barely affected; dark pixels and midtones get the full phosphor texture.
        // No global darkening because bright areas compensate -- they pass through cleanly.
        // This matches real CRT physics: phosphor gaps are only visible in darker areas
        // because bright phosphors bleed light that fills adjacent gaps.

        float  cell_size = max(tw * 0.5, 0.5);
        float2 fc_off    = float2(floor(abs(fc))) +
                           float2(float(crt_mask_offset_x), float(crt_mask_offset_y));

        // Compute QD-OLED subpixel identity and shape (same as type 4)
        uint2 tile_idx = uint2(floor(fc_off / cell_size + 0.001));
        float cx       = float(tile_idx.x & 1u);
        float cy       = float(tile_idx.y & 1u);

        float is_green = (1.0 - cx) * (1.0 - cy) + cx * cy;
        float is_red   = cx * (1.0 - cy);
        float is_blue  = (1.0 - cx) * cy;

        float2 cell_pos = (frac(fc_off / cell_size) * 2.0 - 1.0);

        float fill_scale = 1.0 - 0.08 / max(cell_size, 0.5);

        float g_size   = 0.72 + (0.88 - 0.72) * saturate((cell_size - 1.0) / 3.0);
        g_size        *= fill_scale;
        float g_soft   = max(sharp * 0.1, 0.15) / max(cell_size * 0.5, 1.0);
        float g_shape  = smoothstep(g_size, g_size - g_soft,
                         max(abs(cell_pos.x), abs(cell_pos.y)));

        float rb_size  = 0.52 + (0.76 - 0.52) * saturate((cell_size - 1.0) / 3.0);
        rb_size       *= fill_scale;
        float rb_soft  = max(sharp * 0.1, 0.15) / max(cell_size * 0.5, 1.0);
        float rb_shape = smoothstep(rb_size, rb_size - rb_soft,
                         max(abs(cell_pos.x), abs(cell_pos.y)));

        float3 qdoled_mask = float3(is_red * rb_shape,
                                    is_green * g_shape,
                                    is_blue * rb_shape) * phcol;

        // Luminance gate: use actual image pixel luminance to scale mask application.
        // High luma -> gate near 0 -> mask approaches 1.0 (bright pixels pass through clean)
        // Low luma  -> gate near 1 -> mask applies fully (dark areas get phosphor texture)
        // Power curve 0.5 means midtones get ~70% of full mask strength.
        // Gate: remap pixel_luma against threshold then apply curve
        float gate_input = saturate((pixel_luma - crt_luma_gate_threshold) /
                           max(1.0 - crt_luma_gate_threshold, 0.001));
        float gate = 1.0 - pow(gate_input, crt_luma_gate_curve);
        mask = lerp(1.0, qdoled_mask, gate);
    }

    return saturate(lerp(1.0, mask, strength));
}
float megatron_scanline(float ch, float beam_dist, float scan_min, float scan_max, float attack)
{
    float dist = clamp(beam_dist / ((ch*(scan_max-scan_min)) + scan_min), 0.0, 1.0);
    return Bezier(dist, float4(1.0, 1.0, ch*attack, 0.0));
}
uint grain_uhash(uint x) { x^=x>>16; x*=0x21f0aaad; x^=x>>15; x*=0xd35a2d97; x^=x>>16; return x; }
float  grain_unorm1(uint u) { return asfloat((u>>9u)|0x3F800000u)-1.0; }
#define glin(x)  ((x)*0.283799*((2.52405+(x))*(x)))
float3 crt_to_linear(float3 x)   { return pow(max(x, 0.0), crt_gamma_in); }
float3 crt_from_linear(float3 x) { return pow(max(x, 0.0), 1.0/crt_gamma_out); }
float3 CrtPattern(float3 c, float2 fc, float2 texcoord)
{
    #if ENABLE_BURNIN_PHASE
        // Phase: alternates between 0 and crt_burnin_phase_amp pixels on a slow
        // timer. Shift is rounded to WHOLE pixels: integer-pixel shifts relocate
        // exactly the same set of sampled mask values, guaranteeing identical
        // average brightness at any mask strength (no brightness fluctuation).
        float phase_ms      = crt_burnin_phase_period * 60000.0;
        float phase_steps_  = floor(crt_timer / phase_ms);
        float phase_toggle  = phase_steps_ - floor(phase_steps_ * 0.5) * 2.0; // 0 or 1
        float phase_h       = phase_toggle * round(crt_burnin_phase_amp
                            * (float(BUFFER_WIDTH) / 3840.0)); // integer pixels
    #else
        float phase_h       = 0.0;
    #endif

    #if ENABLE_BURNIN_ORBIT
        // Orbit: steps through 8 discrete positions on a circle of radius
        // crt_burnin_orbit_radius pixels. Each position is rounded to WHOLE
        // pixels (see phase note above -- guarantees zero brightness change).
        // Discrete stepping avoids perceptible motion; each step holds one period.
        float orbit_ms      = crt_burnin_orbit_period * 60000.0;
        float orbit_steps_  = floor(crt_timer / orbit_ms);
        float orbit_idx     = orbit_steps_ - floor(orbit_steps_ * 0.125) * 8.0; // 0..7
        float orbit_angle   = orbit_idx * 0.7853982; // 2pi/8
        float orbit_r       = crt_burnin_orbit_radius * (float(BUFFER_WIDTH) / 3840.0);
        float2 orbit_offs   = round(float2(cos(orbit_angle), sin(orbit_angle)) * orbit_r);
    #else
        float2 orbit_offs   = float2(0.0, 0.0);
    #endif
    // -- CRT gamma decode --
    float3 c_lin = crt_to_linear(c);
    // Source colour and luma stored before any CRT processing.
    // Post-scanline c_lin is near-zero in dark gaps -- cannot use for detail work.
    float  c_src_luma = dot(max(c_lin, 0.0), float3(0.2126, 0.7152, 0.0722));

    // -- Aperture grille mask --
    #if ENABLE_MASK
        // Moiré dither: small random sub-pixel phase offset per 16x16 tile.
        // Breaks strict mask periodicity that causes moiré with certain image
        // frequencies (Haeberli & Segal 1990).
        float2 tile_id  = floor(fc / 16.0);
        uint   tile_rng = grain_uhash(grain_uhash(uint(tile_id.y)) + uint(tile_id.x));
        float  dither_x = (float(tile_rng & 0xFFu) / 255.0 - 0.5) * crt_mask_dither;
        float2 fc_mask = fc + float2(phase_h + orbit_offs.x + dither_x, orbit_offs.y);
        float  mask_pixel_luma = dot(max(c, 0.0), float3(0.2126, 0.7152, 0.0722));
        float3 mask = crt_mask_apply(fc_mask, crt_triad_width, crt_mask_strength,
                                    crt_phosphor_sharpness, crt_phosphor_colour,
                                    crt_mask_type, crt_slot_mask_strength,
                                    crt_mask_offset_x, crt_mask_offset_y, fc,
                                    mask_pixel_luma);
        // Phosphor dot structure: subtle per-dot luminance variation
        if (crt_phosphor_dot > 0.001)
        {
            // Spatially-correlated noise: average hash over a 3x3 neighbourhood
            // in mask-cell space so adjacent phosphors vary smoothly.
            // Slow temporal drift every 10 minutes for burn-in protection.
            uint  drift    = uint(CRT_TIMER / 600000.0);
            uint2 mc       = uint2(uint(fc_mask.x), uint(fc_mask.y));
            float dot_acc  = 0.0;
            for (int ddy = -1; ddy <= 1; ddy++)
            for (int ddx = -1; ddx <= 1; ddx++)
            {
                uint2 nb   = mc + uint2(ddx, ddy);
                uint  seed = nb.x * 2333u + nb.y * 3571u + drift * 0xB5297A4Du;
                dot_acc   += grain_unorm1(grain_uhash(seed));
            }
            float dot_var  = (dot_acc / 9.0 - 0.5) * 2.0; // normalise to [-1,+1]
            mask *= 1.0 + dot_var * crt_phosphor_dot;
            mask  = max(mask, 0.0);
        }
        c_lin = c_lin * mask * crt_mask_boost;
    #endif

    // -- Scanlines with sub-pixel AA --
    // No vertical burn-in offset applied -- any vertical shift changes brightness
    // because frac() maps non-linearly to the gaussian beam profile.
    // Horizontal mask shift (phase_h + orbit_h) handles burn-in protection instead.
    // Resolution-independent scanline width.
    // When SCANLINE_REFERENCE_HEIGHT > 0, scale width proportionally so
    // the same crt_scanline_width value produces identical-looking scanlines
    // at any render resolution.
    #if SCANLINE_REFERENCE_HEIGHT > 0
    float scan_width = crt_scanline_width *
                       (float(BUFFER_HEIGHT) / float(SCANLINE_REFERENCE_HEIGHT));
    #else
    float scan_width = crt_scanline_width;
    #endif
    // Snap to nearest integer: non-integer scan_width causes some rows to
    // get f near 0 (bright) and others near ±0.5 (dark), producing oscillating
    // scanline sizes and inconsistent mask darkness as width increases.
    scan_width = max(round(scan_width), 1.0);

    // Interlace: shift scanline phase by half a scanline width every other frame.
    // Alternates which rows are bright/dark, simulating CRT interlaced mode.
    // Applied here (to the scanline period) not as a UV shift, so the actual
    // dark gaps between scanlines move position rather than the whole image shifting.
    // Snap to integer pixel coordinates before scanline calculation
    float scanline_y = floor(fc.y) + 0.5;
    float f  = frac(scanline_y / scan_width) - 0.5;
    // Analytical half-pixel width -- used by gauss_integral for step-free beam profile
    float hw = 0.5 / max(scan_width, 1.0);
    float da = abs(f - hw) * 2.0;
    float db = abs(f + hw) * 2.0;

    // Per-scanline variation: brightness AND sigma (beam width) vary per row.
    // On a real CRT, HV ripple affects both beam focus and brightness --
    // brighter rows are also slightly wider, dimmer rows slightly narrower.
    // roll_raw is in [-1,1]; roll scales brightness, sigma_roll scales width.
    float scanline_row = floor(scanline_y / scan_width);
    float roll_raw = (crt_scanline_roll_strength > 0.0001)
                   ? crt_scanline_roll(scanline_row
                       + floor(CRT_TIMER * 0.001 * crt_scanline_roll_drift))
                   : 0.0;
    float roll       = 1.0 + crt_scanline_roll_strength * roll_raw;
    // Sigma modulation: wider when brighter, narrower when dimmer
    // Scale is half of brightness variation so it doesn't dominate
    float sigma_roll = 1.0 + crt_scanline_roll_strength * roll_raw * 0.5;

    // Corner beam ellipticity: sigma grows with distance from screen centre.
    // On a real CRT the beam strikes phosphor at increasing angle toward edges,
    // making the spot elliptical. Modelled as sigma *= (1 + spread * r²)
    // where r is normalised distance from centre in [0,1].
    float corner_r2 = 0.0;
    if (crt_beam_corner_spread > 0.001)
    {
        float2 tc_c  = texcoord - 0.5; // [-0.5, 0.5]
        float  ar    = float(BUFFER_WIDTH) / float(BUFFER_HEIGHT);
        // Weight x more heavily since horizontal deflection causes vertical spread
        corner_r2 = tc_c.x * tc_c.x * ar * ar + tc_c.y * tc_c.y;
        corner_r2 = saturate(corner_r2 * 4.0); // normalise so corner = ~1.0
    }
    float corner_sigma_scale = 1.0 + crt_beam_corner_spread * corner_r2;

    #if ENABLE_BEAM_MODULATION
        float sigma_scale = 1.0 / max(scan_width, 1.0);
        float r_sigma = lerp(crt_beam_min_sigma, crt_beam_max_sigma, saturate(c_lin.r)) * sigma_scale * sigma_roll * corner_sigma_scale;
        float g_sigma = lerp(crt_beam_min_sigma, crt_beam_max_sigma, saturate(c_lin.g)) * sigma_scale * sigma_roll * corner_sigma_scale;
        float b_sigma = lerp(crt_beam_min_sigma, crt_beam_max_sigma, saturate(c_lin.b)) * sigma_scale * sigma_roll * corner_sigma_scale;
        // Analytical Gaussian integral -- no stairstepping
        // crt_beam_shape=2 uses fast erf path; higher values use generalized Gaussian
        float beam_r = gen_gauss_integral(f, hw, r_sigma, crt_beam_shape) * roll;
        float beam_g = gen_gauss_integral(f, hw, g_sigma, crt_beam_shape) * roll;
        float beam_b = gen_gauss_integral(f, hw, b_sigma, crt_beam_shape) * roll;
    #else
        // Analytical integral with megatron scanline shaping
        float gi_s   = gen_gauss_integral(f, hw, crt_scanline_sigma * sigma_roll * corner_sigma_scale, crt_beam_shape) * roll;
        float bd     = abs(f) * 2.0;
        float beam_r = megatron_scanline(c_lin.r,bd,crt_r_scanline_min,crt_r_scanline_max,crt_r_scanline_attack) * gi_s;
        float beam_g = megatron_scanline(c_lin.g,bd,crt_g_scanline_min,crt_g_scanline_max,crt_g_scanline_attack) * gi_s;
        float beam_b = megatron_scanline(c_lin.b,bd,crt_b_scanline_min,crt_b_scanline_max,crt_b_scanline_attack) * gi_s;
    #endif

    #if ENABLE_SCANLINE_SS
    // -- Rotated-grid supersampling of the scanline image (RGSS 4x) --
    // True supersampling: each tap contributes its own COLOUR weighted by the
    // beam at its subpixel position. This softens the content edge itself
    // across the pixel footprint, which is where the diagonal staircase lives
    // (averaging beam weights alone does not move the colour edge).
    // Taps replicate the grading chain (colour temp -> BCS -> linear) and the
    // centre's mask factor is applied to the averaged result, matching the
    // single-sample path.
    // Effective SS amount, optionally gated by local gradient coherence so
    // texture detail is left single-sampled while diagonal edges get full AA.
    //
    // Base sampling coordinate must match the main fetch path: with geometry
    // warp on, the single-sample colour comes from geom_warp(texcoord), so the
    // SS taps must read from the same warped position or the blend produces a
    // double image that grows toward the corners (max warp displacement).
    // With preblur on, the preblur texture is already warped so texcoord maps
    // directly. Offsets are applied post-warp; the warp Jacobian is ~identity
    // at subpixel scale so this is accurate for the RGSS jitter.
    #if (ENABLE_PREBLUR == 0) && ENABLE_GEOMETRY
        float2 ss_base = geom_warp(texcoord); // match warped main fetch
    #else
        float2 ss_base = texcoord;            // preblur tex already warped, or no warp
    #endif

    float ss_amount = crt_scanline_ss_strength;
    if (crt_scanline_ss_strength > 0.001 && crt_scanline_ss_coherence > 0.001)
    {
        float2 spx = CrtPixelSize;
        #if ENABLE_PREBLUR
        #define CRT_SSL(dx, dy) dot(max(tex2D(crt_preblur_v_sampler, ss_base + float2(dx, dy) * spx).rgb, 0.0), float3(0.2126, 0.7152, 0.0722))
        #else
        #define CRT_SSL(dx, dy) dot(max(CrtBB(ss_base + float2(dx, dy) * spx).rgb, 0.0), float3(0.2126, 0.7152, 0.0722))
        #endif
        float sl00 = CRT_SSL(-1.0,-1.0); float sl10 = CRT_SSL(0.0,-1.0); float sl20 = CRT_SSL(1.0,-1.0);
        float sl01 = CRT_SSL(-1.0, 0.0);                                 float sl21 = CRT_SSL(1.0, 0.0);
        float sl02 = CRT_SSL(-1.0, 1.0); float sl12 = CRT_SSL(0.0, 1.0); float sl22 = CRT_SSL(1.0, 1.0);
        #undef CRT_SSL
        // Four sub-gradients feed the structure tensor
        float4 ssgx = float4(sl10-sl00, sl20-sl10, sl12-sl02, sl22-sl12);
        float4 ssgy = float4(sl01-sl00, sl21-sl20, sl02-sl01, sl22-sl21);
        float Jxx = dot(ssgx, ssgx);
        float Jyy = dot(ssgy, ssgy);
        float Jxy = dot(ssgx, ssgy);
        float tr  = Jxx + Jyy;
        float aniso = (tr > 1e-6)
                    ? sqrt((Jxx - Jyy)*(Jxx - Jyy) + 4.0*Jxy*Jxy) / tr
                    : 0.0;
        // Coherent edge -> keep full SS; incoherent texture -> pull SS toward 0.
        float edge = smoothstep(0.35, 0.75, aniso);
        ss_amount = lerp(crt_scanline_ss_strength,
                         crt_scanline_ss_strength * edge,
                         crt_scanline_ss_coherence);
    }

    if (ss_amount > 0.001 && crt_scanline_strength > 0.001)
    {
        // Classic RGSS pattern: rotated so no two samples share a row/column
        static const float2 rgss[4] = {
            float2( 0.125,  0.375), float2( 0.375, -0.125),
            float2(-0.125, -0.375), float2(-0.375,  0.125)
        };

        float3 ss_acc = 0.0;
        [unroll] for (int si = 0; si < 4; si++)
        {
            // Source tap at subpixel offset, in the same (warped) space as the
            // main fetch so the supersampled result aligns with the single sample.
            float2 ss_uv = ss_base + rgss[si] * CrtPixelSize;
            #if ENABLE_PREBLUR
            float3 ss_c  = tex2D(crt_preblur_v_sampler, ss_uv).rgb;
            #else
            float3 ss_c  = CrtBB(ss_uv).rgb;
            #endif
            ss_c = max(ss_c, 0.0);

            // Replicate grading chain so taps match the centre signal
            if (abs(crt_colour_temp) > 0.001)
                ss_c = apply_colour_temp(ss_c, crt_colour_temp);
            if (abs(crt_brightness)>0.001 || abs(crt_contrast)>0.001 || abs(crt_saturation)>0.001)
            {
                #if PIPELINE >= 1
                float3 ss_bcs = to_linear(ss_c);
                ss_bcs = apply_bcs(ss_bcs, crt_brightness, crt_contrast, crt_saturation);
                ss_c   = from_linear(max(ss_bcs, 0.0));
                #else
                ss_c   = apply_bcs(ss_c, crt_brightness, crt_contrast, crt_saturation);
                #endif
            }
            float3 ss_lin = crt_to_linear(ss_c);

            // Vertical beam position at this sample's y offset
            float ss_y  = floor(fc.y) + 0.5 + rgss[si].y;
            float ss_f  = frac(ss_y / scan_width) - 0.5;

            float3 ss_beam;
            #if ENABLE_BEAM_MODULATION
            float ss_rs = lerp(crt_beam_min_sigma, crt_beam_max_sigma, saturate(ss_lin.r)) * sigma_scale * sigma_roll * corner_sigma_scale;
            float ss_gs = lerp(crt_beam_min_sigma, crt_beam_max_sigma, saturate(ss_lin.g)) * sigma_scale * sigma_roll * corner_sigma_scale;
            float ss_bs = lerp(crt_beam_min_sigma, crt_beam_max_sigma, saturate(ss_lin.b)) * sigma_scale * sigma_roll * corner_sigma_scale;
            ss_beam.r = gen_gauss_integral(ss_f, hw, ss_rs, crt_beam_shape) * roll;
            ss_beam.g = gen_gauss_integral(ss_f, hw, ss_gs, crt_beam_shape) * roll;
            ss_beam.b = gen_gauss_integral(ss_f, hw, ss_bs, crt_beam_shape) * roll;
            #else
            float ss_gi = gen_gauss_integral(ss_f, hw, crt_scanline_sigma * sigma_roll * corner_sigma_scale, crt_beam_shape) * roll;
            float ss_bd = abs(ss_f) * 2.0;
            ss_beam.r = megatron_scanline(ss_lin.r, ss_bd, crt_r_scanline_min, crt_r_scanline_max, crt_r_scanline_attack) * ss_gi;
            ss_beam.g = megatron_scanline(ss_lin.g, ss_bd, crt_g_scanline_min, crt_g_scanline_max, crt_g_scanline_attack) * ss_gi;
            ss_beam.b = megatron_scanline(ss_lin.b, ss_bd, crt_b_scanline_min, crt_b_scanline_max, crt_b_scanline_attack) * ss_gi;
            #endif

            // Colour x beam, with scanline strength applied per tap
            ss_acc += ss_lin * lerp(1.0, ss_beam, crt_scanline_strength);
        }
        ss_acc *= 0.25;

        // Apply the centre pixel's mask factor (matches single-sample path,
        // where mask multiplies c_lin before the scanline block)
        #if ENABLE_MASK
        ss_acc = ss_acc * mask * crt_mask_boost;
        #endif

        // Blend: single-sample scanline result vs supersampled result
        float3 c_single = c_lin * lerp(1.0, float3(beam_r, beam_g, beam_b), crt_scanline_strength);
        c_lin = lerp(c_single, ss_acc, ss_amount);
    }
    else
    {
        c_lin *= lerp(1.0, float3(beam_r, beam_g, beam_b), crt_scanline_strength);
    }
    #else
    c_lin *= lerp(1.0, float3(beam_r, beam_g, beam_b), crt_scanline_strength);
    #endif // ENABLE_SCANLINE_SS

    // -- Interlaced field blanking --
    // Alternate between odd and even scanline fields each frame, matching
    // how real CRT interlacing works: one field of scanlines is bright,
    // the other is dark, alternating every frame to create field-rate flicker.
    //
    // Blanking operates on scanline periods (not pixels) so it works correctly
    // at any scanline width. Uses the same scan_width as the scanline calculation.
    #if ENABLE_INTERLACE
    if (crt_interlace_strength > 0.001)
    {
        // Which field is this frame: 0 or 1, alternates every frame.
        // When BFI/decay is active, FRAMECOUNT increments every frame including
        // dark frames -- so lit frames may always land on even or odd FRAMECOUNT.
        // Divide by BFI cycle length so the field alternates per lit frame pair.
        #if ENABLE_DECAY
        uint  frame_field  = (FRAMECOUNT / uint(max(crt_decay_frames, 2))) & 1u;
        #else
        uint  frame_field  = FRAMECOUNT & 1u;
        #endif

        // Which scanline period does this pixel belong to: even or odd.
        // Snap scan_width to nearest integer so periods are whole pixels --
        // non-integer widths cause drift in the alternating pattern.
        float snap_width   = max(round(scan_width), 1.0);
        uint  scanline_idx = uint(fc.y / snap_width);
        uint  scan_field   = scanline_idx & 1u;

        // gate=1: full brightness (this field's scanline). gate=0: dimmed.
        float gate = (scan_field == frame_field) ? 1.0 : 0.0;
        float dim  = lerp(1.0, gate, crt_interlace_strength);
        c_lin *= dim;
    }
    #endif

    // -- Spot size / overbrightness --
    // Luminance-squared boost: dark pixels unaffected, bright pixels boosted.
    // Applied directly to c_lin before gamma re-encoding.
    // Debug: at spot_size=3.0 with full-white input, output is 4x brightness.
    if (crt_spot_size > 0.001)
    {
        float luma_s    = dot(c_lin, float3(0.2126, 0.7152, 0.0722));
        float luma_norm = saturate(luma_s); // clamp for gate
        float boost     = 1.0 + crt_spot_size * luma_norm * luma_norm;
        c_lin *= boost;
    }

    // -- Electron beam horizontal bloom --
    // On real CRTs, high-current beams (bright content) spread horizontally
    // due to space charge repulsion between electrons. Simulated as a
    // luminance-gated 3-tap horizontal blur on the post-scanline signal.
    if (crt_beam_h_bloom > 0.001)
    {
        float  luma_bl  = dot(c_lin, float3(0.2126, 0.7152, 0.0722));
        // Gate: only active above 70% luma, full effect above 90%
        float  gate_bl  = smoothstep(0.7, 0.9, luma_bl);
        if (gate_bl > 0.001)
        {
            float  bpx  = CrtPixelSize.x;
            float3 cl   = CrtBB(texcoord - float2(bpx, 0.0)).rgb;
            float3 cr   = CrtBB(texcoord + float2(bpx, 0.0)).rgb;
            // Convert neighbours to linear
            cl = glin(cl); cr = glin(cr);
            // Gaussian 3-tap: weights 0.25, 0.5, 0.25
            float3 bloomed = cl * 0.25 + c_lin * 0.5 + cr * 0.25;
            c_lin = lerp(c_lin, bloomed, gate_bl * crt_beam_h_bloom);
        }
    }

    // -- Re-encode --
    c = crt_from_linear(c_lin);

    // -- Brightboost (hue-preserving) --
    if (crt_bb_mode == 0)
    {
        // Peak channel mode: colour-agnostic, correct for CRT phosphor physics
        float bb_ref    = max(max(c.r, c.g), c.b);
        float bb_gain   = lerp(crt_bb_dark, crt_bb_bright, bb_ref);
        float bb_out    = bb_ref * bb_gain;
        float bb_ratio0 = (bb_ref > 0.0001) ? max(bb_out / bb_ref, 0.0) : 1.0;
        c = max(c * bb_ratio0, 0.0);
    }
    else if (crt_bb_mode == 1)
    {
        // Luma mode: perceptually weighted (Rec.709), may under-represent blue
        float bb_luma   = dot(max(c, 0.0), float3(0.2126, 0.7152, 0.0722));
        float bb_ref    = max(max(c.r, c.g), c.b);
        float bb_gain   = lerp(crt_bb_dark, crt_bb_bright, bb_ref);
        float bb_out    = bb_luma * bb_gain;
        float bb_ratio  = (bb_luma > 0.0001) ? max(bb_out / bb_luma, 0.0) : 1.0;
        c = max(c * bb_ratio, 0.0);
    }
    else
    {
        // Per channel mode: each channel boosted by its own value independently
        float3 bb_gain3 = lerp(crt_bb_dark, crt_bb_bright, c);
        c = max(c * bb_gain3, 0.0);
    }

    // -- Vignette --
    #if ENABLE_VIGNETTE
    if (crt_vignette_strength > 0.001)
    {
        float2 uv_c = texcoord * 2.0 - 1.0; // [-1,1] on both axes

        float vig;
        if (crt_vignette_shape == 1)
        {
            // Circular/elliptical: original dot(uv,uv) radial falloff.
            // Produces an oval on 16:9 (touches top/bottom before sides).
            vig = pow(saturate(1.0 - dot(uv_c, uv_c) * 0.5), crt_vignette_power);
        }
        else
        {
            // Rectangular CRT-authentic: independent H and V power-curve falloffs.
            // Corners naturally darker as product of both falloffs.
            float vig_h = pow(saturate(1.0 - abs(uv_c.x)), crt_vignette_power);
            float vig_v = pow(saturate(1.0 - abs(uv_c.y)), crt_vignette_v_power);
            vig = vig_h * vig_v;
        }
        // Highlight protection: pixels above threshold are progressively lifted
        // toward no-vignette. Strength controls maximum protection at threshold.
        float vig_luma = dot(c, float3(0.2126, 0.7152, 0.0722));
        float protect  = saturate((vig_luma - crt_vignette_hdr_threshold) /
                                   max(1.0 - crt_vignette_hdr_threshold, 0.001));
        // protect=0 below threshold (full vig), =1 at peak brightness
        // Scale by strength so user controls maximum protection level
        float vig_gate = 1.0 - protect * crt_vignette_hdr_strength;
        vig = lerp(1.0, vig, vig_gate);
        vig = lerp(1.0, vig, crt_vignette_strength);
        c *= vig;
    }
    #endif // ENABLE_VIGNETTE

    // -- Corner shadow: bezel-cast darkening at screen extremes --
    #if ENABLE_CORNER_ROUND
    if (crt_corner_shadow > 0.001)
    {
        float2 edge  = abs(texcoord - 0.5) * 2.0; // 0=centre, 1=edge
        float  shadow = pow(max(edge.x, edge.y), 6.0);
        c *= 1.0 - shadow * crt_corner_shadow;
    }
    #endif

    // -- Halation (bright element glass scatter, localised) --
    #if ENABLE_HALATION
    if (crt_halation_strength > 0.001)
    {
        // Bilinear fetch from quarter-res texture -- hardware filtering gives
        // additional free smoothing on top of the blur
        // Use the V-blurred result (H then V) for true 2D anisotropic halation.
        // At anisotropy=1.0 both H and V sigma are equal -- isotropic, same as before.
        float3 halo = CrtHalo(texcoord).rgb;
        // Only add halation where the current pixel is darker than the halo
        // This prevents bright areas from blooming into themselves
        float cur_luma  = dot(c, float3(0.2126, 0.7152, 0.0722));
        float halo_luma = dot(halo, float3(0.2126, 0.7152, 0.0722));
        float gate      = saturate(halo_luma - cur_luma);
        c += halo * gate * crt_halation_strength;
    }
    #endif

    // -- Interference: rolling scanlines and animated chromatic ghosting --


    // -- Glow (tight + wide dual-scale bloom) --
    #if ENABLE_GLOW
    if (crt_glow_strength > 0.001 || crt_glow_wide_strength > 0.001)
    {
        float2 glow_uv = texcoord;
        if (crt_glow_strength > 0.001)
        {
            float3 glow = CrtGlow(glow_uv).rgb;
            c += crt_glow_strength * glow;
        }
        if (crt_glow_wide_strength > 0.001)
        {
            float3 wide_glow = CrtGlowWide(glow_uv).rgb;
            c += crt_glow_wide_strength * wide_glow;
        }
    }
    #endif // ENABLE_GLOW

    return c;
}
#if CRT_POST
float grain_poisson_sigma(float luma, float intensity)
{
    float poisson_shape = luma * (1.0 - luma);
    return (intensity * intensity * 0.35) * (poisson_shape * 4.0);
}
float grain_voronoi(float2 pos, float cell_size, uint frame_salt)
{
    float2 cell    = floor(pos / cell_size);
    float  min_d   = 1e9;
    float  val     = 0.0;

    // Check 3x3 neighbourhood of cells
    for (int cy = -1; cy <= 1; cy++)
    for (int cx = -1; cx <= 1; cx++)
    {
        float2 nc     = cell + float2(cx, cy);
        uint   seed   = grain_uhash(grain_uhash(uint(nc.y * 1031.0 + 7919.0))
                      + uint(nc.x * 3571.0 + 5003.0) + frame_salt);
        // Jitter seed point within cell
        float2 jitter = float2(grain_unorm1(seed),
                               grain_unorm1(grain_uhash(seed))) * 0.5 + 0.5;
        float2 sp     = (nc + jitter) * cell_size;
        float  d      = length(pos - sp);
        if (d < min_d)
        {
            min_d = d;
            // Value from seed -- Gaussian distributed for realistic amplitude
            float u1 = grain_unorm1(grain_uhash(seed + 1u));
            float u2 = grain_unorm1(grain_uhash(seed + 2u));
            val = sqrt(-2.0 * log(max(u1, 1e-6))) * cos(6.2831853 * u2);
        }
    }
    return val;
}
float3 grain_emulsion(float2 pos, float cell_scale, uint frame_salt, float intensity, float luma)
{
    float amp = grain_poisson_sigma(luma, intensity);

    // Layer cell sizes -- ratios from typical colour negative emulsions
    // Multipliers kept close to 1.0 so cell_scale directly controls visible size
    float sz_b = cell_scale * 1.9; // blue: coarsest (top emulsion layer)
    float sz_g = cell_scale * 1.4; // green: mid layer
    float sz_r = cell_scale * 1.0; // red: finest (deepest layer)

    float gR = grain_voronoi(pos, sz_r, frame_salt)           * amp;
    float gG = grain_voronoi(pos, sz_g, frame_salt + 0x1111u) * amp;
    float gB = grain_voronoi(pos, sz_b, frame_salt + 0x2222u) * amp;

    // Highlight residual: small floor so highlights never go completely grain-free
    // Physically represents maximum silver density at Dmax
    float hl_floor = intensity * intensity * 0.015 * luma * luma;
    uint  hl_seed  = grain_uhash(grain_uhash(uint(pos.y)*7919u + uint(pos.x)) + frame_salt);
    float hl_noise = grain_unorm1(hl_seed) * 2.0 - 1.0;
    float hl_grain = hl_noise * hl_floor;

    return float3(gR + hl_grain, gG + hl_grain, gB + hl_grain);
}
#define GWP 15.0
float3 grain_hdr(float3 c) { float w=1+rcp(1e-6+GWP); return c/(w-c); }
float3 grain_sdr(float3 c) { float w=1+rcp(1e-6+GWP); return w*c*rcp(1+c); }
float2 grain_unorm2(uint u) { return asfloat((uint2(u<<7u,u>>9u)&0x7FFF80u)|0x3F800000u)-1.0; }
float2 grain_next2(inout uint r) { r=grain_uhash(r); return grain_unorm2(r); }
float  grain_next1(inout uint r) { r=grain_uhash(r); return grain_unorm1(r); }
float3 grain_bm3(float3 u) { float2 d; sincos(u.x*6.2831853,d.y,d.x); d*=sqrt(-2.0*log(u.z)); return float3(d.x,d.y,d.x*d.y); }
#define genc(x)  (1.14374*(-0.126893*(x)+sqrt((x))))
float2 grain_boxmuller_2d(float2 u) { float2 d; sincos(u.x*6.2831853,d.y,d.x); d*=sqrt(-2.0*log(max(1.0-u.y,1e-6))); return d; }
float3 crt_linear_to_dtucs(float3 lin)
{
    // Step 1: linear RGB (Rec.709) -> XYZ D65
    static const float3x3 M_709_XYZ = float3x3(
        0.4124564, 0.3575761, 0.1804375,
        0.2126729, 0.7151522, 0.0721750,
        0.0193339, 0.1191920, 0.9503041
    );
    float3 XYZ = mul(M_709_XYZ, max(lin, 0.0));

    // Step 2: XYZ -> xyY
    float sum = XYZ.x + XYZ.y + XYZ.z + 1e-9;
    float x = XYZ.x / sum;
    float y = XYZ.y / sum;
    float Y = XYZ.y;

    // Step 3: dt UCS L* (brightness) -- Michaelis-Menten response
    // Fitted on Munsell dataset: L = Y^(1/2) with adjustment
    // Simplified form from darktable source
    float L = (Y > 0.0) ? pow(max(Y, 0.0), 0.4285714) : 0.0; // ~Y^(3/7)

    // Step 4: M (chroma in UCS) and h (hue)
    // dt UCS uses a custom opponent colour transform fitted to perceptual data
    // Linearised Munsell chroma: M = sqrt((x-xn)^2 + (y-yn)^2) * scale
    // xn, yn = D65 white point in xy
    float xn = 0.3127, yn = 0.3290;
    float u = (x - xn);
    float v = (y - yn);
    float M = sqrt(u*u + v*v) * 15.932; // fitted scale from darktable
    float h = atan2(v, u);

    return float3(L, M, h);
}
float3 crt_dtucs_to_linear(float3 LMh)
{
    float L = LMh.x;
    float M = LMh.y;
    float h = LMh.z;

    // Reconstruct xyY
    float xn = 0.3127, yn = 0.3290;
    float u = cos(h) * M / 15.932;
    float v = sin(h) * M / 15.932;
    float x = u + xn;
    float y = v + yn;

    // Reconstruct Y from L (inverse Michaelis-Menten simplified)
    float Y = (L > 0.0) ? pow(max(L, 0.0), 2.3333333) : 0.0; // ~L^(7/3)

    // xyY -> XYZ
    float3 XYZ;
    XYZ.y = Y;
    XYZ.x = (y > 0.0001) ? x * Y / y : 0.0;
    XYZ.z = (y > 0.0001) ? (1.0 - x - y) * Y / y : 0.0;

    // XYZ -> linear Rec.709
    static const float3x3 M_XYZ_709 = float3x3(
         3.2404542, -1.5371385, -0.4985314,
        -0.9692660,  1.8760108,  0.0415560,
         0.0556434, -0.2040259,  1.0572252
    );
    return max(mul(M_XYZ_709, max(XYZ, 0.0)), 0.0);
}
float3 crt_gamut_expand_dtucs(float3 lin, float strength, float neutral, float skin)
{
    float3 LMh = crt_linear_to_dtucs(lin);
    float  L   = LMh.x;
    float  M   = LMh.y;
    float  h   = LMh.z;

    // Neutral protection
    float neutral_mask = saturate((M - neutral*0.5) / max(neutral*0.5+0.01, 0.01));
    neutral_mask = neutral_mask * neutral_mask;

    // Skin tone protection -- in UCS hue space, skin is near orange (~-0.5 to 0.1 rad)
    float skin_dist = saturate(1.0 - abs(h + 0.2) / 0.5);
    float skin_mask = 1.0 - skin_dist * skin;

    // Helmholtz-Kohlrausch: colourful colours appear brighter, so reduce expansion
    // for very bright colours to avoid perceptual over-saturation
    float hk_weight = saturate(1.0 - L * L * 0.8);

    float expand_mask = neutral_mask * skin_mask * hk_weight;
    float new_M = M * (1.0 + strength * expand_mask);

    return max(crt_dtucs_to_linear(float3(L, new_M, h)), 0.0);
}
float3 crt_linear_to_ictcp(float3 c)
{
    // Convert linear Rec.709 -> linear Rec.2020 primaries
    static const float3x3 M709_2020 = float3x3(
        0.6274040, 0.3292820, 0.0433140,
        0.0690970, 0.9195400, 0.0113630,
        0.0163916, 0.0880132, 0.8955952
    );
    float3 c2020 = mul(M709_2020, max(c, 0.0));

    // LMS matrix (Rec.2020 to LMS)
    static const float3x3 M_LMS = float3x3(
         1688.0/4096.0, 2146.0/4096.0,  262.0/4096.0,
          683.0/4096.0, 2951.0/4096.0,  462.0/4096.0,
           99.0/4096.0,  309.0/4096.0, 3688.0/4096.0
    );
    // Clamp to non-negative before the PQ encode below: pow() with a negative
    // base returns NaN in HLSL, and a single NaN propagates to a garbage pixel.
    // Rec.709 input is in-gamut for Rec.2020 so this is normally a no-op, but
    // it matches the guarding already used by the Oklab and dtUCS paths and
    // removes the dependency on upstream passes happening to clamp first.
    float3 lms = max(mul(M_LMS, c2020), 0.0);

    // PQ EOTF (simplified -- maps linear to 0-1 PQ range for ICtCp)
    float m1 = 0.1593017578125;
    float m2 = 78.84375;
    float c1 = 0.8359375;
    float c2 = 18.8515625;
    float c3 = 18.6875;
    float3 lms_pq;
    lms_pq.r = pow(max((c1 + c2*pow(lms.r/10000.0, m1)) / (1.0 + c3*pow(lms.r/10000.0, m1)), 0.0), m2);
    lms_pq.g = pow(max((c1 + c2*pow(lms.g/10000.0, m1)) / (1.0 + c3*pow(lms.g/10000.0, m1)), 0.0), m2);
    lms_pq.b = pow(max((c1 + c2*pow(lms.b/10000.0, m1)) / (1.0 + c3*pow(lms.b/10000.0, m1)), 0.0), m2);

    // ICtCp matrix
    static const float3x3 M_ICTCP = float3x3(
         0.5000,  0.5000,  0.0000,
         1.6138, -3.3235,  1.7097,
         4.3781, -4.2460, -0.1321
    );
    return mul(M_ICTCP, lms_pq);
}
float3 crt_ictcp_to_linear(float3 ictcp)
{
    static const float3x3 M_ICTCP_INV = float3x3(
        1.0000,  0.0086,  0.1111,
        1.0000, -0.0086, -0.1111,
        1.0000,  0.5600, -0.3206
    );
    float3 lms_pq = mul(M_ICTCP_INV, ictcp);
    lms_pq = saturate(lms_pq);

    // Inverse PQ
    float m1 = 0.1593017578125;
    float m2 = 78.84375;
    float c1 = 0.8359375;
    float c2 = 18.8515625;
    float c3 = 18.6875;
    float3 lms;
    lms.r = 10000.0 * pow(max(pow(lms_pq.r, 1.0/m2) - c1, 0.0) / (c2 - c3*pow(lms_pq.r, 1.0/m2)), 1.0/m1);
    lms.g = 10000.0 * pow(max(pow(lms_pq.g, 1.0/m2) - c1, 0.0) / (c2 - c3*pow(lms_pq.g, 1.0/m2)), 1.0/m1);
    lms.b = 10000.0 * pow(max(pow(lms_pq.b, 1.0/m2) - c1, 0.0) / (c2 - c3*pow(lms_pq.b, 1.0/m2)), 1.0/m1);

    // LMS back to Rec.2020
    static const float3x3 M_LMS_INV = float3x3(
         3.43661,  -2.50645,   0.06984,
        -0.79133,   1.98360,  -0.19227,
        -0.02594,  -0.09893,   1.12487
    );
    float3 c2020 = mul(M_LMS_INV, lms);

    // Rec.2020 back to Rec.709
    static const float3x3 M2020_709 = float3x3(
         1.6605,  -0.5876,  -0.0728,
        -0.1246,   1.1329,  -0.0083,
        -0.0182,  -0.1006,   1.1187
    );
    return max(mul(M2020_709, max(c2020, 0.0)), 0.0);
}
float3 crt_gamut_expand_ictcp(float3 lin, float strength, float neutral, float skin)
{
    // Normalise to nits for ICtCp. scRGB defines 1.0 = 80 nits (SDR reference
    // white), so 80 is the correct scale for Pipeline 1 and a reasonable
    // assumption for Pipeline 0/2 linearised signals.
    float3 ictcp = crt_linear_to_ictcp(lin * 80.0);
    float  I     = ictcp.x;
    float  ct    = ictcp.y;
    float  cp    = ictcp.z;
    float  chroma = sqrt(ct*ct + cp*cp);

    // Neutral protection
    float neutral_mask = saturate((chroma - neutral*0.005) / max(neutral*0.005+0.0001, 0.0001));
    neutral_mask = neutral_mask * neutral_mask;

    // Skin tone protection (ICtCp hue ~0.05-0.12 rad from Ct axis toward Cp)
    float hue = atan2(cp, ct);
    float skin_dist = saturate(1.0 - abs(hue - 0.5) / 0.35);
    float skin_mask = 1.0 - skin_dist * skin;

    // Luminance-weighted: bright content expands less (H-K correction)
    // I in ICtCp is ~0.5 at 100 nits. Scale boost inversely with I.
    float lum_weight = saturate(1.0 - I * 1.5);

    float expand_mask = neutral_mask * skin_mask * lum_weight;
    float boost = 1.0 + strength * expand_mask;
    float new_chroma = chroma * boost;

    float2 ctcp_dir = (chroma > 0.00001) ? float2(ct, cp) / chroma : float2(1.0, 0.0);
    float3 ictcp_exp = float3(I, ctcp_dir * new_chroma);
    return max(crt_ictcp_to_linear(ictcp_exp) / 80.0, 0.0);
}
static const float3x3 CRT_OKLAB_M1 = float3x3(
    0.4122214708, 0.5363325363, 0.0514459929,
    0.2119034982, 0.6806995451, 0.1073969566,
    0.0883024619, 0.2817188376, 0.6299787005
);
static const float3x3 CRT_OKLAB_M2 = float3x3(
     0.2104542553,  0.7936177850, -0.0040720468,
     1.9779984951, -2.4285922050,  0.4505937099,
     0.0259040371,  0.7827717662, -0.8086757660
);
float3 crt_linear_to_oklab(float3 c)
{
    float3 lms = mul(CRT_OKLAB_M1, max(c, 0.0));
    lms = pow(max(lms, 0.0), 0.333333);
    return mul(CRT_OKLAB_M2, lms);
}
static const float3x3 CRT_OKLAB_M2_INV = float3x3(
    1.0000000000,  0.3963377774,  0.2158037573,
    1.0000000000, -0.1055613458, -0.0638541728,
    1.0000000000, -0.0894841775, -1.2914855480
);
static const float3x3 CRT_OKLAB_M1_INV = float3x3(
     4.0767416621, -3.3077115913,  0.2309699292,
    -1.2684380046,  2.6097574011, -0.3413193965,
    -0.0041960863, -0.7034186147,  1.7076147010
);
float3 crt_oklab_to_linear(float3 lab)
{
    float3 lms = mul(CRT_OKLAB_M2_INV, lab);
    lms = lms * lms * lms;
    return mul(CRT_OKLAB_M1_INV, max(lms, 0.0));
}
float3 crt_gamut_expand_oklab(float3 lin, float strength, float neutral, float skin)
{
    float3 lab   = crt_linear_to_oklab(lin);
    float  chroma = sqrt(lab.y*lab.y + lab.z*lab.z);

    float neutral_mask = saturate((chroma - neutral*0.1) / max(neutral*0.1+0.01, 0.01));
    neutral_mask = neutral_mask * neutral_mask;

    float hue_angle = atan2(lab.z, lab.y) * 57.29578;
    float skin_dist = saturate(1.0 - abs(hue_angle - 30.0) / 25.0);
    float skin_mask = 1.0 - skin_dist * skin;

    float expand_mask = neutral_mask * skin_mask;
    float boost = 1.0 + strength * expand_mask * (1.0 + chroma * 2.0);
    float new_chroma = chroma * boost;

    float2 ab_dir = (chroma > 0.0001) ? float2(lab.y, lab.z) / chroma : float2(1.0, 0.0);
    return max(crt_oklab_to_linear(float3(lab.x, ab_dir * new_chroma)), 0.0);
}
float3 crt_apply_chroma_ceiling(float3 linear_orig, float3 linear_expanded, float ceiling)
{
    if (ceiling < 0.001) return linear_expanded; // 0 = no ceiling, passthrough

    float3 lab_orig = crt_linear_to_oklab(linear_orig);
    float3 lab_exp  = crt_linear_to_oklab(linear_expanded);

    float chroma_orig = sqrt(lab_orig.y*lab_orig.y + lab_orig.z*lab_orig.z);
    float chroma_exp  = sqrt(lab_exp.y*lab_exp.y   + lab_exp.z*lab_exp.z);

    if (chroma_exp < 0.0001) return linear_expanded;

    // Map ceiling [0,1] to a chroma threshold above the original
    // At ceiling=0: threshold very high (no limiting)
    // At ceiling=1: threshold = original chroma (no expansion at all)
    // Range in between gives partial limiting
    float threshold = lerp(chroma_orig + 0.35, chroma_orig, ceiling);
    threshold = max(threshold, chroma_orig); // never below original

    // Smooth compressor: values below threshold pass through, above are compressed
    float chroma_limited = threshold * (1.0 - exp(-chroma_exp / max(threshold, 0.001)));
    // Ensure output chroma never falls below original
    chroma_limited = max(chroma_limited, chroma_orig);

    float scale = chroma_limited / chroma_exp;
    lab_exp.y *= scale;
    lab_exp.z *= scale;

    return max(crt_oklab_to_linear(lab_exp), 0.0);
}
float3 crt_gamut_expand(float3 linear_in)
{
    float3 result;
    if (crt_gamut_expand_method == 2)
        result = crt_gamut_expand_dtucs(linear_in,
                 crt_gamut_expand_strength,
                 crt_gamut_expand_neutral,
                 crt_gamut_expand_skin);
    else if (crt_gamut_expand_method == 1)
        result = crt_gamut_expand_ictcp(linear_in,
                 crt_gamut_expand_strength,
                 crt_gamut_expand_neutral,
                 crt_gamut_expand_skin);
    else
        result = crt_gamut_expand_oklab(linear_in,
                 crt_gamut_expand_strength,
                 crt_gamut_expand_neutral,
                 crt_gamut_expand_skin);

    return crt_apply_chroma_ceiling(linear_in, result, crt_gamut_expand_ceiling);
}
#define PQ_m2 78.84375
#define PQ_c1 0.8359375
#define PQ_c2 18.8515625
#define PQ_c3 18.6875
#define PQ_m1 0.1593017578125
float3 soop_pq_to_linear(float3 x, float content_peak_nits)
{
    float3 xpow = pow(max(x, 0.0), 1.0 / PQ_m2);
    float3 num  = max(xpow - PQ_c1, 0.0);
    float3 den  = max(PQ_c2 - PQ_c3 * xpow, 1e-10);
    float  S    = 20375.99 * pow(content_peak_nits, -0.995);
    return pow(num / den, 1.0 / PQ_m1) * S;
}
float3 soop_linear_to_pq(float3 x, float content_peak_nits)
{
    float  S       = 0.003789 * content_peak_nits;
    float3 x_sc    = x * S;
    float3 Y       = clamp(x_sc / 80.0, 0.0, 1.0);
    float3 Ym1     = pow(Y, PQ_m1);
    float3 num     = PQ_c1 + PQ_c2 * Ym1;
    float3 den     = 1.0 + PQ_c3 * Ym1;
    return pow(num / den, PQ_m2);
}
float crt_corner_mask(float2 texcoord)
{
    float ar  = float(BUFFER_WIDTH) / float(BUFFER_HEIGHT);
    float2 aspect = float2(1.0, ar);

    // Remap texcoord to [0,1] centred, then take absolute value -> [0, 0.5]
    float2 pos = abs(2.0 * (texcoord - 0.5));

    // Border: adds uniform edge shadow on all four sides
    float b = crt_corner_border * 0.05 + 0.0005;
    // Aspect-correct the vertical border contribution
    pos.y = pos.y + b * (aspect.y - 1.0);

    // Corner radius: must be at least as large as border to avoid artefacts
    float2 crn = max(crt_corner_size.xx, 2.0 * b + 0.0015);

    // Distance into corner region (aspect corrected)
    float2 crp = max(pos - (1.0 - crn * aspect), 0.0) / aspect;
    float  cd  = sqrt(dot(crp, crp));

    // Blend the corner geometry into the position
    pos = max(pos, 1.0 - crn + cd);

    // Smooth mask: 1.0 inside, 0.0 outside, with border transition
    float res = lerp(1.0, 0.0, smoothstep(1.0 - b, 1.0, sqrt(max(pos.x, pos.y))));

    // Power curve controls sharpness of the edge/corner
    return pow(res, crt_corner_intensity);
}
#if ENABLE_SHARPEN
void crt_sharpen_PS(
    in  float4 position : SV_Position,
    in  float2 texcoord : TEXCOORD0,
    out float4 color    : SV_Target)
{
    // Disabled: discard leaves the back buffer as it is -- no read, no write.
    if (crt_sharpen_strength < 0.001)
        { color = CrtPrev(texcoord); return; }
    float2 px = CrtPixelSize;

    float3 c  = CrtPrev(texcoord).rgb;
    float3 n  = CrtPrev(texcoord + float2( 0.0, -px.y)).rgb;
    float3 s  = CrtPrev(texcoord + float2( 0.0,  px.y)).rgb;
    float3 e  = CrtPrev(texcoord + float2( px.x, 0.0)).rgb;
    float3 w  = CrtPrev(texcoord + float2(-px.x, 0.0)).rgb;

    // Diagonal neighbours for more accurate local contrast estimate.
    // AMD full CAS uses 8-neighbour min/max for the weight computation
    // while keeping the 4-axis sharpening kernel -- better on diagonal edges.
    float3 ne = CrtPrev(texcoord + float2( px.x, -px.y)).rgb;
    float3 nw = CrtPrev(texcoord + float2(-px.x, -px.y)).rgb;
    float3 se = CrtPrev(texcoord + float2( px.x,  px.y)).rgb;
    float3 sw = CrtPrev(texcoord + float2(-px.x,  px.y)).rgb;

    // Local min/max across all 8 neighbours (contrast estimate only)
    float3 mn = min(min(min(n, s), min(e, w)), min(min(ne, nw), min(se, sw)));
    float3 mx = max(max(max(n, s), max(e, w)), max(max(ne, nw), max(se, sw)));
    mn = min(mn, c); mx = max(mx, c);

    // Normalise range relative to peak -- makes weight HDR-safe
    // Without normalisation, large HDR values cause near-division-by-zero
    float3 rng      = mx - mn;
    float3 rng_norm = rng / (mx + 0.001);
    float3 w_cas    = -rng_norm / (4.0 - 2.0 * rng_norm + 0.001);
    w_cas = clamp(w_cas, -crt_sharpen_clamp, 0.0) * crt_sharpen_strength;

    float3 sharpened = (c + (n + s + e + w) * w_cas) / (1.0 + 4.0 * w_cas);
    color = float4(max(sharpened, 0.0), 1.0);
}
float4 PSCrtSharpen(float4 pos : SV_Position) : SV_Target
{
    float4 o;
    crt_sharpen_PS(pos, pos.xy * CrtPixelSize, o);
    return o;
}
#endif
#if ENABLE_SCANLINE_SOFTEN
void crt_soften_PS(
    in  float4 position : SV_Position,
    in  float2 texcoord : TEXCOORD0,
    out float4 color    : SV_Target)
{
    // Disabled: discard leaves the back buffer as it is -- no read, no write.
    if (crt_soften_strength < 0.001)
        { color = CrtPrev(texcoord); return; }
    float py    = CrtPixelSize.y;
    float sigma = crt_soften_strength * 0.8;

    // 5-tap asymmetric vertical gaussian
    // Slight downward bias matches CRT beam sweep direction
    float w0 = gauss(0.0,  sigma);
    float w1 = gauss(1.0,  sigma);
    float w2 = gauss(2.0,  sigma);
    float w1d = gauss(0.8, sigma); // slightly closer below
    float w2d = gauss(1.8, sigma);

    float3 c =
        CrtPrev(texcoord + float2(0.0, -2.0*py)).rgb * w2  +
        CrtPrev(texcoord + float2(0.0, -1.0*py)).rgb * w1  +
        CrtPrev(texcoord).rgb                         * w0  +
        CrtPrev(texcoord + float2(0.0,  1.0*py)).rgb * w1d +
        CrtPrev(texcoord + float2(0.0,  2.0*py)).rgb * w2d;

    float wsum = w2 + w1 + w0 + w1d + w2d;
    color = float4(c / wsum, 1.0);
}
float4 PSCrtSoften(float4 pos : SV_Position) : SV_Target
{
    float4 o;
    crt_soften_PS(pos, pos.xy * CrtPixelSize, o);
    return o;
}
#endif
#if ENABLE_GRAIN
void crt_grain_merged_PS(
    in  float4 position  : SV_Position,
    in  float2 texcoord  : TEXCOORD0,
    out float4 out_delta : SV_Target)
{
    // Off: GrainDiffuse discards too, so the delta is never read.
    if (crt_grain_intensity <= 0.001)
        { out_delta = CrtPrev(texcoord); return; }

    float3 c  = CrtPrev(texcoord).rgb;
    float3 delta = float3(0.5, 0.5, 0.5);

    if (crt_grain_intensity > 0.001)
    {
        uint2  p   = uint2(texcoord * float2(BUFFER_WIDTH, BUFFER_HEIGHT));
        float3 cl  = glin(c);
        float  luma_g = dot(cl, float3(0.2126, 0.7152, 0.0722));

        uint rng = grain_uhash(grain_uhash(p.y) + p.x);
        if (crt_grain_animate) rng += FRAMECOUNT;

        float  shadow_blend = lerp(crt_grain_shadows, 1.0, saturate(luma_g * 8.0));
        float3 cl_grained;

        float  luma_e = dot(cl, float3(0.2126, 0.7152, 0.0722));

        if (crt_grain_emulsion)
        {
            uint   fsalt = crt_grain_animate ? FRAMECOUNT : 0u;
            float  cell_scale = 0.25 + crt_grain_size * 1.0;
            float2 pos        = float2(p);
            float3 gn_e  = grain_emulsion(pos, cell_scale,
                                          fsalt, crt_grain_intensity, luma_e);
            gn_e *= shadow_blend;
            cl_grained = grain_hdr(cl);
            cl_grained += gn_e;
            cl_grained = grain_sdr(cl_grained);
        }
        else
        {
            // Standard Gaussian grain (METEOR-style)
            float3 u3 = float3(grain_next2(rng), grain_next1(rng));
            float3 gn = grain_bm3(u3);
            float  poisson_amp = grain_poisson_sigma(luma_e, crt_grain_intensity);
            poisson_amp *= shadow_blend;

            if (crt_grain_colour)
            {
                cl_grained = grain_hdr(cl);
                cl_grained += gn * poisson_amp;
                cl_grained = grain_sdr(cl_grained);
            }
            else
            {
                float grey  = dot(cl, float3(0.2126, 0.7152, 0.0722));
                float grey3 = grain_hdr(grey.xxx).x;
                grey3 += gn.x * poisson_amp;
                grey3 = grain_sdr(grey3.xxx).x;
                float orig  = dot(cl, float3(0.2126, 0.7152, 0.0722));
                cl_grained  = (orig > 0.0001) ? cl * (grey3 / orig) : cl;
            }
        }
        delta = (genc(cl_grained) - c) * 0.5 + 0.5;
    }

    // Temporal grain correlation: blend previous frame's grain into static areas.
    // Real film grain has temporal coherence -- the silver halide crystals are
    // physically fixed on the film stock, so static scenes see consistent grain.
    // Motion mask from prev1_tex: large difference = motion = fresh grain.
    out_delta = float4(delta, 1.0);
}
float4 PSCrtGrainRaw(float4 pos : SV_Position) : SV_Target
{
    float4 o;
    crt_grain_merged_PS(pos, pos.xy * CrtPixelSize, o);
    return o;
}
#endif
#if ENABLE_GRAIN
void crt_grain_diffuse_PS(
    in  float4 position : SV_Position,
    in  float2 texcoord : TEXCOORD0,
    out float4 color    : SV_Target)
{
    // Off: leave the back buffer as it is (same threshold as GrainMerged).
    if (crt_grain_intensity <= 0.001)
        { color = CrtPrev(texcoord); return; }

    // Clean image: GrainMerged renders to a texture, so the back buffer still
    // holds exactly what it read.
    float3 clean = CrtPrev(texcoord).rgb;

    float2 px = CrtPixelSize;
    float2 fc = texcoord * float2(BUFFER_WIDTH, BUFFER_HEIGHT);

    // Sigma: tight diffusion -- the organic look comes from Morton spatial correlation,
    // not from blurring. Keep sigma small so individual grain pixels stay sharp.
    float sigma = lerp(0.3, 1.5, crt_grain_size);

    float3 diffused_delta;

    if (crt_grain_size < 0.001)
    {
        diffused_delta = CrtGrainRaw(texcoord).rgb;
    }
    else
    {
        float3 result = 0.0;
        float  wsum   = 0.0;

        for (int x = -1; x <= 1; x++)
        for (int y = -1; y <= 1; y++)
        {
            float2 tp  = float2(float(x), float(y));
            float2 uv  = texcoord + tp * px;
            float3 d   = CrtGrainRaw(uv).rgb;

            uint2  pi  = uint2(uv * float2(BUFFER_WIDTH, BUFFER_HEIGHT));
            uint   rng = grain_uhash(grain_uhash(pi.y) + pi.x);
            if (crt_grain_animate) rng += FRAMECOUNT;
            float2 rand01 = float2(rng & 31u, rng >> 5u) / 32.0;
            float2 bm     = grain_boxmuller_2d(rand01) * sigma;
            float2 offs   = tp + bm;

            float w = exp(-dot(offs, offs));

            result += d * w;
            wsum   += w;
        }
        diffused_delta = result / max(wsum, 1e-5);
    }

    // Decode delta from [0,1] storage back to signed [-0.5, 0.5]
    float3 grain = (diffused_delta - 0.5) * 2.0;

    // Add diffused grain delta to clean pre-grain image
    color = float4(saturate(clean + grain), 1.0);
}
float4 PSCrtGrain(float4 pos : SV_Position) : SV_Target
{
    float4 o;
    crt_grain_diffuse_PS(pos, pos.xy * CrtPixelSize, o);
    return o;
}
#endif
#if ENABLE_NOISE_FLOOR
void crt_noise_floor_PS(
    in  float4 position : SV_Position,
    in  float2 texcoord : TEXCOORD0,
    out float4 color    : SV_Target)
{
    // Disabled: discard leaves the back buffer as it is -- no read, no write.
    if (crt_noise_floor <= 0.001)
        { color = CrtPrev(texcoord); return; }

    float3 c = CrtPrev(texcoord).rgb;
    {
        uint2 noise_px   = uint2(texcoord * float2(BUFFER_WIDTH, BUFFER_HEIGHT)
                           / max(crt_noise_floor_scale, 1.0));
        // Slow temporal variation: changes every 4 frames so it drifts
        // rather than being static, but slower than film grain (every frame)
        uint  frame_slow = (FRAMECOUNT / 4u) * 0x9E3779B9u;
        uint  noise_seed = noise_px.x * 1973u + noise_px.y * 9277u + frame_slow;
        float noise_val  = (grain_unorm1(grain_uhash(noise_seed)) - 0.5) * 2.0;
        float luma_n    = dot(c, float3(0.299, 0.587, 0.114));
        float dark_gate = saturate(1.0 - luma_n * 2.0); // fades above ~50% luma
        c = saturate(c + noise_val * crt_noise_floor * dark_gate);
    }
    color = float4(c, 1.0);
}
float4 PSCrtNoiseFloor(float4 pos : SV_Position) : SV_Target
{
    float4 o;
    crt_noise_floor_PS(pos, pos.xy * CrtPixelSize, o);
    return o;
}
#endif
#if ENABLE_GAMUT_EXPAND
void crt_gamut_expand_PS(
    in  float4 position : SV_Position,
    in  float2 texcoord : TEXCOORD0,
    out float4 color    : SV_Target)
{
    // Disabled: discard leaves the back buffer as it is -- no read, no write.
    if (crt_gamut_expand_strength < 0.001)
        { color = CrtPrev(texcoord); return; }

    float3 c = CrtPrev(texcoord).rgb;

    float3 linear_c;

#if PIPELINE == 0
    linear_c = pow(max(c, 0.0), 2.2);
    linear_c = crt_gamut_expand(linear_c);
    c = pow(max(linear_c, 0.0), 1.0 / 2.2);
    c = saturate(c);

#elif PIPELINE == 1
    // scRGB: already linear, may exceed 1.0 -- preserve HDR headroom
    linear_c = crt_gamut_expand(c);
    c = max(linear_c, 0.0);

#elif PIPELINE == 2
    // HDR10: decode PQ, expand, re-encode PQ
    c = soop_pq_to_linear(c, crt_soop_hdr10_peak_nits);
    c = crt_gamut_expand(c);
    c = soop_linear_to_pq(max(c, 0.0), crt_soop_hdr10_peak_nits);
#endif

    color = float4(c, 1.0);
}
float4 PSCrtGamut(float4 pos : SV_Position) : SV_Target
{
    float4 o;
    crt_gamut_expand_PS(pos, pos.xy * CrtPixelSize, o);
    return o;
}
#endif
#if ENABLE_CORNER_ROUND
void crt_corner_round_PS(
    in  float4 position : SV_Position,
    in  float2 texcoord : TEXCOORD0,
    out float4 color    : SV_Target)
{
    // Disabled: discard leaves the back buffer as it is -- no read, no write.
    if (crt_corner_size < 0.001 && crt_corner_border < 0.001)
        { color = CrtPrev(texcoord); return; }

    float3 screen = CrtPrev(texcoord).rgb;

    float mask = crt_corner_mask(texcoord);
    // Multiply the image by the mask -- darkens edges/corners, black outside
    color = float4(screen * mask, 1.0);
}
float4 PSCrtCorner(float4 pos : SV_Position) : SV_Target
{
    float4 o;
    crt_corner_round_PS(pos, pos.xy * CrtPixelSize, o);
    return o;
}
#endif
#endif
