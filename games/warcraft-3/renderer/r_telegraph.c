#include "renderer/r_game.h"
#include "renderer/r_shader.h"
#include <math.h>

/* Ground telegraphs: World of Warcraft style encounter warnings (boss slams,
 * breaths, charges) draped over the terrain through the splat tiles.
 *
 * The look follows WoW's red ground decals (the 1.12 AoE reticle's
 * Spell-Shadow-Unacceptable and the later encounter warnings): a translucent
 * red fill that is faint in the middle and denser toward the edge, a bright
 * rim with a soft outer glow, and a lighter fill that sweeps from the origin
 * to the edge over the cast, its front a bright band. The rim pulses faster as
 * the hit nears; at the hit the shape flashes white-orange and fades.
 *
 * Everything is procedural in the fragment shader: the splat UVs are the world
 * XY (uv_mins/uv_maxs equal the rect), so one shader draws every shape at any
 * size with no texture, and a rotated cone or line needs no rotated geometry. */

typedef struct {
    mat4_t viewProjection; /* splat_shader_t prefix: the splat path fills these two */
    mat4_t model;
    vec4_t shape;  /* x shape, y radius (line: length), z inner (ring hole, line width, cone arc), w growth scale */
    vec4_t place;  /* xy origin, zw facing (cos, sin) */
    vec4_t anim;   /* x progress, y alpha, z flash, w seconds */
    vec4_t color;  /* rim colour */
} telegraphState_t;

typedef struct {
    shaderProg_t prog;
    telegraphState_t state;
} telegraphProg_t;

#define SHADER_TYPE telegraphState_t
static const shader_desc_t sd_telegraph = {
    .Name = "telegraph",
    .Uniforms = {
        UNIFORM(viewProjection, UT_FLOAT_MAT4, PRECISION_HIGH),
        UNIFORM(model,          UT_FLOAT_MAT4, PRECISION_HIGH),
        UNIFORM(shape,          UT_FLOAT_VEC4, PRECISION_HIGH),
        UNIFORM(place,          UT_FLOAT_VEC4, PRECISION_HIGH),
        UNIFORM(anim,           UT_FLOAT_VEC4, PRECISION_HIGH),
        UNIFORM(color,          UT_FLOAT_VEC4, PRECISION_HIGH),
    },
    .Attributes = {
        ATTRIB(position, attrib_position, UT_FLOAT_VEC3),
        ATTRIB(texcoord, attrib_texcoord, UT_FLOAT_VEC2),
        ATTRIB(color,    attrib_color,    UT_COLOR),
    },
    .Shared = {
        SHARED(texcoord, UT_FLOAT_VEC2),
        SHARED(color,    UT_COLOR),
    },
    .VertexBody =
        "vec4 vert() {\n"
        "  v_texcoord = a_texcoord;\n"
        "  v_color = a_color;\n"
        "  return u_viewProjection * u_model * vec4(a_position, 1.0);\n"
        "}\n",
    .FragmentBody =
        /* Signed distance to the edge in world units (negative inside) and the
         * fill coordinate g: 0 where the cast fill starts, 1 at the far edge. */
        "vec2 shape_field(vec2 p) {\n"
        "  float R = u_shape.y * u_shape.w;\n"
        "  float r = length(p);\n"
        "  if (u_shape.x < 0.5) {\n"                         /* circle */
        "    return vec2(r - R, r / R);\n"
        "  } else if (u_shape.x < 1.5) {\n"                  /* ring: the hole is safe */
        "    float hole = u_shape.z * u_shape.w;\n"
        "    return vec2(max(r - R, hole - r), (r - hole) / max(R - hole, 1.0));\n"
        "  } else if (u_shape.x < 2.5) {\n"                  /* cone along +x */
        "    float a = abs(atan(p.y, p.x)) - 0.5 * u_shape.z;\n"
        "    float side = r * sin(clamp(a, -1.5707963, 1.5707963));\n"
        "    return vec2(max(r - R, side), r / R);\n"
        "  }\n"
        "  float half_w = 0.5 * u_shape.z * u_shape.w;\n"      /* line along +x */
        "  float d = max(max(-p.x, p.x - R), abs(p.y) - half_w);\n"
        "  return vec2(d, p.x / R);\n"
        "}\n"
        "vec4 frag() {\n"
        "  vec2 rel = v_texcoord - u_place.xy;\n"
        "  vec2 p = vec2(dot(rel, u_place.zw), dot(rel, vec2(-u_place.w, u_place.z)));\n"
        "  vec2 f = shape_field(p);\n"
        "  float d = f.x;\n"
        "  float g = clamp(f.y, 0.0, 1.0);\n"
        "  float progress = u_anim.x, flash = u_anim.z, t = u_anim.w;\n"
        "  float inside = 1.0 - smoothstep(-1.5, 1.5, d);\n"
        /* Body: faint in the middle, denser toward the edge, with slow ripples running outward. */
        "  float ripple = 0.5 + 0.5 * sin(g * u_shape.y / 26.0 - t * 3.0);\n"
        "  float body = mix(0.16, 0.42, g * g) + 0.05 * ripple;\n"
        /* Cast fill: covers the shape from g = 0 up to the progress front. */
        "  float filled = 1.0 - smoothstep(progress - 0.04, progress, g);\n"
        "  float front = exp(-pow((g - progress) * u_shape.y / 7.0, 2.0)) * step(0.001, progress) * step(progress, 0.999);\n"
        /* Rim: a bright line on the edge and a soft glow just outside it; it pulses faster near the hit. */
        "  float pulse = 0.80 + 0.20 * sin(t * mix(5.0, 18.0, progress * progress));\n"
        "  float rim = exp(-pow(d / 3.5, 2.0));\n"
        "  float glow = exp(-max(d, 0.0) / 9.0) * step(0.0, d);\n"
        "  vec3 rimColor = u_color.rgb;\n"
        "  vec3 deep = u_color.rgb * vec3(0.75, 0.45, 0.45);\n"
        "  vec3 hot = mix(u_color.rgb, vec3(1.0, 0.72, 0.30), 0.55);\n"
        "  vec3 col = mix(deep, u_color.rgb, g);\n"
        "  float a = body * inside;\n"
        "  col = mix(col, hot, filled * inside * 0.55);\n"
        "  a += filled * inside * 0.26;\n"
        "  col = mix(col, vec3(1.0, 0.85, 0.55), front * inside * 0.8);\n"
        "  a += front * inside * 0.45;\n"
        "  float edge = max(rim, glow * 0.45) * pulse;\n"
        "  col = mix(col, mix(rimColor, vec3(1.0, 0.8, 0.6), 0.30 * rim), edge);\n"
        "  a = max(a, edge * 0.95);\n"
        /* Impact: the whole shape flashes bright, then everything fades (u_anim.y). */
        "  col = mix(col, vec3(1.0, 0.66, 0.32), flash * 0.70);\n"
        "  a = max(a, (inside * 0.75 + rim) * flash * 0.85);\n"
        "  return vec4(col, clamp(a, 0.0, 1.0) * u_anim.y * v_color.a);\n"
        "}\n",
};
#undef SHADER_TYPE

static telegraphProg_t telegraph_shader;
static bool telegraph_shader_loaded;

void R_ShutdownTelegraphs(void) {
    if (telegraph_shader_loaded) R_DeleteShader(&telegraph_shader.prog);
    telegraph_shader_loaded = false;
}

/* The world box around a shape, glow included, in its own frame (+x forward). */
static void R_TelegraphLocalBounds(renderTelegraph_t const *t, float scale, vec2_t *lo, vec2_t *hi) {
    float const margin = 48.0f;
    float const r = t->radius * scale;
    switch (t->shape) {
        case TELEGRAPH_LINE:
            *lo = (vec2_t){ -margin, -0.5f * t->inner * scale - margin };
            *hi = (vec2_t){ r + margin, 0.5f * t->inner * scale + margin };
            break;
        case TELEGRAPH_CONE: {
            float const half = MIN(0.5f * t->inner, 3.14159265f);
            float const side = half >= 1.5707963f ? r : r * sinf(half);
            *lo = (vec2_t){ (half >= 1.5707963f ? -r * MIN(1.0f, -cosf(half)) : 0.0f) - margin, -side - margin };
            *hi = (vec2_t){ r + margin, side + margin };
            break;
        }
        default:
            *lo = (vec2_t){ -r - margin, -r - margin };
            *hi = (vec2_t){ r + margin, r + margin };
            break;
    }
}

void R_DrawTelegraphs(void) {
    if (!tr.world || !tr.viewDef.num_telegraphs || !tr.viewDef.telegraphs ||
        (tr.viewDef.rdflags & RDF_NOWORLDMODEL)) {
        return;
    }
    if (!telegraph_shader_loaded) {
        memset(&telegraph_shader, 0, sizeof(telegraph_shader));
        R_LoadShader(&sd_telegraph, R_ShaderDefines(false), &telegraph_shader);
        telegraph_shader_loaded = true;
    }
    FOR_LOOP(i, tr.viewDef.num_telegraphs) {
        renderTelegraph_t const *t = &tr.viewDef.telegraphs[i];
        float const c = cosf(t->angle), s = sinf(t->angle);
        /* Appearing, the shape grows from 85 to 100 hundredths of its size. */
        float const grow = MIN(1.0f, t->time / 0.15f);
        float const scale = 0.85f + 0.15f * grow * grow * (3.0f - 2.0f * grow);
        vec2_t lo, hi, mins = { 1e9f, 1e9f }, maxs = { -1e9f, -1e9f };
        if (t->alpha <= 0.0f || t->radius <= 0.0f) continue;
        R_TelegraphLocalBounds(t, scale, &lo, &hi);
        FOR_LOOP(k, 4) {
            float const lx = (k & 1) ? hi.x : lo.x, ly = (k & 2) ? hi.y : lo.y;
            float const wx = t->origin.x + lx * c - ly * s, wy = t->origin.y + lx * s + ly * c;
            mins.x = MIN(mins.x, wx); mins.y = MIN(mins.y, wy);
            maxs.x = MAX(maxs.x, wx); maxs.y = MAX(maxs.y, wy);
        }
        telegraph_shader.state.shape = (vec4_t){ (float)t->shape, t->radius, t->inner, scale };
        telegraph_shader.state.place = (vec4_t){ t->origin.x, t->origin.y, c, s };
        telegraph_shader.state.anim = (vec4_t){ t->progress, t->alpha, t->flash, t->time };
        telegraph_shader.state.color = (vec4_t){ t->color.r / 255.0f, t->color.g / 255.0f, t->color.b / 255.0f, 1.0f };
        {
            /* uv = world XY: the splat lerps uv_mins..uv_maxs across the rect, v downward. */
            vec2_t const uv0 = { mins.x, maxs.y }, uv1 = { maxs.x, mins.y };
            R_BeginSplatBatch(R_SPLAT_SHADER(&telegraph_shader));
            R_AddRectSplatUV(&MAKE(rectSplatParams_t, .mins = &mins, .maxs = &maxs,
                .uv_mins = &uv0, .uv_maxs = &uv1, .texture = tr.texture[TEX_WHITE],
                .shader = R_SPLAT_SHADER(&telegraph_shader), .color = COLOR32_WHITE));
            R_EndSplatBatch();
        }
    }
}
