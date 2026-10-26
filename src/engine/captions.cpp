// Closed captions: VEGAS's caption markup, what a channel shows, and caption files. See captions.h.
#include "captions.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>

namespace reno {

namespace {

// ------------------------------------------------------------------ small helpers
std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            out.push_back(cur);
            cur.clear();
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// UTF-8 text without a byte order mark; UTF-16 files (SAMI and SubRip from Windows tools) made UTF-8.
std::string decodeText(const std::string& raw)
{
    if (raw.size() >= 3 && (uint8_t)raw[0] == 0xEF && (uint8_t)raw[1] == 0xBB && (uint8_t)raw[2] == 0xBF) return raw.substr(3);
    const bool le = raw.size() >= 2 && (uint8_t)raw[0] == 0xFF && (uint8_t)raw[1] == 0xFE;
    const bool be = raw.size() >= 2 && (uint8_t)raw[0] == 0xFE && (uint8_t)raw[1] == 0xFF;
    if (!le && !be) return raw;
    std::string out;
    for (size_t i = 2; i + 1 < raw.size(); i += 2) {
        uint32_t u = le ? ((uint8_t)raw[i] | ((uint8_t)raw[i + 1] << 8)) : (((uint8_t)raw[i] << 8) | (uint8_t)raw[i + 1]);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < raw.size()) {
            const uint32_t lo = le ? ((uint8_t)raw[i + 2] | ((uint8_t)raw[i + 3] << 8)) : (((uint8_t)raw[i + 2] << 8) | (uint8_t)raw[i + 3]);
            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        if (u < 0x80) out += (char)u;
        else if (u < 0x800) { out += (char)(0xC0 | (u >> 6)); out += (char)(0x80 | (u & 0x3F)); }
        else if (u < 0x10000) { out += (char)(0xE0 | (u >> 12)); out += (char)(0x80 | ((u >> 6) & 0x3F)); out += (char)(0x80 | (u & 0x3F)); }
        else { out += (char)(0xF0 | (u >> 18)); out += (char)(0x80 | ((u >> 12) & 0x3F)); out += (char)(0x80 | ((u >> 6) & 0x3F)); out += (char)(0x80 | (u & 0x3F)); }
    }
    return out;
}

void appendUtf8(std::string& s, uint32_t u)
{
    if (u < 0x80) s += (char)u;
    else if (u < 0x800) { s += (char)(0xC0 | (u >> 6)); s += (char)(0x80 | (u & 0x3F)); }
    else { s += (char)(0xE0 | (u >> 12)); s += (char)(0x80 | ((u >> 6) & 0x3F)); s += (char)(0x80 | (u & 0x3F)); }
}

// The code points of a UTF-8 string (bad bytes are taken as Latin-1).
std::vector<uint32_t> codePoints(const std::string& s)
{
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = (uint8_t)s[i];
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + n >= s.size() + (n == 0 ? 1 : 0)) { out.push_back(c); ++i; continue; }
        uint32_t u = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
        bool ok = true;
        for (int k = 1; k <= n; ++k) {
            if (i + k >= s.size() || ((uint8_t)s[i + k] >> 6) != 2) { ok = false; break; }
            u = (u << 6) | ((uint8_t)s[i + k] & 0x3F);
        }
        if (!ok) { out.push_back(c); ++i; continue; }
        out.push_back(u);
        i += (size_t)n + 1;
    }
    return out;
}

size_t charCount(const std::string& s) { return codePoints(s).size(); }

// The first n characters of a UTF-8 string.
std::string firstChars(const std::string& s, size_t n)
{
    const std::vector<uint32_t> cp = codePoints(s);
    std::string out;
    for (size_t k = 0; k < cp.size() && k < n; ++k) appendUtf8(out, cp[k]);
    return out;
}

// HTML-ish text (SAMI, RealText, SubRip's <i>): tags out, <br> as a line break, entities decoded.
std::string stripTags(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '<') {
            const size_t e = s.find('>', i);
            if (e == std::string::npos) break;
            std::string tag = toLower(s.substr(i + 1, e - i - 1));
            if (tag.compare(0, 2, "br") == 0) out += "[BR]";
            i = e;
        } else if (s[i] == '&') {
            const size_t e = s.find(';', i);
            if (e != std::string::npos && e - i < 10) {
                const std::string ent = toLower(s.substr(i + 1, e - i - 1));
                if (ent == "nbsp") out += ' ';
                else if (ent == "amp") out += '&';
                else if (ent == "lt") out += '<';
                else if (ent == "gt") out += '>';
                else if (ent == "quot") out += '"';
                else if (ent == "apos") out += '\'';
                else if (!ent.empty() && ent[0] == '#') appendUtf8(out, (uint32_t)std::strtoul(ent.c_str() + (ent.size() > 1 && ent[1] == 'x' ? 2 : 1), nullptr, ent.size() > 1 && ent[1] == 'x' ? 16 : 10));
                else out += s.substr(i, e - i + 1);
                i = e;
            } else {
                out += '&';
            }
        } else if (s[i] == '\r' || s[i] == '\n' || s[i] == '\t') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    // runs of spaces made one, and none round the line breaks
    std::string c;
    for (char ch : out) {
        if (ch == ' ' && !c.empty() && c.back() == ' ') continue;
        c += ch;
    }
    size_t p;
    while ((p = c.find(" [BR]")) != std::string::npos) c.erase(p, 1);
    while ((p = c.find("[BR] ")) != std::string::npos) c.erase(p + 4, 1);
    return trim(c);
}

std::string xmlEscape(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

// ------------------------------------------------------------------ the markup
struct Token {
    bool code = false;
    std::string s;              // the code's name without braces, or the text
};

std::vector<Token> tokenize(const std::string& markup)
{
    std::vector<Token> out;
    std::string text;
    auto flush = [&] {
        if (!text.empty()) out.push_back(Token{false, text});
        text.clear();
    };
    for (size_t i = 0; i < markup.size(); ++i) {
        if (markup[i] == '{') {
            const size_t e = markup.find('}', i);
            if (e != std::string::npos) {
                flush();
                out.push_back(Token{true, markup.substr(i + 1, e - i - 1)});
                i = e;
                continue;
            }
        }
        if (markup[i] == '[' && markup.compare(i, 4, "[BR]") == 0) {
            flush();
            out.push_back(Token{true, "BR"});
            i += 3;
            continue;
        }
        if (markup[i] == '\n') {
            flush();
            out.push_back(Token{true, "BR"});
            continue;
        }
        if (markup[i] == '\r') continue;
        text += markup[i];
    }
    flush();
    return out;
}

const char* const kColors[7] = {"Wh", "Gr", "Bl", "Cy", "Rd", "Yl", "Ma"};

int colorIndex(const std::string& c)
{
    for (int k = 0; k < 7; ++k)
        if (c == kColors[k]) return k;
    return -1;
}

// A row code: {RyyInxxWh}, {RyyWhIU}... (row 0 is taken as 15).
struct RowCode {
    int row = 15, indent = 0;
    std::string color = "Wh";
    bool italic = false, underline = false;
};

bool parseRowCode(const std::string& c, RowCode& rc)
{
    if (c.size() < 5 || c[0] != 'R' || !std::isdigit((unsigned char)c[1]) || !std::isdigit((unsigned char)c[2])) return false;
    rc = RowCode();
    rc.row = (c[1] - '0') * 10 + (c[2] - '0');
    if (rc.row < 1 || rc.row > 15) rc.row = 15;
    size_t i = 3;
    if (c.compare(i, 2, "In") == 0 && i + 4 <= c.size() && std::isdigit((unsigned char)c[i + 2]) && std::isdigit((unsigned char)c[i + 3])) {
        rc.indent = std::min(28, (c[i + 2] - '0') * 10 + (c[i + 3] - '0'));
        i += 4;
    }
    if (i + 2 > c.size()) return false;
    const std::string col = c.substr(i, 2);
    if (colorIndex(col) < 0) return false;
    rc.color = col;
    i += 2;
    for (; i < c.size(); ++i) {
        if (c[i] == 'I') rc.italic = true;
        else if (c[i] == 'U') rc.underline = true;
        else return false;
    }
    return true;
}

std::string rowCodeText(const RowCode& rc)
{
    std::string s = strfmt("R%02d", rc.row);
    if (rc.indent > 0 || (rc.color == "Wh" && !rc.italic)) s += strfmt("In%02d", rc.indent);
    s += rc.color;
    if (rc.italic) s += "I";
    if (rc.underline) s += "U";
    return s;
}

}  // namespace

bool isCaptionCommand(const std::string& command, int* channel)
{
    std::string c = toLower(trim(command));
    if (c.compare(0, 3, "cea") == 0) c = c.substr(3);
    if (c.size() != 6 || c.compare(0, 5, "608cc") != 0 || c[5] < '1' || c[5] > '4') return false;
    if (channel) *channel = c[5] - '0';
    return true;
}

std::string captionCommand(int channel) { return strfmt("608CC%d", std::min(4, std::max(1, channel))); }

bool hasCaptionMarkup(const std::string& s)
{
    for (const Token& t : tokenize(s))
        if (t.code && t.s != "BR") return true;
    return false;
}

std::string popOnMarkup(const std::string& text, bool centre)
{
    // the lines, long ones broken at spaces into 31-character rows (indent 0 plus tab offset 1 leaves 31)
    std::vector<std::string> rows;
    for (const Token& t : tokenize(text)) {
        if (t.code) {
            if (t.s == "BR") rows.push_back(std::string());
            continue;
        }
        if (rows.empty()) rows.push_back(std::string());
        rows.back() += t.s;
    }
    std::vector<std::string> wrapped;
    const size_t width = centre ? 32 : 31;
    for (std::string r : rows) {
        r = trim(r);
        while (charCount(r) > width) {
            const std::vector<uint32_t> cp = codePoints(r);
            size_t cut = width;
            for (size_t k = width; k > 0; --k)
                if (cp[k] == ' ') { cut = k; break; }
            std::string head;
            for (size_t k = 0; k < cut; ++k) appendUtf8(head, cp[k]);
            wrapped.push_back(trim(head));
            std::string tail;
            for (size_t k = cut; k < cp.size(); ++k) appendUtf8(tail, cp[k]);
            r = trim(tail);
        }
        wrapped.push_back(r);
    }
    while (!wrapped.empty() && wrapped.back().empty()) wrapped.pop_back();
    if (wrapped.size() > 4) wrapped.erase(wrapped.begin(), wrapped.end() - 4);
    std::string out = "{RCL}{ENM}";
    const int first = 16 - (int)wrapped.size();
    for (size_t k = 0; k < wrapped.size(); ++k) {
        RowCode rc;
        rc.row = first + (int)k;
        int tab = 1;
        if (centre) {
            const int col = std::max(0, (32 - (int)charCount(wrapped[k])) / 2);
            rc.indent = col / 4 * 4;
            tab = col % 4;
        }
        out += "{" + rowCodeText(rc) + "}";
        if (tab > 0) out += strfmt("{TabOff%d}", tab);
        out += wrapped[k];
    }
    out += "{EDM}{EOC}";
    return out;
}

std::string captionText(const std::string& markupOrText, const std::string& lineBreak)
{
    std::vector<std::string> rows(1);
    for (const Token& t : tokenize(markupOrText)) {
        if (!t.code) {
            rows.back() += t.s;
            continue;
        }
        RowCode rc;
        if (t.s == "BR" || t.s == "CR" || parseRowCode(t.s, rc)) {
            if (!rows.back().empty()) rows.push_back(std::string());
        }
    }
    std::string out;
    for (const std::string& r : rows) {
        const std::string tr = trim(r);
        if (tr.empty()) continue;
        if (!out.empty()) out += lineBreak;
        out += tr;
    }
    return out;
}

std::string captionMarkupOf(const std::string& parameter)
{
    return hasCaptionMarkup(parameter) ? parameter : popOnMarkup(parameter);
}

std::vector<Caption> projectCaptions(const Project& p, int channel)
{
    std::vector<Caption> out;
    for (const Project::Marker& m : p.markers) {
        int ch = 0;
        if (m.kind != "command" || !isCaptionCommand(m.command, &ch)) continue;
        if (channel && ch != channel) continue;
        Caption c;
        c.time = m.time;
        c.channel = ch;
        c.markup = m.name;
        out.push_back(c);
    }
    std::stable_sort(out.begin(), out.end(), [](const Caption& a, const Caption& b) { return a.time < b.time; });
    return out;
}

// ------------------------------------------------------------------ what a channel shows (a CEA-608 decoder's memories)
namespace {

struct Screen {
    std::map<int, CaptionRow> rows;
    void write(int row, int col, const std::string& text, const RowCode& attr)
    {
        auto it = rows.find(row);
        if (it == rows.end() || it->second.text.empty()) {
            CaptionRow r;
            r.row = row;
            r.column = col;
            r.text = text;
            r.color = attr.color;
            r.italic = attr.italic;
            r.underline = attr.underline;
            rows[row] = r;
            return;
        }
        CaptionRow& r = it->second;
        const int end = r.column + (int)charCount(r.text);
        if (col >= end) {
            r.text += std::string((size_t)(col - end), ' ') + text;
        } else if (col <= r.column) {
            r.column = col;
            r.text = text;
        } else {
            r.text = firstChars(r.text, (size_t)(col - r.column)) + text;
        }
    }
};

}  // namespace

std::vector<CaptionRow> captionScreenAt(const std::vector<Caption>& captions, double t)
{
    Screen shown, loading;
    enum Mode { PopOn, PaintOn, RollUp } mode = PopOn;
    int depth = 2, base = 15;
    int row = 15, col = 0;
    RowCode attr;
    for (const Caption& c : captions) {
        if (c.time > t + 1e-9) break;
        const std::string markup = captionMarkupOf(c.markup);
        for (const Token& tk : tokenize(markup)) {
            Screen& target = mode == PopOn ? loading : shown;
            if (!tk.code) {
                target.write(row, col, tk.s, attr);
                col += (int)charCount(tk.s);
                continue;
            }
            const std::string& s = tk.s;
            RowCode rc;
            if (s == "RCL") mode = PopOn;
            else if (s == "RDC") mode = PaintOn;
            else if (s == "RU2" || s == "RU3" || s == "RU4") {
                mode = RollUp;
                depth = s[2] - '0';
                row = base;
                col = 0;
            } else if (s == "EDM") shown.rows.clear();
            else if (s == "ENM") loading.rows.clear();
            else if (s == "EOC") {
                std::swap(shown.rows, loading.rows);
                mode = PopOn;
            } else if (s == "CR" || s == "BR") {
                if (mode == RollUp && s == "CR") {
                    std::map<int, CaptionRow> moved;
                    for (auto& kv : shown.rows) {
                        const int r = kv.first - 1;
                        if (r > base - depth && r >= 1) {
                            CaptionRow cr = kv.second;
                            cr.row = r;
                            moved[r] = cr;
                        }
                    }
                    shown.rows = moved;
                    row = base;
                } else {
                    row = std::min(15, row + 1);
                }
                col = 0;
            } else if (parseRowCode(s, rc)) {
                row = rc.row;
                col = rc.indent;
                attr = rc;
                if (mode == RollUp) base = rc.row;
            } else if (s.compare(0, 6, "TabOff") == 0 && s.size() == 7) {
                col += std::max(0, std::min(3, s[6] - '0'));
            } else if (s == "Backspace") {
                auto it = target.rows.find(row);
                if (it != target.rows.end() && !it->second.text.empty()) {
                    const size_t n = charCount(it->second.text);
                    it->second.text = firstChars(it->second.text, n - 1);
                    col = std::max(0, col - 1);
                }
            } else if (s == "DelEndRow") {
                auto it = target.rows.find(row);
                if (it != target.rows.end() && col >= it->second.column)
                    it->second.text = firstChars(it->second.text, (size_t)(col - it->second.column));
            } else if (s.size() >= 5 && s.compare(s.size() - 3, 3, "Txt") == 0) {
                // mid-row codes: the colour (or italics) and underline of what follows
                const std::string k = s.substr(0, s.size() - 3);
                const bool u = !k.empty() && k.back() == 'U';
                const std::string base2 = u ? k.substr(0, k.size() - 1) : k;
                if (base2 == "I") attr.italic = true;
                else if (colorIndex(base2) >= 0) { attr.color = base2; attr.italic = false; }
                attr.underline = u;
            }
        }
    }
    std::vector<CaptionRow> out;
    for (auto& kv : shown.rows)
        if (!trim(kv.second.text).empty()) out.push_back(kv.second);
    return out;
}

// ------------------------------------------------------------------ CEA-608 bytes
namespace {

// The standard characters that are not ASCII (0x20 .. 0x7F).
uint32_t standardChar(uint8_t b)
{
    switch (b) {
    case 0x2A: return 0xE1;     // a acute
    case 0x5C: return 0xE9;     // e acute
    case 0x5E: return 0xED;
    case 0x5F: return 0xF3;
    case 0x60: return 0xFA;
    case 0x7B: return 0xE7;
    case 0x7C: return 0xF7;
    case 0x7D: return 0xD1;
    case 0x7E: return 0xF1;
    case 0x7F: return 0x25A0;
    default: return b;
    }
}
const uint32_t kSpecial[16] = {0xAE, 0xB0, 0xBD, 0xBF, 0x2122, 0xA2, 0xA3, 0x266A, 0xE0, 0xA0, 0xE8, 0xE2, 0xEA, 0xEE, 0xF4, 0xFB};
const uint32_t kExtended1[32] = {0xC1, 0xC9, 0xD3, 0xDA, 0xDC, 0xFC, 0x2018, 0xA1, '*', 0x2019, 0x2014, 0xA9, 0x2120, 0x2022, 0x201C, 0x201D,
                                 0xC0, 0xC2, 0xC7, 0xC8, 0xCA, 0xCB, 0xEB, 0xCE, 0xCF, 0xEF, 0xD4, 0xD9, 0xF9, 0xDB, 0xAB, 0xBB};
const uint32_t kExtended2[32] = {0xC3, 0xE3, 0xCD, 0xCC, 0xEC, 0xD2, 0xF2, 0xD5, 0xF5, '{', '}', '\\', '^', '_', '|', '~',
                                 0xC4, 0xE4, 0xD6, 0xF6, 0xDF, 0xA5, 0xA4, 0x2502, 0xC5, 0xE5, 0xD8, 0xF8, 0x250C, 0x2510, 0x2514, 0x2518};
// Rows by PAC: [first byte & 7][second byte's 0x20 bit] (first byte for channel 1).
const int kPacRow[8][2] = {{11, 11}, {1, 2}, {3, 4}, {12, 13}, {14, 15}, {5, 6}, {7, 8}, {9, 10}};

uint8_t withParity(uint8_t b)
{
    b &= 0x7F;
    int n = 0;
    for (int k = 0; k < 7; ++k) n += (b >> k) & 1;
    return (n % 2 == 0) ? (uint8_t)(b | 0x80) : b;
}

// Times of SCC timecodes: frames at 29.97 (';' drop-frame, ':' counted at 30 a second).
double sccSeconds(int h, int m, int s, int f, bool drop)
{
    int64_t frames = ((int64_t)(h * 60 + m) * 60 + s) * 30 + f;
    if (drop) {
        const int64_t minutes = h * 60 + m;
        frames -= 2 * (minutes - minutes / 10);
    }
    return frames * 1001.0 / 30000.0;
}

bool parseTimecode(const std::string& s, int& h, int& m, int& sec, int& f, bool& drop)
{
    int parts[4] = {0, 0, 0, 0};
    int n = 0;
    drop = false;
    std::string cur;
    for (char c : s) {
        if (std::isdigit((unsigned char)c)) cur += c;
        else if (c == ':' || c == ';' || c == '.' || c == ',') {
            if (cur.empty() || n >= 3) return false;
            parts[n++] = std::atoi(cur.c_str());
            cur.clear();
            if (c == ';' || c == ',') drop = true;
        } else return false;
    }
    if (cur.empty() || n != 3) return false;
    parts[3] = std::atoi(cur.c_str());
    h = parts[0]; m = parts[1]; sec = parts[2]; f = parts[3];
    return true;
}

// Builds captions from the CEA-608 byte pairs of one field, in time order: the markup VEGAS writes for them.
class Decoder608 {
public:
    std::vector<Caption>* out = nullptr;
    int field = 1;

    void pair(double t, uint8_t b1, uint8_t b2)
    {
        b1 &= 0x7F;
        b2 &= 0x7F;
        if (b1 == 0 && b2 == 0) return;
        // an Erase Displayed Memory while a pop-on caption loads is the caption's own when End of Caption follows it,
        // else the screen cleared at its time
        if (edmAt_ >= 0 && !(lastControl_ && b1 == last1_ && b2 == last2_)) {
            const bool eoc = (b1 & ~0x08) == 0x14 && b2 == 0x2F;
            if (!eoc) {
                Caption c;
                c.time = edmAt_;
                c.channel = edmChannel_;
                c.markup = "{EDM}";
                if (out) out->push_back(c);
                const size_t at = pending_.rfind("{EDM}");
                if (at != std::string::npos) pending_.erase(at, 5);
            }
            edmAt_ = -1;
        }
        if (b1 >= 0x10 && b1 <= 0x1F) {
            // control codes come twice; the repeat is left out
            if (lastControl_ && b1 == last1_ && b2 == last2_) { lastControl_ = false; return; }
            lastControl_ = true;
            last1_ = b1;
            last2_ = b2;
            control(t, b1, b2);
            return;
        }
        lastControl_ = false;
        if (b1 >= 0x20) chars(t, b1, b2);
    }

    void finish()
    {
        if (!pending_.empty() && hasText_) emit(startTime_, pending_);
        pending_.clear();
    }

private:
    int channel_ = 1;               // the data channel the codes last named (1 or 2)
    bool lastControl_ = false;
    uint8_t last1_ = 0, last2_ = 0;
    std::string pending_;           // the markup of the caption being built
    double startTime_ = 0;
    bool hasText_ = false;
    std::string mode_;              // "RCL", "RDC", "RU2"
    int pendingChannel_ = 1;
    double edmAt_ = -1;             // an EDM heard while a pop-on caption loads
    int edmChannel_ = 1;

    int cc() const { return std::min(4, field == 2 ? channel_ + 2 : channel_); }

    void emit(double t, const std::string& markup)
    {
        if (!out) return;
        Caption c;
        c.time = t;
        c.channel = pendingChannel_;
        c.markup = markup;
        out->push_back(c);
    }

    void add(double t, const std::string& code)
    {
        if (pending_.empty()) {
            startTime_ = t;
            pendingChannel_ = cc();
        }
        pending_ += code;
    }

    void text(double t, const std::string& s)
    {
        add(t, s);
        hasText_ = true;
    }

    void control(double t, uint8_t b1, uint8_t b2)
    {
        const uint8_t c1 = b1 & ~0x08;
        if ((c1 == 0x14 || c1 == 0x15) && b2 >= 0x20 && b2 <= 0x2F) {
            channel_ = ((b1 & 0x08) ? 2 : 1) + (c1 == 0x15 ? 2 : 0);
            misc(t, b2);
            return;
        }
        channel_ = ((b1 & 0x08) ? 2 : 1) + (channel_ > 2 ? 2 : 0);
        if (c1 == 0x17 && b2 >= 0x21 && b2 <= 0x23) {
            add(t, strfmt("{TabOff%d}", b2 - 0x20));
            return;
        }
        if (c1 == 0x17 && b2 >= 0x2D && b2 <= 0x2F) {
            add(t, b2 == 0x2D ? "{BgTran}" : b2 == 0x2E ? "{BlkTxt}" : "{BlkUTxt}");
            return;
        }
        if (c1 == 0x11 && b2 >= 0x20 && b2 <= 0x2F) {
            // mid-row: colour or italics, underline
            const int k = (b2 - 0x20) >> 1;
            const bool u = (b2 & 1) != 0;
            add(t, "{" + std::string(k == 7 ? "I" : kColors[k]) + (u ? "U" : "") + "Txt}");
            return;
        }
        if (c1 == 0x11 && b2 >= 0x30 && b2 <= 0x3F) {
            std::string s;
            appendUtf8(s, kSpecial[b2 - 0x30]);
            text(t, s);
            return;
        }
        if ((c1 == 0x12 || c1 == 0x13) && b2 >= 0x20 && b2 <= 0x3F) {
            // an extended character replaces the standard one sent before it
            if (!pending_.empty()) {
                const std::vector<uint32_t> cp = codePoints(pending_);
                if (!cp.empty() && pending_.back() != '}') {
                    std::string s;
                    for (size_t k = 0; k + 1 < cp.size(); ++k) appendUtf8(s, cp[k]);
                    pending_ = s;
                }
            }
            std::string s;
            appendUtf8(s, (c1 == 0x12 ? kExtended1 : kExtended2)[b2 - 0x20]);
            text(t, s);
            return;
        }
        if (c1 == 0x10 && b2 >= 0x20 && b2 <= 0x2F) {
            const int k = (b2 - 0x20) >> 1;
            const char* names[8] = {"Wh", "Gr", "Bl", "Cy", "Rd", "Yl", "Ma", "Blk"};
            add(t, "{Bg" + std::string(names[k]) + ((b2 & 1) ? "Semi" : "") + "}");
            return;
        }
        if (b2 >= 0x40 && b2 <= 0x7F) {
            // a preamble address code: the row, and the indent or colour
            RowCode rc;
            rc.row = kPacRow[c1 & 7][(b2 & 0x20) ? 1 : 0];
            const int a = b2 & 0x1F;
            rc.underline = (a & 1) != 0;
            if (a < 0x10) {
                const int k = a >> 1;
                if (k == 7) rc.italic = true;
                else rc.color = kColors[k];
            } else {
                rc.indent = ((a - 0x10) >> 1) * 4;
            }
            add(t, "{" + rowCodeText(rc) + "}");
        }
    }

    void misc(double t, uint8_t b2)
    {
        switch (b2) {
        case 0x20: start(t, "RCL"); break;
        case 0x25: start(t, "RU2"); break;
        case 0x26: start(t, "RU3"); break;
        case 0x27: start(t, "RU4"); break;
        case 0x29: start(t, "RDC"); break;
        case 0x21: add(t, "{Backspace}"); break;
        case 0x24: add(t, "{DelEndRow}"); break;
        case 0x28: add(t, "{FlashOn}"); break;
        case 0x2C:      // EDM: part of a pop-on caption being built (if End of Caption follows); else a caption of its own
            if (mode_ == "RCL" && !pending_.empty()) {
                add(t, "{EDM}");
                edmAt_ = t;
                edmChannel_ = cc();
            }
            else {
                finish();
                pendingChannel_ = cc();
                emit(t, "{EDM}");
                mode_.clear();
            }
            break;
        case 0x2D:
            if (mode_.compare(0, 2, "RU") == 0) {
                // each new roll-up line is a caption of its own
                finish();
                hasText_ = false;
                add(t, "{" + mode_ + "}{CR}");
            } else {
                add(t, "{CR}");
            }
            break;
        case 0x2E: add(t, "{ENM}"); break;
        case 0x2F:
            add(t, "{EOC}");
            // a pop-on caption shows at its End of Caption
            if (hasText_ || pending_.find("{EDM}") != std::string::npos) emit(t, pending_);
            pending_.clear();
            hasText_ = false;
            mode_.clear();
            break;
        default: break;
        }
    }

    void start(double t, const char* code)
    {
        const std::string c = code;
        if (c == "RCL" && mode_ == "RCL" && !pending_.empty()) return;
        const bool hadEnm = pending_ == "{ENM}";
        finish();
        pending_.clear();
        hasText_ = false;
        mode_ = c;
        add(t, "{" + c + "}");
        if (hadEnm && c == "RCL") add(t, "{ENM}");
    }

    void chars(double t, uint8_t b1, uint8_t b2)
    {
        std::string s;
        appendUtf8(s, standardChar(b1));
        if (b2 >= 0x20) appendUtf8(s, standardChar(b2));
        text(t, s);
    }
};

// The standard character code of a code point (0 if it has none), or an extended one (set 1 or 2, index).
uint8_t charCode(uint32_t u)
{
    if (u == 0xE1) return 0x2A;
    if (u == 0xE9) return 0x5C;
    if (u == 0xED) return 0x5E;
    if (u == 0xF3) return 0x5F;
    if (u == 0xFA) return 0x60;
    if (u == 0xE7) return 0x7B;
    if (u == 0xF7) return 0x7C;
    if (u == 0xD1) return 0x7D;
    if (u == 0xF1) return 0x7E;
    if (u == 0x25A0) return 0x7F;
    if (u >= 0x20 && u < 0x7F && u != 0x2A && u != 0x5C && u != 0x5E && u != 0x5F && u != 0x60 && u != 0x7B && u != 0x7C && u != 0x7D && u != 0x7E) return (uint8_t)u;
    return 0;
}

// The byte pairs (without parity) of a caption's markup for data channel 'ch' (1 or 2).
std::vector<std::pair<uint8_t, uint8_t>> encode608(const std::string& markup, int ch)
{
    std::vector<std::pair<uint8_t, uint8_t>> out;
    const uint8_t chBit = ch == 2 ? 0x08 : 0x00;
    auto control = [&](uint8_t b1, uint8_t b2) {
        out.push_back(std::make_pair((uint8_t)(b1 | chBit), b2));
        out.push_back(std::make_pair((uint8_t)(b1 | chBit), b2));
    };
    std::vector<uint8_t> pendingChars;
    auto flushChars = [&] {
        for (size_t k = 0; k < pendingChars.size(); k += 2)
            out.push_back(std::make_pair(pendingChars[k], k + 1 < pendingChars.size() ? pendingChars[k + 1] : (uint8_t)0));
        pendingChars.clear();
    };
    for (const Token& t : tokenize(markup)) {
        if (!t.code) {
            for (uint32_t u : codePoints(t.s)) {
                const uint8_t c = charCode(u);
                if (c) { pendingChars.push_back(c); continue; }
                int sp = -1;
                for (int k = 0; k < 16; ++k) if (kSpecial[k] == u) sp = k;
                if (sp >= 0) { flushChars(); control(0x11, (uint8_t)(0x30 + sp)); continue; }
                int e1 = -1, e2 = -1;
                for (int k = 0; k < 32; ++k) {
                    if (kExtended1[k] == u) e1 = k;
                    if (kExtended2[k] == u) e2 = k;
                }
                if (e1 >= 0 || e2 >= 0) {
                    pendingChars.push_back('?');
                    flushChars();
                    control(e1 >= 0 ? 0x12 : 0x13, (uint8_t)(0x20 + (e1 >= 0 ? e1 : e2)));
                    continue;
                }
                pendingChars.push_back('?');
            }
            continue;
        }
        flushChars();
        const std::string& s = t.s;
        RowCode rc;
        if (s == "RCL") control(0x14, 0x20);
        else if (s == "Backspace") control(0x14, 0x21);
        else if (s == "DelEndRow") control(0x14, 0x24);
        else if (s == "RU2") control(0x14, 0x25);
        else if (s == "RU3") control(0x14, 0x26);
        else if (s == "RU4") control(0x14, 0x27);
        else if (s == "FlashOn") control(0x14, 0x28);
        else if (s == "RDC") control(0x14, 0x29);
        else if (s == "EDM") control(0x14, 0x2C);
        else if (s == "CR" || s == "BR") control(0x14, 0x2D);
        else if (s == "ENM") control(0x14, 0x2E);
        else if (s == "EOC") control(0x14, 0x2F);
        else if (s.compare(0, 6, "TabOff") == 0 && s.size() == 7 && s[6] >= '1' && s[6] <= '3') control(0x17, (uint8_t)(0x20 + (s[6] - '0')));
        else if (s == "BgTran") control(0x17, 0x2D);
        else if (s == "BlkTxt") control(0x17, 0x2E);
        else if (s == "BlkUTxt") control(0x17, 0x2F);
        else if (parseRowCode(s, rc)) {
            int b1 = 0x14, hi = 0;
            for (int k = 0; k < 8; ++k)
                for (int j = 0; j < 2; ++j)
                    if (kPacRow[k][j] == rc.row && !(k == 0 && j == 1)) { b1 = 0x10 | k; hi = j; }
            uint8_t a;
            if (rc.indent > 0) a = (uint8_t)(0x10 + (rc.indent / 4) * 2);
            else a = (uint8_t)((rc.italic ? 7 : std::max(0, colorIndex(rc.color))) * 2);
            if (rc.underline) a |= 1;
            control((uint8_t)b1, (uint8_t)(0x40 | (hi ? 0x20 : 0) | a));
        } else if (s.size() >= 5 && s.compare(s.size() - 3, 3, "Txt") == 0) {
            const std::string k = s.substr(0, s.size() - 3);
            const bool u = !k.empty() && k.back() == 'U';
            const std::string b = u ? k.substr(0, k.size() - 1) : k;
            const int idx = b == "I" ? 7 : colorIndex(b);
            if (idx >= 0) control(0x11, (uint8_t)(0x20 + idx * 2 + (u ? 1 : 0)));
        } else if (s.compare(0, 2, "Bg") == 0) {
            const bool semi = s.size() > 4 && s.compare(s.size() - 4, 4, "Semi") == 0;
            const std::string c = s.substr(2, s.size() - 2 - (semi ? 4 : 0));
            int idx = colorIndex(c);
            if (c == "Blk") idx = 7;
            if (idx >= 0) control(0x10, (uint8_t)(0x20 + idx * 2 + (semi ? 1 : 0)));
        }
    }
    flushChars();
    return out;
}

std::string sccTimecode(int64_t frame)
{
    // non-drop timecode: frames counted at 30 a second
    if (frame < 0) frame = 0;
    const int f = (int)(frame % 30);
    int64_t s = frame / 30;
    return strfmt("%02d:%02d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60), f);
}

std::string frameTimecode(double seconds, double fps, char sep = ':')
{
    if (!(fps > 0)) fps = 30;
    const int nominal = std::max(1, (int)std::lround(fps));
    int64_t frame = (int64_t)std::llround(std::max(0.0, seconds) * fps);
    const int f = (int)(frame % nominal);
    const int64_t s = frame / nominal;
    return strfmt("%02d:%02d:%02d%c%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60), sep, f);
}

std::string clockTime(double seconds, char msSep, int decimals)
{
    if (seconds < 0) seconds = 0;
    int64_t ms = (int64_t)std::llround(seconds * 1000.0);
    const int64_t s = ms / 1000;
    ms %= 1000;
    std::string out = strfmt("%02d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    if (decimals == 3) out += strfmt("%c%03d", msSep, (int)ms);
    else out += strfmt("%c%02d", msSep, (int)(ms / 10));
    return out;
}

// "00:00:01,500", "00:00:01.50", "1.5", "00:01.5" as seconds; a 4-part timecode counts frames at fps.
bool parseClock(const std::string& raw, double fps, double& out)
{
    const std::string s = trim(raw);
    if (s.empty()) return false;
    std::vector<std::string> parts;
    std::string cur;
    std::vector<char> seps;
    for (char c : s) {
        if (c == ':' || c == ';') { parts.push_back(cur); seps.push_back(c); cur.clear(); }
        else cur += c;
    }
    parts.push_back(cur);
    for (const std::string& p : parts)
        for (char c : p)
            if (!std::isdigit((unsigned char)c) && c != '.' && c != ',') return false;
    if (parts.size() == 4) {
        const double r = fps > 0 ? fps : 30.0;
        out = ((std::atof(parts[0].c_str()) * 60 + std::atof(parts[1].c_str())) * 60 + std::atof(parts[2].c_str())) + std::atof(parts[3].c_str()) / std::max(1.0, std::round(r)) ;
        return true;
    }
    double v = 0;
    for (std::string p : parts) {
        std::replace(p.begin(), p.end(), ',', '.');
        v = v * 60 + std::atof(p.c_str());
    }
    out = v;
    return true;
}

// Captions with end times: each a pop-on caption, and an {EDM} at its end unless the next one starts there.
void addTimed(std::vector<Caption>& out, std::vector<std::pair<std::pair<double, double>, std::string>> items, int channel)
{
    std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first.first < b.first.first; });
    for (size_t k = 0; k < items.size(); ++k) {
        const std::string text = trim(items[k].second);
        if (text.empty()) continue;
        Caption c;
        c.time = items[k].first.first;
        c.channel = channel;
        c.markup = popOnMarkup(text);
        out.push_back(c);
        const double end = items[k].first.second;
        if (end > c.time) {
            const bool next = k + 1 < items.size() && items[k + 1].first.first <= end + 1e-3;
            if (!next) {
                Caption e;
                e.time = end;
                e.channel = channel;
                e.markup = "{EDM}";
                out.push_back(e);
            }
        }
    }
}

bool readSrt(const std::string& text, std::vector<Caption>& out)
{
    std::vector<std::pair<std::pair<double, double>, std::string>> items;
    const std::vector<std::string> lines = splitLines(text);
    for (size_t i = 0; i < lines.size(); ++i) {
        const size_t arrow = lines[i].find("-->");
        if (arrow == std::string::npos) continue;
        double a = 0, b = 0;
        std::string right = lines[i].substr(arrow + 3);
        const size_t sp = trim(right).find(' ');
        right = trim(right);
        if (sp != std::string::npos) right = right.substr(0, sp);
        if (!parseClock(lines[i].substr(0, arrow), 0, a) || !parseClock(right, 0, b)) continue;
        std::string t;
        size_t j = i + 1;
        for (; j < lines.size() && !trim(lines[j]).empty(); ++j) {
            if (!t.empty()) t += "[BR]";
            t += stripTags(lines[j]);
        }
        items.push_back(std::make_pair(std::make_pair(a, b), t));
        i = j;
    }
    addTimed(out, items, 1);
    return !items.empty();
}

bool readSub(const std::string& text, double fps, std::vector<Caption>& out)
{
    // DVD Architect: "0001<tab>00:00:01:00<tab>00:00:03:00<tab>text", more lines of text below it
    std::vector<std::pair<std::pair<double, double>, std::string>> items;
    for (const std::string& line : splitLines(text)) {
        std::vector<std::string> f;
        std::string cur;
        int fields = 0;
        for (size_t k = 0; k < line.size(); ++k) {
            const char c = line[k];
            if ((c == '\t' || c == ' ') && fields < 3) {
                if (!cur.empty()) { f.push_back(cur); cur.clear(); ++fields; }
                continue;
            }
            cur += c;
        }
        if (!cur.empty()) f.push_back(cur);
        double a = 0, b = 0;
        if (f.size() >= 3 && std::isdigit((unsigned char)f[0][0]) && f[0].find(':') == std::string::npos && parseClock(f[1], fps, a) && parseClock(f[2], fps, b)) {
            items.push_back(std::make_pair(std::make_pair(a, b), f.size() > 3 ? trim(f[3]) : std::string()));
        } else if (!items.empty() && !trim(line).empty()) {
            std::string& t = items.back().second;
            if (!t.empty()) t += "[BR]";
            t += trim(line);
        }
    }
    addTimed(out, items, 1);
    return !items.empty();
}

bool readSmi(const std::string& text, std::vector<Caption>& out)
{
    // <SYNC Start=1000><P Class=ENUSCC>text ; an empty (&nbsp;) one clears the screen.
    const std::string low = toLower(text);
    std::vector<std::string> classes;
    std::map<int, std::vector<std::pair<double, std::string>>> byChannel;
    size_t pos = 0;
    bool any = false;
    while ((pos = low.find("<sync", pos)) != std::string::npos) {
        const size_t end = low.find('>', pos);
        if (end == std::string::npos) break;
        const size_t st = low.find("start", pos);
        double ms = -1;
        if (st != std::string::npos && st < end) {
            size_t k = low.find('=', st);
            if (k != std::string::npos && k < end) {
                ++k;
                while (k < end && (low[k] == ' ' || low[k] == '"' || low[k] == '\'')) ++k;
                ms = std::atof(low.c_str() + k);
            }
        }
        const size_t next = low.find("<sync", end);
        const size_t stop = next == std::string::npos ? low.find("</body", end) : next;
        std::string body = text.substr(end + 1, (stop == std::string::npos ? text.size() : stop) - end - 1);
        // the class of the paragraph
        int channel = 1;
        const std::string lowBody = toLower(body);
        const size_t cl = lowBody.find("class");
        if (cl != std::string::npos) {
            size_t k = lowBody.find('=', cl);
            if (k != std::string::npos) {
                ++k;
                while (k < lowBody.size() && (lowBody[k] == ' ' || lowBody[k] == '"' || lowBody[k] == '\'')) ++k;
                std::string name;
                while (k < lowBody.size() && (std::isalnum((unsigned char)lowBody[k]) || lowBody[k] == '_' || lowBody[k] == '-')) name += lowBody[k++];
                auto it = std::find(classes.begin(), classes.end(), name);
                if (it == classes.end()) { classes.push_back(name); it = classes.end() - 1; }
                channel = (it - classes.begin()) == 0 ? 1 : 3;
            }
        }
        if (ms >= 0) {
            byChannel[channel].push_back(std::make_pair(ms / 1000.0, stripTags(body)));
            any = true;
        }
        pos = end;
    }
    for (auto& kv : byChannel) {
        for (const auto& it : kv.second) {
            Caption c;
            c.time = it.first;
            c.channel = kv.first;
            c.markup = it.second.empty() ? std::string("{EDM}") : popOnMarkup(it.second);
            out.push_back(c);
        }
    }
    return any;
}

bool readRt(const std::string& text, std::vector<Caption>& out)
{
    // RealText: <time begin="00:00:01.00" end="..."/> [<clear/>] text
    std::vector<std::pair<std::pair<double, double>, std::string>> items;
    const std::string low = toLower(text);
    size_t pos = 0;
    while ((pos = low.find("<time", pos)) != std::string::npos) {
        const size_t end = low.find('>', pos);
        if (end == std::string::npos) break;
        auto attr = [&](const char* name) -> double {
            const size_t a = low.find(name, pos);
            if (a == std::string::npos || a > end) return -1;
            size_t q = low.find('"', a);
            if (q == std::string::npos || q > end) return -1;
            const size_t q2 = low.find('"', q + 1);
            double v = -1;
            if (q2 != std::string::npos && !parseClock(low.substr(q + 1, q2 - q - 1), 0, v)) v = -1;
            return v;
        };
        const double b = attr("begin"), e = attr("end");
        const size_t next = low.find("<time", end);
        std::string body = text.substr(end + 1, (next == std::string::npos ? text.size() : next) - end - 1);
        const size_t closeWin = toLower(body).find("</window");
        if (closeWin != std::string::npos) body = body.substr(0, closeWin);
        if (b >= 0) items.push_back(std::make_pair(std::make_pair(b, e), stripTags(body)));
        pos = end;
    }
    addTimed(out, items, 1);
    return !items.empty();
}

bool readTxt(const std::string& text, double fps, std::vector<Caption>& out)
{
    // QuickTime text ([00:00:01.00] then the text) or a transcript (a timecode, then the text on the line)
    std::vector<std::pair<std::pair<double, double>, std::string>> items;
    for (const std::string& raw : splitLines(text)) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '{') continue;
        if (line[0] == '[') {
            const size_t e = line.find(']');
            double t = 0;
            if (e != std::string::npos && parseClock(line.substr(1, e - 1), fps, t)) {
                items.push_back(std::make_pair(std::make_pair(t, -1.0), trim(line.substr(e + 1))));
                continue;
            }
        }
        const size_t sp = line.find_first_of(" \t");
        double t = 0;
        if (sp != std::string::npos && line.find(':') < sp && parseClock(line.substr(0, sp), fps, t)) {
            items.push_back(std::make_pair(std::make_pair(t, -1.0), trim(line.substr(sp + 1))));
            continue;
        }
        if (!items.empty()) {
            std::string& s = items.back().second;
            if (!s.empty()) s += "[BR]";
            s += line;
        }
    }
    bool any = false;
    for (const auto& it : items) {
        Caption c;
        c.time = it.first.first;
        c.channel = 1;
        c.markup = trim(it.second).empty() ? std::string("{EDM}") : popOnMarkup(it.second);
        out.push_back(c);
        any = true;
    }
    return any;
}

bool readMcc(const std::string& text, std::vector<Caption>& out)
{
    // MacCaption: "hh:mm:ss:ff<tab>hex" lines of CEA-708 caption distribution packets, with letters standing for
    // common runs; the CEA-608 pairs inside are decoded as in an SCC file.
    double rate = 30000.0 / 1001.0;
    bool drop = true;
    Decoder608 d1, d2;
    d1.out = &out;
    d2.out = &out;
    d2.field = 2;
    bool any = false;
    for (const std::string& raw : splitLines(text)) {
        const std::string line = trim(raw);
        if (line.compare(0, 14, "Time Code Rate") == 0) {
            const std::string v = line.substr(line.find('=') + 1);
            const double r = std::atof(v.c_str());
            if (r > 0) rate = (r == 30 || r == 60) ? r * 1000.0 / 1001.0 : r;
            drop = v.find("DF") != std::string::npos;
            continue;
        }
        const size_t tab = line.find_first_of("\t ");
        int h, m, s, f;
        bool semi;
        if (tab == std::string::npos || !parseTimecode(line.substr(0, tab), h, m, s, f, semi)) continue;
        const double nominal = std::round(rate);
        const double t = (drop && std::fabs(nominal - 30) < 0.5) ? sccSeconds(h, m, s, f, true)
                                                                  : ((h * 60 + m) * 60 + s + f / nominal) * (std::fabs(rate - nominal) > 0.01 ? 1.001 : 1.0);
        // expand the letters
        std::string hex;
        for (char c : line.substr(tab + 1)) {
            if (std::isxdigit((unsigned char)c)) { hex += c; continue; }
            if (c >= 'G' && c <= 'O') for (int k = 0; k <= c - 'G'; ++k) hex += "FA0000";
            else if (c == 'P') hex += "FB8080";
            else if (c == 'Q') hex += "FC8080";
            else if (c == 'R') hex += "FD8080";
            else if (c == 'S') hex += "9669";
            else if (c == 'T') hex += "6101";
            else if (c == 'U') hex += "E10000";
            else if (c == 'Z') hex += "00";
        }
        std::vector<uint8_t> b;
        for (size_t k = 0; k + 1 < hex.size(); k += 2) b.push_back((uint8_t)std::strtoul(hex.substr(k, 2).c_str(), nullptr, 16));
        // the cc_data section (0x72): count, then triples
        for (size_t k = 0; k + 1 < b.size(); ++k) {
            if (b[k] != 0x72 || (b[k + 1] & 0xE0) != 0xE0) continue;
            const int n = b[k + 1] & 0x1F;
            for (int j = 0; j < n && k + 2 + j * 3 + 2 < b.size(); ++j) {
                const uint8_t marker = b[k + 2 + j * 3];
                if (!(marker & 0x04)) continue;
                const int type = marker & 0x03;
                if (type > 1) continue;
                (type == 0 ? d1 : d2).pair(t, b[k + 3 + j * 3], b[k + 4 + j * 3]);
                any = true;
            }
            break;
        }
    }
    d1.finish();
    d2.finish();
    std::stable_sort(out.begin(), out.end(), [](const Caption& a, const Caption& b) { return a.time < b.time; });
    return any;
}

}  // namespace

bool parseScc(const std::string& text, std::vector<Caption>& out, std::string* error)
{
    const std::vector<std::string> lines = splitLines(decodeText(text));
    if (lines.empty() || trim(lines[0]).compare(0, 12, "Scenarist_SC") != 0) {
        if (error) *error = "not a Scenarist (.scc) caption file";
        return false;
    }
    Decoder608 dec;
    std::vector<Caption> got;
    dec.out = &got;
    bool any = false;
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string line = trim(lines[i]);
        if (line.empty()) continue;
        const size_t tab = line.find_first_of("\t ");
        if (tab == std::string::npos) continue;
        int h, m, s, f;
        bool drop;
        if (!parseTimecode(line.substr(0, tab), h, m, s, f, drop)) continue;
        double t = sccSeconds(h, m, s, f, drop);
        std::string word;
        auto take = [&] {
            if (word.size() == 4) {
                const unsigned v = (unsigned)std::strtoul(word.c_str(), nullptr, 16);
                dec.pair(t, (uint8_t)(v >> 8), (uint8_t)(v & 0xFF));
                t += 1001.0 / 30000.0;
                any = true;
            }
            word.clear();
        };
        for (size_t k = tab; k < line.size(); ++k) {
            if (std::isxdigit((unsigned char)line[k])) word += line[k];
            else take();
        }
        take();
    }
    dec.finish();
    std::stable_sort(got.begin(), got.end(), [](const Caption& a, const Caption& b) { return a.time < b.time; });
    out.insert(out.end(), got.begin(), got.end());
    if (!any && error) *error = "the file has no captions";
    return any;
}

bool readCaptionFile(const std::string& path, double fps, std::vector<Caption>& out, std::string* error)
{
    std::string raw;
    if (!readFile(path, raw)) {
        if (error) *error = "could not be read";
        return false;
    }
    const std::string text = decodeText(raw);
    const std::string ext = toLower(path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.') + 1));
    std::vector<Caption> got;
    bool ok = false;
    if (ext == "scc") ok = parseScc(text, got, error);
    else if (ext == "srt") ok = readSrt(text, got);
    else if (ext == "smi" || ext == "sami") ok = readSmi(text, got);
    else if (ext == "rt") ok = readRt(text, got);
    else if (ext == "txt") ok = readTxt(text, fps, got);
    else if (ext == "mcc") ok = readMcc(text, got);
    else if (ext == "sub") ok = readSub(text, fps, got);
    else {
        if (error) *error = "is not a supported caption file (.scc, .srt, .smi, .rt, .txt, .mcc, .sub)";
        return false;
    }
    if (!ok) {
        if (error && error->empty()) *error = "no captions were found in it";
        return false;
    }
    std::stable_sort(got.begin(), got.end(), [](const Caption& a, const Caption& b) { return a.time < b.time; });
    out.insert(out.end(), got.begin(), got.end());
    return true;
}

std::string sccText(const std::vector<Caption>& captions, double offset)
{
    // Each caption's byte pairs go in frames of their own.
    std::map<int64_t, std::pair<uint8_t, uint8_t>> slots;
    std::vector<Caption> sorted = captions;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Caption& a, const Caption& b) { return a.time < b.time; });
    typedef std::vector<std::pair<uint8_t, uint8_t>> Unit;     // one pair, or a control code's two
    auto unitsOf = [](const std::vector<std::pair<uint8_t, uint8_t>>& pairs) {
        std::vector<Unit> units;
        for (size_t k = 0; k < pairs.size(); ++k) {
            const bool control = (pairs[k].first & 0x7F) >= 0x10 && (pairs[k].first & 0x7F) <= 0x1F;
            if (control && k + 1 < pairs.size() && pairs[k + 1] == pairs[k]) {
                units.push_back(Unit{pairs[k], pairs[k + 1]});
                ++k;
            } else {
                units.push_back(Unit{pairs[k]});
            }
        }
        return units;
    };
    auto freeRun = [&](int64_t at, size_t n) {
        for (size_t k = 0; k < n; ++k)
            if (at + (int64_t)k < 0 || slots.count(at + (int64_t)k)) return false;
        return true;
    };
    auto put = [&](int64_t at, const Unit& u) {
        for (size_t k = 0; k < u.size(); ++k) slots[at + (int64_t)k] = u[k];
    };
    auto forward = [&](int64_t at, const std::vector<Unit>& units) {
        for (const Unit& u : units) {
            while (!freeRun(at, u.size())) ++at;
            put(at, u);
            at += (int64_t)u.size();
        }
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (const Caption& c : sorted) {
            if (c.channel != 1 && c.channel != 2) continue;
            const std::string markup = captionMarkupOf(c.markup);
            const bool popOn = markup.compare(0, 5, "{RCL}") == 0;
            if (popOn != (pass == 1)) continue;
            const std::vector<Unit> units = unitsOf(encode608(markup, c.channel));
            if (units.empty()) continue;
            const int64_t t = (int64_t)std::llround((c.time - offset) * 30000.0 / 1001.0);
            if (!popOn) {
                forward(std::max<int64_t>(0, t), units);
                continue;
            }
            // backwards from the End of Caption at t
            std::vector<std::pair<int64_t, const Unit*>> placed;
            int64_t at = std::max<int64_t>(0, t);
            while (!freeRun(at, units.back().size())) ++at;
            bool fits = true;
            int64_t cur = at;
            placed.push_back(std::make_pair(cur, &units.back()));
            for (size_t k = units.size() - 1; k-- > 0;) {
                int64_t f = cur - (int64_t)units[k].size();
                while (f >= 0 && !freeRun(f, units[k].size())) --f;
                if (f < 0) { fits = false; break; }
                placed.push_back(std::make_pair(f, &units[k]));
                cur = f;
            }
            if (fits) {
                for (const auto& pu : placed) put(pu.first, *pu.second);
            } else {
                forward(0, units);
            }
        }
    }
    std::string out = "Scenarist_SCC V1.0\r\n";
    int64_t last = -2;
    std::string line;
    for (const auto& kv : slots) {
        if (kv.first != last + 1) {
            if (!line.empty()) out += "\r\n" + line + "\r\n";
            line = sccTimecode(kv.first) + "\t";
        } else {
            line += ' ';
        }
        line += strfmt("%02x%02x", withParity(kv.second.first), withParity(kv.second.second));
        last = kv.first;
    }
    if (!line.empty()) out += "\r\n" + line + "\r\n";
    return out;
}

bool writeCaptionFile(const std::string& path, CaptionFormat format, const std::vector<Caption>& captions, double end, double fps,
                      std::string* error, const std::string& mediaName)
{
    std::vector<Caption> sorted = captions;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Caption& a, const Caption& b) { return a.time < b.time; });
    if (format == CaptionFormat::Scc) {
        if (!writeFile(path, sccText(sorted))) { if (error) *error = "could not be written"; return false; }
        return true;
    }
    // each caption shown until the next one of its channel (an {EDM} only ends the one before)
    struct Shown { double from, to; std::string text; int channel; };
    std::vector<Shown> shown;
    for (size_t k = 0; k < sorted.size(); ++k) {
        const std::string text = captionText(captionMarkupOf(sorted[k].markup), "\n");
        if (text.empty()) continue;
        double to = end > sorted[k].time ? end : sorted[k].time + 3.0;
        for (size_t j = k + 1; j < sorted.size(); ++j)
            if (sorted[j].channel == sorted[k].channel) { to = sorted[j].time; break; }
        shown.push_back(Shown{sorted[k].time, std::max(to, sorted[k].time + 0.001), text, sorted[k].channel});
    }
    std::string base = path;
    const size_t dot = pathExtensionStart(base);
    if (dot != std::string::npos) base = base.substr(0, dot);
    const std::string stem = pathFileName(base);
    std::string text;
    if (format == CaptionFormat::Srt) {
        int n = 0;
        for (const Shown& s : shown) {
            if (s.channel != shown.front().channel) continue;
            text += strfmt("%d\r\n%s --> %s\r\n", ++n, clockTime(s.from, ',', 3).c_str(), clockTime(s.to, ',', 3).c_str());
            for (const std::string& l : splitLines(s.text)) text += l + "\r\n";
            text += "\r\n";
        }
    } else if (format == CaptionFormat::Sub) {
        int n = 0;
        for (const Shown& s : shown) {
            if (s.channel != shown.front().channel) continue;
            const std::vector<std::string> lines = splitLines(s.text);
            text += strfmt("%04d\t%s\t%s\t%s\r\n", ++n, frameTimecode(s.from, fps).c_str(), frameTimecode(s.to, fps).c_str(), lines.empty() ? "" : lines[0].c_str());
            for (size_t k = 1; k < lines.size(); ++k) text += lines[k] + "\r\n";
            text += "\r\n";
        }
    } else if (format == CaptionFormat::Smi) {
        text = "<SAMI>\r\n<HEAD>\r\n<TITLE>" + xmlEscape(stem) + "</TITLE>\r\n<STYLE TYPE=\"text/css\">\r\n<!--\r\n"
               "P { font-family: Arial; font-weight: normal; color: white; background-color: black; text-align: center; }\r\n"
               ".ENUSCC { Name: English; lang: en-US; SAMIType: CC; }\r\n.ENUSCC3 { Name: \"English (CC3)\"; lang: en-US; SAMIType: CC; }\r\n"
               "-->\r\n</STYLE>\r\n</HEAD>\r\n<BODY>\r\n";
        for (const Shown& s : shown) {
            const char* cls = s.channel == 1 ? "ENUSCC" : "ENUSCC3";
            std::string body;
            for (const std::string& l : splitLines(s.text)) body += (body.empty() ? "" : "<BR>") + xmlEscape(l);
            text += strfmt("<SYNC Start=%lld><P Class=%s>", (long long)std::llround(s.from * 1000), cls) + body + "\r\n";
            text += strfmt("<SYNC Start=%lld><P Class=%s>&nbsp;\r\n", (long long)std::llround(s.to * 1000), cls);
        }
        text += "</BODY>\r\n</SAMI>\r\n";
        const std::string media = mediaName.empty() ? stem + ".wmv" : mediaName;
        const std::string asx = "<ASX version=\"3.0\">\r\n<ENTRY>\r\n<REF HREF=\"" + xmlEscape(media) + "?sami=" + xmlEscape(pathFileName(path)) +
                                "\" />\r\n</ENTRY>\r\n</ASX>\r\n";
        if (!writeFile(base + ".asx", asx)) { if (error) *error = base + ".asx could not be written"; return false; }
    } else {
        text = "{QTtext}{font:Arial}{plain}{size:14}{textColor: 65535, 65535, 65535}{backColor: 0, 0, 0}{justify:center}"
               "{timeScale:1000}{width:320}{height:60}{timeStamps:absolute}{language:0}{textEncoding:0}\r\n";
        for (const Shown& s : shown) {
            if (s.channel != shown.front().channel) continue;
            text += "[" + clockTime(s.from, '.', 3) + "]\r\n";
            for (const std::string& l : splitLines(s.text)) text += l + "\r\n";
            text += "[" + clockTime(s.to, '.', 3) + "]\r\n";
        }
        const std::string media = mediaName.empty() ? stem + ".mov" : mediaName;
        const std::string smil = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<smil xmlns:qt=\"http://www.apple.com/quicktime/resources/smilextensions\" qt:autoplay=\"true\">\r\n"
                                 "<head>\r\n<layout>\r\n<root-layout id=\"root\" width=\"320\" height=\"300\" background-color=\"black\"/>\r\n"
                                 "<region id=\"videoregion\" top=\"0\" left=\"0\" width=\"320\" height=\"240\"/>\r\n"
                                 "<region id=\"textregion\" top=\"240\" left=\"0\" width=\"320\" height=\"60\"/>\r\n</layout>\r\n</head>\r\n"
                                 "<body>\r\n<par>\r\n<video src=\"" + xmlEscape(media) + "\" region=\"videoregion\"/>\r\n"
                                 "<textstream src=\"" + xmlEscape(pathFileName(path)) + "\" region=\"textregion\"/>\r\n</par>\r\n</body>\r\n</smil>\r\n";
        if (!writeFile(base + ".smil", smil)) { if (error) *error = base + ".smil could not be written"; return false; }
    }
    if (!writeFile(path, text)) { if (error) *error = "could not be written"; return false; }
    return true;
}

}  // namespace reno
