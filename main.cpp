#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

// A small persistent B+ tree.  Leaves contain sorted records, while maxKey is
// the (in-memory) internal level pointing to each leaf.  The leaves are saved
// to disk after every invocation, so a later invocation starts from the same
// tree.
struct Key {
    std::string name;
    int value;
};
struct Entry {
    std::string name;
    int value;
};
static bool less_key(const Entry &a, const Key &b) {
    return a.name != b.name ? a.name < b.name : a.value < b.value;
}
static bool less_entry(const Entry &a, const Entry &b) {
    return a.name != b.name ? a.name < b.name : a.value < b.value;
}

class BPlusTree {
    static constexpr size_t LEAF_CAPACITY = 384;
    static constexpr const char *FILE_NAME = "bpt_database.bin";
    std::vector<std::vector<Entry>> leaves;
    std::vector<Entry> maxKey;
    bool dirty = false;

    void rebuild_index() {
        maxKey.clear();
        maxKey.reserve(leaves.size());
        for (const auto &leaf : leaves) maxKey.push_back(leaf.back());
    }
    size_t leaf_for(const Key &key) const {
        Entry probe{key.name, key.value};
        auto it = std::lower_bound(maxKey.begin(), maxKey.end(), probe, less_entry);
        return static_cast<size_t>(it - maxKey.begin());
    }
    static bool write_u32(std::ofstream &f, uint32_t x) {
        f.write(reinterpret_cast<const char *>(&x), sizeof(x)); return bool(f);
    }
    static bool read_u32(std::ifstream &f, uint32_t &x) {
        f.read(reinterpret_cast<char *>(&x), sizeof(x)); return bool(f);
    }
public:
    BPlusTree() { load(); }
    ~BPlusTree() { if (dirty) save(); }

    void load() {
        std::ifstream f(FILE_NAME, std::ios::binary);
        if (!f) return;
        char magic[8]; uint32_t version, blocks;
        f.read(magic, 8);
        if (!f || std::string(magic, 8) != "OJBP0001" || !read_u32(f, version) || version != 1 || !read_u32(f, blocks) || blocks > 100000) return;
        std::vector<std::vector<Entry>> loaded;
        loaded.reserve(blocks);
        for (uint32_t b = 0; b < blocks; ++b) {
            uint32_t cnt;
            if (!read_u32(f, cnt) || cnt == 0 || cnt > LEAF_CAPACITY * 2) return;
            std::vector<Entry> leaf;
            leaf.reserve(cnt);
            for (uint32_t j = 0; j < cnt; ++j) {
                uint32_t len, valueBits;
                if (!read_u32(f, len) || len > 64 || !read_u32(f, valueBits)) return;
                std::string s(len, '\0');
                f.read(s.data(), len);
                if (!f) return;
                leaf.push_back({std::move(s), static_cast<int32_t>(valueBits)});
            }
            if (!std::is_sorted(leaf.begin(), leaf.end(), less_entry)) return;
            loaded.push_back(std::move(leaf));
        }
        leaves = std::move(loaded);
        rebuild_index();
    }
    void save() const {
        const std::string temp = std::string(FILE_NAME) + ".tmp";
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write("OJBP0001", 8);
        write_u32(f, 1); write_u32(f, static_cast<uint32_t>(leaves.size()));
        for (const auto &leaf : leaves) {
            write_u32(f, static_cast<uint32_t>(leaf.size()));
            for (const auto &e : leaf) {
                write_u32(f, static_cast<uint32_t>(e.name.size()));
                write_u32(f, static_cast<uint32_t>(static_cast<int32_t>(e.value)));
                f.write(e.name.data(), static_cast<std::streamsize>(e.name.size()));
            }
        }
        f.close();
        if (f) std::rename(temp.c_str(), FILE_NAME);
    }
    void insert(std::string name, int value) {
        Key key{name, value};
        if (leaves.empty()) {
            leaves.push_back({{std::move(name), value}});
            maxKey.push_back(leaves[0].back());
            dirty = true;
            return;
        }
        size_t p = leaf_for(key);
        if (p == leaves.size()) p = leaves.size() - 1;
        auto &leaf = leaves[p];
        auto it = std::lower_bound(leaf.begin(), leaf.end(), key, less_key);
        if (it != leaf.end() && it->name == name && it->value == value) return;
        leaf.insert(it, {std::move(name), value});
        if (leaf.size() > LEAF_CAPACITY) {
            std::vector<Entry> right(std::make_move_iterator(leaf.begin() + leaf.size()/2), std::make_move_iterator(leaf.end()));
            leaf.erase(leaf.begin() + leaf.size()/2, leaf.end());
            leaves.insert(leaves.begin() + p + 1, std::move(right));
            maxKey[p] = leaves[p].back();
            maxKey.insert(maxKey.begin() + p + 1, leaves[p + 1].back());
        } else {
            maxKey[p] = leaf.back();
        }
        dirty = true;
    }
    void erase(const std::string &name, int value) {
        if (leaves.empty()) return;
        Key key{name, value}; size_t p = leaf_for(key);
        if (p == leaves.size()) return;
        auto &leaf = leaves[p];
        auto it = std::lower_bound(leaf.begin(), leaf.end(), key, less_key);
        if (it == leaf.end() || it->name != name || it->value != value) return;
        leaf.erase(it);
        if (leaf.empty()) {
            leaves.erase(leaves.begin() + p);
            maxKey.erase(maxKey.begin() + p);
        } else {
            maxKey[p] = leaf.back();
        }
        dirty = true;
    }
    void find(const std::string &name) const {
        if (leaves.empty()) { std::cout << "null\n"; return; }
        Key key{name, std::numeric_limits<int>::min()}; size_t p = leaf_for(key);
        bool any = false;
        for (; p < leaves.size(); ++p) {
            const auto &leaf = leaves[p];
            auto it = std::lower_bound(leaf.begin(), leaf.end(), key, less_key);
            for (; it != leaf.end() && it->name == name; ++it) {
                if (any) std::cout << ' ';
                std::cout << it->value; any = true;
            }
            if (leaf.empty() || leaf.back().name > name || (it != leaf.end() && it->name > name)) break;
        }
        if (!any) std::cout << "null";
        std::cout << '\n';
    }
};

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    BPlusTree db;
    int n; if (!(std::cin >> n)) return 0;
    std::string command, index; int value;
    while (n--) {
        std::cin >> command >> index;
        if (command == "find") db.find(index);
        else { std::cin >> value; if (command == "insert") db.insert(std::move(index), value); else db.erase(index, value); }
    }
}
