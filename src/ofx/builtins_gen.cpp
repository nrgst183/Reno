// Built-in media generators with VEGAS Pro 18's identifiers, parameters and choices (Solid Color, Color Gradient, Checkerboard,
// Noise Texture, Test Pattern, Titles & Text). See builtins.h.
#include "builtins_impl.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {
namespace {

const char* const kGroup = "Reno Built-in";

// VEGAS's generators write 8-bit pictures by truncating (0.25 -> 63, 0.5 -> 127), not by rounding.
inline float truncLevel(double v) { return (float)(std::floor(v * 255.0 + 1e-4) / 255.0); }
inline void putGen(float* p, const Color& c)
{
    const double a = c.a;
    p[0] = truncLevel(c.r * a); p[1] = truncLevel(c.g * a); p[2] = truncLevel(c.b * a); p[3] = truncLevel(a);
}
// blend of two colours in premultiplied values (a transparent colour fades in without turning dark)
inline void putGenMix(float* p, const Color& a, const Color& b, double t)
{
    p[0] = truncLevel(lerp(a.r * a.a, b.r * b.a, t));
    p[1] = truncLevel(lerp(a.g * a.a, b.g * b.a, t));
    p[2] = truncLevel(lerp(a.b * a.a, b.b * b.a, t));
    p[3] = truncLevel(lerp(a.a, b.a, t));
}

// ------------------------------------------------------------------ Solid Color
void solidColor(Ctx& c)
{
    const Color col = c.color("Color");
    float p[4];
    putGen(p, col);
    for (int k = 0; k < c.out.w * c.out.h; ++k) std::memcpy(&c.out.px[(size_t)k * 4], p, sizeof p);
}

// ------------------------------------------------------------------ Color Gradient
// VEGAS has a gradient editor with control points; Reno has two colours, a centre, a size and a direction: Linear runs from Color 1 to Color 2
// across 'Size' of the frame along the angle; Elliptical and Rectangular run outwards from the centre (angle turns the shape, Aspect stretches it).
void colorGradient(Ctx& c)
{
    const int type = c.i("GradientType");
    const Color a = c.color("Color1"), b = c.color("Color2");
    double cx, cy;
    c.xy("Center", cx, cy);
    const double size = std::max(1e-4, c.d("Size"));
    const double ang = c.d("AspectRatioAngle") * kPi / 180.0;
    const double aspect = std::max(0.01, c.d("Aspect"));
    const int W = c.out.w, H = c.out.h;
    const double ca = std::cos(ang), sa = std::sin(ang);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double dx = ((x + 0.5) - cx * W) / H, dy = ((H - (y + 0.5)) - cy * H) / H;
            const double u = dx * ca + dy * sa, v = -dx * sa + dy * ca;
            double t;
            if (type == 0) t = u / size + 0.5;
            else if (type == 1) t = std::sqrt(u * u / (aspect * aspect) + v * v) / (size * 0.5);
            else t = std::max(std::fabs(u) / aspect, std::fabs(v)) / (size * 0.5);
            put(c.out.at(x, y), mix(a, b, clampd(t, 0, 1)));
        }
    });
}

// ------------------------------------------------------------------ Checkerboard
// Tiles of Color 1 and Color 2; tile width/height are fractions of the frame (Square: as tall as wide). Edge blending softens the change over
// that fraction of a tile (Proportional: vertical as horizontal). Grid offset moves the grid by fractions of a tile.
double tileWave(double u, double blend)
{
    const double i = std::floor(u), f = u - i;
    const double base = (((long long)i) & 1) ? 1.0 : 0.0;
    if (blend <= 1e-6) return base;
    const double hb = blend * 0.5;
    if (f < hb) return lerp(1 - base, base, 0.5 + f / blend);
    if (f > 1 - hb) return lerp(1 - base, base, 0.5 + (1 - f) / blend);
    return base;
}

// how much of the interval [u0, u1] (in tiles) lies in odd tiles: the exact coverage of a pixel across a hard edge
double oddCover(double u0, double u1)
{
    auto F = [](double u) {
        const double n = std::floor(u);
        return std::floor(n / 2) + ((((long long)n) & 1) ? (u - n) : 0.0);
    };
    return u1 > u0 ? (F(u1) - F(u0)) / (u1 - u0) : 0.0;
}

void checkerboard(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    // tiles are never smaller than a pixel
    const double tw = std::max(1.0 / std::max(1, W), c.d("TileDimensionsWidth"));
    const bool square = c.b("TileDimensionsSquare");
    const double th = std::max(1.0 / std::max(1, H), square ? tw * W / std::max(1, H) : c.d("TileDimensionsHeight"));
    const double bh = clampd(c.d("EdgeBlendingHorizontal"), 0, 1);
    const double bv = c.b("EdgeBlendingProportional") ? bh : clampd(c.d("EdgeBlendingVertical"), 0, 1);
    const Color a = c.color("Color1"), b = c.color("Color2");
    // the grid offset is a fraction of the whole frame (1 = one frame width/height), not of a tile
    const double ox = c.d("GridOffsetX"), oy = c.d("GridOffsetY");
    forRows(H, W, [&](int y) {
        const double v = (((y + 0.5) / H) - oy) / th;
        // hard edges are anti-aliased by the exact area a pixel covers; soft ones are read at the pixel centre
        const double sy = bv <= 1e-6 ? oddCover(((double)y / H - oy) / th, ((double)(y + 1) / H - oy) / th) : tileWave(v, bv);
        for (int x = 0; x < W; ++x) {
            const double u = (((x + 0.5) / W) - ox) / tw;
            const double sx = bh <= 1e-6 ? oddCover(((double)x / W - ox) / tw, ((double)(x + 1) / W - ox) / tw) : tileWave(u, bh);
            // VEGAS mixes in two 8-bit steps: Color 1 -> Color 2 by the horizontal wave (and the same with colours swapped), each truncated to a
            // level, then those two by the vertical wave (softened exclusive or)
            float* o = c.out.at(x, y);
            const double pa[4] = {a.r * a.a, a.g * a.a, a.b * a.a, a.a}, pb[4] = {b.r * b.a, b.g * b.a, b.b * b.a, b.a};
            for (int k = 0; k < 4; ++k) {
                const double m1 = std::floor(lerp(pa[k], pb[k], sx) * 255.0 + 1e-4);
                const double m2 = std::floor(lerp(pb[k], pa[k], sx) * 255.0 + 1e-4);
                o[k] = (float)(std::floor(m1 + (m2 - m1) * sy + 1e-4) / 255.0);
            }
        }
    });
}

// ------------------------------------------------------------------ Noise Texture
// VEGAS's gradient noise: Perlin's improved noise in 3D (x, y, progress as z) with a fixed permutation and 256 unit gradients (copied from the plug-in), summed over the layers, each doubling the
// frequency and weighing 'Grain' times the previous (last fractional layer by its fraction; layers under 0.01 left out); doubles cut to 8-bit levels, exact. Styles differ in how the sum becomes
// the 0..1 mix: 0 and 1 directly (1 folds each layer to its absolute value), 2 to 5 a smooth wave ("marble"; 5 without folding is "contour"), 6 another wave ("rings").
const unsigned char kNtPerm[256] = {
    209, 59, 152, 243, 49, 141, 92, 83, 155, 106, 77, 35, 164, 111, 206, 68,
    80, 91, 60, 159, 153, 56, 110, 157, 247, 126, 3, 44, 32, 98, 71, 79,
    118, 163, 224, 74, 36, 236, 66, 133, 40, 104, 37, 140, 234, 88, 75, 240,
    120, 214, 144, 150, 165, 197, 11, 238, 115, 69, 86, 251, 183, 82, 57, 239,
    207, 43, 142, 117, 33, 114, 84, 87, 180, 127, 48, 89, 189, 223, 215, 54,
    53, 73, 5, 24, 38, 174, 172, 194, 248, 12, 108, 147, 97, 138, 25, 219,
    137, 129, 233, 101, 119, 124, 195, 186, 130, 19, 131, 241, 212, 232, 217, 218,
    14, 16, 151, 20, 55, 156, 9, 158, 143, 63, 26, 46, 136, 210, 148, 182,
    171, 149, 202, 160, 30, 96, 177, 178, 205, 29, 116, 103, 222, 135, 52, 185,
    184, 225, 61, 17, 252, 90, 23, 64, 179, 72, 216, 250, 204, 169, 162, 213,
    176, 105, 191, 4, 181, 188, 139, 27, 208, 161, 229, 246, 123, 132, 2, 6,
    13, 227, 51, 220, 199, 196, 134, 102, 62, 107, 93, 70, 28, 198, 78, 201,
    21, 95, 146, 235, 109, 244, 42, 125, 193, 168, 41, 187, 122, 31, 7, 175,
    99, 18, 22, 145, 94, 65, 81, 34, 249, 245, 47, 154, 230, 192, 221, 203,
    231, 237, 8, 67, 128, 253, 242, 113, 121, 112, 15, 85, 45, 50, 254, 58,
    190, 166, 0, 1, 255, 100, 76, 211, 167, 226, 39, 10, 170, 228, 173, 200,
};
const float kNtGrad[256][3] = {
    {-0.632021844f, -0.6313169f, 0.449430019f},
    {0.954781651f, -0.147500291f, -0.258138657f},
    {0.353760034f, 0.29955107f, -0.886071622f},
    {-0.472004771f, 0.186954647f, 0.861544788f},
    {0.825473964f, 0.434834719f, -0.359877139f},
    {0.756674886f, -0.383278221f, 0.529661179f},
    {0.572191834f, -0.494837999f, 0.654012144f},
    {0.180834562f, -0.406724811f, 0.895474017f},
    {-0.514184892f, 0.35044539f, -0.782816589f},
    {-0.25449267f, 0.685079396f, -0.682568431f},
    {-0.945111632f, 0.00959634036f, 0.326606691f},
    {0.238931537f, -0.129385144f, -0.962377846f},
    {0.0390419215f, -0.970326364f, -0.238626271f},
    {0.537169456f, -0.123363823f, 0.83440423f},
    {-0.523458719f, -0.210350543f, -0.825677693f},
    {-0.674951375f, 0.161260083f, 0.720024884f},
    {0.76430124f, 0.602141023f, -0.2308027f},
    {0.0482929684f, 0.677604616f, -0.733839095f},
    {0.910870016f, 0.162033543f, 0.379553556f},
    {0.352846593f, 0.400435567f, 0.845665812f},
    {-0.0847442597f, -0.545654058f, 0.833714664f},
    {-0.29420504f, -0.930896401f, -0.216506734f},
    {0.01492415f, -0.934082389f, 0.356745481f},
    {0.44505623f, 0.894269884f, 0.0469726995f},
    {0.147731811f, -0.904640257f, -0.399751693f},
    {-0.969118059f, -0.142977074f, -0.200917035f},
    {-0.631905496f, 0.681078613f, -0.369901836f},
    {0.41505608f, 0.907163441f, 0.0691590309f},
    {0.743749201f, -0.462011307f, -0.483096749f},
    {0.357444078f, 0.763350487f, 0.538079679f},
    {0.559143007f, -0.17122969f, 0.811196327f},
    {0.184999004f, -0.532037914f, 0.826263309f},
    {0.374915779f, -0.514591873f, 0.771124721f},
    {0.335779399f, -0.913899362f, 0.228123218f},
    {-0.434410691f, 0.74835664f, 0.501247942f},
    {0.0321342014f, 0.746676803f, 0.664410412f},
    {0.904275715f, 0.423433036f, 0.0546805598f},
    {0.54545939f, 0.535597742f, 0.644677579f},
    {0.862437487f, -0.167532355f, 0.477634192f},
    {-0.847685218f, 0.279369891f, 0.450979143f},
    {-0.260036379f, -0.18358016f, -0.94798702f},
    {-0.7492221f, -0.589094162f, -0.302711517f},
    {-0.141397998f, 0.989916623f, 0.00847536977f},
    {0.171842396f, -0.0104292696f, -0.985069215f},
    {-0.412385255f, 0.563717544f, -0.715654135f},
    {-0.640807867f, -0.0373211391f, -0.766793668f},
    {-0.227398083f, -0.0550100096f, 0.972246885f},
    {-0.486526698f, 0.168237016f, 0.857314527f},
    {-0.6273036f, 0.26115793f, 0.733680189f},
    {0.104019716f, -0.702412844f, 0.704127848f},
    {0.311345071f, 0.392836839f, 0.865299642f},
    {0.594492197f, 0.451181322f, -0.665593266f},
    {-0.7867468f, 0.302277625f, 0.538198531f},
    {0.867355764f, -0.462026954f, 0.185000181f},
    {0.522615075f, -0.314318866f, 0.792513192f},
    {0.670457065f, -0.041927889f, 0.740762651f},
    {0.793827176f, 0.456744671f, -0.401525706f},
    {0.764932275f, 0.0134084905f, 0.643971026f},
    {0.94707191f, 0.145256609f, 0.286278367f},
    {0.985738695f, 0.0305222496f, 0.1654917f},
    {0.228059441f, -0.301938504f, 0.925646782f},
    {0.857159495f, 0.0802579075f, 0.508759379f},
    {0.849859774f, -0.376360774f, 0.368905067f},
    {0.630335271f, 0.762257338f, 0.147109404f},
    {0.759676337f, 0.628785789f, -0.165892646f},
    {0.36033392f, -0.659636378f, -0.659575045f},
    {-0.499917805f, 0.0709266067f, 0.86316371f},
    {-0.874369264f, 0.483862847f, 0.0368120298f},
    {0.893995047f, 0.444875896f, -0.0534634702f},
    {-0.0775214732f, -0.696994662f, -0.712873638f},
    {-0.304014415f, 0.946924806f, -0.104444697f},
    {0.291053534f, -0.954101384f, 0.0705574229f},
    {-0.44104141f, -0.344178289f, 0.828868985f},
    {-0.465939313f, -0.851628423f, -0.240061596f},
    {0.607420802f, -0.149186835f, 0.780245602f},
    {0.342999518f, -0.189209029f, -0.920082211f},
    {0.618270576f, -0.766479015f, -0.173929706f},
    {0.826916993f, 0.558918476f, 0.061794281f},
    {0.956870496f, 0.261228621f, -0.127116323f},
    {-0.492487609f, -0.198158786f, 0.84746027f},
    {0.350317061f, 0.857484818f, -0.376826018f},
    {0.000688460015f, 0.998100758f, -0.0615991689f},
    {0.715239763f, 0.270071f, 0.644587934f},
    {-0.0374345593f, 0.894938052f, 0.444617301f},
    {0.430314451f, -0.788734734f, 0.439006954f},
    {0.457574517f, 0.777794659f, -0.430884033f},
    {-0.283702523f, -0.956883252f, 0.0623477884f},
    {0.0125136003f, -0.966774702f, -0.25532344f},
    {0.381114572f, 0.789853871f, -0.480502427f},
    {-0.879241884f, -0.204854131f, -0.430079639f},
    {0.186066225f, -0.196513712f, 0.962684691f},
    {0.00578350993f, -0.858432651f, 0.512893558f},
    {-0.0455651805f, -0.276799738f, 0.959846735f},
    {0.168110088f, -0.422939032f, 0.890427768f},
    {0.791633487f, 0.2763578f, -0.544924557f},
    {-0.120776422f, 0.958845675f, 0.256959379f},
    {-0.115582488f, 0.0356028005f, 0.992659688f},
    {0.415114552f, -0.607368767f, -0.677335322f},
    {-0.501600087f, 0.764224768f, 0.405410618f},
    {0.917584538f, 0.0799244195f, -0.389423549f},
    {0.924654543f, 0.230255276f, -0.303309292f},
    {-0.534012854f, 0.00584636023f, -0.845456183f},
    {0.334698617f, 0.934495389f, -0.121224277f},
    {0.105373412f, -0.854051113f, 0.509404659f},
    {0.0811083466f, -0.995676875f, -0.0452658497f},
    {0.686470628f, -0.679825902f, 0.258059651f},
    {-0.401877582f, 0.107448317f, -0.909367561f},
    {-0.617365718f, -0.17219387f, -0.767599404f},
    {0.0408716016f, 0.159160227f, -0.986406446f},
    {-0.335335255f, 0.892566681f, -0.301454961f},
    {-0.369363368f, 0.879991055f, 0.298641145f},
    {-0.268170029f, 0.215565354f, 0.93894434f},
    {0.234693244f, -0.403113842f, 0.884544194f},
    {0.670546472f, 0.551739812f, -0.495934129f},
    {0.853802681f, 0.5204795f, 0.01104852f},
    {0.67592895f, -0.468275011f, -0.569068074f},
    {0.524117351f, -0.745441198f, 0.41184774f},
    {0.145945415f, 0.62643671f, 0.765687287f},
    {0.528777063f, -0.464192837f, -0.71057719f},
    {-0.729022443f, 0.0357923694f, -0.683553338f},
    {-0.976433992f, 0.184612229f, 0.111781299f},
    {0.924077094f, -0.338425726f, 0.177621782f},
    {-0.487800121f, -0.0513456203f, 0.871443987f},
    {-0.672495484f, -0.0840662792f, 0.73531121f},
    {-0.945778191f, 0.0572668016f, 0.319725364f},
    {0.837264776f, 0.197199985f, 0.50999999f},
    {0.49635464f, 0.8610515f, -0.110554643f},
    {-0.405636907f, -0.229653075f, 0.88471365f},
    {-0.823903739f, 0.382124037f, 0.418525696f},
    {-0.192206055f, 0.454568028f, -0.869726837f},
    {-0.704855025f, -0.292509913f, 0.646233261f},
    {-0.304066628f, -0.631276608f, -0.71346575f},
    {0.667836368f, -0.669048071f, -0.326143235f},
    {0.888780713f, 0.324475527f, -0.323704213f},
    {-0.076699771f, 0.91109401f, -0.404999614f},
    {0.606342852f, -0.0118827801f, 0.795114577f},
    {-0.428599954f, 0.468034595f, 0.772816658f},
    {-0.304361165f, 0.887989998f, -0.344729006f},
    {-0.934200644f, -0.197730586f, 0.296937168f},
    {-0.729938984f, 0.676512659f, -0.0975681394f},
    {-0.796298444f, -0.445507258f, 0.409184575f},
    {0.275760531f, 0.10296724f, -0.95569551f},
    {0.780700862f, 0.0920220986f, -0.618092299f},
    {-0.0725622922f, 0.198201835f, 0.97747159f},
    {0.955705583f, -0.246177047f, 0.161319166f},
    {0.125384614f, -0.700275958f, 0.702774704f},
    {0.541255414f, 0.6702106f, 0.507799566f},
    {0.762872219f, 0.221342236f, 0.607481241f},
    {-0.396637142f, -0.513441324f, -0.760958016f},
    {0.0826625973f, 0.732429266f, 0.675806403f},
    {0.0738804266f, 0.270366013f, -0.959918618f},
    {0.462454081f, -0.180588216f, 0.868057609f},
    {-0.630181313f, 0.184952125f, 0.754098237f},
    {-0.232619256f, 0.951409936f, 0.201760575f},
    {0.0603179485f, -0.997192621f, 0.0443698987f},
    {-0.242930859f, 0.165536955f, -0.955814838f},
    {-0.675700545f, -0.427481234f, 0.60057348f},
    {0.559653103f, -0.571622491f, 0.600030005f},
    {-0.420353651f, 0.8397578f, -0.343670905f},
    {-0.326143295f, -0.106156088f, 0.939340889f},
    {-0.914475501f, 0.049554389f, -0.401595563f},
    {-0.98488903f, 0.166598573f, 0.0473133884f},
    {-0.448724627f, -0.183433264f, -0.874641836f},
    {0.740719974f, 0.613282263f, 0.274260312f},
    {0.764341712f, 0.528626561f, -0.369236469f},
    {0.365992099f, 0.255655348f, -0.894812942f},
    {-0.464309782f, 0.0414738916f, 0.884701312f},
    {-0.0377787091f, -0.427899152f, -0.903036594f},
    {0.317592472f, -0.948078036f, 0.0168290194f},
    {0.197550356f, -0.735577345f, 0.647996843f},
    {-0.440842748f, -0.76814425f, 0.464340627f},
    {0.778120637f, 0.541816115f, -0.317747891f},
    {0.800476611f, 0.410247684f, 0.436959803f},
    {0.0599538982f, 0.863843858f, -0.50017941f},
    {0.550501168f, 0.635104239f, -0.541840494f},
    {0.447680801f, 0.443350673f, -0.776544869f},
    {-0.0296798106f, 0.555365622f, -0.831076562f},
    {-0.430254817f, 0.122667469f, -0.894334078f},
    {0.578779817f, -0.649321198f, 0.493351698f},
    {0.0513922907f, 0.69785434f, 0.714393556f},
    {-0.189053297f, 0.734385133f, -0.651872218f},
    {-0.982912719f, 0.00511948997f, 0.184001267f},
    {0.212485507f, 0.550620258f, 0.807259142f},
    {-0.164192602f, -0.594946861f, 0.786815703f},
    {0.411994994f, -0.781010926f, 0.469342113f},
    {0.397296935f, -0.252945602f, -0.882141531f},
    {-0.162458748f, -0.983092546f, -0.0844761133f},
    {0.0434848405f, -0.0790772736f, 0.995919585f},
    {0.980742455f, 0.167313159f, 0.100749582f},
    {-0.255049318f, -0.214634016f, -0.942805469f},
    {0.416761547f, 0.526148975f, -0.741267145f},
    {0.243968189f, 0.390909046f, 0.887507558f},
    {0.0845535621f, 0.971838892f, 0.219954103f},
    {0.482301503f, -0.515166223f, 0.708511889f},
    {-0.51476717f, -0.847400486f, 0.130104572f},
    {-0.276735663f, 0.829997361f, -0.484274358f},
    {0.465685278f, 0.148032546f, 0.872481287f},
    {-0.899321079f, 0.30203706f, -0.316220313f},
    {-0.0942182094f, 0.678336322f, -0.728685558f},
    {0.142340273f, -0.283733249f, 0.948279798f},
    {0.796658158f, -0.222710222f, 0.561903834f},
    {0.29420808f, 0.924680889f, -0.241674915f},
    {-0.939554989f, -0.291558444f, -0.179527104f},
    {-0.941163063f, -0.264440119f, 0.210436404f},
    {-0.869844973f, -0.299776405f, -0.391795784f},
    {0.844697714f, 0.367443413f, 0.389192909f},
    {0.660534918f, 0.167161331f, 0.731949925f},
    {-0.725935042f, 0.687546194f, 0.0172714796f},
    {-0.379518479f, 0.900496483f, 0.212301269f},
    {-0.663716972f, 0.705189347f, 0.249374866f},
    {0.481121778f, 0.662659526f, 0.573937476f},
    {0.842597604f, -0.202597126f, 0.498982728f},
    {0.911723018f, -0.400140733f, -0.0929980427f},
    {-0.678896785f, -0.661218941f, -0.319200218f},
    {-0.307136983f, -0.0264386795f, -0.951298058f},
    {-0.502329588f, -0.1042215f, -0.858372092f},
    {0.779143929f, -0.237307519f, -0.580189526f},
    {-0.0558312498f, 0.959195971f, -0.277175009f},
    {-0.374109924f, -0.30202055f, -0.876826882f},
    {0.921355963f, -0.320382982f, -0.220131636f},
    {0.740400374f, 0.634150565f, 0.22284621f},
    {-0.147570908f, 0.873220384f, 0.464444786f},
    {-0.983156681f, 0.12215741f, -0.135942921f},
    {0.655451179f, -0.661592245f, 0.364251941f},
    {-0.232852355f, 0.853592217f, 0.466004372f},
    {0.722922742f, 0.687986553f, -0.0636973083f},
    {0.0225846004f, 0.267963767f, -0.96316421f},
    {0.247242481f, 0.838057816f, -0.486343801f},
    {0.691602349f, -0.355716467f, -0.628611207f},
    {-0.562794268f, 0.703157246f, 0.434548646f},
    {0.895489156f, 0.31194663f, -0.317472309f},
    {0.267260879f, -0.777879953f, 0.568748176f},
    {-0.433485091f, 0.882578313f, 0.182060584f},
    {0.857562184f, -0.513672888f, 0.0269661006f},
    {0.766079724f, -0.364076525f, -0.529688656f},
    {-0.0497050807f, -0.936199844f, 0.347935766f},
    {0.757798254f, -0.506513536f, 0.411322057f},
    {-0.70532161f, 0.653744221f, 0.274116337f},
    {-0.224687144f, -0.592211545f, 0.773822427f},
    {0.906426668f, 0.401639611f, -0.130676165f},
    {0.0132026104f, 0.144351721f, 0.989438295f},
    {0.902996957f, 0.346173793f, 0.254480332f},
    {0.102231763f, -0.994726598f, -0.00823246967f},
    {0.783261061f, -0.612913966f, 0.104107819f},
    {-0.15579018f, -0.702284396f, -0.694641054f},
    {-0.10738679f, -0.576350093f, -0.81011641f},
    {0.959814548f, -0.159317777f, -0.231027797f},
    {-0.116715349f, 0.078335233f, 0.990071356f},
    {-0.991574645f, 0.0949250534f, 0.0881422907f},
    {0.274067521f, -0.01922171f, 0.961518407f},
    {0.138985708f, 0.976548493f, 0.164426148f},
    {0.409180582f, -0.912011504f, -0.0283960402f},
    {0.0600317903f, -0.507165253f, 0.859755516f},
    {-0.0720754489f, -0.075561069f, 0.994532943f},
    {-0.753437877f, -0.375127822f, 0.540009499f},
    {-0.643426239f, -0.192163929f, -0.74099648f},
};

// 16.16 fixed point and round-to-nearest as VEGAS gets from adding 2^52+2^51 and reading the low bits
inline int32_t ntRound(double v)
{
    v += 6755399441055744.0;
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    return (int32_t)(uint32_t)bits;
}
const double kNtHalf = 0.499999985;

// One layer of noise for one row: adds 'count' values with x step dx to out[]. The row start uses full interpolation; inside a cell the two x-end
// values move by their slopes (as VEGAS does; its rounding is part of the exact result).
void ntLayer(double x, double y, double z, double dx, int count, bool fold, double amp, double* out)
{
    amp *= 1.5151515151515151;
    const double X = x + 1000000.0, Y = y + 1000000.0, Z = z + 1000000.25;
    int32_t xi = ntRound(X - kNtHalf);
    const int32_t yi = ntRound(Y - kNtHalf), zi = ntRound(Z - kNtHalf);
    double xf = X - (double)xi;
    const double yf = Y - (double)yi, zf = Z - (double)zi;
    const int y0 = yi & 255, y1 = (yi + 1) & 255, z0 = zi & 255, z1 = (zi + 1) & 255;
    const int ha = kNtPerm[(kNtPerm[z0] + y0) & 255], hb = kNtPerm[(kNtPerm[z0] + y1) & 255];
    const int hc = kNtPerm[(kNtPerm[z1] + y0) & 255], hd = kNtPerm[(kNtPerm[z1] + y1) & 255];
    const double fy = ((yf * 6.0 - 15.0) * yf + 10.0) * ((yf * yf) * yf);
    const double fz = ((zf * 6.0 - 15.0) * zf + 10.0) * ((zf * zf) * zf);
    const double yf1 = yf - 1.0, zf1 = zf - 1.0;
    double l0 = 0, l1 = 0, s0 = 0, s1 = 0;
    auto cell = [&]() {
        const int x0 = xi & 255, x1 = (xi + 1) & 255;
        const int g000 = kNtPerm[(ha + x0) & 255], g100 = kNtPerm[(ha + x1) & 255], g010 = kNtPerm[(hb + x0) & 255], g110 = kNtPerm[(hb + x1) & 255];
        const int g001 = kNtPerm[(hc + x0) & 255], g101 = kNtPerm[(hc + x1) & 255], g011 = kNtPerm[(hd + x0) & 255], g111 = kNtPerm[(hd + x1) & 255];
        const double xf1 = xf - 1.0;
        auto dot = [](int g, double a, double b, double c) { const float* v = kNtGrad[g]; return ((double)v[1] * b + (double)v[0] * a) + (double)v[2] * c; };
        const double d000 = dot(g000, xf, yf, zf), d100 = dot(g100, xf1, yf, zf), d010 = dot(g010, xf, yf1, zf), d110 = dot(g110, xf1, yf1, zf);
        const double d001 = dot(g001, xf, yf, zf1), d101 = dot(g101, xf1, yf, zf1), d011 = dot(g011, xf, yf1, zf1), d111 = dot(g111, xf1, yf1, zf1);
        const double a00 = (d001 - d000) * fz + d000, a10 = (d101 - d100) * fz + d100;
        const double a01 = (d011 - d010) * fz + d010, a11 = (d111 - d110) * fz + d110;
        l0 = (a01 - a00) * fy + a00;
        l1 = (a11 - a10) * fy + a10;
        auto gx = [](int g) { return (double)kNtGrad[g][0]; };
        const double p00 = (gx(g001) - gx(g000)) * fz + gx(g000), p01 = (gx(g011) - gx(g010)) * fz + gx(g010);
        const double p10 = (gx(g101) - gx(g100)) * fz + gx(g100), p11 = (gx(g111) - gx(g110)) * fz + gx(g110);
        s0 = ((p01 - p00) * fy + p00) * dx;
        s1 = ((p11 - p10) * fy + p10) * dx;
    };
    cell();
    for (int i = 0; i < count; ++i) {
        const double f = ((xf * 6.0 - 15.0) * xf + 10.0) * ((xf * xf) * xf);
        double v = ((l1 - l0) * f + l0) * amp;
        if (fold) v = std::fabs(v);
        out[i] = v + out[i];
        xf += dx;
        if (xf < 1.0) {
            l0 += s0;
            l1 += s1;
        } else {
            const int32_t n = ntRound(xf - kNtHalf);
            xi += n;
            xf -= (double)n;
            cell();
        }
    }
}

void noiseTexture(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const int style = (int)clampd((double)c.i("NoiseStyle"), 0, 6);
    const double layers = c.d("NumberOfLayers");
    const Color c1 = c.color("Color1"), c2 = c.color("Color2");
    const bool swap = c.b("Swapcolors");
    const double fx = c.d("FrequencyX"), fy = c.d("FrequencyY");
    const double ox = c.d("OffsetX"), oy = c.d("OffsetY");
    const double z0 = c.d("OffsetprogressinDegrees");
    const double lo = c.d("NoiseParametersMin"), hi = c.d("NoiseParametersMax");
    const double bias = c.d("NoiseParametersBias"), amp = c.d("NoiseParametersAmplitude");
    const double falloff = c.d("NoiseParametersGrainAmplitudeFalloff");

    struct Style { int mode; bool fold; float p[5]; };
    static const Style kStyles[7] = {{0, false, {}},
                                     {0, true, {}},
                                     {1, true, {0.9f, 0.15f, 0.4f, 1.2f, 0.1f}},
                                     {1, true, {0.05f, 0.9f, 1.3f, 0.f, 0.f}},
                                     {1, true, {0.6f, 0.f, 0.2f, 0.5f, 0.1f}},
                                     {1, false, {1.0f, 0.f, 1.5f, 0.5f, 0.05f}},
                                     {2, false, {4.0f, 0, 0, 0, 0}}};
    const Style& st = kStyles[style];

    // the two colours in the float premultiplied form VEGAS mixes, in its B, G, R, A order (colour A is the one at mix 0)
    float k1[4] = {(float)(c1.a * c1.b), (float)(c1.a * c1.g), (float)(c1.a * c1.r), (float)c1.a};
    float k2[4] = {(float)(c2.a * c2.b), (float)(c2.a * c2.g), (float)(c2.a * c2.r), (float)c2.a};
    const float* ca = swap ? k2 : k1;
    const float* cb = swap ? k1 : k2;

    double w[16] = {1.0};
    const int nl = (int)clampd((double)ntRound(layers - kNtHalf), 0, 14);
    const double frac = layers - (double)nl;
    for (int i = 1; i <= nl; ++i) w[i] = falloff * w[i - 1];
    w[nl] *= frac;
    int used = 0;
    double sumW = 0;
    while (used <= nl && w[used] > 0.01) sumW += w[used++];

    double k, b;
    if (lo < hi) {
        k = amp / (hi - lo) * 65536.0;
        b = bias * 65536.0 - lo * k + 0.5;
    } else {
        k = 0;
        b = (((lo + hi) * 0.5) * amp + bias) * 65536.0 + 0.5;
    }
    if (st.mode == 0) {
        double s = sumW;
        if (!st.fold) {
            b = k * 0.5 + b;
            s = sumW + sumW;
        }
        k /= s;
    } else if (st.mode == 2) {
        k *= 0.5;
    }

    const double stepX = (1.0 / (double)(float)W) * fx, stepY = (1.0 / (double)(float)W) * fy;
    forRows(H, W, [&](int y) {
        std::vector<double> sum((size_t)W, 0.0);
        double xx = (ox + 0.0) * fx, yy = ((double)((float)y / (float)W) + oy) * fy, zz = z0, dd = stepX;
        for (int o = 0; o < used; ++o) {
            ntLayer(xx, yy, zz, dd, W, st.fold, w[o], sum.data());
            xx += xx; yy += yy; zz += zz; dd += dd;
        }
        double base = 0, p0dx = 0, p2s = 0;
        if (st.mode == 1) {
            const double p0 = st.p[0], p1 = st.p[1], p2 = st.p[2], p3 = st.p[3], p4 = st.p[4];
            p2s = p2 * sumW;
            p0dx = p0 * stepX;
            double t = -(((double)W * oy + (double)y) * (-stepY) * p1);
            if (p3 > 0) {
                double v = -(((oy - 1000.0) * (double)W + (double)y) * (-stepY) * p3);
                const double fr = v - (double)ntRound(v - kNtHalf);
                const double g = fr < 0.5 ? 1.0 - (fr * 4.0 - 1.0) * (fr * 4.0 - 1.0) : (fr * 4.0 - 3.0) * (fr * 4.0 - 3.0) - 1.0;
                t += p4 * g;
            }
            base = (double)W * ox * p0dx + t;
        }
        for (int x = 0; x < W; ++x) {
            const double n = sum[(size_t)x];
            double v;
            if (st.mode == 0) {
                v = k * n + b;
            } else if (st.mode == 1) {
                const double t = p2s * n + (double)x * p0dx + base;
                const double fr = t - (double)ntRound(t - kNtHalf);
                const double g = 1.0 - (fr + fr - 1.0) * (fr + fr - 1.0);
                v = g * k + b;
            } else {
                const double t = (n + 2.0) * ((double)st.p[0] * sumW);
                const double fr = t - (double)ntRound(t - kNtHalf);
                const double g = fr < 0.5 ? 2.0 - (fr * 4.0 - 1.0) * (fr * 4.0 - 1.0) : (fr * 4.0 - 3.0) * (fr * 4.0 - 3.0);
                v = g * k + b;
            }
            int32_t q = ntRound(v + 1.5e-8);
            if ((q | 0xffff) != 0xffff) q = q < 0 ? 0 : 0x10000;
            const double mix = (double)q * 1.52587890625e-05;
            float* o = c.out.at(x, y);
            // VEGAS keeps the bytes in B, G, R, A order; the mode 0 rounds to nearest, the waves truncate
            for (int ch = 0; ch < 4; ++ch) {
                double m = ((double)cb[ch] * mix + (double)ca[ch] * (1.0 - mix)) * 255.0;
                if (st.mode == 0) m += 0.5;
                const int level = (int)m;
                o[ch == 3 ? 3 : 2 - ch] = (float)((double)level / 255.0);
            }
        }
    });
}

// ------------------------------------------------------------------ Test Pattern
// Test pattern levels: Reno's frames are computer RGB, so black is 0 and white 1 (not studio levels).
void fillRect(Buf& b, double x0, double y0, double x1, double y1, const Color& col)
{
    const int X0 = (int)std::lround(x0 * b.w), X1 = (int)std::lround(x1 * b.w);
    const int Y0 = (int)std::lround(y0 * b.h), Y1 = (int)std::lround(y1 * b.h);
    for (int y = std::max(0, Y0); y < std::min(b.h, Y1); ++y)
        for (int x = std::max(0, X0); x < std::min(b.w, X1); ++x) put(b.at(x, y), col);
}

// Test Pattern: VEGAS's nine patterns in broadcast 8-bit levels (black 7, white 235, 75% bars 180, 16/25 pluge steps). The seven bars are
// floor(width/7) wide, the last taking the rest; the "porch" of framed patterns is the middle 80% of the frame (ends included, rows from the
// bottom); the Gradient uses the frame's own coordinates. Matches VEGAS Pro 18 at 320x180.
void testPattern(Ctx& c)
{
    Buf& o = c.out;
    const int type = c.i("TestPatternType");
    const int W = o.w, H = o.h;
    struct Rgb { int r, g, b; };
    const Rgb bars[7] = {{180, 180, 180}, {16, 180, 180}, {180, 180, 16}, {16, 180, 16}, {180, 16, 180}, {16, 16, 180}, {180, 16, 16}};
    const Rgb castle[7] = {{235, 7, 7}, {16, 16, 16}, {180, 7, 180}, {16, 16, 16}, {180, 180, 7}, {16, 16, 16}, {180, 180, 180}};
    const Rgb black = {7, 7, 7};
    const int stepsLvl[4] = {70, 125, 180, 235};            // the porch's four steps, darkest at the bottom
    const int barW = std::max(1, W / 7);
    auto setPx = [&](int x, int y, Rgb v) {
        float* p = o.at(x, y);
        p[0] = (float)(v.r / 255.0); p[1] = (float)(v.g / 255.0); p[2] = (float)(v.b / 255.0); p[3] = 1.f;
    };
    for (int y = 0; y < H; ++y) {
        const int yb = H - 1 - y;
        const bool rowIn = yb * 10 >= H && yb * 10 <= 9 * H;
        // which of the four steps this row belongs to (counted from the bottom of the porch area)
        int stepRow = 0;
        if (rowIn) stepRow = std::min(3, std::max(0, (int)std::ceil(4.0 * (yb - 0.1 * H) / (0.8 * H)) - 1));
        for (int x = 0; x < W; ++x) {
            const bool colIn = x * 10 >= W && x * 10 <= 9 * W;
            const int bar = std::min(6, x / barW);
            Rgb v = black;
            switch (type) {
            case 0: case 1:
                v = bars[bar];
                if (type == 1 && bar == 0) v = {235, 235, 235};
                break;
            case 2: {
                const double fx = x * 255.0 / W;
                v = {(int)std::floor((y + 1) * 255.0 / H), (int)std::floor(fx), (int)std::floor(255.0 - fx)};
                break;
            }
            case 3:                 // Pluge and porches
                if (rowIn) {
                    if (x * 5 >= W && x * 10 < W * 3) v = {16, 16, 16};
                    else if (x * 10 >= W * 3 && x * 10 < W * 4) v = {25, 25, 25};
                    else if (x * 10 >= W * 4 && x * 2 < W) v = {16, 16, 16};
                    else if (x * 2 >= W && colIn) v = {stepsLvl[stepRow], stepsLvl[stepRow], stepsLvl[stepRow]};
                }
                break;
            case 4:                 // Ramp
                if (rowIn && colIn) {
                    const int g = (int)std::floor(25.0 + (x - 0.1 * W) * 204.0 / (0.8 * W) + 0.5);
                    v = {g, g, g};
                }
                break;
            case 5: case 6: {       // SMPTE bars (NTSC / PAL)
                if (y * 3 < 2 * H) {
                    v = bars[bar];
                    if (type == 6 && bar == 0) v = {235, 235, 235};
                } else if (y * 4 < 3 * H) {
                    v = castle[bar];
                } else {
                    const int xl = barW * 5;
                    if (x >= xl) {
                        const int u = barW / 3;
                        if (x < xl + u) v = {7, 7, 7};
                        else if (x < xl + 2 * u) v = {16, 16, 16};
                        else if (x < xl + 3 * u) v = {25, 25, 25};
                        else v = {16, 16, 16};
                    } else if (type == 6) {
                        v = {16, 16, 16};
                    } else if (x < W / 6) v = {113, 40, 7};                // -I
                    else if (x < W * 2 / 6) v = {235, 235, 235};
                    else if (x < W * 3 / 6) v = {165, 7, 70};              // +Q
                    else v = {16, 16, 16};
                }
                break;
            }
            case 7: case 8:
                if (rowIn && colIn) {
                    const int k = std::min(3, (int)std::floor((x - 0.1 * W) * 4.0 / (0.8 * W)));
                    const int l = stepsLvl[3 - k];
                    v = type == 7 ? Rgb{l, l, l} : Rgb{7, l, l};
                }
                break;
            default:
                break;
            }
            setPx(x, y, v);
        }
    }
}

// ------------------------------------------------------------------ Titles & Text
// Text (VEGAS keeps rich text; plain text uses the Font settings) at Location (anchor of the block, y from the bottom), scaled, with outline, shadow and background. A 48 point font is an eighth of
// the frame height at Scale 1 (VEGAS's exact factor is unknown). VEGAS's Animation list is kept by name in the hidden AnimationName string ("_Bounce", as in its presets and projects) and played as
// Reno's own version: the block moved, scaled, squeezed and faded over the first second or so (Scroll and Scroll Left over ten seconds, Earthquake, Rough Day and Menace throughout);
// Title01 .. Title25 fade in.
const char* const kTextAnimations[] = {"None", "Action Flip", "Bounce", "Coming at You", "Double Flash Glow", "Drop Split", "Dropping Words",
                                       "Earthquake", "Fall Down", "Float and Pop", "Fly In", "Fly in from Right", "Jump", "Menace", "Popup",
                                       "Rolling Glow and Enlarge", "Rough Day", "Scroll", "Scroll Left", "Slide", "Slide Down", "Slide Left",
                                       "Slide Right", "Slide Up", "Speedy", "Twist In"};
const int kTextAnimationCount = (int)(sizeof kTextAnimations / sizeof kTextAnimations[0]);

// The animation's name without VEGAS's leading underscore, spaces for underscores ("_Fly_in_from_Right" -> "fly in from right").
std::string animationKey(const std::string& raw)
{
    std::string s;
    for (char ch : raw) s += ch == '_' ? ' ' : (char)std::tolower((unsigned char)ch);
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// Where the text block is at time t (seconds from the media start): moved by (dx, dy) pixels, scaled by (sx, sy) about its middle, its opacity.
// 'bx, by, bw, bh': the block at rest (pixels, from the top left), W x H the frame.
struct TextMotion { double dx = 0, dy = 0, sx = 1, sy = 1, opacity = 1; };
TextMotion textMotion(const std::string& anim, double t, int W, int H, double bx, double by, double bw, double bh)
{
    TextMotion m;
    const std::string a = animationKey(anim);
    if (a.empty() || a == "none") return m;
    const double D = 1.0;
    const double u = clampd(t / D, 0, 1);
    const double out = 1 - (1 - u) * (1 - u) * (1 - u);
    auto hash = [](double v) { const double x = std::sin(v * 12.9898) * 43758.5453; return x - std::floor(x); };
    const double frame = std::floor(t * 30.0);
    if (a == "fly in") m.dx = -(bx + bw + 4) * (1 - out);
    else if (a == "fly in from right") m.dx = (W - bx + 4) * (1 - out);
    else if (a == "slide" || a == "slide right") { m.dx = -0.25 * W * (1 - out); m.opacity = u; }
    else if (a == "slide left") { m.dx = 0.25 * W * (1 - out); m.opacity = u; }
    else if (a == "slide up") { m.dy = 0.25 * H * (1 - out); m.opacity = u; }
    else if (a == "slide down") { m.dy = -0.25 * H * (1 - out); m.opacity = u; }
    else if (a == "speedy") { const double f = clampd(t / 0.4, 0, 1); m.dx = -(bx + bw + 4) * (1 - f) * (1 - f); }
    else if (a == "fall down" || a == "drop split" || a == "dropping words") m.dy = -(by + bh + 4) * (1 - u * u) * (u < 1 ? 1 : 0);
    else if (a == "bounce") {
        const double fall = by + bh + 4;
        if (u < 0.4) m.dy = -fall * (1 - (u / 0.4) * (u / 0.4));
        else {
            const double b = (u - 0.4) / 0.6 * 3.0;             // three bounces
            const int k = std::min(2, (int)b);
            const double ph = b - k;
            m.dy = -fall * 0.25 / (1 << (2 * k)) * 4 * ph * (1 - ph);
        }
    } else if (a == "jump") m.dy = -0.15 * H * 4 * u * (1 - u);
    else if (a == "popup") {
        // grows from nothing past its size and back (an overshoot)
        const double k = 1.70158, v = u - 1;
        m.sx = m.sy = std::max(0.0, 1 + v * v * ((k + 1) * v + k));
    } else if (a == "coming at you") { m.sx = m.sy = 0.1 + 0.9 * out; m.opacity = u; }
    else if (a == "twist in" || a == "action flip") { m.sx = (a == "action flip" ? -1 + 2 * out : out); m.opacity = std::min(1.0, 2 * u); }
    else if (a == "float and pop") {
        const double f = clampd(t / 1.5, 0, 1);
        m.dy = 0.1 * H * (1 - f);
        m.opacity = std::min(1.0, 2 * f);
        if (f >= 1) m.sx = m.sy = 1 + 0.15 * std::max(0.0, 1 - (t - 1.5) / 0.25);
    } else if (a == "double flash glow") m.opacity = t < 0.6 ? (std::fmod(t, 0.3) < 0.15 ? 1.0 : 0.25) : 1.0;
    else if (a == "rolling glow and enlarge") { m.sx = m.sy = 0.8 + 0.3 * clampd(t / 2.0, 0, 1); m.opacity = std::min(1.0, 2 * u); }
    else if (a == "earthquake") { m.dx = (hash(frame) - 0.5) * 0.04 * W; m.dy = (hash(frame + 0.37) - 0.5) * 0.04 * H; }
    else if (a == "rough day") { m.dx = (hash(frame) - 0.5) * 0.012 * W; m.dy = (hash(frame + 0.71) - 0.5) * 0.012 * H; }
    else if (a == "menace") m.sx = m.sy = 1 + 0.08 * (0.5 - 0.5 * std::cos(t * kPi));
    else if (a == "scroll") {
        const double f = clampd(t / 10.0, 0, 1);
        m.dy = (H - by) + 4 - f * (H + bh + 8);
    } else if (a == "scroll left") {
        const double f = clampd(t / 10.0, 0, 1);
        m.dx = (W - bx) + 4 - f * (W + bw + 8);
    } else m.opacity = clampd(t / 0.5, 0, 1);                   // VEGAS's numbered titles and anything else: a fade in
    return m;
}

void titlesAndText(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const Color bg = c.color("Background");
    if (bg.a > 0) {
        float p[4];
        put(p, bg);
        for (int k = 0; k < W * H; ++k) std::memcpy(&c.out.px[(size_t)k * 4], p, sizeof p);
    }
    const RichText rt = parseRichText(c.s("Text"));
    if (rt.text.empty() || W <= 0 || H <= 0) return;
    TextRequest req;
    req.text = rt.text;
    req.font = !rt.font.empty() ? rt.font : c.s("FontName");
    if (req.font.empty()) req.font = "Arial";
    const double points = rt.points > 0 ? rt.points : std::max(1.0, c.d("FontSize"));
    const double scale = std::max(0.0, c.d("Scale"));
    req.pixelSize = points * H / 400.0 * scale;
    if (req.pixelSize < c.px(0.5)) return;
    req.pixelSize = std::min(req.pixelSize, (double)H * 4);
    req.bold = rt.bold || c.b("Bold");
    req.italic = rt.italic || c.b("Italic");
    req.underline = rt.underline;
    req.tracking = c.d("Tracking");
    req.lineSpacing = std::max(0.05, c.d("LineSpacing"));
    const int anchor = (int)clampd(c.i("Alignment"), 0, 8);
    req.align = anchor % 3;
    TextMask m;
    if (!rasterizeText(req, m)) return;

    double lx, ly;
    c.xy("Location", lx, ly);
    const double ax = lx * W, ay = (1 - ly) * H;
    const double bx = ax - (anchor % 3) * 0.5 * m.w, by = ay - (anchor / 3) * 0.5 * m.h;
    const int X0 = (int)std::lround(bx), Y0 = (int)std::lround(by);

    std::string anim;
    const int pick = c.i("collection");
    if (pick > 0 && pick < kTextAnimationCount) anim = kTextAnimations[pick];
    else anim = c.s("AnimationName");
    const double secs = c.fps > 0 ? c.time / c.fps : c.time / 30.0;
    const TextMotion mo = textMotion(anim, secs, W, H, X0, Y0, m.w, m.h);

    std::vector<float> cover((size_t)W * H, 0.f);
    const bool still = mo.dx == 0 && mo.dy == 0 && mo.sx == 1 && mo.sy == 1;
    if (still) {
        for (int y = 0; y < m.h; ++y) {
            const int fy = Y0 + y;
            if (fy < 0 || fy >= H) continue;
            for (int x = 0; x < m.w; ++x) {
                const int fx = X0 + x;
                if (fx < 0 || fx >= W) continue;
                cover[(size_t)fy * W + fx] = m.a[(size_t)y * m.w + x] / 255.f;
            }
        }
    } else if (std::fabs(mo.sx) > 1e-4 && std::fabs(mo.sy) > 1e-4) {
        const double cx = X0 + m.w * 0.5 + mo.dx, cy = Y0 + m.h * 0.5 + mo.dy;
        const double hw = std::fabs(mo.sx) * m.w * 0.5 + 1, hh = std::fabs(mo.sy) * m.h * 0.5 + 1;
        const int x0 = std::max(0, (int)std::floor(cx - hw)), x1 = std::min(W - 1, (int)std::ceil(cx + hw));
        const int y0 = std::max(0, (int)std::floor(cy - hh)), y1 = std::min(H - 1, (int)std::ceil(cy + hh));
        auto at = [&](int x, int y) -> float { return (x < 0 || y < 0 || x >= m.w || y >= m.h) ? 0.f : m.a[(size_t)y * m.w + x] / 255.f; };
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double u = (x + 0.5 - cx) / mo.sx + m.w * 0.5 - 0.5, v = (y + 0.5 - cy) / mo.sy + m.h * 0.5 - 0.5;
                const int iu = (int)std::floor(u), iv = (int)std::floor(v);
                const float fu = (float)(u - iu), fv = (float)(v - iv);
                cover[(size_t)y * W + x] = (at(iu, iv) * (1 - fu) + at(iu + 1, iv) * fu) * (1 - fv) + (at(iu, iv + 1) * (1 - fu) + at(iu + 1, iv + 1) * fu) * fv;
            }
    }
    if (mo.opacity < 1) {
        const float op = (float)clampd(mo.opacity, 0, 1);
        for (float& v : cover) v *= op;
    }
    const double ow = std::max(0.0, c.d("OutlineWidth")) * H / 720.0;
    std::vector<float> outline;
    if (ow > c.px(0.25)) {
        outline = cover;
        std::vector<float> tmp((size_t)W * H);
        const int r = (int)std::ceil(ow);
        // a square max filter, across and down, then the corners rounded off by a light blur
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float v = 0;
                for (int k = std::max(0, x - r); k <= std::min(W - 1, x + r); ++k) v = std::max(v, outline[(size_t)y * W + k]);
                tmp[(size_t)y * W + x] = v;
            }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float v = 0;
                for (int k = std::max(0, y - r); k <= std::min(H - 1, y + r); ++k) v = std::max(v, tmp[(size_t)k * W + x]);
                outline[(size_t)y * W + x] = v;
            }
        gaussianBlurPlane(outline, W, H, ow * 0.25, ow * 0.25);
    }
    if (c.b("ShadowEnable")) {
        const std::vector<float>& shape = outline.empty() ? cover : outline;
        const int dx = (int)std::lround(c.d("ShadowOffsetX") * H), dy = (int)std::lround(c.d("ShadowOffsetY") * H);
        std::vector<float> sh((size_t)W * H, 0.f);
        for (int y = 0; y < H; ++y) {
            const int sy = y - dy;
            if (sy < 0 || sy >= H) continue;
            for (int x = 0; x < W; ++x) {
                const int sx = x - dx;
                if (sx >= 0 && sx < W) sh[(size_t)y * W + x] = shape[(size_t)sy * W + sx];
            }
        }
        const double blur = std::max(0.0, c.d("ShadowBlur")) * H * 0.1;
        gaussianBlurPlane(sh, W, H, blur, blur);
        fillThrough(c.out, sh, c.color("ShadowColor"));
    }
    if (!outline.empty()) fillThrough(c.out, outline, c.color("OutlineColor"));
    fillThrough(c.out, cover, c.color("TextColor"));
}


// ------------------------------------------------------------------ Credit Roll
// VEGAS's Credit Roll: a row is a title (line starts with a space), a two-column line (tab between columns) or a sub-item (anything else); "//" starts a comment line, an empty line is a gap.
// The block rolls from below the frame to above over 'Roll duration' seconds (VEGAS uses the media's length, which a generator cannot see: set it to the event's length).
struct CreditRow {
    std::string left, right;
    bool title = false, gap = false;
};

std::vector<CreditRow> creditRows(const std::string& text)
{
    std::vector<CreditRow> rows;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 2, "//") == 0) continue;
        CreditRow r;
        if (line.empty()) r.gap = true;
        else if (line[0] == ' ') {
            r.title = true;
            size_t k = 0;
            while (k < line.size() && line[k] == ' ') ++k;
            r.left = line.substr(k);
        } else if (line.size() >= 2 && line.front() == '"' && line.back() == '"') {
            r.left = line.substr(1, line.size() - 2);
        } else {
            const size_t tab = line.find('\t');
            if (tab == std::string::npos) r.left = line;
            else {
                r.left = line.substr(0, tab);
                r.right = line.substr(tab + 1);
            }
        }
        rows.push_back(r);
        if (end >= text.size()) break;
    }
    return rows;
}

void creditRoll(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const Color bg = c.color("Background");
    if (bg.a > 0) {
        float p[4];
        put(p, bg);
        for (int k = 0; k < W * H; ++k) std::memcpy(&c.out.px[(size_t)k * 4], p, sizeof p);
    }
    const std::vector<CreditRow> rows = creditRows(c.s("Text"));
    if (rows.empty() || W <= 0 || H <= 0) return;
    const double points = std::max(1.0, c.d("FontSize"));
    const double px = std::max(c.px(0.5), std::min((double)H * 4, points * H / 400.0));
    const std::string font = c.s("FontName").empty() ? std::string("Arial") : c.s("FontName");
    const double gapRows = px * 0.35, colGap = px * std::max(0.0, c.d("ColumnGap"));

    struct Placed { TextMask m; double x = 0, y = 0; };
    std::vector<Placed> placed;
    double y = 0;
    auto make = [&](const std::string& text, bool title, double size, int align, TextMask& m) {
        TextRequest rq;
        rq.text = text;
        rq.font = font;
        rq.pixelSize = size;
        rq.bold = title || c.b("Bold");
        rq.italic = c.b("Italic");
        rq.align = align;
        return !text.empty() && rasterizeText(rq, m) && m.w > 0 && m.h > 0;
    };
    for (const CreditRow& r : rows) {
        if (r.gap) { y += px * 1.2; continue; }
        const double size = r.title ? px * 1.3 : px;
        if (r.title) y += gapRows * 1.5;
        double rowH = 0;
        if (r.right.empty()) {
            Placed pl;
            if (make(r.left, r.title, size, 1, pl.m)) {
                pl.x = (W - pl.m.w) * 0.5;      // centred
                pl.y = y;
                rowH = pl.m.h;
                placed.push_back(std::move(pl));
            }
        } else {
            Placed a, b;
            const bool ha = make(r.left, false, size, 2, a.m), hb = make(r.right, false, size, 0, b.m);
            if (ha) { a.x = W * 0.5 - colGap * 0.5 - a.m.w; a.y = y; rowH = std::max(rowH, (double)a.m.h); placed.push_back(std::move(a)); }
            if (hb) { b.x = W * 0.5 + colGap * 0.5; b.y = y; rowH = std::max(rowH, (double)b.m.h); placed.push_back(std::move(b)); }
        }
        y += rowH + gapRows;
    }
    const double block = y;
    const double secs = c.fps > 0 ? c.time / c.fps : c.time / 30.0;
    const double f = clampd(secs / std::max(0.1, c.d("RollDuration")), 0, 1);
    const double top = H - f * (H + block);
    std::vector<float> cover((size_t)W * H, 0.f);
    for (const Placed& pl : placed) {
        const int X0 = (int)std::lround(pl.x), Y0 = (int)std::lround(top + pl.y);
        for (int yy = 0; yy < pl.m.h; ++yy) {
            const int fy = Y0 + yy;
            if (fy < 0 || fy >= H) continue;
            for (int xx = 0; xx < pl.m.w; ++xx) {
                const int fx = X0 + xx;
                if (fx >= 0 && fx < W) cover[(size_t)fy * W + fx] = std::max(cover[(size_t)fy * W + fx], pl.m.a[(size_t)yy * pl.m.w + xx] / 255.f);
            }
        }
    }
    fillThrough(c.out, cover, c.color("TextColor"));
}

}  // namespace

void addGenerators(std::vector<Def>& out)
{
    {
        Def d;
        d.id = "com.vegascreativesoftware:solidcolor";
        d.label = "Solid Color";
        d.group = kGroup;
        d.generator = true;
        d.description = "A frame of one colour (Reno's own, standing in for VEGAS Solid Color).";
        d.params = {rgba("Color", "Color", 0, 0, 0, 1)};
        d.render = solidColor;
        out.push_back(d);
    }
    {
        Def d;
        d.id = "com.vegascreativesoftware:colorgradient";
        d.label = "Color Gradient";
        d.group = kGroup;
        d.generator = true;
        d.description = "A blend from one colour to another (Reno's own, standing in for VEGAS Color Gradient).";
        d.params = {choice("GradientType", "Gradient type", 0, {"Linear", "Elliptical", "Rectangular"}),
                    rgba("Color1", "Color 1", 0, 0, 0, 1), rgba("Color2", "Color 2", 1, 1, 1, 1),
                    point("Center", "Center", 0.5, 0.5), dbl("Size", "Size", 1.0, 0.0, 4.0),
                    dbl("AspectRatioAngle", "Aspect ratio angle", 0, -360, 360), dbl("Aspect", "Aspect ratio", 1, 0.01, 10)};
        d.params[4].dmax = 2;
        d.params[6].dmax = 4;
        d.render = colorGradient;
        out.push_back(d);
    }
    {
        Def d;
        d.id = "com.vegascreativesoftware:checkerboard";
        d.label = "Checkerboard";
        d.group = kGroup;
        d.generator = true;
        d.description = "Tiles of two colours (Reno's own, standing in for VEGAS Checkerboard).";
        d.params = {group("TileDimensions", "Tile dimensions"),
                    dbl("TileDimensionsWidth", "Width", 0.1, 0.001, 1, "TileDimensions"),
                    dbl("TileDimensionsHeight", "Height", 0.1, 0.001, 1, "TileDimensions"),
                    boolean("TileDimensionsSquare", "Square", true, "TileDimensions"),
                    group("EdgeBlending", "Edge blending"),
                    dbl("EdgeBlendingHorizontal", "Horizontal", 0, 0, 1, "EdgeBlending"),
                    dbl("EdgeBlendingVertical", "Vertical", 0, 0, 1, "EdgeBlending"),
                    boolean("EdgeBlendingProportional", "Proportional", true, "EdgeBlending"),
                    rgba("Color1", "Color 1", 0, 0, 0, 1), rgba("Color2", "Color 2", 1, 1, 1, 1),
                    group("GridOffset", "Grid offset"),
                    dbl("GridOffsetX", "X", 0, -1, 1, "GridOffset"), dbl("GridOffsetY", "Y", 0, -1, 1, "GridOffset")};
        d.render = checkerboard;
        out.push_back(d);
    }
    {
        Def d;
        d.id = "com.vegascreativesoftware:noisetexture";
        d.label = "Noise Texture";
        d.group = kGroup;
        d.generator = true;
        d.description = "Clouds, marble, wood and other textures made of noise (Reno's own, standing in for VEGAS Noise Texture).";
        d.params = {choice("NoiseStyle", "Noise style", 0, {"Fractal - smooth", "Fractal - turbulent", "Marble - light grain", "Marble - heavy grain",
                                                          "Jagged lines", "Contours", "Rings"}),
                    dbl("NumberOfLayers", "Number of layers", 1, 1, 10), boolean("Swapcolors", "Swap colors", false),
                    rgba("Color1", "Color 1", 0, 0, 0, 1), rgba("Color2", "Color 2", 1, 1, 1, 1),
                    group("Frequency", "Frequency"),
                    dbl("FrequencyX", "X", 10, 0, 100, "Frequency"), dbl("FrequencyY", "Y", 10, 0, 100, "Frequency"),
                    group("Offset", "Offset"),
                    dbl("OffsetX", "X", 0, -100, 100, "Offset"), dbl("OffsetY", "Y", 0, -100, 100, "Offset"),
                    dbl("OffsetprogressinDegrees", "Progress (in degrees)", 0, -3600, 3600, "Offset"),
                    group("NoiseParameters", "Noise parameters"),
                    dbl("NoiseParametersMin", "Min", 0, 0, 1, "NoiseParameters"), dbl("NoiseParametersMax", "Max", 1, 0, 1, "NoiseParameters"),
                    dbl("NoiseParametersBias", "Bias", 0, -1, 1, "NoiseParameters"),
                    dbl("NoiseParametersAmplitude", "Amplitude", 1, 0, 10, "NoiseParameters"),
                    dbl("NoiseParametersGrainAmplitudeFalloff", "Grain (amplitude falloff)", 0.5, 0, 1, "NoiseParameters")};
        d.params[6].dmax = d.params[7].dmax = 40;
        d.params[11].dmin = 0; d.params[11].dmax = 360;
        d.render = noiseTexture;
        out.push_back(d);
    }
    {
        Def d;
        d.id = "com.vegascreativesoftware:testpattern";
        d.label = "Test Pattern";
        d.group = kGroup;
        d.generator = true;
        d.description = "Colour bars and other test patterns (Reno's own, standing in for VEGAS Test Pattern).";
        d.params = {choice("TestPatternType", "Test pattern type", 0, {"Color bars (NTSC)", "Color bars (PAL)", "Gradient", "Pluge and porches", "Ramp",
                                                                      "SMPTE bars (NTSC)", "SMPTE bars (PAL)", "White porches", "Yellow porches"})};
        d.render = testPattern;
        out.push_back(d);
    }
    {
        Def d;
        d.id = "com.vegascreativesoftware:creditroll";
        d.label = "Credit Roll";
        d.group = kGroup;
        d.generator = true;
        d.description = "Credits rolling up the frame. A line starting with a space is a title, a tab splits two columns, // starts a comment.";
        PDef t = text("Text", "Credits text", " Cast\nFirst Actor\tThe Hero\nSecond Actor\tThe Villain\n\n Crew\nDirector\tA. Person\nEditor\tB. Person", true);
        t.hint = "A line starting with a space is a title; a tab splits two columns; // starts a comment; an empty line is a gap";
        d.params = {t, text("FontName", "Font", "Arial", false), dbl("FontSize", "Size (points)", 32, 1, 1000), boolean("Bold", "Bold", false),
                    boolean("Italic", "Italic", false), rgba("TextColor", "Text color", 1, 1, 1, 1), rgba("Background", "Background color", 0, 0, 0, 1),
                    dbl("ColumnGap", "Column gap (ems)", 1.0, 0, 20), dbl("RollDuration", "Roll duration (seconds)", 10, 0.1, 3600)};
        d.render = creditRoll;
        out.push_back(d);
    }
    {
        Def d;
        d.id = "com.vegascreativesoftware:titlesandtext";
        d.label = "Titles & Text";
        d.group = kGroup;
        d.generator = true;
        d.description = "Text over the picture (Reno's own, standing in for VEGAS Titles & Text; its animations are Reno's versions of VEGAS's).";
        PDef t = text("Text", "Text", "Sample Text", true);
        t.hint = "Plain text, or VEGAS's rich text (its font, size, bold and italic are then used)";
        PDef loc = point("Location", "Location", 0.5, 0.5);
        d.params = {t, dbl("Scale", "Scale", 1, 0, 10), loc, rgba("TextColor", "Text color", 1, 1, 1, 1),
                    choice("Alignment", "Anchor Point", 4, {"Top Left", "Top Center", "Top Right", "Center Left", "Center", "Center Right",
                                                            "Bottom Left", "Bottom Center", "Bottom Right"}),
                    group("FontGroup", "Font"),
                    text("FontName", "Font", "Arial", false), dbl("FontSize", "Size (points)", 48, 1, 1000), boolean("Bold", "Bold", false),
                    boolean("Italic", "Italic", false),
                    group("AdvancedGroup", "Advanced"),
                    rgba("Background", "Background", 0, 0, 0, 0, "AdvancedGroup"), dbl("Tracking", "Tracking", 0, -1, 2, "AdvancedGroup"),
                    dbl("LineSpacing", "Line spacing", 1, 0.1, 5, "AdvancedGroup"),
                    group("OutlineGroup", "Outline"),
                    dbl("OutlineWidth", "Outline width", 0, 0, 50, "OutlineGroup"), rgba("OutlineColor", "Outline color", 0, 0, 0, 1, "OutlineGroup"),
                    group("ShadowGroup", "Shadow"),
                    boolean("ShadowEnable", "Shadow enable", false, "ShadowGroup"), rgba("ShadowColor", "Shadow color", 0, 0, 0, 1, "ShadowGroup"),
                    dbl("ShadowOffsetX", "Shadow offset X", 0.01, -1, 1, "ShadowGroup"), dbl("ShadowOffsetY", "Shadow offset Y", 0.01, -1, 1, "ShadowGroup"),
                    dbl("ShadowBlur", "Shadow blur", 0.01, 0, 1, "ShadowGroup")};
        // VEGAS's Animation list, and the hidden name it keeps the animation by
        {
            std::vector<std::string> names(kTextAnimations, kTextAnimations + kTextAnimationCount);
            PDef an = choice("collection", "Animation", 0, names);
            an.hint = "How the text comes in. None plays the animation a VEGAS preset or project named, if any";
            PDef keep = text("AnimationName", "Animation name", "", false);
            keep.secret = true;
            d.params.insert(d.params.begin() + 5, an);
            d.params.push_back(keep);
        }
        for (const char* n : {"FontName", "FontSize", "Bold", "Italic"})
            for (auto& p : d.params) if (p.name == n) p.parent = "FontGroup";
        for (auto& p : d.params) {
            if (p.name == "Scale") p.dmax = 4;
            if (p.name == "FontSize") p.dmax = 200;
            if (p.name == "OutlineWidth") p.dmax = 20;
            if (p.name == "ShadowOffsetX" || p.name == "ShadowOffsetY") { p.dmin = -0.2; p.dmax = 0.2; }
        }
        d.render = titlesAndText;
        out.push_back(d);
    }
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
