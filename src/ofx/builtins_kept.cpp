// The maps effects make from their settings alone and keep for the next frames (see keptMap in builtins_impl.h).
#include "builtins_impl.h"
#include <algorithm>
#include <list>
#include <mutex>

namespace reno {
namespace ofx {
namespace builtin {

namespace {
const size_t kKeptLimit = (size_t)384 << 20;      // bytes of all the kept maps together

struct Kept {
    std::string kind, key;
    KeptMap map;
    std::vector<const void*> owners;        // the instances whose last map of this kind it is
    uint64_t used = 0;
};
struct Store {
    std::mutex m;
    std::list<Kept> kept;
    std::map<std::string, std::shared_ptr<std::mutex>> making;     // one at a time per kind (render threads ask for the same map at once)
    uint64_t clock = 0;
    size_t bytes = 0;
};
Store& store()
{
    static Store* s = new Store();          // (never destroyed: render threads may still ask while the program ends)
    return *s;
}
size_t bytesOf(const KeptMap& m) { return m ? m->capacity() * sizeof(float) : 0; }

// (under s.m) 'owner' no longer uses its map of this kind, other than 'except'; maps nobody uses go
void release(Store& s, const void* owner, const std::string* kind, const Kept* except)
{
    for (auto it = s.kept.begin(); it != s.kept.end();) {
        if (&*it != except && (!kind || it->kind == *kind)) it->owners.erase(std::remove(it->owners.begin(), it->owners.end(), owner), it->owners.end());
        if (it->owners.empty() && &*it != except) {
            s.bytes -= bytesOf(it->map);
            it = s.kept.erase(it);
        } else {
            ++it;
        }
    }
}
}  // namespace

KeptMap keptMap(const void* owner, const char* kind, const std::string& key, const std::function<void(std::vector<float>&)>& make)
{
    Store& s = store();
    const std::string k = kind ? kind : "";
    std::shared_ptr<std::mutex> mk;
    {
        std::lock_guard<std::mutex> lk(s.m);
        for (Kept& e : s.kept)
            if (e.kind == k && e.key == key) {
                if (std::find(e.owners.begin(), e.owners.end(), owner) == e.owners.end()) e.owners.push_back(owner);
                e.used = ++s.clock;
                release(s, owner, &k, &e);
                return e.map;
            }
        std::shared_ptr<std::mutex>& p = s.making[k];
        if (!p) p = std::make_shared<std::mutex>();
        mk = p;
    }
    std::lock_guard<std::mutex> one(*mk);
    {
        // (another thread may have made it while this one waited)
        std::lock_guard<std::mutex> lk(s.m);
        for (Kept& e : s.kept)
            if (e.kind == k && e.key == key) {
                if (std::find(e.owners.begin(), e.owners.end(), owner) == e.owners.end()) e.owners.push_back(owner);
                e.used = ++s.clock;
                release(s, owner, &k, &e);
                return e.map;
            }
    }
    std::shared_ptr<std::vector<float>> made = std::make_shared<std::vector<float>>();
    make(*made);
    KeptMap out = made;
    std::lock_guard<std::mutex> lk(s.m);
    s.kept.emplace_back();
    Kept& e = s.kept.back();
    e.kind = k;
    e.key = key;
    e.map = out;
    e.owners.push_back(owner);
    e.used = ++s.clock;
    s.bytes += bytesOf(out);
    release(s, owner, &k, &e);
    // over the limit: the least recently used others go (the one just made is kept even when it alone is larger)
    while (s.bytes > kKeptLimit) {
        auto oldest = s.kept.end();
        for (auto it = s.kept.begin(); it != s.kept.end(); ++it)
            if (&*it != &e && (oldest == s.kept.end() || it->used < oldest->used)) oldest = it;
        if (oldest == s.kept.end()) break;
        s.bytes -= bytesOf(oldest->map);
        s.kept.erase(oldest);
    }
    return out;
}

void dropKeptMaps(const void* owner)
{
    Store& s = store();
    std::lock_guard<std::mutex> lk(s.m);
    release(s, owner, nullptr, nullptr);
}

size_t keptMapBytes()
{
    Store& s = store();
    std::lock_guard<std::mutex> lk(s.m);
    return s.bytes;
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
