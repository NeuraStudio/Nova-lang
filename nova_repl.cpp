#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <cctype>

struct NovaValue {
    enum class Type { Int, Float, String, Null };
    Type type = Type::Null;
    long long int_value = 0;
    double float_value = 0.0;
    std::string string_value;

    static std::shared_ptr<NovaValue> make_int(long long v) { auto val = std::make_shared<NovaValue>(); val->type = Type::Int; val->int_value = v; return val; }
    static std::shared_ptr<NovaValue> make_float(double v) { auto val = std::make_shared<NovaValue>(); val->type = Type::Float; val->float_value = v; return val; }
    static std::shared_ptr<NovaValue> make_string(const std::string& v) { auto val = std::make_shared<NovaValue>(); val->type = Type::String; val->string_value = v; return val; }
    static std::shared_ptr<NovaValue> make_null() { return std::make_shared<NovaValue>(); }

    std::string display() const {
        if (type == Type::Int) return std::to_string(int_value);
        if (type == Type::Float) return std::to_string(float_value);
        if (type == Type::String) return string_value;
        return "null";
    }
};

class Lexer {
public:
    enum class Kind { End, Number, Identifier, String, Plus, Minus, Star, Slash, LParen, RParen, Equal, Dot };
    struct Token { Kind kind; std::string text; };
private:
    std::string src; size_t pos = 0;
public:
    Lexer(const std::string& s) : src(s) {}
    std::vector<Token> tokenize() {
        std::vector<Token> tokens;
        while (pos < src.size()) {
            char c = src[pos];
            if (isspace(c)) { pos++; continue; }
            if (c == '.' && (pos + 1 >= src.size() || !isdigit(src[pos+1]))) {
                tokens.push_back({Kind::Dot, "."}); pos++; continue;
            }
            if (isdigit(c) || c == '.') {
                size_t start = pos; bool has_dot = (c == '.'); pos++;
                while (pos < src.size() && (isdigit(src[pos]) || (src[pos] == '.' && !has_dot))) {
                    if (src[pos] == '.') has_dot = true; pos++;
                }
                tokens.push_back({Kind::Number, src.substr(start, pos - start)});
                continue;
            }
            if (isalpha(c) || c == '_') {
                size_t start = pos;
                while (pos < src.size() && (isalnum(src[pos]) || src[pos] == '_')) pos++;
                tokens.push_back({Kind::Identifier, src.substr(start, pos - start)});
                continue;
            }
            if (c == '"') {
                pos++; size_t start = pos;
                while (pos < src.size() && src[pos] != '"') pos++;
                tokens.push_back({Kind::String, src.substr(start, pos - start)});
                if (pos < src.size()) pos++;
                continue;
            }
            Kind k = Kind::End;
            if (c == '+') k = Kind::Plus; else if (c == '-') k = Kind::Minus;
            else if (c == '*') k = Kind::Star; else if (c == '/') k = Kind::Slash;
            else if (c == '(') k = Kind::LParen; else if (c == ')') k = Kind::RParen;
            else if (c == '=') k = Kind::Equal;
            
            if (k != Kind::End) { tokens.push_back({k, std::string(1, c)}); pos++; }
            else { throw std::runtime_error("SyntaxError: invalid syntax"); }
        }
        tokens.push_back({Kind::End, ""});
        return tokens;
    }
};

class NovaInterpreter {
    std::unordered_map<std::string, std::shared_ptr<NovaValue>> env;
    std::vector<Lexer::Token> tokens; size_t current = 0;

    bool match(Lexer::Kind k) { if (tokens[current].kind == k) { current++; return true; } return false; }
    bool check(Lexer::Kind k) { return tokens[current].kind == k; }
    Lexer::Token prev() { return tokens[current - 1]; }

    std::shared_ptr<NovaValue> primary() {
        if (match(Lexer::Kind::Number)) {
            std::string t = prev().text;
            return (t.find('.') != std::string::npos) ? NovaValue::make_float(std::stod(t)) : NovaValue::make_int(std::stoll(t));
        }
        if (match(Lexer::Kind::String)) return NovaValue::make_string(prev().text);
        if (match(Lexer::Kind::Identifier)) {
            std::string name = prev().text;
            
            if (match(Lexer::Kind::Dot)) {
                if (match(Lexer::Kind::Identifier)) {
                    std::string method = prev().text;
                    if (name == "Nova" && method == "show") return builtin_print();
                    throw std::runtime_error("AttributeError: '" + name + "' has no attribute '" + method + "'");
                }
            }
            
            if (name == "show" || name == "print") return builtin_print();
            if (name == "input") return builtin_input();
            if (name == "int") return builtin_cast(NovaValue::Type::Int);
            if (name == "float") return builtin_cast(NovaValue::Type::Float);
            if (name == "str") return builtin_cast(NovaValue::Type::String);
            
            if (env.count(name)) return env[name];
            throw std::runtime_error("NameError: name '" + name + "' is not defined");
        }
        if (match(Lexer::Kind::LParen)) {
            auto expr = expression();
            if (!match(Lexer::Kind::RParen)) throw std::runtime_error("SyntaxError: missing ')'");
            return expr;
        }
        throw std::runtime_error("SyntaxError: invalid syntax");
    }

    std::shared_ptr<NovaValue> builtin_print() {
        if (!match(Lexer::Kind::LParen)) throw std::runtime_error("SyntaxError: missing '('");
        auto expr = expression();
        if (!match(Lexer::Kind::RParen)) throw std::runtime_error("SyntaxError: missing ')'");
        std::cout << expr->display() << "\n";
        return NovaValue::make_null();
    }

    std::shared_ptr<NovaValue> builtin_input() {
        if (!match(Lexer::Kind::LParen)) throw std::runtime_error("SyntaxError: missing '(' in input");
        if (!check(Lexer::Kind::RParen)) {
            auto prompt = expression();
            std::cout << prompt->display();
        }
        if (!match(Lexer::Kind::RParen)) throw std::runtime_error("SyntaxError: missing ')' in input");
        
        std::string line;
        std::getline(std::cin, line);
        return NovaValue::make_string(line);
    }

    std::shared_ptr<NovaValue> builtin_cast(NovaValue::Type targetType) {
        if (!match(Lexer::Kind::LParen)) throw std::runtime_error("SyntaxError: missing '('");
        auto arg = expression();
        if (!match(Lexer::Kind::RParen)) throw std::runtime_error("SyntaxError: missing ')'");
        
        try {
            if (targetType == NovaValue::Type::Int) {
                if (arg->type == NovaValue::Type::String) return NovaValue::make_int(std::stoll(arg->string_value));
                if (arg->type == NovaValue::Type::Float) return NovaValue::make_int(static_cast<long long>(arg->float_value));
                return NovaValue::make_int(arg->int_value);
            }
            if (targetType == NovaValue::Type::Float) {
                if (arg->type == NovaValue::Type::String) return NovaValue::make_float(std::stod(arg->string_value));
                if (arg->type == NovaValue::Type::Int) return NovaValue::make_float(static_cast<double>(arg->int_value));
                return NovaValue::make_float(arg->float_value);
            }
            if (targetType == NovaValue::Type::String) {
                return NovaValue::make_string(arg->display());
            }
        } catch (...) {
            throw std::runtime_error("ValueError: invalid literal for type casting");
        }
        return NovaValue::make_null();
    }

    std::shared_ptr<NovaValue> multiplicative() {
        auto left = primary();
        while (match(Lexer::Kind::Star) || match(Lexer::Kind::Slash)) {
            auto op = prev().kind; auto right = primary();
            if (left->type == NovaValue::Type::Int && right->type == NovaValue::Type::Int) {
                if (op == Lexer::Kind::Star) left = NovaValue::make_int(left->int_value * right->int_value);
                else { if (right->int_value == 0) throw std::runtime_error("ZeroDivisionError: division by zero"); left = NovaValue::make_int(left->int_value / right->int_value); }
            } else {
                double l = (left->type == NovaValue::Type::Float) ? left->float_value : left->int_value;
                double r = (right->type == NovaValue::Type::Float) ? right->float_value : right->int_value;
                if (op == Lexer::Kind::Star) left = NovaValue::make_float(l * r);
                else { if (r == 0.0) throw std::runtime_error("ZeroDivisionError: division by zero"); left = NovaValue::make_float(l / r); }
            }
        }
        return left;
    }

    std::shared_ptr<NovaValue> expression() {
        auto left = multiplicative();
        while (match(Lexer::Kind::Plus) || match(Lexer::Kind::Minus)) {
            auto op = prev().kind; auto right = multiplicative();
            if (left->type == NovaValue::Type::String || right->type == NovaValue::Type::String) {
                left = NovaValue::make_string(left->display() + right->display());
                continue;
            }
            if (left->type == NovaValue::Type::Int && right->type == NovaValue::Type::Int) {
                left = NovaValue::make_int(op == Lexer::Kind::Plus ? left->int_value + right->int_value : left->int_value - right->int_value);
            } else {
                double l = (left->type == NovaValue::Type::Float) ? left->float_value : left->int_value;
                double r = (right->type == NovaValue::Type::Float) ? right->float_value : right->int_value;
                left = NovaValue::make_float(op == Lexer::Kind::Plus ? l + r : l - r);
            }
        }
        return left;
    }

public:
    std::shared_ptr<NovaValue> execute(const std::string& source) {
        Lexer lex(source); tokens = lex.tokenize(); current = 0;
        if (tokens.size() > 0 && tokens[0].kind == Lexer::Kind::End) return NovaValue::make_null();
        
        if (tokens.size() > 2 && tokens[0].kind == Lexer::Kind::Identifier && tokens[1].kind == Lexer::Kind::Equal) {
            std::string varName = tokens[0].text;
            current = 2;
            auto val = expression();
            env[varName] = val;
            return NovaValue::make_null();
        }
        return expression();
    }
};

int main() {
    std::cout << "Nova 1.0.0 (default, Aug 28 2026, 12:21:49) [AArch64]\n";
    std::cout << "Engineered by Architect Javed | Neura Studio\n";
    std::cout << "Type \"help\", \"copyright\", or \"license\" for more information.\n";
    
    NovaInterpreter interpreter;
    std::string line;

    while (true) {
        std::cout << ">>> ";
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        if (line == "exit()" || line == ".exit" || line == "quit()") break;
        if (line == "help") { std::cout << "Use exit() or Ctrl-D to exit.\n"; continue; }
        
        try {
            auto result = interpreter.execute(line);
            if (result->type != NovaValue::Type::Null) {
                if (result->type == NovaValue::Type::String) std::cout << "'" << result->display() << "'\n";
                else std::cout << result->display() << "\n";
            }
        } catch (const std::exception& e) {
            std::cout << "Traceback (most recent call last):\n";
            std::cout << "  File \"<stdin>\", line 1, in <module>\n";
            std::cout << e.what() << "\n";
        }
    }
    return 0;
}

