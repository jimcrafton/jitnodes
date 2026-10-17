#include "jitnodes/jit/exprParser.h"

#include <cctype>
#include <cstdlib>
#include <map>
#include <stdexcept>

namespace jitnodes {

namespace {

class Parser {
public:
    Parser(const std::string& src, const std::vector<std::string>& paramNames) : src_(src), paramNames_(paramNames) {
        spec_.paramCount = static_cast<int>(paramNames.size());
    }

    KernelSpec run() {
        skipSpace();
        while (pos_ < src_.size()) {
            std::string name = identifier();
            if (name.empty()) fail("expected assignment target");
            for (const auto& p : paramNames_)
                if (p == name) fail("cannot assign to parameter '" + name + "'");
            expect('=');
            int value = expr();
            expect(';');
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

    void skipSpace() {
        while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_]))) ++pos_;
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
            while (pos_ < src_.size() && (std::isalnum(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '_')) ++pos_;
        }
        return src_.substr(start, pos_ - start);
    }

    int expr() { return ternary(); }

    int ternary() {
        int cond = logicalOr();
        if (accept("?")) {
            int a = expr();
            expect(':');
            int b = expr();
            return spec_.graph.select(cond, a, b);
        }
        return cond;
    }

    int logicalOr() {
        int l = logicalAnd();
        while (accept("||")) l = spec_.graph.binary(Op::logicalOr, l, logicalAnd());
        return l;
    }

    int logicalAnd() {
        int l = comparison();
        while (accept("&&")) l = spec_.graph.binary(Op::logicalAnd, l, comparison());
        return l;
    }

    int comparison() {
        int l = additive();
        ExprGraph& g = spec_.graph;
        if (accept("==")) return g.binary(Op::eq, l, additive());
        if (accept("!=")) return g.unary(Op::logicalNot, g.binary(Op::eq, l, additive()));
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
        if (accept("-")) return spec_.graph.unary(Op::neg, unaryExpr());
        if (acceptOp("!", "=")) return spec_.graph.unary(Op::logicalNot, unaryExpr());
        return primary();
    }

    int primary() {
        skipSpace();
        if (pos_ >= src_.size()) fail("unexpected end of input");
        char c = src_[pos_];
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
        if (fn == "min") { need(2); return g.binary(Op::min, args[0], args[1]); }
        if (fn == "max") { need(2); return g.binary(Op::max, args[0], args[1]); }
        if (fn == "abs") { need(1); return g.unary(Op::abs, args[0]); }
        if (fn == "sqrt") { need(1); return g.unary(Op::sqrt, args[0]); }
        if (fn == "clamp") { need(3); return g.binary(Op::min, g.binary(Op::max, args[0], args[1]), args[2]); }
        if (fn == "lerp") {
            need(3);
            return g.binary(Op::add, args[0], g.binary(Op::mul, g.binary(Op::sub, args[1], args[0]), args[2]));
        }
        fail("unknown function '" + fn + "'");
    }

    const std::string& src_;
    const std::vector<std::string>& paramNames_;
    size_t pos_ = 0;
    KernelSpec spec_;
    std::map<int, int> current_;  // channel -> latest expression assigned this program
};

}  // namespace

KernelSpec parseWrangle(const std::string& source, const std::vector<std::string>& paramNames) {
    return Parser(source, paramNames).run();
}

}  // namespace jitnodes
