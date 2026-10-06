#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace chia {

struct Token {
    std::string text;
    int line;
    int column;
};

class ChiaError : public std::runtime_error {
public:
    ChiaError(const Token& token, const std::string& message)
        : std::runtime_error("строка " + std::to_string(token.line) + ", столбец "
              + std::to_string(token.column) + ": " + message),
          line(token.line), column(token.column) {}

    int line;
    int column;
};

struct Dictionary;
using DictionaryPtr = std::shared_ptr<Dictionary>;
using ValueData = std::variant<std::monostate, bool, double, std::string, DictionaryPtr>;

struct Value {
    ValueData data;
};

struct Dictionary {
    std::map<std::string, Value> entries;
    std::string keyType;
    std::string valueType;
};

using ProjectFiles = std::vector<std::pair<std::filesystem::path, std::string>>;
static std::string createPlatformProject(const std::string& kind, const std::string& name,
                                         const std::filesystem::path& directory,
                                         const std::string& target = "");
static bool runGitCommand(const std::string& operation, const std::filesystem::path& repository,
                         const std::string& message = "");

static std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static bool isIdentifierStart(char c) {
    const unsigned char value = static_cast<unsigned char>(c);
    return std::isalpha(value) || c == '_';
}

static bool isIdentifierPart(char c) {
    const unsigned char value = static_cast<unsigned char>(c);
    return std::isalnum(value) || c == '_';
}

static std::vector<Token> tokenize(const std::string& source) {
    std::vector<Token> tokens;
    std::size_t index = 0;
    int line = 1;
    int column = 1;
    const auto advance = [&](char c, int& currentLine, int& currentColumn) {
        if (c == '\n') {
            ++currentLine;
            currentColumn = 1;
        } else {
            ++currentColumn;
        }
    };

    while (index < source.size()) {
        const char c = source[index];
        if (std::isspace(static_cast<unsigned char>(c))) {
            advance(c, line, column);
            ++index;
            continue;
        }

        const int tokenLine = line;
        const int tokenColumn = column;
        if (c == '/' && index + 1 < source.size() && source[index + 1] == '/') {
            while (index < source.size() && source[index] != '\n') {
                advance(source[index], line, column);
                ++index;
            }
            continue;
        }
        if (c == '/' && index + 1 < source.size() && source[index + 1] == '*') {
            advance(source[index++], line, column);
            advance(source[index++], line, column);
            bool closed = false;
            while (index < source.size()) {
                if (source[index] == '*' && index + 1 < source.size() && source[index + 1] == '/') {
                    advance(source[index++], line, column);
                    advance(source[index++], line, column);
                    closed = true;
                    break;
                }
                advance(source[index], line, column);
                ++index;
            }
            if (!closed) throw ChiaError({ "", tokenLine, tokenColumn }, "незакрытый комментарий");
            continue;
        }
        if (c == '"' || c == '\'') {
            const char quote = c;
            std::string value;
            advance(source[index++], line, column);
            bool closed = false;
            while (index < source.size()) {
                const char current = source[index];
                if (current == quote) {
                    advance(source[index++], line, column);
                    closed = true;
                    break;
                }
                if (current == '\n') throw ChiaError({ "", tokenLine, tokenColumn }, "перенос строки внутри текста");
                if (current == '\\') {
                    advance(source[index++], line, column);
                    if (index >= source.size()) break;
                    const char escaped = source[index];
                    switch (escaped) {
                        case 'n': value += '\n'; break;
                        case 'r': value += '\r'; break;
                        case 't': value += '\t'; break;
                        case '\\': value += '\\'; break;
                        case '"': value += '"'; break;
                        case '\'': value += '\''; break;
                        default: throw ChiaError({ "", line, column }, "неизвестная escape-последовательность");
                    }
                    advance(source[index++], line, column);
                    continue;
                }
                value += current;
                advance(current, line, column);
                ++index;
            }
            if (!closed) throw ChiaError({ "", tokenLine, tokenColumn }, "незакрытая строка");
            tokens.push_back({ "\x01" + value, tokenLine, tokenColumn });
            continue;
        }
        if (isIdentifierStart(c)) {
            std::string value;
            while (index < source.size() && isIdentifierPart(source[index])) {
                value += source[index];
                advance(source[index++], line, column);
            }
            tokens.push_back({ value, tokenLine, tokenColumn });
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::string value;
            bool decimalPoint = false;
            while (index < source.size()) {
                const char current = source[index];
                if (std::isdigit(static_cast<unsigned char>(current))) {
                    value += current;
                    advance(source[index++], line, column);
                } else if (current == '.' && !decimalPoint && index + 1 < source.size()
                           && std::isdigit(static_cast<unsigned char>(source[index + 1]))) {
                    decimalPoint = true;
                    value += current;
                    advance(source[index++], line, column);
                } else {
                    break;
                }
            }
            tokens.push_back({ value, tokenLine, tokenColumn });
            continue;
        }

        std::string symbol(1, c);
        if (index + 1 < source.size()) {
            const std::string pair = source.substr(index, 2);
            if (pair == "==" || pair == "!=" || pair == "<=" || pair == ">="
                || pair == "&&" || pair == "||") {
                symbol = pair;
            }
        }
        if (std::string("{}()[];,.<>+-*/%!=&|").find(c) == std::string::npos) {
            throw ChiaError({ "", tokenLine, tokenColumn }, "неизвестный символ '" + std::string(1, c) + "'");
        }
        for (char part : symbol) advance(part, line, column);
        index += symbol.size();
        tokens.push_back({ symbol, tokenLine, tokenColumn });
    }
    tokens.push_back({ "<конец>", line, column });
    return tokens;
}

static std::string valueType(const Value& value) {
    if (std::holds_alternative<std::monostate>(value.data)) return "null";
    if (std::holds_alternative<bool>(value.data)) return "boolean";
    if (std::holds_alternative<double>(value.data)) return "number";
    if (std::holds_alternative<std::string>(value.data)) return "String";
    return "Object";
}

static std::string stringify(const Value& value) {
    if (std::holds_alternative<std::monostate>(value.data)) return "null";
    if (const auto* boolean = std::get_if<bool>(&value.data)) return *boolean ? "true" : "false";
    if (const auto* number = std::get_if<double>(&value.data)) {
        std::ostringstream output;
        output << std::setprecision(15) << *number;
        return output.str();
    }
    if (const auto* text = std::get_if<std::string>(&value.data)) return *text;
    return "[Object]";
}

static std::string encodeHex(const std::string& value) {
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (unsigned char c : value) {
        result += digits[c >> 4];
        result += digits[c & 15];
    }
    return result;
}

static std::string decodeHex(const std::string& value) {
    if (value.size() % 2 != 0) throw std::runtime_error("повреждён файл локального хранилища Chia");
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string result;
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int high = digit(value[i]);
        const int low = digit(value[i + 1]);
        if (high < 0 || low < 0) throw std::runtime_error("повреждён файл локального хранилища Chia");
        result += static_cast<char>((high << 4) | low);
    }
    return result;
}

class Interpreter {
public:
    Interpreter(std::vector<Token> tokens, std::filesystem::path directory, bool checkOnly)
        : tokens_(std::move(tokens)), directory_(std::move(directory)), checkOnly_(checkOnly) {}

    void run() {
        parseImports();
        if (match("public") || match("final")) {
            if (check("class")) parseClass();
            else fail(peek(), "ожидалось объявление class");
        } else if (match("class")) {
            parseClassBody();
        } else {
            while (!atEnd()) statement();
        }
    }

private:
    std::vector<Token> tokens_;
    std::filesystem::path directory_;
    bool checkOnly_;
    std::size_t current_ = 0;
    std::map<std::string, Value> variables_;
    std::map<std::string, std::string> variableTypes_;
    std::map<std::string, Value> database_;
    std::map<std::string, bool> loadedFiles_;

    const Token& peek() const { return tokens_[current_]; }
    const Token& previous() const { return tokens_[current_ - 1]; }
    bool atEnd() const { return peek().text == "<конец>"; }
    bool check(const std::string& text) const { return peek().text == text; }

    bool match(const std::string& text) {
        if (!check(text)) return false;
        ++current_;
        return true;
    }

    Token consume(const std::string& text, const std::string& message) {
        if (check(text)) return tokens_[current_++];
        fail(peek(), message);
    }

    [[noreturn]] static void fail(const Token& token, const std::string& message) {
        throw ChiaError(token, message);
    }

    void parseImports() {
        while (match("import")) {
            std::string name = consumeIdentifier("ожидалось имя библиотеки").text;
            while (match(".")) name += "." + consumeIdentifier("ожидалось имя библиотеки").text;
            consume(";", "после import нужна точка с запятой");
            if (name != "Chia.Biblion" && name != "Chia.Database" && name != "Chia.User"
                && name != "Chia.Collections" && name != "Chia.Diagnostics" && name != "Chia.Game"
                && name != "Chia.OS" && name != "Chia.App" && name != "Chia.Project"
                && name != "Chia.Git" && name != "Chia.HttpExchange" && name != "Chia.Server"
                && name != "Chia.Database.Java" && name != "Chia.Database.Oracle"
                && name != "com.sun.net.httpserver.HttpExchange") {
                fail(previous(), "библиотека '" + name + "' не найдена");
            }
        }
    }

    void parseClass() {
        consume("class", "ожидалось class");
        consumeIdentifier("ожидалось имя класса");
        parseClassBody();
    }

    void parseClassBody() {
        consume("{", "после имени класса нужна {");
        while (!atEnd() && !check("}")) {
            if (match("public") || match("private") || match("protected") || match("static")
                || match("final") || match("void") || match("String") || match("int")
                || match("double") || match("boolean")) {
                if (match("main")) {
                    while (!atEnd() && !check("{")) ++current_;
                    consume("{", "у метода main нет тела");
                    while (!atEnd() && !check("}")) statement();
                    consume("}", "не закрыто тело метода main");
                    consume("}", "не закрыт класс");
                    return;
                }
                continue;
            }
            if (match("class")) {
                fail(previous(), "вложенные классы пока не поддерживаются");
            }
            fail(peek(), "ожидался метод main; поля и методы классов пока не поддерживаются");
        }
        consume("}", "не закрыт класс");
        fail(previous(), "класс не содержит метода main");
    }

    Token consumeIdentifier(const std::string& message) {
        if (isIdentifierStart(peek().text.empty() ? '\0' : peek().text[0])) return tokens_[current_++];
        fail(peek(), message);
    }

    std::string parseType() {
        std::string type = consumeIdentifier("ожидался тип данных").text;
        if (match("<")) {
            type += "<" + parseType();
            while (match(",")) type += "," + parseType();
            consume(">", "не закрыт список типов");
            type += ">";
        }
        return type;
    }

    bool isTypeName(const std::string& name) const {
        return name == "String" || name == "int" || name == "double" || name == "float"
            || name == "boolean" || name == "bool" || name == "var" || name == "Object"
            || name == "Dictionary" || name == "List";
    }

    void statement() {
        if (match(";")) return;
        if (match("{")) {
            while (!atEnd() && !check("}")) statement();
            consume("}", "не закрыт блок");
            return;
        }
        if (isTypeName(peek().text)) {
            declaration();
            return;
        }
        if (isIdentifierStart(peek().text.empty() ? '\0' : peek().text[0])
            && current_ + 1 < tokens_.size() && tokens_[current_ + 1].text == "=") {
            const Token name = tokens_[current_++];
            ++current_;
            if (variables_.find(name.text) == variables_.end()) fail(name, "переменная '" + name.text + "' не объявлена");
            Value value = expression();
            validateDeclaredType(name, variableTypes_[name.text], value);
            variables_[name.text] = std::move(value);
            consume(";", "после присваивания нужна точка с запятой");
            return;
        }
        expression();
        consume(";", "после выражения нужна точка с запятой");
    }

    void declaration() {
        const Token typeToken = peek();
        const std::string type = parseType();
        const Token name = consumeIdentifier("ожидалось имя переменной");
        if (variables_.find(name.text) != variables_.end()) fail(name, "переменная '" + name.text + "' уже объявлена");
        Value value{std::monostate{}};
        if (match("=")) value = expression();
        else if (type.rfind("Dictionary<", 0) == 0 || type == "Object") value.data = std::make_shared<Dictionary>();
        else if (type == "int" || type == "double" || type == "float") value.data = 0.0;
        else if (type == "boolean" || type == "bool") value.data = false;
        consume(";", "после объявления нужна точка с запятой");
        if (type == "var" && std::holds_alternative<std::monostate>(value.data))
            fail(typeToken, "переменная var должна иметь начальное значение");
        validateDeclaredType(typeToken, type, value);
        if (type.rfind("Dictionary<", 0) == 0
            && std::holds_alternative<DictionaryPtr>(value.data)) {
            const std::size_t comma = type.find(',');
            const std::size_t close = type.rfind('>');
            if (comma != std::string::npos && close != std::string::npos && comma < close) {
                auto dictionary = std::get<DictionaryPtr>(value.data);
                dictionary->keyType = type.substr(std::string("Dictionary<").size(),
                    comma - std::string("Dictionary<").size());
                dictionary->valueType = type.substr(comma + 1, close - comma - 1);
            }
        }
        const std::string storedType = type == "var" ? "inferred:" + valueType(value) : type;
        variables_.emplace(name.text, std::move(value));
        variableTypes_.emplace(name.text, storedType);
    }

    void validateDeclaredType(const Token& token, const std::string& type, const Value& value) {
        if (std::holds_alternative<std::monostate>(value.data)) {
            if (type == "int" || type == "double" || type == "float"
                || type == "boolean" || type == "bool")
                fail(token, "примитивный тип '" + type + "' не может быть null");
            return;
        }
        const std::string actual = valueType(value);
        const bool inferredType = type.rfind("inferred:", 0) == 0;
        const bool valid = (inferredType && type.substr(std::string("inferred:").size()) == actual)
            || type == "var" || type == "Object"
            || (type == "number" && actual == "number")
            || (type == "String" && actual == "String")
            || ((type == "int" || type == "double" || type == "float") && actual == "number")
            || ((type == "boolean" || type == "bool") && actual == "boolean")
            || ((type.rfind("Dictionary<", 0) == 0 || type == "List") && actual == "Object");
        if (!valid) fail(token, "тип '" + actual + "' нельзя присвоить переменной типа '" + type + "'");
        if (type == "int") {
            const double number = std::get<double>(value.data);
            if (std::floor(number) != number) fail(token, "значение типа int должно быть целым числом");
        }
    }

    Value expression() { return logicalOr(); }

    Value logicalOr() {
        Value left = logicalAnd();
        while (match("||")) {
            const Token op = previous();
            Value right = logicalAnd();
            if (!std::holds_alternative<bool>(left.data) || !std::holds_alternative<bool>(right.data))
                fail(op, "оператор || принимает только boolean");
            left.data = std::get<bool>(left.data) || std::get<bool>(right.data);
        }
        return left;
    }

    Value logicalAnd() {
        Value left = equality();
        while (match("&&")) {
            const Token op = previous();
            Value right = equality();
            if (!std::holds_alternative<bool>(left.data) || !std::holds_alternative<bool>(right.data))
                fail(op, "оператор && принимает только boolean");
            left.data = std::get<bool>(left.data) && std::get<bool>(right.data);
        }
        return left;
    }

    Value equality() {
        Value left = comparison();
        while (match("==") || match("!=")) {
            const std::string op = previous().text;
            Value right = comparison();
            bool equal = false;
            if (left.data.index() == right.data.index()) {
                if (std::holds_alternative<std::monostate>(left.data)) equal = true;
                else if (const auto* leftBoolean = std::get_if<bool>(&left.data)) equal = *leftBoolean == std::get<bool>(right.data);
                else if (const auto* leftNumber = std::get_if<double>(&left.data)) equal = *leftNumber == std::get<double>(right.data);
                else if (const auto* leftString = std::get_if<std::string>(&left.data)) equal = *leftString == std::get<std::string>(right.data);
                else equal = std::get<DictionaryPtr>(left.data) == std::get<DictionaryPtr>(right.data);
            }
            left.data = op == "==" ? equal : !equal;
        }
        return left;
    }

    Value comparison() {
        Value left = term();
        while (match("<") || match("<=") || match(">") || match(">=")) {
            const Token op = previous();
            Value right = term();
            if (std::holds_alternative<double>(left.data) && std::holds_alternative<double>(right.data)) {
                const double a = std::get<double>(left.data);
                const double b = std::get<double>(right.data);
                if (op.text == "<") left.data = a < b;
                else if (op.text == "<=") left.data = a <= b;
                else if (op.text == ">") left.data = a > b;
                else left.data = a >= b;
            } else if (std::holds_alternative<std::string>(left.data)
                       && std::holds_alternative<std::string>(right.data)) {
                const auto& a = std::get<std::string>(left.data);
                const auto& b = std::get<std::string>(right.data);
                if (op.text == "<") left.data = a < b;
                else if (op.text == "<=") left.data = a <= b;
                else if (op.text == ">") left.data = a > b;
                else left.data = a >= b;
            } else {
                fail(op, "сравнивать можно только два числа или две строки");
            }
        }
        return left;
    }

    Value term() {
        Value left = factor();
        while (match("+") || match("-")) {
            const Token op = previous();
            Value right = factor();
            if (op.text == "+" && (std::holds_alternative<std::string>(left.data)
                                   || std::holds_alternative<std::string>(right.data))) {
                left.data = stringify(left) + stringify(right);
            } else {
                if (!std::holds_alternative<double>(left.data) || !std::holds_alternative<double>(right.data))
                    fail(op, "арифметические операторы принимают числа");
                const double a = std::get<double>(left.data);
                const double b = std::get<double>(right.data);
                left.data = op.text == "+" ? a + b : a - b;
            }
        }
        return left;
    }

    Value factor() {
        Value left = unary();
        while (match("*") || match("/") || match("%")) {
            const Token op = previous();
            Value right = unary();
            if (!std::holds_alternative<double>(left.data) || !std::holds_alternative<double>(right.data))
                fail(op, "арифметические операторы принимают числа");
            const double a = std::get<double>(left.data);
            const double b = std::get<double>(right.data);
            if ((op.text == "/" || op.text == "%") && b == 0) fail(op, "деление на ноль");
            if (op.text == "*") left.data = a * b;
            else if (op.text == "/") left.data = a / b;
            else left.data = std::fmod(a, b);
        }
        return left;
    }

    Value unary() {
        if (match("!") || match("-")) {
            const Token op = previous();
            Value value = unary();
            if (op.text == "!") {
                if (!std::holds_alternative<bool>(value.data)) fail(op, "оператор ! принимает boolean");
                value.data = !std::get<bool>(value.data);
            } else {
                if (!std::holds_alternative<double>(value.data)) fail(op, "унарный - принимает число");
                value.data = -std::get<double>(value.data);
            }
            return value;
        }
        return postfix(primary());
    }

    Value primary() {
        if (match("(")) {
            Value value = expression();
            consume(")", "не закрыта скобка");
            return value;
        }
        if (match("true")) return Value{true};
        if (match("false")) return Value{false};
        if (match("null")) return Value{std::monostate{}};
        if (match("new")) {
            const Token type = consumeIdentifier("после new ожидался тип объекта");
            if (type.text != "Dictionary" && type.text != "Object" && type.text != "List")
                fail(type, "создание объектов типа '" + type.text + "' пока не поддерживается");
            if (match("<")) {
                std::string keyType;
                std::string itemType;
                if (!check(">")) {
                    keyType = parseType();
                    if (match(",")) itemType = parseType();
                }
                consume(">", "не закрыт список типов");
                if (!keyType.empty()) {
                    auto dictionary = std::make_shared<Dictionary>();
                    dictionary->keyType = keyType;
                    dictionary->valueType = itemType;
                    consume("(", "после типа объекта нужна скобка");
                    consume(")", "конструктор объекта не принимает аргументы");
                    return Value{dictionary};
                }
            }
            consume("(", "после типа объекта нужна скобка");
            consume(")", "конструктор объекта не принимает аргументы");
            return Value{std::make_shared<Dictionary>()};
        }
        if (!atEnd() && !peek().text.empty() && peek().text[0] == '\x01') {
            const std::string value = peek().text.substr(1);
            ++current_;
            return Value{value};
        }
        if (!atEnd() && std::isdigit(static_cast<unsigned char>(peek().text[0]))) {
            const Token number = tokens_[current_++];
            try {
                return Value{std::stod(number.text)};
            } catch (const std::exception&) {
                fail(number, "неверное число");
            }
        }
        if (isIdentifierStart(peek().text.empty() ? '\0' : peek().text[0])) {
            const Token identifier = tokens_[current_++];
            const auto found = variables_.find(identifier.text);
            if (found != variables_.end()) return found->second;
            std::string qualified = identifier.text;
            while (match(".")) qualified += "." + consumeIdentifier("после точки ожидалось имя метода").text;
            if (match("(")) return callBuiltin(qualified, arguments());
            fail(identifier, "переменная или функция '" + identifier.text + "' не объявлена");
        }
        fail(peek(), "ожидалось выражение");
    }

    std::vector<Value> arguments() {
        std::vector<Value> args;
        if (!check(")")) {
            do {
                args.push_back(expression());
            } while (match(","));
        }
        consume(")", "не закрыт список аргументов");
        return args;
    }

    Value postfix(Value value) {
        while (match(".")) {
            const Token method = consumeIdentifier("после точки ожидалось имя метода");
            consume("(", "метод должен вызываться со скобками");
            const std::vector<Value> args = arguments();
            if (!std::holds_alternative<DictionaryPtr>(value.data))
                fail(method, "метод '" + method.text + "' доступен только для Dictionary/Object");
            const DictionaryPtr object = std::get<DictionaryPtr>(value.data);
            if (method.text == "put" || method.text == "set") {
                requireArgumentCount(method, args, 2);
                validateContainerValue(method, object->keyType, args[0]);
                validateContainerValue(method, object->valueType, args[1]);
                object->entries[stringify(args[0])] = args[1];
                value = args[1];
            } else if (method.text == "get") {
                requireArgumentCount(method, args, 1);
                validateContainerValue(method, object->keyType, args[0]);
                const auto found = object->entries.find(stringify(args[0]));
                value = found == object->entries.end() ? Value{std::monostate{}} : found->second;
            } else if (method.text == "containsKey" || method.text == "contains") {
                requireArgumentCount(method, args, 1);
                validateContainerValue(method, object->keyType, args[0]);
                value.data = object->entries.find(stringify(args[0])) != object->entries.end();
            } else if (method.text == "remove" || method.text == "delete") {
                requireArgumentCount(method, args, 1);
                validateContainerValue(method, object->keyType, args[0]);
                value.data = object->entries.erase(stringify(args[0])) != 0;
            } else if (method.text == "size") {
                requireArgumentCount(method, args, 0);
                value.data = static_cast<double>(object->entries.size());
            } else {
                fail(method, "неизвестный метод объекта '" + method.text + "'");
            }
        }
        return value;
    }

    static void validateContainerValue(const Token& token, const std::string& expected, const Value& value) {
        if (expected.empty() || expected == "var" || expected == "Object") return;
        if (std::holds_alternative<std::monostate>(value.data)) {
            if (expected == "int" || expected == "double" || expected == "float"
                || expected == "boolean" || expected == "bool")
                fail(token, "null нельзя сохранить для типа '" + expected + "'");
            return;
        }
        const std::string actual = valueType(value);
        const bool valid = (expected == "String" && actual == "String")
            || ((expected == "int" || expected == "double" || expected == "float"
                 || expected == "number") && actual == "number")
            || ((expected == "boolean" || expected == "bool") && actual == "boolean")
            || (expected.rfind("Dictionary<", 0) == 0 && actual == "Object");
        if (!valid) fail(token, "тип '" + actual + "' не соответствует типу контейнера '" + expected + "'");
        if (expected == "int" && std::floor(std::get<double>(value.data)) != std::get<double>(value.data))
            fail(token, "значение типа int должно быть целым числом");
    }

    static void requireArgumentCount(const Token& token, const std::vector<Value>& args, std::size_t count) {
        if (args.size() != count) fail(token, "метод '" + token.text + "' ожидает аргументов: " + std::to_string(count));
    }

    Value callBuiltin(const std::string& name, const std::vector<Value>& args) {
        if (name == "print" || name == "println" || name == "Chia.print" || name == "Chia.println"
            || name == "Chia.Biblion.print" || name == "Chia.Biblion.println"
            || name == "System.out.print" || name == "System.out.println") {
            if (args.size() != 1) fail(previous(), "функция печати ожидает один аргумент");
            if (!checkOnly_) {
                std::cout << stringify(args[0]);
                if (name == "println" || name == "Chia.println" || name == "Chia.Biblion.println"
                    || name == "System.out.println") std::cout << '\n';
            }
            return Value{std::monostate{}};
        }
        if (name == "typeOf" || name == "Chia.typeOf" || name == "Chia.Biblion.typeOf"
            || name == "Chia.Diagnostics.typeOf") {
            if (args.size() != 1) fail(previous(), "typeOf ожидает один аргумент");
            return Value{valueType(args[0])};
        }
        if (name == "length" || name == "Chia.Biblion.length") {
            if (args.size() != 1 || !std::holds_alternative<std::string>(args[0].data))
                fail(previous(), "length ожидает одну строку");
            return Value{static_cast<double>(std::get<std::string>(args[0].data).size())};
        }
        if (name == "Chia.Database.put" || name == "Chia.Database.set") {
            if (args.size() != 2) fail(previous(), "Chia.Database.put ожидает ключ и значение");
            if (!std::holds_alternative<std::string>(args[1].data)
                && !std::holds_alternative<double>(args[1].data)
                && !std::holds_alternative<bool>(args[1].data))
                fail(previous(), "база Chia хранит только строки, числа и boolean");
            loadDatabase();
            database_[stringify(args[0])] = args[1];
            if (!checkOnly_) saveDatabase();
            return Value{true};
        }
        if (name == "Chia.Database.get") {
            if (args.size() != 1) fail(previous(), "Chia.Database.get ожидает ключ");
            loadDatabase();
            const auto found = database_.find(stringify(args[0]));
            return found == database_.end() ? Value{std::monostate{}} : found->second;
        }
        if (name == "Chia.Database.contains") {
            if (args.size() != 1) fail(previous(), "Chia.Database.contains ожидает ключ");
            loadDatabase();
            return Value{database_.find(stringify(args[0])) != database_.end()};
        }
        if (name == "Chia.Database.delete" || name == "Chia.Database.remove") {
            if (args.size() != 1) fail(previous(), "Chia.Database.delete ожидает ключ");
            loadDatabase();
            const bool removed = database_.erase(stringify(args[0])) != 0;
            if (!checkOnly_) saveDatabase();
            return Value{removed};
        }
        if (name == "Chia.User.create") {
            if (args.size() != 1) fail(previous(), "Chia.User.create ожидает имя локального пользователя");
            const std::string username = trim(stringify(args[0]));
            if (username.empty()) fail(previous(), "имя пользователя не может быть пустым");
            auto users = readUsers();
            const bool created = users.insert(username).second;
            if (created && !checkOnly_) writeUsers(users);
            return Value{created};
        }
        if (name == "Chia.User.exists") {
            if (args.size() != 1) fail(previous(), "Chia.User.exists ожидает имя пользователя");
            const auto users = readUsers();
            return Value{users.find(stringify(args[0])) != users.end()};
        }
        if (name == "Chia.OS.createKernel" || name == "Chia.Project.createKernel") {
            if (args.size() != 2) fail(previous(), name + " ожидает имя проекта и путь");
            requireStringArguments(name, args);
            if (checkOnly_) return Value{stringify(args[1])};
            return Value{createPlatformProject("kernel", stringify(args[0]), stringify(args[1]))};
        }
        if (name == "Chia.App.createDesktop" || name == "Chia.Project.createDesktop") {
            if (args.size() != 3) fail(previous(), name + " ожидает имя, путь и цель windows|macos|linux");
            requireStringArguments(name, args);
            if (checkOnly_) return Value{stringify(args[1])};
            return Value{createPlatformProject("desktop", stringify(args[0]), stringify(args[1]),
                stringify(args[2]))};
        }
        if (name == "Chia.App.createMobile" || name == "Chia.Project.createMobile") {
            if (args.size() != 3) fail(previous(), name + " ожидает имя, путь и цель android|ios");
            requireStringArguments(name, args);
            if (checkOnly_) return Value{stringify(args[1])};
            return Value{createPlatformProject("mobile", stringify(args[0]), stringify(args[1]),
                stringify(args[2]))};
        }
        if (name == "Chia.Server.createHttpExchange" || name == "Chia.HttpExchange.create") {
            if (args.size() != 2) fail(previous(), name + " ожидает имя Java-проекта и путь");
            requireStringArguments(name, args);
            if (checkOnly_) return Value{stringify(args[1])};
            return Value{createPlatformProject("http", stringify(args[0]), stringify(args[1]))};
        }
        if (name == "Chia.Database.createJava" || name == "Chia.Database.Java.create") {
            if (args.size() != 2) fail(previous(), name + " ожидает имя Java-проекта и путь");
            requireStringArguments(name, args);
            if (checkOnly_) return Value{stringify(args[1])};
            return Value{createPlatformProject("java-database", stringify(args[0]), stringify(args[1]))};
        }
        if (name == "Chia.Database.createOracle" || name == "Chia.Database.Oracle.create") {
            if (args.size() != 2) fail(previous(), name + " ожидает имя Java-проекта и путь");
            requireStringArguments(name, args);
            if (checkOnly_) return Value{stringify(args[1])};
            return Value{createPlatformProject("oracle", stringify(args[0]), stringify(args[1]))};
        }
        if (name == "Chia.Git.init" || name == "Chia.Git.status" || name == "Chia.Git.add") {
            if (args.size() != 1) fail(previous(), name + " ожидает путь к репозиторию");
            requireStringArguments(name, args);
            if (checkOnly_) return Value{true};
            return Value{runGitCommand(name.substr(std::string("Chia.Git.").size()),
                stringify(args[0]))};
        }
        if (name == "Chia.Git.commit") {
            if (args.size() != 2) fail(previous(), name + " ожидает путь и сообщение коммита");
            requireStringArguments(name, args);
            if (checkOnly_) return Value{true};
            return Value{runGitCommand("commit", stringify(args[0]), stringify(args[1]))};
        }
        fail(previous(), "неизвестная функция '" + name + "'");
    }

    void requireStringArguments(const std::string& name, const std::vector<Value>& args) const {
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (!std::holds_alternative<std::string>(args[i].data))
                fail(previous(), name + ": аргумент " + std::to_string(i + 1) + " должен быть String");
        }
    }

    std::filesystem::path databasePath() const { return directory_ / ".chia_database"; }
    std::filesystem::path usersPath() const { return directory_ / ".chia_users"; }

    void loadDatabase() {
        const std::filesystem::path path = databasePath();
        if (loadedFiles_[path.string()]) return;
        loadedFiles_[path.string()] = true;
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            if (std::filesystem::exists(path)) throw std::runtime_error("не удалось прочитать базу Chia: " + path.string());
            return;
        }
        std::string line;
        while (std::getline(input, line)) {
            const std::size_t separator = line.find('\t');
            if (separator == std::string::npos) throw std::runtime_error("повреждён файл базы Chia: " + path.string());
            const std::string key = decodeHex(line.substr(0, separator));
            const std::string encoded = line.substr(separator + 1);
            if (encoded.size() < 2 || encoded[1] != ':') throw std::runtime_error("повреждён файл базы Chia: " + path.string());
            const std::string payload = decodeHex(encoded.substr(2));
            if (encoded[0] == 's') database_[key] = Value{payload};
            else if (encoded[0] == 'n') {
                try { database_[key] = Value{std::stod(payload)}; }
                catch (const std::exception&) { throw std::runtime_error("повреждено число в базе Chia"); }
            } else if (encoded[0] == 'b' && (payload == "true" || payload == "false"))
                database_[key] = Value{payload == "true"};
            else throw std::runtime_error("неподдерживаемый тип значения в базе Chia");
        }
        if (!input.eof()) throw std::runtime_error("ошибка чтения базы Chia: " + path.string());
    }

    void saveDatabase() {
        const std::filesystem::path path = databasePath();
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("не удалось записать базу Chia: " + path.string());
        for (const auto& entry : database_) {
            const Value& value = entry.second;
            char type = 's';
            std::string payload;
            if (const auto* text = std::get_if<std::string>(&value.data)) payload = *text;
            else if (const auto* number = std::get_if<double>(&value.data)) {
                type = 'n';
                payload = stringify(value);
            } else if (const auto* boolean = std::get_if<bool>(&value.data)) {
                type = 'b';
                payload = *boolean ? "true" : "false";
            } else {
                throw std::runtime_error("база Chia хранит только строки, числа и boolean");
            }
            output << encodeHex(entry.first) << '\t' << type << ':' << encodeHex(payload) << '\n';
        }
        if (!output) throw std::runtime_error("ошибка записи базы Chia: " + path.string());
    }

    std::set<std::string> readUsers() const {
        std::set<std::string> users;
        const std::filesystem::path path = usersPath();
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            if (std::filesystem::exists(path)) throw std::runtime_error("не удалось прочитать список пользователей Chia");
            return users;
        }
        std::string line;
        while (std::getline(input, line)) users.insert(decodeHex(line));
        if (!input.eof()) throw std::runtime_error("ошибка чтения списка пользователей Chia");
        return users;
    }

    void writeUsers(const std::set<std::string>& users) const {
        const std::filesystem::path path = usersPath();
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("не удалось сохранить локальных пользователей Chia");
        for (const std::string& user : users) output << encodeHex(user) << '\n';
        if (!output) throw std::runtime_error("ошибка записи локальных пользователей Chia");
    }
};

struct TextWord {
    std::size_t start;
    std::size_t length;
    int line;
    int column;
    std::string text;
};

static bool isTextLetter(unsigned char value) {
    return value >= 0x80 || std::isalpha(value) || value == '_';
}

static std::vector<TextWord> textWords(const std::string& source) {
    std::vector<TextWord> words;
    std::size_t index = 0;
    const auto scanWords = [&](std::size_t begin, std::size_t end, auto& output) {
        std::size_t cursor = begin;
        while (cursor < end) {
            if (!isTextLetter(static_cast<unsigned char>(source[cursor]))) {
                ++cursor;
                continue;
            }
            const std::size_t start = cursor;
            while (cursor < end && isTextLetter(static_cast<unsigned char>(source[cursor]))) {
                ++cursor;
            }
            output.push_back(TextWord{start, cursor - start, 0, 0,
                source.substr(start, cursor - start)});
        }
    };

    while (index < source.size()) {
        const char current = source[index];
        if (current == '/' && index + 1 < source.size() && source[index + 1] == '/') {
            const std::size_t begin = index;
            while (index < source.size() && source[index] != '\n') ++index;
            scanWords(begin + 2, index, words);
            continue;
        }
        if (current == '/' && index + 1 < source.size() && source[index + 1] == '*') {
            const std::size_t begin = index;
            index += 2;
            while (index + 1 < source.size() && !(source[index] == '*' && source[index + 1] == '/')) ++index;
            const std::size_t end = index;
            if (index + 1 < source.size()) index += 2;
            scanWords(begin + 2, end, words);
            continue;
        }
        if (current == '"' || current == '\'') {
            const char quote = current;
            const std::size_t begin = ++index;
            while (index < source.size()) {
                if (source[index] == '\\' && index + 1 < source.size()) {
                    index += 2;
                } else if (source[index++] == quote) {
                    break;
                }
            }
            const std::size_t end = index > begin && source[index - 1] == quote ? index - 1 : index;
            scanWords(begin, end, words);
            continue;
        }
        ++index;
    }
    std::size_t position = 0;
    int line = 1;
    int column = 1;
    for (TextWord& word : words) {
        while (position < word.start) {
            const unsigned char byte = static_cast<unsigned char>(source[position++]);
            if (byte == '\n') {
                ++line;
                column = 1;
            } else if ((byte & 0xc0) != 0x80) {
                ++column;
            }
        }
        word.line = line;
        word.column = column;
    }
    return words;
}

static std::string lowerUtf8(const std::string& value) {
    std::string result = value;
    for (std::size_t i = 0; i < result.size(); ++i) {
        const unsigned char first = static_cast<unsigned char>(result[i]);
        if (first == 0xd0 && i + 1 < result.size()) {
            const unsigned char second = static_cast<unsigned char>(result[i + 1]);
            if (second >= 0x90 && second <= 0x9f)
                result[i + 1] = static_cast<char>(second + 0x20);
            else if (second >= 0xa0 && second <= 0xaf) {
                result[i] = static_cast<char>(0xd1);
                result[i + 1] = static_cast<char>(second - 0x20);
            }
            else if (second == 0x81) {
                result[i] = static_cast<char>(0xd1);
                result[i + 1] = static_cast<char>(0x91);
            }
        } else if (first < 0x80) {
            result[i] = static_cast<char>(std::tolower(first));
        }
    }
    return result;
}

static const std::map<std::string, std::string>& russianCorrections() {
    static const std::map<std::string, std::string> corrections{
        {"превет", "привет"}, {"превед", "привет"}, {"превит", "привет"},
        {"спосибо", "спасибо"}, {"спосиба", "спасибо"}, {"пожалусто", "пожалуйста"},
        {"пажалуйста", "пожалуйста"}, {"пожалуста", "пожалуйста"},
        {"севодня", "сегодня"}, {"сиводня", "сегодня"}, {"завтро", "завтра"},
        {"завтраа", "завтра"}, {"карова", "корова"}, {"малако", "молоко"},
        {"сабака", "собака"}, {"сабаке", "собаке"}, {"вообщем", "в общем"},
        {"извени", "извини"}, {"извените", "извините"}, {"потаму", "потому"},
        {"пачему", "почему"}, {"чилавек", "человек"}, {"чилавека", "человека"},
        {"програма", "программа"}, {"програмирование", "программирование"},
        {"ошыбка", "ошибка"}, {"ошипка", "ошибка"}, {"зделать", "сделать"},
        {"зделал", "сделал"}, {"каординаты", "координаты"}, {"интиресно", "интересно"},
        {"харошо", "хорошо"}, {"хорошава", "хорошего"}, {"сабщение", "сообщение"},
        {"обявление", "объявление"}, {"обьект", "объект"}, {"обект", "объект"},
        {"переменая", "переменная"}, {"переменые", "переменные"},
        {"компелятор", "компилятор"}, {"компилятыр", "компилятор"},
        {"библиатека", "библиотека"}, {"библеотека", "библиотека"},
        {"пользаватель", "пользователь"}, {"пользователья", "пользователя"},
        {"коректировка", "корректировка"}, {"орфаграфия", "орфография"},
        {"транслитор", "транслятор"}, {"транслейт", "транслятор"}
    };
    return corrections;
}

static std::string preserveInitialCapital(const std::string& original, std::string corrected) {
    if (original.size() >= 2
        && static_cast<unsigned char>(original[0]) == 0xd0
        && static_cast<unsigned char>(original[1]) >= 0x90
        && static_cast<unsigned char>(original[1]) <= 0x9f
        && corrected.size() >= 2
        && static_cast<unsigned char>(corrected[0]) == 0xd0
        && static_cast<unsigned char>(corrected[1]) >= 0xb0
        && static_cast<unsigned char>(corrected[1]) <= 0xbf) {
        corrected[1] = static_cast<char>(static_cast<unsigned char>(corrected[1]) - 0x20);
    }
    return corrected;
}

static std::vector<std::pair<TextWord, std::string>> findSpellingIssues(const std::string& source) {
    std::vector<std::pair<TextWord, std::string>> issues;
    const auto& dictionary = russianCorrections();
    for (const TextWord& word : textWords(source)) {
        const auto found = dictionary.find(lowerUtf8(word.text));
        if (found != dictionary.end())
            issues.emplace_back(word, preserveInitialCapital(word.text, found->second));
    }
    return issues;
}

struct SyntaxHint {
    int line;
    int column;
    std::string fingerprint;
    std::string advice;
};

static bool isAssignmentTarget(const std::string& name) {
    return !name.empty() && isIdentifierStart(name[0]);
}

static std::vector<SyntaxHint> findSyntaxFingerprints(const std::string& source) {
    std::vector<SyntaxHint> hints;
    std::vector<Token> tokens;
    try {
        tokens = tokenize(source);
    } catch (const ChiaError& error) {
        const std::string message = error.what();
        std::string advice = "проверьте строку и символы рядом с местом ошибки";
        if (message.find("незакрытая строка") != std::string::npos)
            advice = "закройте строку такой же кавычкой, которой она была открыта";
        else if (message.find("перенос строки внутри текста") != std::string::npos)
            advice = "закройте кавычки до конца строки или используйте escape-последовательность \\n";
        else if (message.find("незакрытый комментарий") != std::string::npos)
            advice = "добавьте */ в конце блочного комментария или используйте //";
        else if (message.find("escape-последовательность") != std::string::npos)
            advice = "используйте поддерживаемый escape-код: \\n, \\r, \\t, \\\\, \\\" или \\'";
        else if (message.find("неизвестный символ") != std::string::npos)
            advice = "удалите этот символ или замените его допустимым оператором Chia";
        hints.push_back({error.line, error.column, "ошибка лексики: " + message, advice});
        return hints;
    }

    std::vector<Token> delimiters;
    for (const Token& token : tokens) {
        if (token.text == "(" || token.text == "{" || token.text == "[") {
            delimiters.push_back(token);
        } else if (token.text == ")" || token.text == "}" || token.text == "]") {
            const std::string expected = token.text == ")" ? "(" : token.text == "}" ? "{" : "[";
            if (delimiters.empty() || delimiters.back().text != expected) {
                const std::string opener = token.text == ")" ? "(" : token.text == "}" ? "{" : "[";
                hints.push_back({token.line, token.column, "лишняя закрывающая скобка " + token.text,
                    "удалите эту скобку или добавьте соответствующую открывающую " + opener});
            } else {
                delimiters.pop_back();
            }
        }
    }
    for (const Token& token : delimiters) {
        const std::string closing = token.text == "(" ? ")" : token.text == "{" ? "}" : "]";
        hints.push_back({token.line, token.column, "не закрыта скобка " + token.text,
            "добавьте парную закрывающую скобку " + closing});
    }

    std::size_t begin = 0;
    while (begin < tokens.size() && tokens[begin].text != "<конец>") {
        std::size_t end = begin;
        while (end + 1 < tokens.size() && tokens[end + 1].text != "<конец>"
               && tokens[end + 1].line == tokens[begin].line) ++end;
        const Token& first = tokens[begin];
        const Token& last = tokens[end];
        const std::string& firstText = first.text;
        const std::string& lastText = last.text;
        std::size_t declarationName = begin + 1;
        if ((firstText == "Dictionary" || firstText == "List") && declarationName <= end
            && tokens[declarationName].text == "<") {
            int genericDepth = 0;
            do {
                if (tokens[declarationName].text == "<") ++genericDepth;
                else if (tokens[declarationName].text == ">") --genericDepth;
                ++declarationName;
            } while (declarationName <= end && genericDepth > 0);
        }
        const bool declaration = (firstText == "String" || firstText == "int" || firstText == "double"
            || firstText == "float" || firstText == "boolean" || firstText == "bool" || firstText == "var"
            || firstText == "Dictionary" || firstText == "List" || firstText == "Object")
            && declarationName <= end && isAssignmentTarget(tokens[declarationName].text);
        const bool assignment = begin + 2 <= end && isAssignmentTarget(firstText)
            && tokens[begin + 1].text == "=";
        const bool doubledAssignment = begin + 2 <= end && isAssignmentTarget(firstText)
            && tokens[begin + 1].text == "==";
        bool call = lastText == ")";
        bool methodHeader = false;
        bool controlHeader = firstText == "if" || firstText == "while"
            || firstText == "for" || firstText == "switch";
        for (std::size_t i = begin; i <= end; ++i) {
            if (tokens[i].text == "main" || tokens[i].text == "class"
                || tokens[i].text == "import" || tokens[i].text == "public"
                || tokens[i].text == "private" || tokens[i].text == "protected"
                || tokens[i].text == "static") {
                methodHeader = true;
                break;
            }
        }
        const bool unfinished = lastText == "=" || lastText == "+" || lastText == "-"
            || lastText == "*" || lastText == "/" || lastText == "%" || lastText == ","
            || lastText == "." || lastText == "&&" || lastText == "||" || lastText == "!";
        const bool nextStartsNewLine = end + 1 < tokens.size()
            && (tokens[end + 1].text == "<конец>" || tokens[end + 1].line > last.line);
        const bool needsCondition = firstText == "if" || firstText == "while"
            || firstText == "for" || firstText == "switch";
        if (needsCondition && begin + 1 <= end && tokens[begin + 1].text != "(") {
            hints.push_back({tokens[begin + 1].line, tokens[begin + 1].column,
                "после управляющего слова нет открывающей скобки условия",
                "напишите условие в круглых скобках, например if (условие)"});
        } else if (needsCondition && begin + 1 > end && nextStartsNewLine) {
            hints.push_back({last.line, last.column + static_cast<int>(last.text.size()),
                "после управляющего слова отсутствует условие",
                "добавьте условие в круглых скобках, например if (условие)"});
        }
        if (nextStartsNewLine && !methodHeader && !controlHeader && !unfinished
            && lastText != ";" && lastText != "{" && lastText != "}"
            && (declaration || assignment || call)) {
            hints.push_back({last.line, last.column + static_cast<int>(last.text.size()),
                "похоже, пропущена точка с запятой",
                "добавьте ; в конце инструкции на строке " + std::to_string(last.line)});
        }
        if (declaration && declarationName + 1 <= end
            && tokens[declarationName + 1].text != "=" && tokens[declarationName + 1].text != ";") {
            hints.push_back({tokens[declarationName + 1].line, tokens[declarationName + 1].column,
                "похоже, пропущен знак присваивания в объявлении переменной",
                "добавьте = между именем переменной и её начальным значением"});
        }
        if (doubledAssignment) {
            hints.push_back({tokens[begin + 1].line, tokens[begin + 1].column,
                "возможно, вместо присваивания введено сравнение ==",
                "для изменения переменной используйте один знак =; для проверки равенства — == в условии"});
        }
        begin = end + 1;
    }
    std::sort(hints.begin(), hints.end(), [](const SyntaxHint& left, const SyntaxHint& right) {
        if (left.line != right.line) return left.line < right.line;
        return left.column < right.column;
    });
    return hints;
}

struct CppExpression {
    std::string code;
    std::string type;
};

class CppTranslator {
public:
    explicit CppTranslator(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    std::string translate() {
        while (match("import")) {
            std::string library = consumeIdentifier("ожидалось имя библиотеки").text;
            while (match(".")) library += "." + consumeIdentifier("ожидалось имя библиотеки").text;
            consume(";", "после import нужна точка с запятой");
            if (library != "Chia.Biblion" && library != "Chia.Database" && library != "Chia.User"
                && library != "Chia.Collections" && library != "Chia.Diagnostics" && library != "Chia.Game"
                && library != "Chia.OS" && library != "Chia.App" && library != "Chia.Project"
                && library != "Chia.Git" && library != "Chia.HttpExchange" && library != "Chia.Server"
                && library != "Chia.Database.Java" && library != "Chia.Database.Oracle"
                && library != "com.sun.net.httpserver.HttpExchange")
                fail(previous(), "библиотека '" + library + "' не найдена");
            if (library == "Chia.Game") gameEnabled_ = true;
        }

        bool foundMain = false;
        while (!atEnd()) {
            if (match("class")) {
                consumeIdentifier("ожидалось имя класса");
                consume("{", "после имени класса нужна {");
                continue;
            }
            if (match("public") || match("private") || match("protected")
                || match("static") || match("final") || match("void")) continue;
            if (match("main")) {
                foundMain = true;
                consume("(", "после main нужна (");
                int depth = 1;
                while (!atEnd() && depth > 0) {
                    if (match("(")) ++depth;
                    else if (match(")")) --depth;
                    else ++current_;
                }
                if (depth != 0) fail(peek(), "не закрыты параметры main");
                consume("{", "после main нужна {");
                while (!atEnd() && !check("}")) statement();
                consume("}", "не закрыто тело main");
                while (match("}")) {}
                if (!atEnd()) fail(peek(), "транслятор ожидает один класс с одним методом main");
                break;
            }
            if (match("}")) continue;
            fail(peek(), "транслятор поддерживает класс Main и метод main");
        }
        if (!foundMain) fail(peek(), "не найден метод main");

        std::string output;
        if (gameEnabled_) output += "#include <raylib.h>\n";
        output += "#include <iostream>\n#include <sstream>\n#include <string>\n\n"
               "template <typename T> std::string chiaText(const T& value) {\n"
               "    std::ostringstream output; output << value; return output.str();\n"
               "}\n"
               "inline std::string chiaText(const std::string& value) { return value; }\n"
               "inline std::string chiaText(bool value) { return value ? \"true\" : \"false\"; }\n"
               "inline const char* chiaCString(const std::string& value) { return value.c_str(); }\n\n";
        if (usesTextures_ || usesModels_ || usesAudio_) output += "#include <map>\n\n";
        if (usesTextures_) {
            output += "static std::map<std::string, Texture2D> chiaTextures;\n"
                      "static void chiaDrawTexture(const std::string& path, float x, float y, float width, float height) {\n"
                      "    auto item = chiaTextures.find(path);\n"
                      "    if (item == chiaTextures.end()) item = chiaTextures.emplace(path, LoadTexture(path.c_str())).first;\n"
                      "    const Texture2D& texture = item->second;\n"
                      "    DrawTexturePro(texture, Rectangle{0, 0, static_cast<float>(texture.width), static_cast<float>(texture.height)},\n"
                      "        Rectangle{x, y, width, height}, Vector2{0, 0}, 0, WHITE);\n"
                      "}\n\n";
        }
        if (usesAudio_) {
            output += "static std::map<std::string, Sound> chiaSounds;\n"
                      "static void chiaPlaySound(const std::string& path) {\n"
                      "    if (!IsAudioDeviceReady()) InitAudioDevice();\n"
                      "    auto item = chiaSounds.find(path);\n"
                      "    if (item == chiaSounds.end()) item = chiaSounds.emplace(path, LoadSound(path.c_str())).first;\n"
                      "    PlaySound(item->second);\n"
                      "}\n\n";
        }
        if (usesModels_) {
            output += "static std::map<std::string, Model> chiaModels;\n"
                      "static void chiaDrawModel(const std::string& path, float x, float y, float z, float scale, Color tint) {\n"
                      "    auto item = chiaModels.find(path);\n"
                      "    if (item == chiaModels.end()) item = chiaModels.emplace(path, LoadModel(path.c_str())).first;\n"
                      "    DrawModel(item->second, Vector3{x, y, z}, scale, tint);\n"
                      "}\n\n";
        }
        if (gameEnabled_ && usesCloseWindow_) {
            output += "static void chiaCloseWindow() {\n";
            if (usesTextures_)
                output += "    for (const auto& item : chiaTextures) UnloadTexture(item.second);\n";
            if (usesModels_)
                output += "    for (const auto& item : chiaModels) UnloadModel(item.second);\n";
            if (usesAudio_) {
                output += "    if (IsAudioDeviceReady()) {\n"
                          "        for (const auto& item : chiaSounds) UnloadSound(item.second);\n"
                          "        CloseAudioDevice();\n"
                          "    }\n";
            }
            output += "    CloseWindow();\n}\n\n";
        }
        if (uses3D_) {
            output += "static void chiaBegin3D(float px, float py, float pz, float tx, float ty, float tz, float fov) {\n"
                      "    Camera3D camera{};\n"
                      "    camera.position = Vector3{px, py, pz};\n"
                      "    camera.target = Vector3{tx, ty, tz};\n"
                      "    camera.up = Vector3{0.0f, 1.0f, 0.0f};\n"
                      "    camera.fovy = fov;\n"
                      "    camera.projection = CAMERA_PERSPECTIVE;\n"
                      "    BeginMode3D(camera);\n"
                      "}\n\n";
        }
        output += "int main() {\n" + body_ + "    return 0;\n}\n";
        return output;
    }

private:
    std::vector<Token> tokens_;
    std::size_t current_ = 0;
    std::string body_;
    std::map<std::string, std::string> variables_;
    bool gameEnabled_ = false;
    bool uses3D_ = false;
    bool usesTextures_ = false;
    bool usesModels_ = false;
    bool usesAudio_ = false;
    bool usesCloseWindow_ = false;

    const Token& peek() const { return tokens_[current_]; }
    const Token& previous() const { return tokens_[current_ - 1]; }
    bool atEnd() const { return peek().text == "<конец>"; }
    bool check(const std::string& text) const { return peek().text == text; }

    bool match(const std::string& text) {
        if (!check(text)) return false;
        ++current_;
        return true;
    }

    Token consume(const std::string& text, const std::string& message) {
        if (check(text)) return tokens_[current_++];
        fail(peek(), message);
    }

    Token consumeIdentifier(const std::string& message) {
        if (!peek().text.empty() && isIdentifierStart(peek().text[0])) return tokens_[current_++];
        fail(peek(), message);
    }

    [[noreturn]] static void fail(const Token& token, const std::string& message) {
        throw ChiaError(token, message);
    }

    void emit(const std::string& code) { body_ += "    " + code + "\n"; }

    void statement() {
        if (match(";")) return;
        if (match("{")) {
            body_ += "    {\n";
            while (!atEnd() && !check("}")) statement();
            consume("}", "не закрыт блок");
            body_ += "    }\n";
            return;
        }
        if (match("if")) {
            consume("(", "после if нужна скобка условия");
            CppExpression condition = expression();
            if (condition.type != "boolean" && condition.type != "bool")
                fail(previous(), "условие if должно иметь тип boolean");
            consume(")", "не закрыто условие if");
            body_ += "    if (" + condition.code + ") {\n";
            scopedStatement();
            body_ += "    }";
            if (match("else")) {
                body_ += " else {\n";
                scopedStatement();
                body_ += "    }";
            }
            body_ += "\n";
            return;
        }
        if (match("while")) {
            consume("(", "после while нужна скобка условия");
            CppExpression condition = expression();
            if (condition.type != "boolean" && condition.type != "bool")
                fail(previous(), "условие while должно иметь тип boolean");
            consume(")", "не закрыто условие while");
            body_ += "    while (" + condition.code + ") {\n";
            scopedStatement();
            body_ += "    }\n";
            return;
        }
        if (check("Dictionary") || check("Object") || check("List"))
            fail(peek(), "транслятор C++ пока не поддерживает Dictionary/List/Object");

        if (check("String") || check("int") || check("double") || check("float")
            || check("boolean") || check("bool") || check("var")) {
            declaration();
            return;
        }
        if (!peek().text.empty() && isIdentifierStart(peek().text[0])
            && current_ + 1 < tokens_.size() && tokens_[current_ + 1].text == "=") {
            const Token name = tokens_[current_++];
            ++current_;
            const auto found = variables_.find(name.text);
            if (found == variables_.end()) fail(name, "переменная '" + name.text + "' не объявлена");
            CppExpression value = expression();
            checkType(name, found->second, value);
            emit(name.text + " = " + value.code + ";");
            consume(";", "после присваивания нужна точка с запятой");
            return;
        }
        if (match("Chia") || match("System")) {
            const std::string root = previous().text;
            std::string call = root;
            while (match(".")) call += "." + consumeIdentifier("ожидалось имя метода").text;
            if (root == "Chia" && call.rfind("Chia.Game.", 0) == 0) {
                consume("(", "после игрового метода нужна (");
                std::vector<CppExpression> args;
                if (!check(")")) {
                    do {
                        args.push_back(expression());
                    } while (match(","));
                }
                consume(")", "не закрыт список аргументов");
                consume(";", "после игрового вызова нужна точка с запятой");
                const CppExpression gameCall = translateGameCall(previous(), call, args);
                emit(gameCall.code + ";");
                return;
            }
            if (root == "System") {
                if (call != "System.out.println" && call != "System.out.print")
                    fail(previous(), "транслятор поддерживает только System.out.print/println");
            } else if (call != "Chia.println" && call != "Chia.print"
                       && call != "Chia.Biblion.println" && call != "Chia.Biblion.print") {
                fail(previous(), "транслятор поддерживает Chia.print/println");
            }
            consume("(", "после метода печати нужна (");
            CppExpression value = expression();
            consume(")", "не закрыт вызов печати");
            consume(";", "после печати нужна точка с запятой");
            emit("std::cout << chiaText(" + value.code + ")"
                + ((call == "Chia.println" || call == "Chia.Biblion.println"
                    || call == "System.out.println") ? " << '\\n';" : ";"));
            return;
        }
        if (isIdentifierStart(peek().text.empty() ? '\0' : peek().text[0])) {
            CppExpression value = expression();
            consume(";", "после вызова нужна точка с запятой");
            emit(value.code + ";");
            return;
        }
        fail(peek(), "конструкция не поддерживается транслятором C++");
    }

    void scopedStatement() {
        if (match("{")) {
            while (!atEnd() && !check("}")) statement();
            consume("}", "не закрыт блок");
            return;
        }
        statement();
    }

    void declaration() {
        const Token typeToken = tokens_[current_++];
        const Token name = consumeIdentifier("ожидалось имя переменной");
        std::string cppType;
        if (typeToken.text == "String") cppType = "std::string";
        else if (typeToken.text == "boolean" || typeToken.text == "bool") cppType = "bool";
        else if (typeToken.text == "var") cppType = "auto";
        else cppType = typeToken.text;
        if (variables_.find(name.text) != variables_.end()) fail(name, "переменная уже объявлена");
        if (!match("=")) fail(name, "переменная должна иметь начальное значение для трансляции в C++");
        CppExpression value = expression();
        const std::string chiaType = typeToken.text == "var" ? value.type : typeToken.text;
        checkType(name, chiaType, value);
        variables_[name.text] = chiaType;
        emit(cppType + " " + name.text + " = " + value.code + ";");
        consume(";", "после объявления нужна точка с запятой");
    }

    static void checkType(const Token& token, const std::string& expected, const CppExpression& value) {
        const bool valid = expected == "Object" || expected == "var" || expected == value.type
            || ((expected == "double" || expected == "float") && value.type == "int");
        if (!valid) fail(token, "нельзя присвоить '" + value.type + "' переменной типа '" + expected + "'");
    }

    CppExpression expression() { return parseBinary(0); }

    static int precedence(const std::string& op) {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "==" || op == "!=") return 3;
        if (op == "<" || op == "<=" || op == ">" || op == ">=") return 4;
        if (op == "+" || op == "-") return 5;
        if (op == "*" || op == "/" || op == "%") return 6;
        return -1;
    }

    CppExpression parseBinary(int minimumPrecedence) {
        CppExpression left = unary();
        while (precedence(peek().text) >= minimumPrecedence) {
            const std::string op = tokens_[current_++].text;
            const int nextPrecedence = precedence(op) + 1;
            CppExpression right = parseBinary(nextPrecedence);
            std::string code;
            std::string type;
            if (op == "+" && (left.type == "String" || right.type == "String")) {
                code = "chiaText(" + left.code + ") + chiaText(" + right.code + ")";
                type = "String";
            } else {
                if ((op == "+" || op == "-" || op == "*" || op == "/" || op == "%")
                    && (left.type != "int" && left.type != "double"
                        && left.type != "float" && left.type != "number"))
                    fail(previous(), "арифметический оператор принимает числа");
                if ((op == "+" || op == "-" || op == "*" || op == "/" || op == "%")
                    && (right.type != "int" && right.type != "double"
                        && right.type != "float" && right.type != "number"))
                    fail(previous(), "арифметический оператор принимает числа");
                code = "(" + left.code + " " + op + " " + right.code + ")";
                if (op == "==" || op == "!=" || op == "<" || op == "<="
                    || op == ">" || op == ">=" || op == "&&" || op == "||")
                    type = "boolean";
                else type = left.type == "double" || right.type == "double" ? "double" : "int";
            }
            left = {code, type};
        }
        return left;
    }

    CppExpression unary() {
        if (match("!") || match("-")) {
            const std::string op = previous().text;
            const CppExpression operand = unary();
            if (op == "!" && operand.type != "boolean" && operand.type != "bool")
                fail(previous(), "оператор ! принимает boolean");
            if (op == "-" && operand.type != "int" && operand.type != "double" && operand.type != "float")
                fail(previous(), "унарный - принимает число");
            return {"(" + op + operand.code + ")", op == "!" ? "boolean" : operand.type};
        }
        return primary();
    }

    CppExpression primary() {
        if (match("(")) {
            CppExpression value = expression();
            consume(")", "не закрыта скобка");
            return {"(" + value.code + ")", value.type};
        }
        if (match("true")) return {"true", "boolean"};
        if (match("false")) return {"false", "boolean"};
        if (!atEnd() && !peek().text.empty() && peek().text[0] == '\x01') {
            const std::string text = peek().text.substr(1);
            ++current_;
            return {cppString(text), "String"};
        }
        if (!atEnd() && std::isdigit(static_cast<unsigned char>(peek().text[0]))) {
            const std::string number = tokens_[current_++].text;
            return {number, number.find('.') == std::string::npos ? "int" : "double"};
        }
        if (!atEnd() && !peek().text.empty() && isIdentifierStart(peek().text[0])) {
            const Token identifier = tokens_[current_++];
            std::string qualified = identifier.text;
            while (match(".")) qualified += "." + consumeIdentifier("после точки ожидалось имя метода").text;
            if (match("(")) {
                std::vector<CppExpression> args;
                if (!check(")")) {
                    do {
                        args.push_back(expression());
                    } while (match(","));
                }
                consume(")", "не закрыт список аргументов");
                if (qualified.rfind("Chia.Game.", 0) == 0)
                    return translateGameCall(identifier, qualified, args);
                fail(identifier, "неизвестная функция '" + qualified + "'");
            }
            const auto found = variables_.find(identifier.text);
            if (found == variables_.end()) fail(identifier, "переменная '" + identifier.text + "' не объявлена");
            return {identifier.text, found->second};
        }
        fail(peek(), "ожидалось выражение, поддерживаемое транслятором");
    }

    CppExpression translateGameCall(const Token& token, const std::string& name,
                                   const std::vector<CppExpression>& args) {
        if (!gameEnabled_)
            fail(token, "добавьте import Chia.Game; для использования игрового API");
        const std::string method = name.substr(std::string("Chia.Game.").size());
        const auto count = [&](std::size_t expected) {
            if (args.size() != expected)
                fail(token, "Chia.Game." + method + " ожидает аргументов: " + std::to_string(expected));
        };
        const auto numeric = [&](std::size_t index) {
            if (index >= args.size() || (args[index].type != "int" && args[index].type != "double"
                && args[index].type != "float" && args[index].type != "number"))
                fail(token, "аргумент " + std::to_string(index + 1) + " должен быть числом");
        };
        const auto color = [&]() {
            return "Color{static_cast<unsigned char>(" + args[args.size() - 3].code
                + "), static_cast<unsigned char>(" + args[args.size() - 2].code
                + "), static_cast<unsigned char>(" + args[args.size() - 1].code + "), 255}";
        };
        const auto numericArgs = [&](std::size_t start, std::size_t end) {
            for (std::size_t i = start; i < end; ++i) numeric(i);
        };
        if (method == "window") {
            count(3);
            numeric(0); numeric(1);
            if (args[2].type != "String") fail(token, "название окна должно быть String");
            return {"InitWindow(static_cast<int>(" + args[0].code + "), static_cast<int>("
                + args[1].code + "), chiaCString(" + args[2].code + "))", "void"};
        }
        if (method == "frameRate") {
            count(1); numeric(0);
            return {"SetTargetFPS(static_cast<int>(" + args[0].code + "))", "void"};
        }
        if (method == "shouldClose") {
            count(0);
            return {"WindowShouldClose()", "boolean"};
        }
        if (method == "beginFrame" || method == "endFrame" || method == "closeWindow") {
            count(0);
            if (method == "closeWindow") usesCloseWindow_ = true;
            return {method == "beginFrame" ? "BeginDrawing()"
                : method == "endFrame" ? "EndDrawing()" : "chiaCloseWindow()", "void"};
        }
        if (method == "clear") {
            count(3); numericArgs(0, 3);
            return {"ClearBackground(" + color() + ")", "void"};
        }
        if (method == "drawText") {
            count(7);
            if (args[0].type != "String") fail(token, "первый аргумент drawText должен быть String");
            numericArgs(1, 7);
            return {"DrawText(chiaCString(" + args[0].code + "), static_cast<int>(" + args[1].code
                + "), static_cast<int>(" + args[2].code + "), static_cast<int>(" + args[3].code
                + "), " + color() + ")", "void"};
        }
        if (method == "drawCircle") {
            count(6); numericArgs(0, 6);
            return {"DrawCircle(static_cast<int>(" + args[0].code + "), static_cast<int>("
                + args[1].code + "), static_cast<int>(" + args[2].code + "), " + color() + ")", "void"};
        }
        if (method == "drawRectangle") {
            count(7); numericArgs(0, 7);
            return {"DrawRectangle(static_cast<int>(" + args[0].code + "), static_cast<int>("
                + args[1].code + "), static_cast<int>(" + args[2].code + "), static_cast<int>("
                + args[3].code + "), " + color() + ")", "void"};
        }
        if (method == "drawTexture") {
            count(5);
            if (args[0].type != "String") fail(token, "путь к текстуре должен быть String");
            numericArgs(1, 5);
            usesTextures_ = true;
            return {"chiaDrawTexture(" + args[0].code + ", static_cast<float>(" + args[1].code
                + "), static_cast<float>(" + args[2].code + "), static_cast<float>("
                + args[3].code + "), static_cast<float>(" + args[4].code + "))", "void"};
        }
        if (method == "drawModel") {
            count(8);
            if (args[0].type != "String") fail(token, "путь к 3D-модели должен быть String");
            numericArgs(1, 8);
            usesModels_ = true;
            return {"chiaDrawModel(" + args[0].code + ", static_cast<float>(" + args[1].code
                + "), static_cast<float>(" + args[2].code + "), static_cast<float>("
                + args[3].code + "), static_cast<float>(" + args[4].code + "), " + color() + ")", "void"};
        }
        if (method == "collidesRect") {
            count(8); numericArgs(0, 8);
            return {"CheckCollisionRecs(Rectangle{static_cast<float>(" + args[0].code
                + "), static_cast<float>(" + args[1].code + "), static_cast<float>("
                + args[2].code + "), static_cast<float>(" + args[3].code
                + ")}, Rectangle{static_cast<float>(" + args[4].code + "), static_cast<float>("
                + args[5].code + "), static_cast<float>(" + args[6].code + "), static_cast<float>("
                + args[7].code + ")})", "boolean"};
        }
        if (method == "collidesCircles") {
            count(6); numericArgs(0, 6);
            return {"CheckCollisionCircles(Vector2{static_cast<float>(" + args[0].code
                + "), static_cast<float>(" + args[1].code + ")}, static_cast<float>("
                + args[2].code + "), Vector2{static_cast<float>(" + args[3].code
                + "), static_cast<float>(" + args[4].code + ")}, static_cast<float>("
                + args[5].code + "))", "boolean"};
        }
        if (method == "keyDown" || method == "keyPressed") {
            count(1);
            if (args[0].type != "String") fail(token, "имя клавиши должно быть String");
            static const std::map<std::string, std::string> keys{
                {"LEFT", "KEY_LEFT"}, {"RIGHT", "KEY_RIGHT"}, {"UP", "KEY_UP"}, {"DOWN", "KEY_DOWN"},
                {"SPACE", "KEY_SPACE"}, {"ENTER", "KEY_ENTER"}, {"ESCAPE", "KEY_ESCAPE"},
                {"A", "KEY_A"}, {"D", "KEY_D"}, {"S", "KEY_S"}, {"W", "KEY_W"},
                {"LEFT_SHIFT", "KEY_LEFT_SHIFT"}, {"RIGHT_SHIFT", "KEY_RIGHT_SHIFT"}
            };
            const auto key = keys.find(args[0].code.substr(1, args[0].code.size() - 2));
            if (key == keys.end()) fail(token, "неизвестная клавиша; смотрите список клавиш в CHIA.md");
            return {std::string(method == "keyDown" ? "IsKeyDown(" : "IsKeyPressed(") + key->second + ")",
                "boolean"};
        }
        if (method == "mouseX" || method == "mouseY") {
            count(0);
            return {method == "mouseX" ? "GetMouseX()" : "GetMouseY()", "int"};
        }
        if (method == "mouseDown" || method == "mousePressed") {
            count(0);
            return {method == "mouseDown" ? "IsMouseButtonDown(MOUSE_BUTTON_LEFT)"
                                          : "IsMouseButtonPressed(MOUSE_BUTTON_LEFT)", "boolean"};
        }
        if (method == "deltaTime") {
            count(0);
            return {"GetFrameTime()", "double"};
        }
        if (method == "initAudio") {
            count(0);
            usesAudio_ = true;
            return {"InitAudioDevice()", "void"};
        }
        if (method == "playSound") {
            count(1);
            if (args[0].type != "String") fail(token, "путь к аудиофайлу должен быть String");
            usesAudio_ = true;
            return {"chiaPlaySound(" + args[0].code + ")", "void"};
        }
        if (method == "begin3D") {
            count(7); numericArgs(0, 7);
            uses3D_ = true;
            std::string call = "chiaBegin3D(";
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i != 0) call += ", ";
                call += "static_cast<float>(" + args[i].code + ")";
            }
            return {call + ")", "void"};
        }
        if (method == "end3D") {
            count(0);
            return {"EndMode3D()", "void"};
        }
        if (method == "drawCube" || method == "drawCubeWires") {
            count(9); numericArgs(0, 9);
            const std::string function = method == "drawCube" ? "DrawCube" : "DrawCubeWires";
            return {function + "(Vector3{static_cast<float>(" + args[0].code
                + "), static_cast<float>(" + args[1].code + "), static_cast<float>("
                + args[2].code + ")}, static_cast<float>(" + args[3].code
                + "), static_cast<float>(" + args[4].code + "), static_cast<float>("
                + args[5].code + "), " + color() + ")", "void"};
        }
        if (method == "drawGrid") {
            count(2); numericArgs(0, 2);
            return {"DrawGrid(static_cast<int>(" + args[0].code + "), static_cast<float>("
                + args[1].code + "))", "void"};
        }
        if (method == "time") {
            count(0);
            return {"GetTime()", "double"};
        }
        fail(token, "неизвестный метод Chia.Game." + method);
    }

    static std::string cppString(const std::string& value) {
        std::string result = "\"";
        for (char character : value) {
            switch (character) {
                case '\\': result += "\\\\"; break;
                case '"': result += "\\\""; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += character; break;
            }
        }
        result += '"';
        return result;
    }
};

static const char* starterScript =
    "import Chia.Biblion;\n"
    "import Chia.Database;\n"
    "\n"
    "public class Main {\n"
    "    public static void main(String[] args) {\n"
    "        String message = \"Привет, Chia!\";\n"
    "        Chia.println(message);\n"
    "\n"
    "        Dictionary<String, int> scores = new Dictionary<>();\n"
    "        scores.put(\"Аня\", 10);\n"
    "        Chia.println(\"Счёт: \" + scores.get(\"Аня\"));\n"
    "    }\n"
    "}\n";

static const char* starter2DGame =
    "import Chia.Game;\n"
    "\n"
    "public class Main {\n"
    "    public static void main(String[] args) {\n"
    "        Chia.Game.window(960, 540, \"Chia 2D Game\");\n"
    "        Chia.Game.frameRate(60);\n"
    "        int playerX = 460;\n"
    "        int playerY = 250;\n"
    "\n"
    "        while (!Chia.Game.shouldClose()) {\n"
    "            if (Chia.Game.keyDown(\"LEFT\")) { playerX = playerX - 4; }\n"
    "            if (Chia.Game.keyDown(\"RIGHT\")) { playerX = playerX + 4; }\n"
    "            if (Chia.Game.keyDown(\"UP\")) { playerY = playerY - 4; }\n"
    "            if (Chia.Game.keyDown(\"DOWN\")) { playerY = playerY + 4; }\n"
    "\n"
    "            Chia.Game.beginFrame();\n"
    "            Chia.Game.clear(20, 24, 38);\n"
    "            Chia.Game.drawText(\"CHIA 2D — move with arrow keys\", 24, 20, 20, 230, 235, 245);\n"
    "            Chia.Game.drawRectangle(playerX, playerY, 40, 40, 80, 190, 255);\n"
    "            Chia.Game.endFrame();\n"
    "        }\n"
    "\n"
    "        Chia.Game.closeWindow();\n"
    "    }\n"
    "}\n";

static const char* starter3DGame =
    "import Chia.Game;\n"
    "\n"
    "public class Main {\n"
    "    public static void main(String[] args) {\n"
    "        Chia.Game.window(1100, 700, \"Chia 3D Game\");\n"
    "        Chia.Game.frameRate(60);\n"
    "\n"
    "        while (!Chia.Game.shouldClose()) {\n"
    "            Chia.Game.beginFrame();\n"
    "            Chia.Game.clear(18, 23, 38);\n"
    "            Chia.Game.begin3D(8, 7, 10, 0, 1, 0, 45);\n"
    "            Chia.Game.drawGrid(20, 1.0);\n"
    "            Chia.Game.drawCube(0, 1, 0, 2, 2, 2, 80, 180, 255);\n"
    "            Chia.Game.drawCubeWires(0, 1, 0, 2, 2, 2, 230, 240, 255);\n"
    "            Chia.Game.end3D();\n"
    "            Chia.Game.drawText(\"CHIA 3D — Raylib\", 24, 20, 20, 230, 235, 245);\n"
    "            Chia.Game.endFrame();\n"
    "        }\n"
    "\n"
    "        Chia.Game.closeWindow();\n"
    "    }\n"
    "}\n";

static void printHelp() {
    std::cout
        << "Chia Swing Compilator — компилятор-интерпретатор Chia\n\n"
        << "Использование:\n"
        << "  chia new [файл.chia]   создать файл с примером программы\n"
        << "  chia run <файл.chia>   выполнить программу\n"
        << "  chia check <файл.chia> проверить синтаксис и типы\n"
        << "  chia translate <вход.chia> <выход.cpp> транслировать поддерживаемый код в C++\n"
        << "  chia spell <файл.chia>  проверить орфографию и отпечатки синтаксиса\n"
        << "  chia correct <вход.chia> <выход.chia> исправить известные опечатки\n"
        << "  chia game engines       найти установленные игровые движки\n"
        << "  chia game new <движок> <2d|3d> <папка> создать игровой проект\n"
        << "  chia game open <папка>  открыть проект в редакторе\n"
        << "  chia game build <папка> собрать игру установленным движком\n"
        << "  chia run <app.chia>     запустить сценарий Chia и его API\n"
        << "  chia --help            показать эту справку\n\n"
        << "Поддерживаются Java-подобный класс Main, переменные и типы, "
        << "арифметика, строки, игровой API Raylib для 2D/3D,\n"
        << "Dictionary, Chia.Biblion, Chia.Database, Chia.User, создание проектов ОС/приложений,\n"
        << "Git, JDBC и Java HttpExchange (для последних требуются их внешние инструменты).\n";
}

static bool readSourceFile(const std::filesystem::path& path, std::string& source) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "Chia: не удалось открыть файл: " << path.string() << '\n';
        return false;
    }
    std::ostringstream content;
    content << input.rdbuf();
    if (!input.good() && !input.eof()) {
        std::cerr << "Chia: ошибка чтения файла: " << path.string() << '\n';
        return false;
    }
    source = content.str();
    return true;
}

static bool writeNewFile(const std::filesystem::path& path, const std::string& contents) {
    if (std::filesystem::exists(path)) {
        std::cerr << "Chia: выходной файл уже существует: " << path.string() << '\n';
        return false;
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        std::cerr << "Chia: не удалось создать файл: " << path.string() << '\n';
        return false;
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        std::cerr << "Chia: ошибка записи файла: " << path.string() << '\n';
        return false;
    }
    return true;
}

static int checkSpelling(const std::filesystem::path& inputPath) {
    std::string source;
    if (!readSourceFile(inputPath, source)) return 1;
    const auto issues = findSpellingIssues(source);
    const auto hints = findSyntaxFingerprints(source);
    if (issues.empty()) {
        std::cout << "Орфограф: известных опечаток в строках и комментариях не найдено.\n";
    } else {
        std::cout << "Орфографические подсказки:\n";
        for (const auto& issue : issues) {
            std::cout << inputPath.string() << ':' << issue.first.line << ':' << issue.first.column
                      << ": " << issue.first.text << " -> " << issue.second << '\n';
        }
        std::cout << "Найдено известных опечаток: " << issues.size() << ".\n";
    }
    if (hints.empty()) {
        std::cout << "Синтаксических отпечатков не найдено.\n";
    } else {
        std::cout << "Синтаксические отпечатки и советы:\n";
        for (const SyntaxHint& hint : hints) {
            std::cout << inputPath.string() << ':' << hint.line << ':' << hint.column
                      << ": " << hint.fingerprint << "\n  Совет: " << hint.advice << '\n';
        }
    }
    return issues.empty() && hints.empty() ? 0 : 1;
}

static int correctSpelling(const std::filesystem::path& inputPath,
                           const std::filesystem::path& outputPath) {
    const std::filesystem::path input = std::filesystem::absolute(inputPath).lexically_normal();
    const std::filesystem::path output = std::filesystem::absolute(outputPath).lexically_normal();
    if (input == output) {
        std::cerr << "Chia: корректор сохраняет результат в отдельный файл, не перезаписывая исходник.\n";
        return 2;
    }
    std::string source;
    if (!readSourceFile(input, source)) return 1;
    const auto issues = findSpellingIssues(source);
    for (auto issue = issues.rbegin(); issue != issues.rend(); ++issue) {
        source.replace(issue->first.start, issue->first.length, issue->second);
    }
    if (!writeNewFile(output, source)) return 1;
    std::cout << "Исправлено известных опечаток: " << issues.size()
              << ". Результат: " << output.string() << '\n';
    return 0;
}

static int translateFile(const std::filesystem::path& inputPath,
                         const std::filesystem::path& outputPath) {
    const std::filesystem::path input = std::filesystem::absolute(inputPath).lexically_normal();
    const std::filesystem::path output = std::filesystem::absolute(outputPath).lexically_normal();
    if (input == output) {
        std::cerr << "Chia: выходной C++ файл должен отличаться от исходного файла Chia.\n";
        return 2;
    }
    std::string source;
    if (!readSourceFile(input, source)) return 1;
    try {
        CppTranslator translator(tokenize(source));
        const std::string generated = translator.translate();
        if (!writeNewFile(output, generated)) return 1;
        std::cout << "Код Chia транслирован в C++: " << output.string() << '\n';
        return 0;
    } catch (const ChiaError& error) {
        std::cerr << "Chia Translator: " << error.what() << '\n';
    }
    return 1;
}

static int runFile(const std::filesystem::path& inputPath, bool checkOnly) {
    const std::filesystem::path path = std::filesystem::absolute(inputPath).lexically_normal();
    std::string source;
    if (!readSourceFile(path, source)) return 1;
    try {
        std::vector<Token> tokens = tokenize(source);
        if (checkOnly) {
            bool gameImport = false;
            for (std::size_t i = 0; i + 3 < tokens.size(); ++i) {
                if (tokens[i].text == "import" && tokens[i + 1].text == "Chia"
                    && tokens[i + 2].text == "." && tokens[i + 3].text == "Game") {
                    gameImport = true;
                    break;
                }
            }
            if (gameImport) {
                CppTranslator translator(std::move(tokens));
                translator.translate();
                std::cout << "Проверка игрового проекта пройдена.\n";
                return 0;
            }
        }
        Interpreter interpreter(std::move(tokens), path.parent_path(), checkOnly);
        interpreter.run();
        if (checkOnly) std::cout << "Проверка пройдена: ошибок не найдено.\n";
        return 0;
    } catch (const ChiaError& error) {
        std::cerr << "Chia: " << error.what() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Chia: ошибка выполнения: " << error.what() << '\n';
    }
    return 1;
}

static int createFile(const std::filesystem::path& requestedPath) {
    std::filesystem::path path = requestedPath;
    if (path.extension().empty()) path += ".chia";
    if (std::filesystem::exists(path)) {
        std::cerr << "Chia: файл уже существует: " << path.string() << '\n';
        return 1;
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        std::cerr << "Chia: не удалось создать файл: " << path.string() << '\n';
        return 1;
    }
    output << starterScript;
    if (!output) {
        std::cerr << "Chia: ошибка записи файла: " << path.string() << '\n';
        return 1;
    }
    std::cout << "Создан файл Chia с примером программы: " << path.string() << '\n';
    return 0;
}

struct EngineInstallation {
    std::string name;
    std::filesystem::path editor;
    std::filesystem::path buildTool;
    std::filesystem::path root;
};

static std::string environmentValue(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || !value) return "";
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

static std::optional<std::filesystem::path> findOnPath(const std::string& executable) {
    const std::string pathValue = environmentValue("PATH");
    if (pathValue.empty()) return std::nullopt;
    std::istringstream paths(pathValue);
    std::string entry;
    while (std::getline(paths, entry, ';')) {
        if (entry.empty()) continue;
        const std::filesystem::path candidate = std::filesystem::path(entry) / executable;
        if (std::filesystem::is_regular_file(candidate)) return candidate;
    }
    return std::nullopt;
}

static std::optional<std::filesystem::path> findConfiguredExecutable(const char* variable) {
    const std::string configured = environmentValue(variable);
    if (configured.empty()) return std::nullopt;
    std::filesystem::path candidate(configured);
    if (std::filesystem::is_regular_file(candidate)) return candidate;
    return std::nullopt;
}

static std::optional<EngineInstallation> findEngine(const std::string& engine) {
    if (engine == "raylib") {
        const auto cmake = findOnPath("cmake.exe");
        if (!cmake) return std::nullopt;
        return EngineInstallation{"raylib", *cmake, {}, {}};
    }
    if (engine == "unity") {
        std::vector<std::filesystem::path> candidates;
        if (const auto configured = findConfiguredExecutable("UNITY_EDITOR")) candidates.push_back(*configured);
        if (const auto inPath = findOnPath("Unity.exe")) candidates.push_back(*inPath);
        candidates.emplace_back(L"C:\\Program Files\\Unity\\Hub\\Editor");
        candidates.emplace_back(L"C:\\Program Files\\Unity\\Editor\\Unity.exe");
        const std::string local = environmentValue("LOCALAPPDATA");
        if (!local.empty())
            candidates.emplace_back(std::filesystem::path(local) / L"Programs/Unity/Hub/Editor");
        for (const auto& candidate : candidates) {
            if (std::filesystem::is_regular_file(candidate))
                return EngineInstallation{"unity", candidate, {},
                    candidate.parent_path().filename() == L"Editor"
                        ? candidate.parent_path().parent_path() : candidate.parent_path()};
            if (!std::filesystem::is_directory(candidate)) continue;
            for (const auto& version : std::filesystem::directory_iterator(candidate)) {
                const auto editor = version.path() / L"Editor/Unity.exe";
                if (std::filesystem::is_regular_file(editor))
                    return EngineInstallation{"unity", editor, {}, version.path()};
            }
        }
        return std::nullopt;
    }
    if (engine == "godot") {
        if (const auto configured = findConfiguredExecutable("GODOT_EXE"))
            return EngineInstallation{"godot", *configured, {}, configured->parent_path()};
        for (const std::string& executable : {"godot.exe", "godot4.exe"})
            if (const auto found = findOnPath(executable))
                return EngineInstallation{"godot", *found, {}, found->parent_path()};
        std::vector<std::filesystem::path> godotRoots{
            std::filesystem::path(L"C:\\Program Files"),
            std::filesystem::path(L"C:\\Program Files (x86)")};
        const std::string local = environmentValue("LOCALAPPDATA");
        if (!local.empty()) godotRoots.emplace_back(std::filesystem::path(local) / L"Programs");
        for (const auto& base : godotRoots) {
            if (!std::filesystem::is_directory(base)) continue;
            for (const auto& entry : std::filesystem::directory_iterator(base)) {
                if (entry.path().filename().wstring().find(L"Godot") == std::wstring::npos) continue;
                if (entry.is_regular_file() && entry.path().extension() == L".exe")
                    return EngineInstallation{"godot", entry.path(), {}, base};
                for (const wchar_t* executable : {L"Godot.exe", L"godot.exe", L"Godot_v4.4-stable_win64.exe"}) {
                    const auto candidate = entry.path() / executable;
                    if (std::filesystem::is_regular_file(candidate))
                        return EngineInstallation{"godot", candidate, {}, entry.path()};
                }
            }
        }
        return std::nullopt;
    }
    if (engine == "unreal") {
        std::vector<std::filesystem::path> roots;
        const std::string configured = environmentValue("UNREAL_ENGINE_ROOT");
        if (!configured.empty()) roots.emplace_back(configured);
        roots.emplace_back(L"C:\\Program Files\\Epic Games");
        for (const auto& base : roots) {
            std::vector<std::filesystem::path> engineRoots{base};
            if (std::filesystem::is_directory(base))
                for (const auto& entry : std::filesystem::directory_iterator(base))
                    if (entry.is_directory()) engineRoots.push_back(entry.path());
            for (const auto& root : engineRoots) {
                const auto editor = root / L"Engine/Binaries/Win64/UnrealEditor.exe";
                const auto build = root / L"Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe";
                const auto buildDll = root / L"Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.dll";
                if (std::filesystem::is_regular_file(editor))
                    return EngineInstallation{"unreal", editor,
                        std::filesystem::is_regular_file(build) ? build : buildDll, root};
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

#ifdef _WIN32
static std::wstring quoteWindowsArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
        } else if (character == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result += L'"';
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            slashes = 0;
            result += character;
        }
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}
#endif

static int launchProcess(const std::filesystem::path& executable,
                         const std::vector<std::wstring>& arguments, bool wait) {
#ifdef _WIN32
    std::wstring command = quoteWindowsArgument(executable.wstring());
    for (const std::wstring& argument : arguments) command += L" " + quoteWindowsArgument(argument);
    std::vector<wchar_t> writable(command.begin(), command.end());
    writable.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), writable.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup, &process)) {
        std::cerr << "Chia: не удалось запустить " << executable.string()
                  << " (код Windows " << GetLastError() << ").\n";
        return 1;
    }
    CloseHandle(process.hThread);
    if (!wait) {
        CloseHandle(process.hProcess);
        return 0;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    const BOOL readExitCode = GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    if (!readExitCode) {
        std::cerr << "Chia: не удалось получить код завершения процесса.\n";
        return 1;
    }
    return static_cast<int>(exitCode);
#else
    (void)executable;
    (void)arguments;
    (void)wait;
    std::cerr << "Chia: запуск игровых движков поддерживается в Windows.\n";
    return 1;
#endif
}

static std::string projectIdentifier(const std::filesystem::path& path) {
    std::string result;
    for (unsigned char character : path.filename().string()) {
        if (std::isalnum(character) || character == '_') result += static_cast<char>(character);
        else if (!result.empty() && result.back() != '_') result += '_';
    }
    if (result.empty() || !std::isalpha(static_cast<unsigned char>(result[0]))) result = "Chia_" + result;
    return result;
}

static std::string godot2DScript() {
    return R"CHIA(extends Node2D

var player_position := Vector2(400, 300)
const PLAYER_SPEED := 280.0

func _process(delta: float) -> void:
    var direction := Input.get_vector("ui_left", "ui_right", "ui_up", "ui_down")
    player_position += direction * PLAYER_SPEED * delta
    queue_redraw()

func _draw() -> void:
    draw_rect(Rect2(Vector2.ZERO, get_viewport_rect().size), Color("#141826"))
    draw_circle(player_position, 24.0, Color("#50beff"))
    draw_string(ThemeDB.fallback_font, Vector2(24, 36), "Chia + Godot 2D | Arrow keys to move",
        HORIZONTAL_ALIGNMENT_LEFT, -1, 22, Color("#e6ebf5"))
)CHIA";
}

static std::string godot3DScript() {
    return R"CHIA(extends Node3D

var cube: MeshInstance3D

func _ready() -> void:
    var camera := Camera3D.new()
    camera.position = Vector3(5, 4, 7)
    camera.look_at(Vector3.ZERO)
    add_child(camera)

    var light := DirectionalLight3D.new()
    light.rotation_degrees = Vector3(-45, -30, 0)
    add_child(light)

    cube = MeshInstance3D.new()
    cube.mesh = BoxMesh.new()
    var material := StandardMaterial3D.new()
    material.albedo_color = Color("#50beff")
    cube.material_override = material
    add_child(cube)

func _process(delta: float) -> void:
    cube.rotate_y(delta)
)CHIA";
}

static std::string unityController(bool is2D) {
    if (is2D) return R"CHIA(using UnityEngine;

public sealed class ChiaGameController : MonoBehaviour
{
    [SerializeField] private float speed = 5f;

    private void Update()
    {
        float x = Input.GetAxisRaw("Horizontal");
        float y = Input.GetAxisRaw("Vertical");
        transform.position += new Vector3(x, y, 0f).normalized * speed * Time.deltaTime;
    }
}
)CHIA";
    return R"CHIA(using UnityEngine;

public sealed class ChiaGameController : MonoBehaviour
{
    [SerializeField] private float speed = 5f;

    private void Update()
    {
        float x = Input.GetAxisRaw("Horizontal");
        float z = Input.GetAxisRaw("Vertical");
        transform.position += new Vector3(x, 0f, z).normalized * speed * Time.deltaTime;
    }
}
)CHIA";
}

static std::string unityBootstrap(bool is2D) {
    std::string source = R"CHIA(using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

[InitializeOnLoad]
internal static class ChiaBootstrap
{
    private const string ScenePath = "Assets/Scenes/Main.unity";

    static ChiaBootstrap()
    {
        EditorApplication.delayCall += EnsureScene;
    }

    internal static void EnsureScene()
    {
        if (AssetDatabase.LoadAssetAtPath<SceneAsset>(ScenePath) != null)
            return;

        System.IO.Directory.CreateDirectory("Assets/Scenes");
        Scene scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
        GameObject cameraObject = new GameObject("Main Camera");
        cameraObject.tag = "MainCamera";
        Camera camera = cameraObject.AddComponent<Camera>();
        if (CHIA_2D)
        {
            cameraObject.transform.position = new Vector3(0, 0, -10);
            camera.orthographic = true;
            camera.orthographicSize = 5;
        }
        else
        {
            cameraObject.transform.position = new Vector3(0, 5, -8);
            cameraObject.transform.LookAt(Vector3.zero);
        }
        cameraObject.AddComponent<AudioListener>();

        GameObject lightObject = new GameObject("Directional Light");
        lightObject.transform.rotation = Quaternion.Euler(50, -30, 0);
        lightObject.AddComponent<Light>().type = LightType.Directional;

        GameObject player = GameObject.CreatePrimitive(CHIA_2D ? PrimitiveType.Quad : PrimitiveType.Cube);
        player.name = "Player";
        if (CHIA_2D)
        {
            MeshRenderer renderer = player.GetComponent<MeshRenderer>();
            renderer.sharedMaterial = new Material(Shader.Find("Sprites/Default"))
            {
                color = new Color(0.31f, 0.75f, 1f)
            };
        }
        player.AddComponent<ChiaGameController>();
        EditorSceneManager.SaveScene(scene, ScenePath);
        AssetDatabase.Refresh();
        if (EditorBuildSettings.scenes.Length == 0)
            EditorBuildSettings.scenes = new[] { new EditorBuildSettingsScene(ScenePath, true) };
    }
}
 )CHIA";
    const std::string marker = "CHIA_2D";
    const std::string replacement = is2D ? "true" : "false";
    std::size_t position = 0;
    while ((position = source.find(marker, position)) != std::string::npos) {
        source.replace(position, marker.size(), replacement);
        position += replacement.size();
    }
    return source;
}

static std::string unityBuildScript() {
    return R"CHIA(using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEditor.Build.Reporting;
using UnityEngine;

public static class ChiaBuild
{
    public static void PerformBuild()
    {
        ChiaBootstrap.EnsureScene();
        System.IO.Directory.CreateDirectory("Build");
        BuildPlayerOptions options = new BuildPlayerOptions
        {
            scenes = new[] { "Assets/Scenes/Main.unity" },
            locationPathName = "Build/ChiaGame.exe",
            target = BuildTarget.StandaloneWindows64,
            options = BuildOptions.None
        };
        BuildReport report = BuildPipeline.BuildPlayer(options);
        if (report.summary.result != BuildResult.Succeeded)
            throw new System.Exception("Chia Unity build failed: " + report.summary.result);
        Debug.Log("Chia game built: " + options.locationPathName);
    }
}
)CHIA";
}

static std::string unrealGameModeHeader(const std::string& name) {
    std::string api = name;
    std::transform(api.begin(), api.end(), api.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return "#pragma once\n#include \"CoreMinimal.h\"\n#include \"GameFramework/GameModeBase.h\"\n#include \""
        + name + "GameMode.generated.h\"\n\nUCLASS()\nclass " + api
        + "_API A" + name + "GameMode : public AGameModeBase\n{\n    GENERATED_BODY()\npublic:\n    A"
        + name + "GameMode();\n};\n";
}

static std::string unrealGameModeSource(const std::string& name) {
    return "#include \"" + name + "GameMode.h\"\n#include \"" + name
        + "Character.h\"\n\nA" + name + "GameMode::A" + name
        + "GameMode()\n{\n    DefaultPawnClass = A" + name + "Character::StaticClass();\n}\n";
}

static std::string unrealCharacterHeader(const std::string& name) {
    std::string api = name;
    std::transform(api.begin(), api.end(), api.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return "#pragma once\n#include \"CoreMinimal.h\"\n#include \"GameFramework/Character.h\"\n#include \""
        + name + "Character.generated.h\"\n\nUCLASS()\nclass " + api
        + "_API A" + name + "Character : public ACharacter\n{\n    GENERATED_BODY()\npublic:\n    A"
        + name + "Character();\n    virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;\nprivate:\n"
          "    void MoveForward(float Value);\n    void MoveRight(float Value);\n};\n";
}

static std::string unrealCharacterSource(const std::string& name, bool is2D) {
    const std::string camera = is2D
        ? "    CameraBoom->SetRelativeRotation(FRotator(-90.0f, 0.0f, 0.0f));\n"
          "    CameraBoom->TargetArmLength = 900.0f;\n"
          "    GetCharacterMovement()->GravityScale = 0.0f;\n"
          "    GetCharacterMovement()->SetMovementMode(MOVE_Flying);\n"
        : "    CameraBoom->SetRelativeRotation(FRotator(-12.0f, 0.0f, 0.0f));\n"
          "    CameraBoom->TargetArmLength = 500.0f;\n";
    return "#include \"" + name + "Character.h\"\n#include \"Camera/CameraComponent.h\"\n"
        "#include \"GameFramework/SpringArmComponent.h\"\n#include \"GameFramework/Controller.h\"\n"
        "#include \"GameFramework/CharacterMovementComponent.h\"\n#include \"Components/InputComponent.h\"\n"
        "#include \"Components/StaticMeshComponent.h\"\n#include \"Engine/StaticMesh.h\"\n"
        "#include \"UObject/ConstructorHelpers.h\"\n\n"
        "A" + name + "Character::A" + name
        + "Character()\n{\n    PrimaryActorTick.bCanEverTick = true;\n"
          "    UStaticMeshComponent* PlayerVisual = CreateDefaultSubobject<UStaticMeshComponent>(TEXT(\"PlayerVisual\"));\n"
          "    PlayerVisual->SetupAttachment(RootComponent);\n"
          "    static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT(\"/Engine/BasicShapes/Cube.Cube\"));\n"
          "    if (CubeMesh.Succeeded()) PlayerVisual->SetStaticMesh(CubeMesh.Object);\n"
          "    PlayerVisual->SetRelativeLocation(FVector(0.0f, 0.0f, 50.0f));\n"
          "    PlayerVisual->SetRelativeScale3D(FVector(0.8f, 0.8f, 1.0f));\n"
          "    USpringArmComponent* CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT(\"CameraBoom\"));\n"
          "    CameraBoom->SetupAttachment(RootComponent);\n"
        + camera
        + "    UCameraComponent* FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT(\"FollowCamera\"));\n"
          "    FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);\n}\n\n"
        "void A" + name + "Character::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)\n{\n"
        "    Super::SetupPlayerInputComponent(PlayerInputComponent);\n"
        "    PlayerInputComponent->BindAxis(\"MoveForward\", this, &A" + name + "Character::MoveForward);\n"
        "    PlayerInputComponent->BindAxis(\"MoveRight\", this, &A" + name + "Character::MoveRight);\n}\n\n"
        "void A" + name + "Character::MoveForward(float Value)\n{\n"
        "    if (Controller && Value != 0.0f) AddMovementInput(FVector::ForwardVector, Value);\n}\n\n"
        "void A" + name + "Character::MoveRight(float Value)\n{\n"
        "    if (Controller && Value != 0.0f) AddMovementInput(FVector::RightVector, Value);\n}\n";
}

static std::vector<std::pair<std::filesystem::path, std::string>> engineProjectFiles(
    const std::string& engine, const std::string& dimension, const std::string& projectName,
    const std::string& unityVersion) {
    std::vector<std::pair<std::filesystem::path, std::string>> files;
    if (engine == "godot") {
        const bool is2D = dimension == "2d";
        const std::string nodeType = is2D ? "Node2D" : "Node3D";
        files.emplace_back("project.godot",
            "[application]\nconfig/name=\"Chia " + projectName + "\"\nrun/main_scene=\"res://Main.tscn\"\n"
            "config/features=PackedStringArray(\"4.0\", \"" + (is2D ? "GL Compatibility" : "GL Compatibility") + "\")\n\n"
            "[rendering]\nrenderer/rendering_method=\"gl_compatibility\"\n"
            "renderer/rendering_method.mobile=\"gl_compatibility\"\n");
        files.emplace_back("Main.tscn", "[gd_scene load_steps=2 format=3]\n\n"
            "[ext_resource type=\"Script\" path=\"res://Game.gd\" id=\"1_game\"]\n\n"
            "[node name=\"Main\" type=\"" + nodeType + "\"]\nscript = ExtResource(\"1_game\")\n");
        files.emplace_back("Game.gd", is2D ? godot2DScript() : godot3DScript());
        files.emplace_back("export_presets.cfg",
            "[preset.0]\nname=\"Windows Desktop\"\nplatform=\"Windows Desktop\"\nrunnable=true\n"
            "dedicated_server=false\ncustom_features=\"\"\nexport_filter=\"all_resources\"\n"
            "include_filter=\"\"\nexclude_filter=\"\"\nexport_path=\"build/ChiaGame.exe\"\n"
            "encryption_include_filters=\"\"\nencryption_exclude_filters=\"\"\n"
            "encrypt_pck=false\nencrypt_directory=false\nscript_export_mode=2\n\n"
            "[preset.0.options]\ncustom_template/debug=\"\"\ncustom_template/release=\"\"\n"
            "debug/export_console_wrapper= true\n");
        return files;
    }
    if (engine == "unity") {
        if (!unityVersion.empty())
            files.emplace_back("ProjectSettings/ProjectVersion.txt",
                "m_EditorVersion: " + unityVersion + "\n");
        files.emplace_back("Assets/Scripts/ChiaGameController.cs", unityController(dimension == "2d"));
        files.emplace_back("Assets/Editor/ChiaBootstrap.cs", unityBootstrap(dimension == "2d"));
        files.emplace_back("Assets/Editor/ChiaBuild.cs", unityBuildScript());
        files.emplace_back("Packages/manifest.json",
            "{\n  \"dependencies\": {\n    \"com.unity.modules.audio\": \"1.0.0\",\n"
            "    \"com.unity.modules.physics\": \"1.0.0\",\n"
            "    \"com.unity.modules.ui\": \"1.0.0\"\n  }\n}\n");
        return files;
    }
    if (engine == "unreal") {
        const std::string gameType = dimension == "2d" ? "2D top-down" : "3D";
        files.emplace_back(projectName + ".uproject",
            "{\n  \"FileVersion\": 3,\n  \"EngineAssociation\": \"\",\n"
            "  \"Category\": \"Games\",\n  \"Description\": \"Chia " + gameType + " game\",\n"
            "  \"Modules\": [{ \"Name\": \"" + projectName
            + "\", \"Type\": \"Runtime\", \"LoadingPhase\": \"Default\" }]\n}\n");
        files.emplace_back("Source/" + projectName + ".Target.cs",
            "using UnrealBuildTool;\nusing System.Collections.Generic;\n\npublic class " + projectName
            + "Target : TargetRules\n{\n    public " + projectName + "Target(TargetInfo Target) : base(Target)\n"
              "    {\n        Type = TargetType.Game;\n        DefaultBuildSettings = BuildSettingsVersion.V2;\n"
              "        ExtraModuleNames.Add(\"" + projectName + "\");\n    }\n}\n");
        files.emplace_back("Source/" + projectName + "Editor.Target.cs",
            "using UnrealBuildTool;\nusing System.Collections.Generic;\n\npublic class " + projectName
            + "EditorTarget : TargetRules\n{\n    public " + projectName
            + "EditorTarget(TargetInfo Target) : base(Target)\n    {\n        Type = TargetType.Editor;\n"
              "        DefaultBuildSettings = BuildSettingsVersion.V2;\n"
              "        ExtraModuleNames.Add(\"" + projectName + "\");\n    }\n}\n");
        files.emplace_back("Source/" + projectName + "/" + projectName + ".Build.cs",
            "using UnrealBuildTool;\n\npublic class " + projectName
            + " : ModuleRules\n{\n    public " + projectName
            + "(ReadOnlyTargetRules Target) : base(Target)\n    {\n        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;\n"
              "        PublicDependencyModuleNames.AddRange(new string[] { \"Core\", \"CoreUObject\", \"Engine\", \"InputCore\" });\n"
              "    }\n}\n");
        files.emplace_back("Source/" + projectName + "/Public/" + projectName + "GameMode.h",
            unrealGameModeHeader(projectName));
        files.emplace_back("Source/" + projectName + "/Private/" + projectName + "GameMode.cpp",
            unrealGameModeSource(projectName));
        files.emplace_back("Source/" + projectName + "/Public/" + projectName + "Character.h",
            unrealCharacterHeader(projectName));
        files.emplace_back("Source/" + projectName + "/Private/" + projectName + "Character.cpp",
            unrealCharacterSource(projectName, dimension == "2d"));
        files.emplace_back("Source/" + projectName + "/Private/" + projectName + ".cpp",
            "#include \"Modules/ModuleManager.h\"\nIMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, "
            + projectName + ", \"" + projectName + "\");\n");
        files.emplace_back("Config/DefaultInput.ini",
            "[/Script/Engine.InputSettings]\n"
            "+AxisMappings=(AxisName=\"MoveForward\",Scale=1.000000,Key=W)\n"
            "+AxisMappings=(AxisName=\"MoveForward\",Scale=-1.000000,Key=S)\n"
            "+AxisMappings=(AxisName=\"MoveRight\",Scale=1.000000,Key=D)\n"
            "+AxisMappings=(AxisName=\"MoveRight\",Scale=-1.000000,Key=A)\n");
        files.emplace_back("Config/DefaultEngine.ini",
            "[/Script/EngineSettings.GameMapsSettings]\nGlobalDefaultGameMode=/Script/"
            + projectName + "." + projectName + "GameMode\n");
        return files;
    }
    if (engine == "raylib") {
        files.emplace_back("Game.chia", dimension == "2d" ? starter2DGame : starter3DGame);
        files.emplace_back("CMakeLists.txt",
            "cmake_minimum_required(VERSION 3.16)\nproject(ChiaGame LANGUAGES CXX)\n"
            "find_package(raylib CONFIG REQUIRED)\nfind_program(CHIA_COMPILER NAMES chia chia.exe)\n"
            "if(NOT CHIA_COMPILER)\n    message(FATAL_ERROR \"Chia compiler not found. Add chia.exe to PATH.\")\nendif()\n"
            "set(CHIA_GENERATED_CPP \"${CMAKE_CURRENT_BINARY_DIR}/Game.cpp\")\n"
            "add_custom_command(OUTPUT \"${CHIA_GENERATED_CPP}\"\n"
            "    COMMAND \"${CHIA_COMPILER}\" translate \"${CMAKE_CURRENT_SOURCE_DIR}/Game.chia\" \"${CHIA_GENERATED_CPP}\"\n"
            "    DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/Game.chia\"\n    VERBATIM)\n"
            "add_executable(ChiaGame \"${CHIA_GENERATED_CPP}\")\n"
            "target_compile_features(ChiaGame PRIVATE cxx_std_17)\n"
            "if(MSVC)\n    target_compile_options(ChiaGame PRIVATE /utf-8)\nendif()\n"
            "target_link_libraries(ChiaGame PRIVATE raylib)\n");
        return files;
    }
    return files;
}

static std::string engineReadme(const std::string& engine, const std::string& dimension) {
    return "# Chia " + dimension + "D game project\n\n"
        "Engine: " + engine + "\n\n"
        "Open this folder with the installed engine using `chia game open .`.\n"
        "Build with `chia game build .`. Chia detects the project from its engine files.\n"
        "The project contains a small playable starter; use the engine's normal editor for scenes, assets and content.\n";
}

static bool createProjectFiles(const std::filesystem::path& directory,
                               const std::vector<std::pair<std::filesystem::path, std::string>>& files);
static std::vector<std::wstring> openArguments(const EngineInstallation& installation,
                                              const std::filesystem::path& project);

static int createEngineProject(const std::string& engine, const std::string& dimension,
                               const std::filesystem::path& requestedPath) {
    if (engine != "unity" && engine != "unreal" && engine != "godot" && engine != "raylib") {
        std::cerr << "Chia: движок должен быть raylib, unity, unreal или godot.\n";
        return 2;
    }
    if (dimension != "2d" && dimension != "3d") {
        std::cerr << "Chia: укажите размерность игры 2d или 3d.\n";
        return 2;
    }
    const std::filesystem::path directory = std::filesystem::absolute(requestedPath).lexically_normal();
    if (std::filesystem::exists(directory)) {
        std::cerr << "Chia: папка проекта уже существует, чтобы не перезаписать файлы: "
                  << directory.string() << '\n';
        return 1;
    }
    std::string projectName = projectIdentifier(directory);
    std::string unityVersion;
    if (const auto installation = findEngine(engine)) {
        if (engine == "unity" && !installation->root.empty()) {
            const std::string detectedVersion = installation->root.filename().string();
            if (detectedVersion.size() >= 4 && std::isdigit(static_cast<unsigned char>(detectedVersion[0]))
                && detectedVersion.find('.') != std::string::npos)
                unityVersion = detectedVersion;
        }
    }
    auto files = engineProjectFiles(engine, dimension, projectName, unityVersion);
    files.emplace_back("README.md", engineReadme(engine, dimension));
    files.emplace_back(".gitignore",
        "build/\nLibrary/\nTemp/\nObj/\nLogs/\n.vs/\nBinaries/\nIntermediate/\nSaved/\n.export/\n");
    if (files.empty() || !createProjectFiles(directory, files)) return 1;
    std::cout << "Создан проект " << engine << " (" << dimension << "): "
              << directory.string() << '\n';
    if (const auto installation = findEngine(engine)) {
        if (engine != "raylib") {
            std::cout << "Найден движок: " << installation->editor.string() << "\nОткрываю редактор...\n";
            const int opened = launchProcess(installation->editor, openArguments(*installation, directory), false);
            if (opened != 0) std::cerr << "Chia: проект создан, но открыть редактор автоматически не удалось.\n";
        } else {
            std::cout << "Найден CMake: " << installation->editor.string() << '\n';
            std::cout << "Для запуска: chia game build \"" << directory.string() << "\"\n";
        }
    } else {
        if (engine == "raylib") {
            std::cout << "CMake не найден. Проект создан; установите CMake и настройте Raylib, затем выполните chia game build \""
                      << directory.string() << "\".\n";
        } else {
            std::cout << "Редактор не найден. Проект создан; настройте путь к " << engine
                      << " и выполните chia game open \"" << directory.string() << "\".\n";
        }
    }
    return 0;
}

static bool createProjectFiles(const std::filesystem::path& directory,
                               const std::vector<std::pair<std::filesystem::path, std::string>>& files) {
    std::error_code error;
    if (!std::filesystem::create_directories(directory, error) || error) {
        std::cerr << "Chia: не удалось создать папку проекта: "
                  << (error ? error.message() : directory.string()) << '\n';
        return false;
    }
    for (const auto& file : files) {
        const std::filesystem::path parent = (directory / file.first).parent_path();
        if (!std::filesystem::create_directories(parent, error) && error) {
            std::cerr << "Chia: не удалось создать папку проекта " << parent.string()
                      << ": " << error.message() << '\n';
            return false;
        }
        if (!writeNewFile(directory / file.first, file.second)) {
            std::cerr << "Chia: проект создан не полностью; уже записанные файлы сохранены.\n";
            return false;
        }
    }
    return true;
}

static std::string javaDatabaseSource(const bool oracle) {
    std::string source =
        "package app;\n\n"
        "import java.sql.Connection;\n"
        "import java.sql.DriverManager;\n"
        "import java.sql.PreparedStatement;\n"
        "import java.sql.SQLException;\n\n"
        "public final class DatabaseApp {\n"
        "    private DatabaseApp() {}\n\n"
        "    public static void main(String[] args) throws SQLException {\n";
    if (oracle) {
        source +=
            "        String url = System.getenv().getOrDefault(\"ORACLE_JDBC_URL\", "
            "\"jdbc:oracle:thin:@//localhost:1521/FREEPDB1\");\n"
            "        String user = System.getenv(\"ORACLE_USER\");\n"
            "        String password = System.getenv(\"ORACLE_PASSWORD\");\n"
            "        if (user == null || password == null) {\n"
            "            throw new IllegalStateException(\"Set ORACLE_USER and ORACLE_PASSWORD.\");\n"
            "        }\n"
            "        try (Connection connection = DriverManager.getConnection(url, user, password)) {\n"
            "            try (PreparedStatement statement = connection.prepareStatement("
            "\"CREATE TABLE CHIA_NOTES (ID NUMBER PRIMARY KEY, BODY VARCHAR2(4000))\")) {\n"
            "                try { statement.execute(); }\n"
            "                catch (SQLException error) { if (error.getErrorCode() != 955) throw error; }\n"
            "            }\n"
            "            System.out.println(\"Oracle database connected. Credentials are read from environment.\");\n";
    } else {
        source +=
            "        String url = \"jdbc:h2:file:./data/chia;AUTO_SERVER=TRUE\";\n"
            "        try (Connection connection = DriverManager.getConnection(url)) {\n"
            "            try (PreparedStatement statement = connection.prepareStatement("
            "\"CREATE TABLE IF NOT EXISTS CHIA_NOTES (ID INTEGER PRIMARY KEY, BODY VARCHAR(4000))\")) {\n"
            "                statement.execute();\n"
            "            }\n"
            "            System.out.println(\"Local H2 database connected. Data is stored under ./data.\");\n";
    }
    source += "        }\n    }\n}\n";
    return source;
}

static ProjectFiles platformProjectFiles(const std::string& kind, const std::string& name,
                                        const std::string& target) {
    ProjectFiles files;
    if (kind == "kernel") {
        files.emplace_back("boot.S", R"CHIA(.section .multiboot
.align 4
.long 0x1BADB002
.long 0x00000003
.long -(0x1BADB002 + 0x00000003)

.section .text
.global _start
.type _start, @function
_start:
    cli
    call kernel_main
1:
    hlt
    jmp 1b
)CHIA");
        files.emplace_back("kernel.c", R"CHIA(void kernel_main(void) {
    volatile unsigned short* const video = (unsigned short*)0xB8000;
    const char message[] = "Chia kernel started. This is a freestanding boot scaffold.";
    for (unsigned int i = 0; message[i] != '\0'; ++i) {
        video[i] = (unsigned short)(0x0F00 | (unsigned char)message[i]);
    }
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
)CHIA");
        files.emplace_back("linker.ld", R"CHIA(ENTRY(_start)
SECTIONS {
    . = 1M;
    .multiboot : { KEEP(*(.multiboot)) }
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(COMMON) *(.bss*) }
}
)CHIA");
        files.emplace_back("Makefile", R"CHIA(CROSS ?= i686-elf-
CC := $(CROSS)gcc
AS := $(CROSS)gcc
CFLAGS := -std=gnu11 -ffreestanding -fno-stack-protector -fno-pie -m32 -O2 -Wall -Wextra
LDFLAGS := -T linker.ld -ffreestanding -nostdlib -no-pie -m32

all: kernel.elf

kernel.elf: boot.o kernel.o linker.ld
	$(CC) $(LDFLAGS) boot.o kernel.o -lgcc -o $@

boot.o: boot.S
	$(AS) $(CFLAGS) -c $< -o $@

kernel.o: kernel.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f boot.o kernel.o kernel.elf

.PHONY: all clean
)CHIA");
        files.emplace_back("README.md",
            "# Chia freestanding kernel scaffold\n\n"
            "This is a minimal 32-bit Multiboot kernel entry, not a complete operating system. "
            "It writes to VGA text memory and halts the CPU.\n\n"
            "Requirements: an i686-elf GCC cross-toolchain and a Multiboot-capable emulator/bootloader. "
            "Build with `make`; boot `kernel.elf` with QEMU/GRUB. "
            "Add memory management, interrupts, drivers, scheduling and storage yourself.\n");
        return files;
    }
    if (kind == "desktop") {
        files.emplace_back("CMakeLists.txt",
            "cmake_minimum_required(VERSION 3.16)\nproject(" + name + " LANGUAGES CXX)\n"
            "add_executable(" + name + " src/main.cpp)\n"
            "target_compile_features(" + name + " PRIVATE cxx_std_17)\n"
            "if(APPLE)\n  set_target_properties(" + name
            + " PROPERTIES MACOSX_BUNDLE TRUE)\nendif()\n"
            "install(TARGETS " + name + " RUNTIME DESTINATION bin BUNDLE DESTINATION .)\n");
        files.emplace_back("src/main.cpp",
            "#include <iostream>\n\nint main() {\n"
            "    std::cout << \"Hello from " + name + " on " + target + "!\\n\";\n"
            "    return 0;\n}\n");
        files.emplace_back("README.md",
            "# " + name + " desktop application\n\nTarget: " + target + ".\n\n"
            "Build with CMake and the native toolchain for the target OS. "
            "A build made on one operating system is not automatically a binary for the others; "
            "use a native or configured cross compiler for each target.\n");
        return files;
    }
    if (kind == "mobile" && target == "android") {
        files.emplace_back("settings.gradle",
            "pluginManagement { repositories { google(); mavenCentral(); gradlePluginPortal() } }\n"
            "dependencyResolutionManagement { repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS); "
            "repositories { google(); mavenCentral() } }\n"
            "rootProject.name = '" + name + "'\ninclude ':app'\n");
        files.emplace_back("build.gradle",
            "plugins {\n    id 'com.android.application' version '8.7.3' apply false\n}\n");
        files.emplace_back("app/build.gradle",
            "plugins { id 'com.android.application' }\n\n"
            "android {\n    namespace 'com.chia." + name + "'\n"
            "    compileSdk 35\n    defaultConfig {\n        applicationId 'com.chia." + name + "'\n"
            "        minSdk 23\n        targetSdk 35\n        versionCode 1\n        versionName '1.0'\n"
            "    }\n}\n");
        files.emplace_back("app/src/main/AndroidManifest.xml",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\">\n"
            "  <application android:theme=\"@android:style/Theme.Material.Light.NoActionBar\" "
            "android:label=\"" + name + "\" android:usesCleartextTraffic=\"false\">\n"
            "    <activity android:name=\".MainActivity\" android:exported=\"true\">\n"
            "      <intent-filter><action android:name=\"android.intent.action.MAIN\"/>"
            "<category android:name=\"android.intent.category.LAUNCHER\"/></intent-filter>\n"
            "    </activity>\n  </application>\n</manifest>\n");
        files.emplace_back("app/src/main/java/com/chia/" + name + "/MainActivity.java",
            "package com.chia." + name + ";\n\n"
            "import android.app.Activity;\nimport android.os.Bundle;\n"
            "import android.view.Gravity;\nimport android.widget.TextView;\n\n"
            "public final class MainActivity extends Activity {\n"
            "    @Override protected void onCreate(Bundle state) {\n"
            "        super.onCreate(state);\n"
            "        TextView label = new TextView(this);\n"
            "        label.setText(\"Hello from " + name + "!\");\n"
            "        label.setTextSize(24);\n"
            "        label.setGravity(Gravity.CENTER);\n"
            "        setContentView(label);\n"
            "    }\n}\n");
        files.emplace_back("README.md",
            "# " + name + " Android app\n\n"
            "Requires Android Studio/SDK, JDK 17 and Gradle. Open this folder in Android Studio "
            "or run `gradle :app:assembleDebug`. Chia does not bundle an Android SDK or Gradle wrapper.\n");
        return files;
    }
    if (kind == "mobile" && target == "ios") {
        files.emplace_back("project.yml",
            "name: " + name + "\noptions:\n  bundleIdPrefix: com.chia\n"
            "settings:\n  base:\n    SWIFT_VERSION: 5.0\n"
            "targets:\n  " + name + ":\n    type: application\n    platform: iOS\n"
            "    deploymentTarget: \"16.0\"\n    sources: [Sources]\n"
            "    settings:\n      base:\n        PRODUCT_BUNDLE_IDENTIFIER: com.chia." + name + "\n");
        files.emplace_back("Sources/" + name + "App.swift",
            "import SwiftUI\n\n@main\nstruct " + name + "App: App {\n"
            "    var body: some Scene {\n        WindowGroup { ContentView() }\n    }\n}\n");
        files.emplace_back("Sources/ContentView.swift",
            "import SwiftUI\n\nstruct ContentView: View {\n"
            "    var body: some View {\n        VStack(spacing: 16) {\n"
            "            Image(systemName: \"sparkles\").font(.largeTitle)\n"
            "            Text(\"Hello from " + name + "!\").font(.title)\n"
            "        }.padding()\n    }\n}\n");
        files.emplace_back("README.md",
            "# " + name + " iOS app\n\nRequires macOS, Xcode and XcodeGen. "
            "Run `xcodegen generate`, then open the generated `.xcodeproj` in Xcode. "
            "Building/signing iOS applications requires Apple tooling and signing credentials.\n");
        return files;
    }
    if (kind == "http") {
        files.emplace_back("pom.xml",
            "<project xmlns=\"http://maven.apache.org/POM/4.0.0\" "
            "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
            "xsi:schemaLocation=\"http://maven.apache.org/POM/4.0.0 "
            "https://maven.apache.org/xsd/maven-4.0.0.xsd\">\n"
            "  <modelVersion>4.0.0</modelVersion>\n  <groupId>com.chia</groupId>\n"
            "  <artifactId>" + name + "</artifactId>\n  <version>1.0.0</version>\n"
            "  <properties><maven.compiler.release>17</maven.compiler.release>"
            "<project.build.sourceEncoding>UTF-8</project.build.sourceEncoding></properties>\n"
            "  <build><plugins><plugin><groupId>org.apache.maven.plugins</groupId>"
            "<artifactId>maven-compiler-plugin</artifactId><version>3.13.0</version>"
            "<configuration><compilerArgs><arg>--add-modules</arg><arg>jdk.httpserver</arg>"
            "</compilerArgs></configuration></plugin></plugins></build>\n</project>\n");
        files.emplace_back("src/main/java/app/App.java", R"CHIA(package app;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.Executors;

public final class App {
    private App() {}

    public static void main(String[] args) throws IOException {
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 8080), 0);
        server.createContext("/", App::handle);
        server.setExecutor(Executors.newFixedThreadPool(4));
        Runtime.getRuntime().addShutdownHook(new Thread(() -> server.stop(1)));
        server.start();
        System.out.println("Chia HttpExchange server listening on http://127.0.0.1:8080");
    }

    private static void handle(HttpExchange exchange) throws IOException {
        byte[] body = "Hello from Chia HttpExchange!\n".getBytes(StandardCharsets.UTF_8);
        exchange.getResponseHeaders().set("Content-Type", "text/plain; charset=utf-8");
        exchange.getResponseHeaders().set("X-Content-Type-Options", "nosniff");
        if (!"GET".equals(exchange.getRequestMethod())) {
            exchange.getResponseHeaders().set("Allow", "GET");
            exchange.sendResponseHeaders(405, -1);
        } else {
            exchange.sendResponseHeaders(200, body.length);
            try (var output = exchange.getResponseBody()) {
                output.write(body);
            }
        }
        exchange.close();
    }
}
)CHIA");
        files.emplace_back("README.md",
            "# " + name + " HttpExchange server\n\nRequires JDK 17 and Maven. "
            "Build with `mvn package`; run with "
            "`java --add-modules jdk.httpserver -cp target/classes app.App`. "
            "The sample binds to localhost only. Add authentication, TLS/reverse proxy, "
            "input limits and production request handling before deployment.\n");
        return files;
    }
    if (kind == "java-database" || kind == "oracle") {
        const bool oracle = kind == "oracle";
        const std::string dependency = oracle
            ? "    <dependency><groupId>com.oracle.database.jdbc</groupId><artifactId>ojdbc11</artifactId>"
              "<version>23.6.0.24.10</version></dependency>\n"
            : "    <dependency><groupId>com.h2database</groupId><artifactId>h2</artifactId>"
              "<version>2.3.232</version></dependency>\n";
        files.emplace_back("pom.xml",
            "<project xmlns=\"http://maven.apache.org/POM/4.0.0\" "
            "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
            "xsi:schemaLocation=\"http://maven.apache.org/POM/4.0.0 "
            "https://maven.apache.org/xsd/maven-4.0.0.xsd\">\n"
            "  <modelVersion>4.0.0</modelVersion>\n  <groupId>com.chia</groupId>\n"
            "  <artifactId>" + name + "</artifactId>\n  <version>1.0.0</version>\n"
            "  <properties><maven.compiler.release>17</maven.compiler.release>"
            "<project.build.sourceEncoding>UTF-8</project.build.sourceEncoding></properties>\n"
            "  <dependencies>\n" + dependency + "  </dependencies>\n"
            "  <build><plugins><plugin><groupId>org.apache.maven.plugins</groupId>"
            "<artifactId>maven-compiler-plugin</artifactId><version>3.13.0</version></plugin>"
            "<plugin><groupId>org.codehaus.mojo</groupId><artifactId>exec-maven-plugin</artifactId>"
            "<version>3.5.0</version></plugin></plugins></build>\n"
            "</project>\n");
        files.emplace_back("src/main/java/app/DatabaseApp.java", javaDatabaseSource(oracle));
        files.emplace_back(".gitignore", "target/\ndata/\n.env\n");
        files.emplace_back("README.md", std::string("# ") + name + (oracle ? " Oracle JDBC" : " Java JDBC/H2")
            + " starter\n\nRequires JDK 17 and Maven. Build with `mvn package`; run with "
            "`mvn exec:java -Dexec.mainClass=app.DatabaseApp`, "
            "or run `java -cp target/classes:<jdbc-driver> app.DatabaseApp`.\n\n"
            + (oracle
                ? "Install/use an Oracle database and set ORACLE_JDBC_URL, ORACLE_USER and "
                  "ORACLE_PASSWORD in the environment. The Oracle JDBC driver is fetched by Maven. "
                  "Never commit database credentials.\n"
                : "Uses an embedded H2 file database under ./data; Maven fetches the JDBC driver.\n"));
        return files;
    }
    return files;
}

static std::string createPlatformProject(const std::string& kind, const std::string& name,
                                         const std::filesystem::path& requestedDirectory,
                                         const std::string& target) {
    const std::filesystem::path directory = std::filesystem::absolute(requestedDirectory).lexically_normal();
    if (std::filesystem::exists(directory))
        throw std::runtime_error("папка проекта уже существует; Chia не перезаписывает её: " + directory.string());
    const bool validKind = kind == "kernel" || kind == "desktop" || kind == "mobile"
        || kind == "http" || kind == "java-database" || kind == "oracle";
    if (!validKind) throw std::runtime_error("неизвестный тип проекта Chia: " + kind);
    if (kind == "desktop" && target != "windows" && target != "macos" && target != "linux")
        throw std::runtime_error("для desktop укажите windows, macos или linux");
    if (kind == "mobile" && target != "android" && target != "ios")
        throw std::runtime_error("для mobile укажите android или ios");
    const std::string projectName = projectIdentifier(std::filesystem::path(name));
    ProjectFiles files = platformProjectFiles(kind, projectName, target);
    if (files.empty()) throw std::runtime_error("не удалось создать файлы проекта для " + kind);
    if (!createProjectFiles(directory, files))
        throw std::runtime_error("не удалось записать проект в " + directory.string());
    std::cout << "Создан проект Chia (" << kind;
    if (!target.empty()) std::cout << "/" << target;
    std::cout << "): " << directory.string() << '\n';
    return directory.string();
}

static bool runGitCommand(const std::string& operation, const std::filesystem::path& repository,
                          const std::string& message) {
    const auto git = findOnPath("git.exe");
    if (!git) throw std::runtime_error("Git не найден в PATH; установите Git и повторите команду");
    const std::filesystem::path path = std::filesystem::absolute(repository).lexically_normal();
    std::vector<std::wstring> arguments{L"-C", path.wstring()};
    if (operation == "init") {
        arguments.push_back(L"init");
    } else if (operation == "status") {
        arguments.push_back(L"status");
    } else if (operation == "add") {
        arguments.push_back(L"add");
        arguments.push_back(L"--all");
    } else if (operation == "commit") {
        if (trim(message).empty()) throw std::runtime_error("сообщение коммита не может быть пустым");
        arguments.push_back(L"commit");
        arguments.push_back(L"-m");
        arguments.push_back(std::filesystem::path(message).wstring());
    } else {
        throw std::runtime_error("неизвестная операция Git: " + operation);
    }
    const int exitCode = launchProcess(*git, arguments, true);
    if (exitCode != 0)
        throw std::runtime_error("Git " + operation + " завершился с кодом " + std::to_string(exitCode));
    return true;
}

static std::optional<std::string> projectEngine(const std::filesystem::path& directory) {
    if (std::filesystem::is_regular_file(directory / "project.godot")) return "godot";
    if (std::filesystem::is_directory(directory))
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            if (entry.is_regular_file() && entry.path().extension() == ".uproject") return "unreal";
    if (std::filesystem::is_regular_file(directory / "ProjectSettings/ProjectVersion.txt")
        || (std::filesystem::is_directory(directory / "Assets")
            && std::filesystem::is_directory(directory / "Packages"))) return "unity";
    if (std::filesystem::is_regular_file(directory / "CMakeLists.txt")) return "raylib";
    return std::nullopt;
}

static std::filesystem::path unrealDescriptor(const std::filesystem::path& project) {
    for (const auto& entry : std::filesystem::directory_iterator(project))
        if (entry.is_regular_file() && entry.path().extension() == ".uproject") return entry.path();
    return {};
}

static std::vector<std::wstring> openArguments(const EngineInstallation& installation,
                                              const std::filesystem::path& project) {
    if (installation.name == "unity") return {L"-projectPath", project.wstring()};
    if (installation.name == "godot") return {L"--editor", L"--path", project.wstring()};
    if (installation.name == "unreal") {
        return {unrealDescriptor(project).wstring()};
    }
    return {};
}

static int openGameProject(const std::filesystem::path& requestedPath) {
    const std::filesystem::path project = std::filesystem::absolute(requestedPath).lexically_normal();
    const auto engineName = projectEngine(project);
    if (!engineName) {
        std::cerr << "Chia: папка не распознана как проект Chia/Raylib, Unity, Unreal или Godot.\n";
        return 1;
    }
    const auto installation = findEngine(*engineName);
    if (!installation) {
        std::cerr << "Chia: движок " << *engineName << " не найден. Настройте PATH или переменную пути для движка.\n";
        return 1;
    }
    if (*engineName == "raylib") {
        std::filesystem::path built = project / "build/Release/ChiaGame.exe";
        if (!std::filesystem::is_regular_file(built)) built = project / "build/ChiaGame.exe";
        if (std::filesystem::is_regular_file(built)) return launchProcess(built, {}, false);
        std::cerr << "Chia: сначала соберите Raylib-проект командой chia game build \""
                  << project.string() << "\".\n";
        return 2;
    }
    std::cout << "Открываю проект в " << *engineName << ": " << project.string() << '\n';
    return launchProcess(installation->editor, openArguments(*installation, project), false);
}

static int buildGameProject(const std::filesystem::path& requestedPath) {
    const std::filesystem::path project = std::filesystem::absolute(requestedPath).lexically_normal();
    const auto engineName = projectEngine(project);
    if (!engineName) {
        std::cerr << "Chia: папка не распознана как проект Chia/Raylib, Unity, Unreal или Godot.\n";
        return 1;
    }
    const auto installation = findEngine(*engineName);
    if (!installation) {
        std::cerr << "Chia: движок " << *engineName << " не найден. Настройте переменную пути для движка.\n";
        return 1;
    }
    if (*engineName == "unity") {
        return launchProcess(installation->editor,
            {L"-batchmode", L"-quit", L"-projectPath", project.wstring(),
             L"-executeMethod", L"ChiaBuild.PerformBuild", L"-logFile", L"-"}, true);
    }
    if (*engineName == "godot") {
        std::error_code error;
        std::filesystem::create_directories(project / "build", error);
        if (error) {
            std::cerr << "Chia: не удалось создать папку сборки: " << error.message() << '\n';
            return 1;
        }
        return launchProcess(installation->editor,
            {L"--headless", L"--path", project.wstring(), L"--export-release",
             L"Windows Desktop", (project / L"build/ChiaGame.exe").wstring()}, true);
    }
    if (*engineName == "unreal") {
        const std::filesystem::path descriptor = unrealDescriptor(project);
        const std::wstring target = descriptor.stem().wstring();
        std::vector<std::wstring> arguments{target, L"Win64", L"Development",
            L"-Project=" + descriptor.wstring(), L"-WaitMutex"};
        std::filesystem::path buildTool = installation->buildTool;
        if (buildTool.empty() || !std::filesystem::is_regular_file(buildTool)) {
            std::cerr << "Chia: UnrealBuildTool не найден рядом с Unreal Engine; откройте .uproject в редакторе "
                         "и создайте файлы проекта C++.\n";
            return 1;
        }
        if (buildTool.extension() == L".dll") {
            const auto dotnet = findOnPath("dotnet.exe");
            if (!dotnet) {
                std::cerr << "Chia: UnrealBuildTool — DLL; установите .NET SDK/Runtime или добавьте dotnet в PATH.\n";
                return 1;
            }
            arguments.insert(arguments.begin(), buildTool.wstring());
            buildTool = *dotnet;
        }
        return launchProcess(buildTool, arguments, true);
    }
    const auto cmake = installation->editor;
    const std::filesystem::path build = project / "build";
    std::vector<std::wstring> configureArgs{L"-S", project.wstring(), L"-B", build.wstring()};
    const std::string vcpkgRoot = environmentValue("VCPKG_ROOT");
    if (!vcpkgRoot.empty()) {
        const std::filesystem::path toolchain = std::filesystem::path(vcpkgRoot)
            / "scripts/buildsystems/vcpkg.cmake";
        if (std::filesystem::is_regular_file(toolchain))
            configureArgs.push_back(L"-DCMAKE_TOOLCHAIN_FILE=" + toolchain.wstring());
    }
    int result = launchProcess(cmake, configureArgs, true);
    if (result != 0) return result;
    return launchProcess(cmake, {L"--build", build.wstring(), L"--config", L"Release"}, true);
}

static int listGameEngines() {
    for (const std::string& engine : {"raylib", "unity", "unreal", "godot"}) {
        const auto installation = findEngine(engine);
        if (installation)
            std::cout << engine << ": найдено — " << installation->editor.string() << '\n';
        else
            std::cout << engine << ": не найдено (проект можно создать; задайте переменную пути или PATH)\n";
    }
    return 0;
}

static int createGameProject(const std::string& dimension, const std::filesystem::path& requestedPath) {
    if (dimension != "2d" && dimension != "3d") {
        std::cerr << "Chia: укажите тип игры 2d или 3d.\n";
        return 2;
    }
    const std::filesystem::path directory = std::filesystem::absolute(requestedPath).lexically_normal();
    if (std::filesystem::exists(directory)) {
        std::cerr << "Chia: папка проекта уже существует, чтобы не перезаписать файлы: "
                  << directory.string() << '\n';
        return 1;
    }
    const std::string script = dimension == "2d" ? starter2DGame : starter3DGame;
    try {
        CppTranslator translator(tokenize(script));
        translator.translate();
    } catch (const ChiaError& error) {
        std::cerr << "Chia: ошибка игрового шаблона: " << error.what() << '\n';
        return 1;
    }
    std::error_code error;
    if (!std::filesystem::create_directories(directory, error) || error) {
        std::cerr << "Chia: не удалось создать папку проекта: "
                  << (error ? error.message() : directory.string()) << '\n';
        return 1;
    }
    const std::string cmake =
        "cmake_minimum_required(VERSION 3.16)\n"
        "project(ChiaGame LANGUAGES CXX)\n"
        "find_package(raylib CONFIG REQUIRED)\n"
        "find_program(CHIA_COMPILER NAMES chia chia.exe)\n"
        "if(NOT CHIA_COMPILER)\n"
        "    message(FATAL_ERROR \"Chia compiler not found. Add chia.exe to PATH.\")\n"
        "endif()\n"
        "set(CHIA_GENERATED_CPP \"${CMAKE_CURRENT_BINARY_DIR}/Game.cpp\")\n"
        "add_custom_command(OUTPUT \"${CHIA_GENERATED_CPP}\"\n"
        "    COMMAND \"${CHIA_COMPILER}\" translate \"${CMAKE_CURRENT_SOURCE_DIR}/Game.chia\" \"${CHIA_GENERATED_CPP}\"\n"
        "    DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/Game.chia\"\n"
        "    VERBATIM)\n"
        "add_executable(ChiaGame \"${CHIA_GENERATED_CPP}\")\n"
        "target_compile_features(ChiaGame PRIVATE cxx_std_17)\n"
        "if(MSVC)\n"
        "    target_compile_options(ChiaGame PRIVATE /utf-8)\n"
        "endif()\n"
        "target_link_libraries(ChiaGame PRIVATE raylib)\n";
    if (!writeNewFile(directory / "Game.chia", script)
        || !writeNewFile(directory / "CMakeLists.txt", cmake)) {
        std::cerr << "Chia: игровой проект создан не полностью; проверьте права записи в папку.\n";
        return 1;
    }
    std::cout << "Создан игровой проект Chia + Raylib (" << dimension << "): "
              << directory.string() << '\n'
              << "Добавьте chia.exe и CMake в PATH; установите raylib через vcpkg.\n"
              << "Сборка: cmake -S \"" << directory.string() << "\" -B \""
              << (directory / "build").string() << "\" -DCMAKE_TOOLCHAIN_FILE=\"<путь-к-vcpkg>/scripts/buildsystems/vcpkg.cmake\"\n"
              << "       cmake --build \"" << (directory / "build").string() << "\"\n";
    return 0;
}

} // namespace chia

int main(int argc, char* argv[]) {
    using namespace chia;
    try {
        if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
            printHelp();
            return 0;
        }
        const std::string command = argv[1];
        if (command == "new") {
            if (argc > 3) {
                std::cerr << "Использование: chia new [файл.chia]\n";
                return 2;
            }
            return createFile(argc == 3 ? std::filesystem::path(argv[2]) : std::filesystem::path("Hello.chia"));
        }
        if (command == "game") {
            if (argc == 3 && std::string(argv[2]) == "engines") return listGameEngines();
            if (argc == 4 && (std::string(argv[2]) == "open" || std::string(argv[2]) == "build")) {
                return std::string(argv[2]) == "open"
                    ? openGameProject(argv[3])
                    : buildGameProject(argv[3]);
            }
            if (argc == 5 && std::string(argv[2]) == "new")
                return createGameProject(argv[3], argv[4]);
            if (argc == 6 && std::string(argv[2]) == "new")
                return createEngineProject(argv[3], argv[4], argv[5]);
            std::cerr << "Использование:\n"
                      << "  chia game engines\n"
                      << "  chia game new <raylib|unity|unreal|godot> <2d|3d> <папка>\n"
                      << "  chia game open <папка>\n"
                      << "  chia game build <папка>\n";
            return 2;
        }
        if (command == "run" || command == "check") {
            if (argc != 3) {
                std::cerr << "Использование: chia " << command << " <файл.chia>\n";
                return 2;
            }
            return runFile(argv[2], command == "check");
        }
        if (command == "translate" || command == "correct") {
            if (argc != 4) {
                std::cerr << "Использование: chia " << command << " <входной файл> <новый выходной файл>\n";
                return 2;
            }
            return command == "translate"
                ? translateFile(argv[2], argv[3])
                : correctSpelling(argv[2], argv[3]);
        }
        if (command == "spell") {
            if (argc != 3) {
                std::cerr << "Использование: chia spell <файл.chia>\n";
                return 2;
            }
            return checkSpelling(argv[2]);
        }
        if (argc == 2) return runFile(argv[1], false);
        printHelp();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Chia: ошибка: " << error.what() << '\n';
        return 1;
    }
}