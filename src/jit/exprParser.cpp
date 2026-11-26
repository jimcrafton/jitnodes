#include "jitnodes/jit/exprParser.h"

#include <cctype>
#include <cstdlib>
#include <map>
#include <stdexcept>

#include "jitnodes/core/stackGuard.h"
#include "jitnodes/graph/ramp.h"

namespace jitnodes {

namespace {

class Parser {
public:
    Parser(const std::string& src, const std::vector<std::string>& paramNames, const std::map<std::string, float>& constants,
           std::vector<WrangleParam>* declared)
        : src_(src), paramNames_(paramNames), constants_(constants), declared_(declared) {
        spec_.paramCount = static_cast<int>(paramNames.size());
        if (declared_ != nullptr) {
            declared_->clear();
        }
    }

    KernelSpec run() {
        skipSpace();
        while (pos_ < src_.size()) {
            if (declared_ != nullptr && src_[pos_] == '|') {
                if (assigned_) {
                    fail("declarations must come before the first assignment");
                }
                declarations();
                skipSpace();
                continue;
            }
            assigned_ = true;
            std::string name = identifier();
            if (name.empty()) fail("expected assignment target");
            for (const auto& p : paramNames_)
                if (p == name) fail("cannot assign to parameter '" + name + "'");
            if (declaredSlots_.count(name) != 0 || rampSlots_.count(name) != 0) {
                fail("cannot assign to parameter '" + name + "'");
            }
            expect('=');
            int value = asValue(expr());
            expect(';');
            if (isLocal(name)) {
                locals_[name] = value;
                skipSpace();
                continue;
            }
            int channel = spec_.channelIndex(name);
            current_[channel] = value;
            skipSpace();
        }
        for (const auto& [channel, node] : current_) spec_.stores.emplace_back(channel, node);
        return std::move(spec_);
    }

private:
    [[noreturn]] void fail(const std::string& msg) const {
        throw std::runtime_error("wrangle parse error at offset " + std::to_string(pos_) + ": " + msg);
    }

    bool peekIs(char c) {
        skipSpace();
        return pos_ < src_.size() && src_[pos_] == c;
    }

    float signedNumber() {
        skipSpace();
        const bool negative = accept("-");
        skipSpace();
        char* end = nullptr;
        const char* start = src_.c_str() + pos_;
        if (pos_ >= src_.size() || !(std::isdigit(static_cast<unsigned char>(*start)) || *start == '.')) {
            fail("expected a number");
        }
        const float v = std::strtof(start, &end);
        if (end == start) {
            fail("bad number");
        }
        pos_ = static_cast<size_t>(end - src_.c_str());
        return negative ? -v : v;
    }

    // `| name [= value] [(min, max)], ... |`: pos_ is on the opening bar.
    void declarations() {
        ++pos_;
        do {
            declaration();
        } while (accept(","));
        expect('|');
    }

    // One item of a list: `gain`, `gain = 1.5`, `radius = 2(0, 10)`, `count(1,)`, `ratio(,1)`. A bound left empty is not set.
    void declaration() {
        const std::string name = identifier();
        if (name.empty()) {
            fail("expected a parameter name");
        }
        static const char* const reserved[] = {"code", "class", "time", "frame", "pi", "parent", "sampleIndex"};
        bool taken = declaredSlots_.count(name) != 0 || rampSlots_.count(name) != 0 || constants_.count(name) != 0 || isLocal(name) || name.find('.') != std::string::npos ||
                     name.compare(0, 2, "__") == 0;
        for (const auto& p : paramNames_) {
            taken = taken || p == name;
        }
        for (const char* r : reserved) {
            taken = taken || name == r;
        }
        if (taken) {
            fail("cannot declare parameter '" + name + "'");
        }
        WrangleParam param;
        param.name = name;
        if (accept("=")) {
            if (acceptRamp()) {
                rampDeclaration(param);
                return;
            }
            param.value = signedNumber();
        }
        if (accept("(")) {
            if (!peekIs(',')) {
                param.minimum = signedNumber();
                param.hasMinimum = true;
            }
            expect(',');
            if (!peekIs(')')) {
                param.maximum = signedNumber();
                param.hasMaximum = true;
            }
            expect(')');
            if (param.hasMinimum && param.hasMaximum && param.minimum > param.maximum) {
                fail("the limits of '" + name + "' have the minimum above the maximum");
            }
        }
        param.slot = spec_.paramCount++;
        declaredSlots_[name] = param.slot;
        declared_->push_back(param);
    }

    // Takes `ramp(`, leaving anything else where it is.
    bool acceptRamp() {
        skipSpace();
        const size_t start = pos_;
        if (identifier() == "ramp" && accept("(")) {
            return true;
        }
        pos_ = start;
        return false;
    }

    // The rest of `name = ramp("...")`.
    void rampDeclaration(WrangleParam& param) {
        static const char* const functions[] = {"min", "max", "abs", "sqrt", "sin", "cos", "exp", "log", "floor", "ceil", "pow", "clamp", "lerp"};
        for (const char* function : functions) {
            if (param.name == function) {
                fail("cannot declare ramp '" + param.name + "', a function has that name");
            }
        }
        param.ramp = true;
        param.rampDefault = atStringLiteral() ? stringLiteral() : Ramp().toText();
        expect(')');
        if (Ramp::parentReference(param.rampDefault).empty()) {
            const std::string problem = Ramp::check(param.rampDefault);
            if (!problem.empty()) {
                fail("ramp '" + param.name + "': " + problem);
            }
        }
        param.slot = spec_.paramCount;
        spec_.paramCount += WrangleParam::rampSlots;
        rampSlots_[param.name] = param.slot;
        declared_->push_back(param);
    }

    // The value of a declared ramp at `x`: linear between the two table entries around it. The comparisons are written so that a NaN
    // ends up at the start of the table, never outside it.
    int rampCall(int slot, int x) {
        ExprGraph& g = spec_.graph;
        const int zero = g.constant(0.0f);
        const int last = g.constant(static_cast<float>(WrangleParam::rampSamples - 1));
        int t = g.binary(Op::mul, g.binary(Op::sub, x, g.param(slot)), g.param(slot + 1));
        t = g.select(g.binary(Op::gt, t, zero), t, zero);
        t = g.select(g.binary(Op::lt, t, last), t, last);
        const int low = g.unary(Op::floor, t);
        const int next = g.binary(Op::add, low, g.constant(1.0f));
        const int high = g.select(g.binary(Op::lt, next, last), next, last);
        const int from = g.gather(slot + 2, low);
        const int to = g.gather(slot + 2, high);
        return g.binary(Op::add, from, g.binary(Op::mul, g.binary(Op::sub, to, from), g.binary(Op::sub, t, low)));
    }

    // '__' is reserved (the scalar-expression wrapper stores to `__result`).
    static bool isLocal(const std::string& name) { return name.size() > 1 && name[0] == '_' && name[1] != '_'; }

    // A '#' starts a comment that runs to the end of the line; strings never reach here, so a '#' inside one is text.
    void skipSpace() {
        for (;;) {
            while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_]))) {
                ++pos_;
            }
            if (pos_ >= src_.size() || src_[pos_] != '#') {
                return;
            }
            while (pos_ < src_.size() && src_[pos_] != '\n') {
                ++pos_;
            }
        }
    }

    bool accept(const char* tok) {
        skipSpace();
        size_t n = std::char_traits<char>::length(tok);
        if (src_.compare(pos_, n, tok) != 0) return false;
        pos_ += n;
        return true;
    }

    // Like accept, but refuses to consume a prefix of a longer operator (e.g. '<' of '<=').
    bool acceptOp(const char* tok, const char* notFollowedBy = "") {
        skipSpace();
        size_t n = std::char_traits<char>::length(tok);
        if (src_.compare(pos_, n, tok) != 0) return false;
        if (pos_ + n < src_.size()) {
            for (const char* p = notFollowedBy; *p; ++p)
                if (src_[pos_ + n] == *p) return false;
        }
        pos_ += n;
        return true;
    }

    void expect(char c) {
        skipSpace();
        if (pos_ >= src_.size() || src_[pos_] != c) fail(std::string("expected '") + c + "'");
        ++pos_;
    }

    std::string identifier() {
        skipSpace();
        size_t start = pos_;
        if (pos_ < src_.size() && (std::isalpha(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '_')) {
            while (pos_ < src_.size() && (std::isalnum(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '_' || isComponentDot())) ++pos_;
        }
        return src_.substr(start, pos_ - start);
    }

    int asValue(int node) {
        switch (spec_.graph.nodes()[static_cast<size_t>(node)].op) {
            case Op::lt: case Op::gt: case Op::le: case Op::ge: case Op::eq:
            case Op::logicalAnd: case Op::logicalOr: case Op::logicalNot:
                return spec_.graph.select(node, spec_.graph.constant(1.0f), spec_.graph.constant(0.0f));
            default: return node;
        }
    }

    bool isComponentDot() const {
        return src_[pos_] == '.' && pos_ + 1 < src_.size() && std::isalpha(static_cast<unsigned char>(src_[pos_ + 1]));
    }

    int expr() {
        DepthGuard guard(*this);
        return ternary();
    }

    // Conditions must be real masks (all ones / all zeros) so every instruction set can blend with them;
    // any other value is truthy when it is not zero (NaN counts as true), as in C.
    int asMask(int node) {
        switch (spec_.graph.nodes()[static_cast<size_t>(node)].op) {
            case Op::lt: case Op::gt: case Op::le: case Op::ge: case Op::eq:
            case Op::logicalAnd: case Op::logicalOr: case Op::logicalNot:
                return node;
            default:
                return spec_.graph.unary(Op::logicalNot, spec_.graph.binary(Op::eq, node, spec_.graph.constant(0.0f)));
        }
    }

    int ternary() {
        int cond = logicalOr();
        if (accept("?")) {
            cond = asMask(cond);
            int a = expr();
            expect(':');
            int b = expr();
            return spec_.graph.select(cond, a, b);
        }
        return cond;
    }

    int logicalOr() {
        int l = logicalAnd();
        while (accept("||")) l = spec_.graph.binary(Op::logicalOr, asMask(l), asMask(logicalAnd()));
        return l;
    }

    int logicalAnd() {
        int l = comparison();
        while (accept("&&")) l = spec_.graph.binary(Op::logicalAnd, asMask(l), asMask(comparison()));
        return l;
    }

    bool atStringLiteral() {
        skipSpace();
        return pos_ < src_.size() && src_[pos_] == '"';
    }

    std::string stringLiteral() {
        ++pos_;
        std::string text;
        for (;; ++pos_) {
            if (pos_ >= src_.size() || src_[pos_] == '\n') fail("unterminated string");
            char c = src_[pos_];
            if (c == '"') break;
            if (c == '\\' && pos_ + 1 < src_.size() && (src_[pos_ + 1] == '"' || src_[pos_ + 1] == '\\')) c = src_[++pos_];
            text += c;
        }
        ++pos_;
        return text;
    }

    // `attribute == "text"` compares ids: the literal becomes a parameter that holds its id in the attribute's table (see KernelSpec::StringParam).
    int stringComparison(int attribute, const std::string& text, bool equal) {
        ExprGraph& g = spec_.graph;
        const ExprNode& n = g.nodes()[static_cast<size_t>(attribute)];
        if (n.op != Op::load) fail("a string can only be compared with an attribute");
        int param = -1;
        for (const KernelSpec::StringParam& sp : spec_.stringParams)
            if (sp.channel == n.index && sp.text == text) param = sp.paramIndex;
        if (param < 0) {
            param = spec_.paramCount++;
            spec_.stringParams.push_back({param, n.index, text});
        }
        const int same = g.binary(Op::eq, attribute, g.param(param));
        return equal ? same : g.unary(Op::logicalNot, same);
    }

    int comparison() {
        ExprGraph& g = spec_.graph;
        if (atStringLiteral()) {
            const std::string text = stringLiteral();
            const bool equal = accept("==");
            if (!equal && !accept("!=")) fail("a string can only be compared with == or !=");
            return stringComparison(additive(), text, equal);
        }
        int l = additive();
        if (accept("==")) {
            if (atStringLiteral()) return stringComparison(l, stringLiteral(), true);
            return g.binary(Op::eq, l, additive());
        }
        if (accept("!=")) {
            if (atStringLiteral()) return stringComparison(l, stringLiteral(), false);
            return g.unary(Op::logicalNot, g.binary(Op::eq, l, additive()));
        }
        if (accept("<=")) return g.binary(Op::le, l, additive());
        if (accept(">=")) return g.binary(Op::ge, l, additive());
        if (accept("<")) return g.binary(Op::lt, l, additive());
        if (accept(">")) return g.binary(Op::gt, l, additive());
        return l;
    }

    int additive() {
        int l = multiplicative();
        for (;;) {
            if (accept("+")) l = spec_.graph.binary(Op::add, l, multiplicative());
            else if (accept("-")) l = spec_.graph.binary(Op::sub, l, multiplicative());
            else return l;
        }
    }

    int multiplicative() {
        int l = unaryExpr();
        for (;;) {
            if (accept("*")) l = spec_.graph.binary(Op::mul, l, unaryExpr());
            else if (accept("/")) l = spec_.graph.binary(Op::div, l, unaryExpr());
            else return l;
        }
    }

    int unaryExpr() {
        DepthGuard guard(*this);
        if (accept("-")) return spec_.graph.unary(Op::neg, unaryExpr());
        if (acceptOp("!", "=")) return spec_.graph.unary(Op::logicalNot, asMask(unaryExpr()));
        return primary();
    }

    int primary() {
        skipSpace();
        if (pos_ >= src_.size()) fail("unexpected end of input");
        char c = src_[pos_];
        if (c == '"') fail("a string can only be compared with an attribute using == or !=");
        if (c == '(') {
            ++pos_;
            int v = expr();
            expect(')');
            return v;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            char* end = nullptr;
            float v = std::strtof(src_.c_str() + pos_, &end);
            if (end == src_.c_str() + pos_) fail("bad number");
            pos_ = static_cast<size_t>(end - src_.c_str());
            return spec_.graph.constant(v);
        }
        std::string name = identifier();
        if (name.empty()) fail(std::string("unexpected character '") + c + "'");
        if (accept("(")) return call(name);
        for (size_t i = 0; i < paramNames_.size(); ++i)
            if (paramNames_[i] == name) return spec_.graph.param(static_cast<int>(i));
        const auto declaredSlot = declaredSlots_.find(name);
        if (declaredSlot != declaredSlots_.end()) {
            return spec_.graph.param(declaredSlot->second);
        }
        if (rampSlots_.count(name) != 0) {
            fail("'" + name + "' is a ramp; read it as " + name + "(x)");
        }
        const auto constant = constants_.find(name);
        if (constant != constants_.end()) return spec_.graph.constant(constant->second);
        if (name == "sampleIndex") return spec_.graph.index();
        if (isLocal(name)) {
            const auto local = locals_.find(name);
            if (local == locals_.end()) fail("'" + name + "' is read before it is assigned");
            return local->second;
        }
        int channel = spec_.channelIndex(name);
        auto it = current_.find(channel);
        return it != current_.end() ? it->second : spec_.graph.load(channel);
    }

    int call(const std::string& fn) {
        std::vector<int> args;
        if (!accept(")")) {
            do args.push_back(expr());
            while (accept(","));
            expect(')');
        }
        ExprGraph& g = spec_.graph;
        auto need = [&](size_t n) {
            if (args.size() != n) fail(fn + "() expects " + std::to_string(n) + " argument(s)");
        };
        const auto ramp = rampSlots_.find(fn);
        if (ramp != rampSlots_.end()) {
            need(1);
            return rampCall(ramp->second, asValue(args[0]));
        }
        if (fn == "min") { need(2); return g.binary(Op::min, args[0], args[1]); }
        if (fn == "max") { need(2); return g.binary(Op::max, args[0], args[1]); }
        if (fn == "abs") { need(1); return g.unary(Op::abs, args[0]); }
        if (fn == "sqrt") { need(1); return g.unary(Op::sqrt, args[0]); }
        if (fn == "sin") { need(1); return g.unary(Op::sin, args[0]); }
        if (fn == "cos") { need(1); return g.unary(Op::cos, args[0]); }
        if (fn == "exp") { need(1); return g.unary(Op::exp, args[0]); }
        if (fn == "log") { need(1); return g.unary(Op::log, args[0]); }
        if (fn == "floor") { need(1); return g.unary(Op::floor, args[0]); }
        if (fn == "ceil") { need(1); return g.unary(Op::ceil, args[0]); }
        if (fn == "pow") { need(2); return g.binary(Op::pow, args[0], args[1]); }
        if (fn == "clamp") { need(3); return g.binary(Op::min, g.binary(Op::max, args[0], args[1]), args[2]); }
        if (fn == "lerp") {
            need(3);
            return g.binary(Op::add, args[0], g.binary(Op::mul, g.binary(Op::sub, args[1], args[0]), args[2]));
        }
        fail("unknown function '" + fn + "'");
    }

    // Recursive descent: a program nested thousands of levels deep would overflow the stack.
    static constexpr int maxNesting = 256;
    struct DepthGuard {
        explicit DepthGuard(Parser& p) : parser(p) {
            if (++parser.depth_ > maxNesting || stackHeadroom() < minimumStackHeadroom) parser.fail("expression is nested too deeply");
        }
        ~DepthGuard() { --parser.depth_; }
        Parser& parser;
    };

    const std::string& src_;
    const std::vector<std::string>& paramNames_;
    const std::map<std::string, float>& constants_;
    std::vector<WrangleParam>* declared_ = nullptr;
    std::map<std::string, int> declaredSlots_;
    std::map<std::string, int> rampSlots_;
    bool assigned_ = false;   // an assignment was seen: declarations must come before it
    int depth_ = 0;
    size_t pos_ = 0;
    KernelSpec spec_;
    std::map<int, int> current_;  // channel -> latest expression assigned this program
    std::map<std::string, int> locals_;  // '_name' temporaries: never a channel, never stored
};

}  // namespace

KernelSpec parseWrangle(const std::string& source, const std::vector<std::string>& paramNames, const std::map<std::string, float>& constants,
                        std::vector<WrangleParam>* declared) {
    return Parser(source, paramNames, constants, declared).run();
}

}  // namespace jitnodes
