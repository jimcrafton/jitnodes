#include "jitnodes/geo/geoFile.h"

#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>

#include "jitnodes/core/utf8.h"
#include "jitnodes/core/xxhash.h"

namespace jitnodes {

namespace {

static_assert(std::endian::native == std::endian::little, ".jngeo is little-endian and the loader copies arrays as they are");

constexpr char magic[8] = {'J', 'N', 'G', 'E', 'O', '1', 0, 0};
constexpr uint32_t formatMajor = 1;
constexpr uint32_t formatMinor = 1;
constexpr uint32_t formatVersion = (formatMajor << 16) | formatMinor;
constexpr uint32_t endianMarker = 0x01020304;
constexpr size_t headerSize = 64;
constexpr size_t sectionEntrySize = 24;
constexpr size_t alignment = 64;
constexpr size_t maxSections = 64;
constexpr uint64_t maxElements = 0x7fffffff;

// stringTables (1.1) exists only when an attribute is a string attribute; the payload stays last in the file.
enum SectionId : uint32_t { strings = 1, counts = 2, prims = 3, attribs = 4, details = 5, payload = 6, stringTables = 7, sectionTotal = 7 };

constexpr size_t primEntrySize = 12;
constexpr size_t attribEntrySize = 32;
constexpr size_t detailEntrySize = 8;

size_t alignUp(size_t n) { return (n + alignment - 1) & ~(alignment - 1); }

// One table of the stringTables section: u32 attribute index, u32 count, u32 offsets[count + 1], the text, padded to 4 bytes.
size_t tableBytes(const StringTable& table) {
    size_t text = 0;
    for (const std::string& s : table.strings) text += s.size();
    return (8 + 4 * (table.strings.size() + 1) + text + 3) & ~size_t{3};
}

template <class T>
void put(std::vector<uint8_t>& out, size_t at, T value) {
    std::memcpy(out.data() + at, &value, sizeof(T));
}

[[noreturn]] void bad(const std::string& why) { throw std::runtime_error("invalid .jngeo file: " + why); }

struct Section {
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool present = false;

    template <class T>
    T get(size_t at) const {
        T v;
        std::memcpy(&v, data + at, sizeof(T));
        return v;
    }
};

struct AttribEntry {
    std::string_view name;
    AttribClass cls;
    AttribType type;
    uint64_t count;
    uint64_t offset;
};

}  // namespace

std::vector<uint8_t> saveGeometry(const Geometry& geo) {
    std::vector<std::string> names;
    std::map<std::string, uint32_t> nameIndex;
    const auto intern = [&](const std::string& s) {
        auto [it, inserted] = nameIndex.try_emplace(s, static_cast<uint32_t>(names.size()));
        if (inserted) names.push_back(s);
        return it->second;
    };

    struct Dir {
        uint32_t name;
        AttribClass cls;
        const AttribArray* array;
        uint64_t offset;
    };
    std::vector<Dir> dir;
    std::vector<size_t> stringDir;
    size_t tablesSize = 8;
    uint64_t payloadBytes = 0;
    for (AttribClass cls : {AttribClass::point, AttribClass::primitive}) {
        for (const auto& [name, array] : geo.attribs(cls)) {
            if (array.type() == AttribType::string) {
                for (const std::string& s : array.table().strings)
                    if (!isValidUtf8(s)) throw std::runtime_error("string attribute '" + name + "' holds text that is not valid UTF-8");
                stringDir.push_back(dir.size());
                tablesSize += tableBytes(array.table());
            }
            dir.push_back({intern(name), cls, &array, payloadBytes});
            payloadBytes += static_cast<uint64_t>(array.paddedSize()) * 4;
        }
    }
    for (const auto& [name, value] : geo.details()) intern(name);

    size_t stringBytes = 0;
    for (const std::string& s : names) stringBytes += s.size();

    const size_t primCount = geo.primitiveCount();
    size_t sizes[sectionTotal + 1] = {};
    sizes[strings] = 4 + 4 * (names.size() + 1) + stringBytes;
    sizes[counts] = 16;
    sizes[prims] = 8 + primEntrySize * primCount;
    sizes[attribs] = 8 + attribEntrySize * dir.size();
    sizes[details] = 8 + detailEntrySize * geo.details().size();
    sizes[payload] = payloadBytes;
    sizes[stringTables] = tablesSize;

    const bool hasTables = !stringDir.empty();
    const uint32_t sectionCount = hasTables ? stringTables : payload;
    std::vector<uint32_t> layout = {strings, counts, prims, attribs, details};
    if (hasTables) layout.push_back(stringTables);
    layout.push_back(payload);

    size_t offsets[sectionTotal + 1] = {};
    size_t cursor = alignUp(headerSize + sectionEntrySize * sectionCount);
    for (uint32_t id : layout) {
        offsets[id] = cursor;
        cursor = alignUp(cursor + sizes[id]);
    }
    const size_t fileSize = offsets[payload] + sizes[payload];

    std::vector<uint8_t> out(fileSize, 0);
    std::memcpy(out.data(), magic, sizeof(magic));
    put<uint32_t>(out, 8, formatVersion);
    put<uint32_t>(out, 12, endianMarker);
    put<uint32_t>(out, 20, sectionCount);
    put<uint64_t>(out, 24, fileSize);
    for (uint32_t id = strings; id <= sectionCount; ++id) {
        const size_t entry = headerSize + sectionEntrySize * (id - 1);
        put<uint32_t>(out, entry, id);
        put<uint64_t>(out, entry + 8, offsets[id]);
        put<uint64_t>(out, entry + 16, sizes[id]);
    }

    size_t at = offsets[strings];
    put<uint32_t>(out, at, static_cast<uint32_t>(names.size()));
    uint32_t running = 0;
    const size_t textBase = at + 4 + 4 * (names.size() + 1);
    for (size_t i = 0; i < names.size(); ++i) {
        put<uint32_t>(out, at + 4 + 4 * i, running);
        std::memcpy(out.data() + textBase + running, names[i].data(), names[i].size());
        running += static_cast<uint32_t>(names[i].size());
    }
    put<uint32_t>(out, at + 4 + 4 * names.size(), running);

    at = offsets[counts];
    put<uint64_t>(out, at, geo.pointCount());
    put<uint64_t>(out, at + 8, primCount);

    at = offsets[prims];
    put<uint64_t>(out, at, primCount);
    for (size_t i = 0; i < primCount; ++i) {
        const Primitive& p = geo.primitives()[i];
        const size_t e = at + 8 + primEntrySize * i;
        put<uint32_t>(out, e, p.start);
        put<uint32_t>(out, e + 4, p.count);
        out[e + 8] = p.closed ? 1 : 0;
        out[e + 9] = static_cast<uint8_t>(p.kind);
    }

    at = offsets[attribs];
    put<uint64_t>(out, at, dir.size());
    for (size_t i = 0; i < dir.size(); ++i) {
        const size_t e = at + 8 + attribEntrySize * i;
        const AttribArray& a = *dir[i].array;
        put<uint32_t>(out, e, dir[i].name);
        out[e + 4] = static_cast<uint8_t>(dir[i].cls);
        out[e + 5] = static_cast<uint8_t>(a.type());
        put<uint64_t>(out, e + 8, a.size());
        put<uint64_t>(out, e + 16, dir[i].offset);
        put<uint64_t>(out, e + 24, static_cast<uint64_t>(a.paddedSize()) * 4);
        if (a.paddedSize())
            std::memcpy(out.data() + offsets[payload] + dir[i].offset, a.data(), a.paddedSize() * 4);
    }

    at = offsets[details];
    put<uint64_t>(out, at, geo.details().size());
    size_t d = 0;
    for (const auto& [name, value] : geo.details()) {
        const size_t e = at + 8 + detailEntrySize * d++;
        put<uint32_t>(out, e, nameIndex.at(name));
        put<float>(out, e + 4, value);
    }

    if (hasTables) {
        at = offsets[stringTables];
        put<uint64_t>(out, at, stringDir.size());
        at += 8;
        for (size_t index : stringDir) {
            const StringTable& table = dir[index].array->table();
            put<uint32_t>(out, at, static_cast<uint32_t>(index));
            put<uint32_t>(out, at + 4, static_cast<uint32_t>(table.strings.size()));
            const size_t tableText = at + 8 + 4 * (table.strings.size() + 1);
            uint32_t used = 0;
            for (size_t i = 0; i < table.strings.size(); ++i) {
                put<uint32_t>(out, at + 8 + 4 * i, used);
                std::memcpy(out.data() + tableText + used, table.strings[i].data(), table.strings[i].size());
                used += static_cast<uint32_t>(table.strings[i].size());
            }
            put<uint32_t>(out, at + 8 + 4 * table.strings.size(), used);
            at += tableBytes(table);
        }
    }

    put<uint64_t>(out, 32, xxh64(out.data() + headerSize, fileSize - headerSize));
    return out;
}

Geometry loadGeometry(const uint8_t* data, size_t size) {
    if (size < headerSize) bad("shorter than the header");
    if (std::memcmp(data, magic, sizeof(magic)) != 0) bad("wrong magic");
    const auto header32 = [&](size_t at) { uint32_t v; std::memcpy(&v, data + at, 4); return v; };
    const auto header64 = [&](size_t at) { uint64_t v; std::memcpy(&v, data + at, 8); return v; };
    if (header32(8) >> 16 != formatMajor) bad("unsupported format version " + std::to_string(header32(8) >> 16) + "." + std::to_string(header32(8) & 0xffff));
    if (header32(12) != endianMarker) bad("wrong byte order marker");
    if (header32(16) != 0) bad("unknown flags");
    const uint32_t sectionCount = header32(20);
    if (sectionCount > maxSections) bad("too many sections");
    if (header64(24) != size) bad("size does not match the header (truncated or extended)");
    for (size_t i = 40; i < headerSize; ++i)
        if (data[i] != 0) bad("reserved header bytes are not zero");
    if (header64(32) != xxh64(data + headerSize, size - headerSize)) bad("checksum mismatch");

    const size_t tableEnd = headerSize + sectionEntrySize * static_cast<size_t>(sectionCount);
    if (tableEnd > size) bad("section table past the end");
    Section sections[sectionTotal + 1];
    for (uint32_t i = 0; i < sectionCount; ++i) {
        const size_t e = headerSize + sectionEntrySize * i;
        uint32_t id, flags;
        uint64_t offset, bytes;
        std::memcpy(&id, data + e, 4);
        std::memcpy(&flags, data + e + 4, 4);
        std::memcpy(&offset, data + e + 8, 8);
        std::memcpy(&bytes, data + e + 16, 8);
        if (flags != 0) bad("unknown section flags");
        if (offset % alignment != 0 || offset < tableEnd || offset > size || bytes > size - offset) bad("section outside the file");
        if (id < strings || id > stringTables) continue;
        if (sections[id].present) bad("duplicate section");
        sections[id] = {data + offset, static_cast<size_t>(bytes), true};
    }
    for (uint32_t id = strings; id <= payload; ++id)
        if (!sections[id].present) bad("missing section " + std::to_string(id));

    const Section& strSec = sections[strings];
    if (strSec.size < 8) bad("string table too small");
    const uint32_t nameCount = strSec.get<uint32_t>(0);
    if (nameCount > (strSec.size - 8) / 4) bad("string table larger than its section");
    const size_t textBase = 4 + 4 * (static_cast<size_t>(nameCount) + 1);
    const size_t textBytes = strSec.size - textBase;
    std::vector<std::string_view> names(nameCount);
    uint32_t previous = 0;
    for (uint32_t i = 0; i < nameCount; ++i) {
        const uint32_t from = strSec.get<uint32_t>(4 + 4 * i);
        const uint32_t to = strSec.get<uint32_t>(8 + 4 * i);
        if (from != previous || to < from || to > textBytes) bad("string offsets out of order");
        names[i] = std::string_view(reinterpret_cast<const char*>(strSec.data + textBase + from), to - from);
        previous = to;
    }

    if (sections[counts].size < 16) bad("counts section too small");
    const uint64_t pointCount = sections[counts].get<uint64_t>(0);
    const uint64_t primCount = sections[counts].get<uint64_t>(8);
    if (pointCount > maxElements || primCount > maxElements) bad("element count too large");

    const Section& primSec = sections[prims];
    if (primSec.size < 8 || primSec.get<uint64_t>(0) != primCount || (primSec.size - 8) / primEntrySize < primCount) bad("primitive table does not match");
    std::vector<Primitive> primitives(static_cast<size_t>(primCount));
    for (size_t i = 0; i < primitives.size(); ++i) {
        const size_t e = 8 + primEntrySize * i;
        Primitive& p = primitives[i];
        p.start = primSec.get<uint32_t>(e);
        p.count = primSec.get<uint32_t>(e + 4);
        const uint8_t closed = primSec.data[e + 8], kind = primSec.data[e + 9];
        if (closed > 1 || kind > static_cast<uint8_t>(PrimKind::cubic)) bad("bad primitive flags");
        if (static_cast<uint64_t>(p.start) + p.count > pointCount) bad("primitive points outside the geometry");
        p.closed = closed != 0;
        p.kind = static_cast<PrimKind>(kind);
    }

    const Section& attribSec = sections[attribs];
    if (attribSec.size < 8) bad("attribute directory too small");
    const uint64_t attribCount = attribSec.get<uint64_t>(0);
    if (attribCount > (attribSec.size - 8) / attribEntrySize) bad("attribute directory larger than its section");
    std::vector<AttribEntry> entries(static_cast<size_t>(attribCount));
    std::set<std::pair<int, std::string_view>> seen;
    uint64_t nextFree = 0;
    bool hasX = false, hasY = false;
    for (size_t i = 0; i < entries.size(); ++i) {
        const size_t e = 8 + attribEntrySize * i;
        AttribEntry& a = entries[i];
        const uint32_t nameIdx = attribSec.get<uint32_t>(e);
        const uint8_t cls = attribSec.data[e + 4], type = attribSec.data[e + 5];
        a.count = attribSec.get<uint64_t>(e + 8);
        a.offset = attribSec.get<uint64_t>(e + 16);
        const uint64_t bytes = attribSec.get<uint64_t>(e + 24);
        if (nameIdx >= nameCount || names[nameIdx].empty()) bad("bad attribute name");
        if (cls > static_cast<uint8_t>(AttribClass::primitive) || type > static_cast<uint8_t>(AttribType::string)) bad("bad attribute class or type");
        a.name = names[nameIdx];
        a.cls = static_cast<AttribClass>(cls);
        a.type = static_cast<AttribType>(type);
        if (a.count != (a.cls == AttribClass::point ? pointCount : primCount)) bad("attribute length differs from its element count");
        if (bytes != padToLanes(static_cast<size_t>(a.count)) * 4) bad("attribute byte size is not the padded size");
        // Entries are written in increasing order without overlap, so a small file cannot make many attributes share one large block.
        if (a.offset % alignment != 0 || a.offset < nextFree || a.offset > sections[payload].size || bytes > sections[payload].size - a.offset) bad("attribute data outside the payload");
        nextFree = a.offset + bytes;
        if (!seen.emplace(cls, a.name).second) bad("duplicate attribute");
        if (a.cls == AttribClass::point && a.type == AttribType::float32) {
            hasX |= a.name == "x";
            hasY |= a.name == "y";
        }
    }
    if (!hasX || !hasY) bad("missing point attributes x and y");

    std::vector<std::shared_ptr<const StringTable>> tables(entries.size());
    size_t tableCount = 0;
    if (sections[stringTables].present) {
        const Section& ts = sections[stringTables];
        if (ts.size < 8) bad("string tables section too small");
        const uint64_t declared = ts.get<uint64_t>(0);
        if (declared > (ts.size - 8) / 16) bad("string tables larger than their section");
        size_t at = 8;
        for (uint64_t t = 0; t < declared; ++t) {
            if (ts.size - at < 8) bad("string table past the end");
            const uint32_t index = ts.get<uint32_t>(at), count = ts.get<uint32_t>(at + 4);
            if (index >= entries.size() || entries[index].type != AttribType::string || tables[index]) bad("string table for a bad attribute");
            if (count == 0 || count > maxStrings) bad("bad string count");
            const size_t offsetBytes = 4 * (static_cast<size_t>(count) + 1);
            if (offsetBytes > ts.size - at - 8) bad("string table larger than its section");
            const size_t base = at + 8, tableText = base + offsetBytes, textLimit = ts.size - tableText;
            auto table = std::make_shared<StringTable>();
            table->strings.clear();
            table->index.clear();
            table->strings.reserve(count);
            uint32_t from = 0;
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t start = ts.get<uint32_t>(base + 4 * static_cast<size_t>(i));
                const uint32_t end = ts.get<uint32_t>(base + 4 * static_cast<size_t>(i) + 4);
                if (start != from || end < start || end > textLimit) bad("string offsets out of order");
                const std::string_view text(reinterpret_cast<const char*>(ts.data + tableText + start), end - start);
                if (i == 0 && !text.empty()) bad("first string is not empty");
                if (!isValidUtf8(text)) bad("string is not valid UTF-8");
                if (!table->index.emplace(std::string(text), static_cast<int32_t>(i)).second) bad("duplicate string");
                table->strings.emplace_back(text);
                from = end;
            }
            at = (tableText + from + 3) & ~size_t{3};
            if (at > ts.size) bad("string table past the end");
            tables[index] = std::move(table);
            ++tableCount;
        }
        if (at != ts.size) bad("string tables section has trailing bytes");
    }
    size_t stringAttribs = 0;
    for (const AttribEntry& a : entries) stringAttribs += a.type == AttribType::string;
    if (stringAttribs != tableCount) bad("string attribute without a string table");

    const Section& detailSec = sections[details];
    if (detailSec.size < 8) bad("detail table too small");
    const uint64_t detailCount = detailSec.get<uint64_t>(0);
    if (detailCount > (detailSec.size - 8) / detailEntrySize) bad("detail table larger than its section");
    std::map<std::string, float> detailValues;
    for (uint64_t i = 0; i < detailCount; ++i) {
        const size_t e = 8 + detailEntrySize * static_cast<size_t>(i);
        const uint32_t nameIdx = detailSec.get<uint32_t>(e);
        if (nameIdx >= nameCount) bad("bad detail name");
        if (!detailValues.emplace(std::string(names[nameIdx]), detailSec.get<float>(e + 4)).second) bad("duplicate detail");
    }

    Geometry geo;
    geo.resizePoints(static_cast<size_t>(pointCount));
    for (const Primitive& p : primitives) geo.addPrimitive(p);
    for (size_t i = 0; i < entries.size(); ++i) {
        const AttribEntry& a = entries[i];
        AttribArray& array = geo.attrib(a.cls, std::string(a.name), a.type);
        if (a.count) std::memcpy(array.data(), sections[payload].data + a.offset, static_cast<size_t>(a.count) * 4);
        if (a.type != AttribType::string) continue;
        const size_t idCount = tables[i]->strings.size();
        for (size_t k = 0; k < a.count; ++k)
            if (array.ints()[k] < 0 || static_cast<size_t>(array.ints()[k]) >= idCount) bad("string id outside its table");
        array.setTable(std::move(tables[i]));
    }
    for (const auto& [name, value] : detailValues) geo.setDetail(name, value);
    return geo;
}

void saveGeometryToFile(const Geometry& geo, const std::string& path) {
    const std::vector<uint8_t> bytes = saveGeometry(geo);
    // Written beside the target and renamed, so an interrupted save never leaves a truncated cache that looks valid.
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary);
        if (!out) throw std::runtime_error("cannot open '" + temp + "' for writing");
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error("failed writing '" + temp + "'");
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        throw std::runtime_error("cannot replace '" + path + "'");
    }
}

Geometry loadGeometryFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("cannot open '" + path + "'");
    const std::streamoff size = in.tellg();
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!in) throw std::runtime_error("failed reading '" + path + "'");
    return loadGeometry(bytes.data(), bytes.size());
}

}  // namespace jitnodes
