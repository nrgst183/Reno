// Label, grouping and description for OpenFX effects from their bundle's resource file.
#include "ofxinternal.h"
#include <map>
#include <mutex>

namespace reno {
namespace ofx {

// OpenFX resource overrides: a bundle's Contents/Resources/<binary's name>.xml can give each effect the label, grouping and description the host shows (and per context the parameters' labels), per host name
// ("default" for any host) and language:
//   <OfxImageEffectResource><OfxPlugin name="com.vegascreativesoftware:gaussianblur"><OfxResourceSet ofxHost="default"> <OfxPropLabel>VEGAS Gaussian Blur</OfxPropLabel> ...
// Effects that leave their label out (or use their identifier as label) get it from here; only label, grouping and description are taken.
namespace {

std::string xmlDecode(const std::string& s)
{
    std::string out;
    for (size_t k = 0; k < s.size(); ++k) {
        if (s[k] != '&') { out += s[k]; continue; }
        const size_t semi = s.find(';', k);
        if (semi == std::string::npos || semi - k > 10) { out += s[k]; continue; }
        const std::string e = s.substr(k + 1, semi - k - 1);
        unsigned long cp = 0;
        if (e == "amp") cp = '&';
        else if (e == "lt") cp = '<';
        else if (e == "gt") cp = '>';
        else if (e == "quot") cp = '"';
        else if (e == "apos") cp = '\'';
        else if (e.size() > 1 && e[0] == '#') cp = std::strtoul(e.c_str() + (e[1] == 'x' || e[1] == 'X' ? 2 : 1), nullptr, e[1] == 'x' || e[1] == 'X' ? 16 : 10);
        if (cp == 0 || cp > 0x10FFFF) { out += s[k]; continue; }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        k = semi;
    }
    size_t a = 0, b = out.size();
    while (a < b && (unsigned char)out[a] <= ' ') ++a;
    while (b > a && (unsigned char)out[b - 1] <= ' ') --b;
    return out.substr(a, b - a);
}

// The value of attribute 'name' in an opening tag's text ("" if it has none).
std::string xmlAttr(const std::string& tag, const std::string& name)
{
    size_t at = 0;
    while ((at = tag.find(name, at)) != std::string::npos) {
        const bool startsWord = at > 0 && (unsigned char)tag[at - 1] <= ' ';
        size_t k = at + name.size();
        while (k < tag.size() && (unsigned char)tag[k] <= ' ') ++k;
        if (startsWord && k < tag.size() && tag[k] == '=') {
            ++k;
            while (k < tag.size() && (unsigned char)tag[k] <= ' ') ++k;
            if (k < tag.size() && (tag[k] == '"' || tag[k] == '\'')) {
                const size_t end = tag.find(tag[k], k + 1);
                if (end != std::string::npos) return xmlDecode(tag.substr(k + 1, end - k - 1));
            }
        }
        at += name.size();
    }
    return std::string();
}

// The text of the first <name>...</name> in 'body' ("" if none).
std::string xmlChildText(const std::string& body, const std::string& name)
{
    const size_t a = body.find("<" + name + ">");
    if (a == std::string::npos) return std::string();
    const size_t from = a + name.size() + 2;
    const size_t b = body.find("</" + name + ">", from);
    return b == std::string::npos ? std::string() : xmlDecode(body.substr(from, b - from));
}

// What the resource file 'xml' says about effect 'id' for this host: the resource set for this host's name if there is one, else the one for any host ("default", "all" or none named);
// sets for a language other than English are skipped. False if the file says nothing about the effect.
bool resourceLabels(const std::string& xml, const std::string& id, const std::string& hostName, ResourceLabels& out)
{
    std::string text;
    for (size_t k = 0; k < xml.size();) {
        const size_t c = xml.find("<!--", k);
        if (c == std::string::npos) { text.append(xml, k, std::string::npos); break; }
        text.append(xml, k, c - k);
        const size_t e = xml.find("-->", c + 4);
        if (e == std::string::npos) break;
        k = e + 3;
    }
    for (size_t at = 0; (at = text.find("<OfxPlugin", at)) != std::string::npos;) {
        const size_t tagEnd = text.find('>', at);
        if (tagEnd == std::string::npos) return false;
        const char after = text[at + 10];
        if (after != ' ' && after != '\t' && after != '\r' && after != '\n') { at = tagEnd; continue; }
        const std::string name = xmlAttr(text.substr(at, tagEnd - at), "name");
        const size_t end = text.find("</OfxPlugin>", tagEnd);
        const std::string body = text.substr(tagEnd + 1, (end == std::string::npos ? text.size() : end) - tagEnd - 1);
        at = end == std::string::npos ? text.size() : end;
        if (name != id) continue;
        int bestScore = 0;
        std::string best;
        for (size_t r = 0; (r = body.find("<OfxResourceSet", r)) != std::string::npos;) {
            const size_t rEnd = body.find('>', r);
            if (rEnd == std::string::npos) break;
            const std::string tag = body.substr(r, rEnd - r);
            const size_t close = body.find("</OfxResourceSet>", rEnd);
            std::string set = body.substr(rEnd + 1, (close == std::string::npos ? body.size() : close) - rEnd - 1);
            r = close == std::string::npos ? body.size() : close;
            const std::string host = xmlAttr(tag, "ofxHost"), locale = toLower(xmlAttr(tag, "ofxLocale"));
            if (!locale.empty() && locale.compare(0, 2, "en") != 0) continue;
            int score = 0;
            if (!hostName.empty() && host == hostName) score = 3;
            else if (host.empty() || host == "default" || host == "all") score = 1;
            if (score == 0) continue;
            if (!locale.empty()) ++score;
            if (score > bestScore) {
                bestScore = score;
                const size_t ctx = set.find("<OfxImageEffectContext");
                best = ctx == std::string::npos ? set : set.substr(0, ctx);
            }
        }
        if (bestScore == 0) return false;
        out.label = xmlChildText(best, "OfxPropLabel");
        if (out.label.empty()) out.label = xmlChildText(best, "OfxPropLongLabel");
        out.group = xmlChildText(best, "OfxImageEffectPluginPropGrouping");
        out.description = xmlChildText(best, "OfxPropPluginDescription");
        return true;
    }
    return false;
}

}  // namespace

// The resource files of a binary's bundle: <name of the binary>.xml first, then any other .xml there. Read once each.
bool bundleResourceLabels(const std::string& bundlePath, const std::string& binaryPath, const std::string& id, const std::string& hostName,
                          ResourceLabels& out)
{
    static std::mutex cacheMutex;
    static std::map<std::string, std::string> cache;        // file -> its text ("" if not a resource file)
    const std::string dir = pathJoin(pathJoin(bundlePath, "Contents"), "Resources");
    std::string base = pathFileName(binaryPath);
    if (base.find('.') != std::string::npos) base = base.substr(0, base.find_last_of('.'));
    std::vector<std::string> files;
    const std::vector<std::string> names = listDir(dir);
    for (const auto& n : names)
        if (toLower(n) == toLower(base + ".xml")) files.insert(files.begin(), pathJoin(dir, n));
        else if (endsWithNoCase(n, ".xml")) files.push_back(pathJoin(dir, n));
    for (const auto& f : files) {
        std::string text;
        {
            std::lock_guard<std::mutex> lk(cacheMutex);
            auto it = cache.find(f);
            if (it != cache.end()) text = it->second;
            else {
                std::string t;
                if (readFile(f, t) && t.size() < (16u << 20) && t.find("<OfxImageEffectResource") != std::string::npos) text = t;
                cache[f] = text;
            }
        }
        if (!text.empty() && resourceLabels(text, id, hostName, out)) return true;
    }
    return false;
}

}  // namespace ofx
}  // namespace reno
