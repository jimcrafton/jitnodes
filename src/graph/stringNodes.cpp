#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "jitnodes/graph/network.h"

namespace jitnodes {

namespace {

AttribClass classOf(const std::string& s) {
    if (s == "point") return AttribClass::point;
    if (s == "primitive") return AttribClass::primitive;
    throw std::runtime_error("attribute class must be 'point' or 'primitive', got '" + s + "'");
}

std::vector<std::string> splitNames(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(',', start);
        if (end == std::string::npos) end = text.size();
        size_t from = start, to = end;
        while (from < to && (text[from] == ' ' || text[from] == '\t')) ++from;
        while (to > from && (text[to - 1] == ' ' || text[to - 1] == '\t')) --to;
        out.push_back(text.substr(from, to - from));
        start = end + 1;
    }
    return out;
}

std::string expandPattern(const std::string& pattern, size_t index) {
    std::string out;
    const std::string number = std::to_string(index);
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '$' && i + 1 < pattern.size() && pattern[i + 1] == 'P') {
            out += number;
            ++i;
        } else {
            out += pattern[i];
        }
    }
    return out;
}

class AttribStringNode : public Node {
public:
    AttribStringNode() : Node("attribString", 1) {
        declareString("name", "label");
        declareString("class", "point");
        declareString("source", "pattern");
        declareString("text", "pt$P");
        declareString("group", "");
        declareString("from", "");
        declareString("names", "");
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        auto geo = std::make_shared<Geometry>(*in[0]);
        const AttribClass cls = classOf(getString("class"));
        const std::string& source = getString("source");
        if (source != "group" && source != "ints" && source != "pattern") throw std::runtime_error("attribString: source must be 'group', 'ints' or 'pattern'");
        const AttribArray* existing = geo->findAttrib(cls, getString("name"));
        if (existing != nullptr && existing->type() != AttribType::string) throw std::runtime_error("attribString: '" + getString("name") + "' exists and is not a string attribute");
        const size_t count = geo->count(cls);

        std::vector<int32_t> ids;
        if (source == "ints") {
            const AttribArray* from = geo->findAttrib(cls, getString("from"));
            if (from == nullptr || from->type() != AttribType::int32) throw std::runtime_error("attribString: 'from' must name an int attribute");
            ids.assign(count, 0);
            AttribArray& a = geo->attrib(cls, getString("name"), AttribType::string);
            std::vector<int32_t> byValue;
            for (const std::string& name : splitNames(getString("names"))) byValue.push_back(a.intern(name));
            for (size_t i = 0; i < count; ++i) {
                const int32_t v = from->ints()[i];
                ids[i] = v >= 0 && static_cast<size_t>(v) < byValue.size() ? byValue[static_cast<size_t>(v)] : 0;
            }
            for (size_t i = 0; i < count; ++i) a.ints()[i] = ids[i];
            return geo;
        }

        AttribArray& a = geo->attrib(cls, getString("name"), AttribType::string);
        if (source == "group") {
            const int32_t id = a.intern(getString("text"));
            for (size_t i = 0; i < count; ++i)
                if (geo->inGroup(cls, getString("group"), i)) a.ints()[i] = id;
        } else {
            const std::string& pattern = getString("text");
            for (size_t i = 0; i < count; ++i) a.ints()[i] = a.intern(expandPattern(pattern, i));
        }
        return geo;
    }
};

class GroupByStringNode : public Node {
public:
    GroupByStringNode() : Node("groupByString", 1) {
        declareString("class", "point");
        declareString("attrib", "label");
        declareString("match", "equals");
        declareString("text", "");
        declareString("group", "match");
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        auto geo = std::make_shared<Geometry>(*in[0]);
        const AttribClass cls = classOf(getString("class"));
        const std::string& match = getString("match");
        if (match != "equals" && match != "startsWith" && match != "contains") throw std::runtime_error("groupByString: match must be 'equals', 'startsWith' or 'contains'");
        const AttribArray* s = geo->findAttrib(cls, getString("attrib"));
        if (s == nullptr || s->type() != AttribType::string) throw std::runtime_error("groupByString: '" + getString("attrib") + "' is not a string attribute");

        const std::string& text = getString("text");
        const std::vector<std::string>& strings = s->table().strings;
        std::vector<int32_t> hit(strings.size());
        for (size_t k = 0; k < strings.size(); ++k) {
            const std::string& t = strings[k];
            if (match == "equals") hit[k] = t == text;
            else if (match == "startsWith") hit[k] = t.compare(0, text.size(), text) == 0 && t.size() >= text.size();
            else hit[k] = t.find(text) != std::string::npos;
        }
        const size_t count = geo->count(cls);
        std::vector<int32_t> member(count);
        for (size_t i = 0; i < count; ++i) member[i] = hit[static_cast<size_t>(s->ints()[i])];
        AttribArray& group = geo->createGroup(cls, getString("group"));
        for (size_t i = 0; i < count; ++i) group.ints()[i] = member[i];
        return geo;
    }
};

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerStringNodes() {
    add<AttribStringNode>("attribString");
    add<GroupByStringNode>("groupByString");
}

}  // namespace jitnodes
