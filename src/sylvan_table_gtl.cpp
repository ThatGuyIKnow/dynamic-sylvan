/*
 * Alternative unique-table hash backend using gtl::parallel_flat_hash_map.
 * Enabled with -DSYLVAN_TABLE_GTL=ON. The node data array, mark bitmaps and
 * gc protocol live in sylvan_table.c; this file only maps (a,b) -> data index.
 */

#include <cstdint>
#include <mutex>

#include <gtl/phmap.hpp>

extern "C" {
// implemented in sylvan_table.c (dispatch to the custom-leaf callbacks)
uint64_t llmsset_gtl_hash(uint64_t a, uint64_t b, int custom);
int llmsset_gtl_equals(uint64_t a1, uint64_t b1, uint64_t a2, uint64_t b2, int custom);
}

namespace {

struct Key
{
    uint64_t a, b;
    uint8_t custom;
};

struct KeyHash
{
    size_t operator()(const Key& k) const
    {
        return llmsset_gtl_hash(k.a, k.b, k.custom);
    }
};

struct KeyEq
{
    bool operator()(const Key& x, const Key& y) const
    {
        if (x.custom != y.custom) return false;
        return llmsset_gtl_equals(x.a, x.b, y.a, y.b, x.custom) != 0;
    }
};

using Map = gtl::parallel_flat_hash_map<Key, uint64_t, KeyHash, KeyEq,
        std::allocator<std::pair<const Key, uint64_t>>, 6, std::mutex>;

Map the_map;

} // namespace

extern "C" {

uint64_t
sylvan_gtl_find(uint64_t a, uint64_t b, int custom)
{
    const Key k{a, b, (uint8_t)custom};
    uint64_t res = 0; // node indices 0/1 are forbidden, so 0 means "not found"
    the_map.if_contains(k, [&](const Map::value_type& v) { res = v.second; });
    return res;
}

uint64_t
sylvan_gtl_insert(uint64_t a, uint64_t b, int custom, uint64_t idx)
{
    const Key k{a, b, (uint8_t)custom};
    uint64_t res = idx;
    the_map.lazy_emplace_l(k,
            [&](Map::value_type& v) { res = v.second; },
            [&](const Map::constructor& ctor) { ctor(k, idx); });
    return res;
}

void
sylvan_gtl_clear(void)
{
    the_map.clear();
}

} // extern "C"
