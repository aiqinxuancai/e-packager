#include "SourceExpressionParser.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>

namespace e2txt {
namespace {

enum class TokenKind {
	End,
	Name,
	Number,
	Text,
	DateTime,
	Operator,
	LeftParen,
	RightParen,
	LeftBracket,
	RightBracket,
	LeftBrace,
	RightBrace,
	Comma,
	Dot,
};

struct Token {
	TokenKind kind = TokenKind::End;
	std::string text;
	std::size_t position = 0;
};

constexpr std::string_view kChineseLeftQuote = "“";
constexpr std::string_view kChineseRightQuote = "”";

bool StartsAt(const std::string& text, const std::size_t position, const std::string_view token)
{
	return position <= text.size() && text.compare(position, token.size(), token) == 0;
}

bool IsSpace(const char ch)
{
	return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

std::size_t NextCharacterIndex(const std::string& text, const std::size_t position)
{
	if (position >= text.size()) return text.size();
	const char* begin = text.c_str();
	const char* current = begin + position;
	const char* next = CharNextExA(CP_ACP, current, 0);
	if (next == nullptr || next <= current) return position + 1;
	return (std::min)(text.size(), static_cast<std::size_t>(next - begin));
}

bool IsWordOperatorAt(const std::string& text, const std::size_t position, const std::string_view token)
{
	if (!StartsAt(text, position, token)) return false;
	const bool leftBoundary = position == 0 || IsSpace(text[position - 1]) || text[position - 1] == '(' || text[position - 1] == ',';
	const std::size_t after = position + token.size();
	const bool rightBoundary = after == text.size() || IsSpace(text[after]) || text[after] == ')' || text[after] == ',';
	return leftBoundary && rightBoundary;
}

bool IsOperatorStart(const std::string& text, const std::size_t position)
{
	static constexpr std::string_view kOperators[] = {
		"＋", "－", "×", "÷", "＝", "≠", "＜", "＞", "≤", "≥",
		"?=", "==", "!=", "<=", ">=", "<>", "+", "-", "*", "/", "\\", "%", "=", "<", ">", "&", "|",
	};
	for (const auto operatorText : kOperators) {
		if (StartsAt(text, position, operatorText)) {
			return true;
		}
	}
	return IsWordOperatorAt(text, position, "且") || IsWordOperatorAt(text, position, "或");
}

bool IsNumberStart(const std::string& text, const std::size_t position)
{
	if (position >= text.size()) {
		return false;
	}
	if (std::isdigit(static_cast<unsigned char>(text[position])) != 0) {
		return true;
	}
	return text[position] == '.' && position + 1 < text.size() &&
		std::isdigit(static_cast<unsigned char>(text[position + 1])) != 0;
}

class Lexer {
public:
	explicit Lexer(const std::string& source) : source_(source) {}

	std::vector<Token> Lex(std::string& outError, std::size_t& outErrorOffset)
	{
		std::vector<Token> tokens;
		while (position_ < source_.size()) {
			if (IsSpace(source_[position_])) {
				++position_;
				continue;
			}
			const std::size_t tokenPosition = position_;
			if (StartsAt(source_, position_, kChineseLeftQuote) || source_[position_] == '"') {
				const bool chinese = StartsAt(source_, position_, kChineseLeftQuote);
				const std::string_view left = chinese ? kChineseLeftQuote : std::string_view("\"");
				const std::string_view right = chinese ? kChineseRightQuote : std::string_view("\"");
				position_ += left.size();
				bool closed = false;
				while (position_ < source_.size()) {
					if (StartsAt(source_, position_, right)) {
						position_ += right.size();
						closed = true;
						break;
					}
					position_ = NextCharacterIndex(source_, position_);
				}
				if (!closed) {
					outError = "text_quote_unclosed";
					outErrorOffset = tokenPosition;
					return {};
				}
				tokens.push_back({ TokenKind::Text, source_.substr(tokenPosition, position_ - tokenPosition), tokenPosition });
				continue;
			}
			if (IsNumberStart(source_, position_)) {
				const std::size_t begin = position_;
				bool sawDot = false;
				while (position_ < source_.size()) {
					const char ch = source_[position_];
					if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
						++position_;
						continue;
					}
					if (ch == '.' && !sawDot) {
						sawDot = true;
						++position_;
						continue;
					}
					break;
				}
				tokens.push_back({ TokenKind::Number, source_.substr(begin, position_ - begin), begin });
				continue;
			}
			const char ascii = source_[position_];
			if (ascii == '(') { ++position_; tokens.push_back({ TokenKind::LeftParen, "(", tokenPosition }); continue; }
			if (ascii == ')') { ++position_; tokens.push_back({ TokenKind::RightParen, ")", tokenPosition }); continue; }
			if (ascii == '[') {
				const std::size_t closing = source_.find(']', position_ + 1);
				if (closing != std::string::npos) {
					const std::string content = source_.substr(position_ + 1, closing - position_ - 1);
					if (content.find("年") != std::string::npos && content.find("月") != std::string::npos &&
						content.find("日") != std::string::npos) {
						position_ = closing + 1;
						tokens.push_back({ TokenKind::DateTime, source_.substr(tokenPosition, position_ - tokenPosition), tokenPosition });
						continue;
					}
				}
				++position_;
				tokens.push_back({ TokenKind::LeftBracket, "[", tokenPosition });
				continue;
			}
			if (ascii == ']') { ++position_; tokens.push_back({ TokenKind::RightBracket, "]", tokenPosition }); continue; }
			if (ascii == '{') { ++position_; tokens.push_back({ TokenKind::LeftBrace, "{", tokenPosition }); continue; }
			if (ascii == '}') { ++position_; tokens.push_back({ TokenKind::RightBrace, "}", tokenPosition }); continue; }
			if (ascii == ',') { ++position_; tokens.push_back({ TokenKind::Comma, ",", tokenPosition }); continue; }
			if (ascii == '.') { ++position_; tokens.push_back({ TokenKind::Dot, ".", tokenPosition }); continue; }
			if (IsOperatorStart(source_, position_)) {
				static constexpr std::string_view kOperators[] = {
					"＋", "－", "×", "÷", "＝", "≠", "＜", "＞", "≤", "≥",
					"?=", "==", "!=", "<=", ">=", "<>", "+", "-", "*", "/", "\\", "%", "=", "<", ">", "&", "|",
				};
				std::string_view matched;
				for (const auto operatorText : kOperators) {
					if (StartsAt(source_, position_, operatorText) && operatorText.size() > matched.size()) {
						matched = operatorText;
					}
				}
				if (matched.empty()) {
					if (IsWordOperatorAt(source_, position_, "且")) matched = "且";
					else if (IsWordOperatorAt(source_, position_, "或")) matched = "或";
				}
				position_ += matched.size();
				tokens.push_back({ TokenKind::Operator, std::string(matched), tokenPosition });
				continue;
			}
			const std::size_t begin = position_;
			while (position_ < source_.size()) {
				if (IsSpace(source_[position_]) || source_[position_] == '(' || source_[position_] == ')' ||
					source_[position_] == '[' || source_[position_] == ']' || source_[position_] == '{' ||
					source_[position_] == '}' || source_[position_] == ',' || source_[position_] == '.' ||
					IsOperatorStart(source_, position_)) {
					break;
				}
				position_ = NextCharacterIndex(source_, position_);
			}
			if (begin == position_) {
				outError = "unexpected_character";
				outErrorOffset = position_;
				return {};
			}
			tokens.push_back({ TokenKind::Name, source_.substr(begin, position_ - begin), begin });
		}
		tokens.push_back({ TokenKind::End, {}, source_.size() });
		return tokens;
	}

private:
	const std::string& source_;
	std::size_t position_ = 0;
};

std::unique_ptr<SourceExpressionNode> MakeNode(
	const SourceExpressionKind kind,
	std::string text = {})
{
	auto node = std::make_unique<SourceExpressionNode>();
	node->kind = kind;
	node->text = std::move(text);
	return node;
}

class Parser {
public:
	explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

	SourceExpressionParseResult Parse()
	{
		SourceExpressionParseResult result;
		if (Current().kind == TokenKind::End) {
			result.root = MakeNode(SourceExpressionKind::Missing);
			return result;
		}
		result.root = ParseExpression(0, result);
		if (!result.root) {
			return result;
		}
		if (Current().kind != TokenKind::End) {
			Fail(result, "unexpected_token", Current().position);
			result.root.reset();
		}
		return result;
	}

private:
	const Token& Current() const { return tokens_[index_]; }

	const Token& Consume()
	{
		const Token& token = tokens_[index_];
		if (index_ + 1 < tokens_.size()) {
			++index_;
		}
		return token;
	}

	bool Match(const TokenKind kind, const std::string_view text = {})
	{
		if (Current().kind != kind || (!text.empty() && Current().text != text)) {
			return false;
		}
		Consume();
		return true;
	}

	void Fail(SourceExpressionParseResult& result, const std::string_view error, const std::size_t position)
	{
		if (result.error.empty()) {
			result.error = std::string(error);
			result.errorOffset = position;
		}
	}

	std::unique_ptr<SourceExpressionNode> ParseExpression(
		const int minPrecedence,
		SourceExpressionParseResult& result)
	{
		auto left = ParseUnary(result);
		if (!left) {
			return nullptr;
		}
		for (;;) {
			if (Current().kind != TokenKind::Operator) {
				break;
			}
			const int precedence = BinaryPrecedence(Current().text);
			if (precedence < minPrecedence) {
				break;
			}
			const std::string op = Consume().text;
			auto right = ParseExpression(precedence + 1, result);
			if (!right) {
				return nullptr;
			}
			auto node = MakeNode(SourceExpressionKind::Binary, op);
			node->children.push_back(std::move(left));
			node->children.push_back(std::move(right));
			left = std::move(node);
		}
		return left;
	}

	std::unique_ptr<SourceExpressionNode> ParseUnary(SourceExpressionParseResult& result)
	{
		if (Current().kind == TokenKind::Operator &&
			(Current().text == "＋" || Current().text == "+" || Current().text == "－" || Current().text == "-" ||
				Current().text == "!" || Current().text == "&")) {
			const std::string op = Consume().text;
			auto child = ParseUnary(result);
			if (!child) {
				return nullptr;
			}
			auto node = MakeNode(op == "&" ? SourceExpressionKind::AddressOf : SourceExpressionKind::Unary, op);
			node->children.push_back(std::move(child));
			return node;
		}
		return ParsePostfix(result);
	}

	std::unique_ptr<SourceExpressionNode> ParsePostfix(SourceExpressionParseResult& result)
	{
		auto node = ParsePrimary(result);
		if (!node) {
			return nullptr;
		}
		for (;;) {
			if (Match(TokenKind::LeftParen)) {
				auto call = MakeNode(SourceExpressionKind::Call);
				call->children.push_back(std::move(node));
				if (!ParseArgumentList(*call, result, TokenKind::RightParen)) {
					return nullptr;
				}
				node = std::move(call);
				continue;
			}
			if (Match(TokenKind::LeftBracket)) {
				auto index = MakeNode(SourceExpressionKind::Index);
				index->children.push_back(std::move(node));
				if (Current().kind == TokenKind::RightBracket) {
					Fail(result, "index_expression_missing", Current().position);
					return nullptr;
				}
				for (;;) {
					auto child = ParseExpression(0, result);
					if (!child) return nullptr;
					index->children.push_back(std::move(child));
					if (!Match(TokenKind::Comma)) break;
					if (Current().kind == TokenKind::RightBracket) {
						Fail(result, "index_expression_missing", Current().position);
						return nullptr;
					}
				}
				if (!Match(TokenKind::RightBracket)) {
					Fail(result, "index_closing_bracket_missing", Current().position);
					return nullptr;
				}
				node = std::move(index);
				continue;
			}
			if (Match(TokenKind::Dot)) {
				if (Current().kind != TokenKind::Name) {
					Fail(result, "member_name_missing", Current().position);
					return nullptr;
				}
				std::string memberName = Consume().text;
				// 支持库回调名称可能含有连字符和数字，例如 `_Lib-4Cmd0`。
				// 仅在连字符后同时出现数字和名称片段时合并，避免把普通
				// `对象.成员 - 1` 误识别成成员名。
				if (Current().kind == TokenKind::Operator && Current().text == "-" &&
					index_ + 2 < tokens_.size() &&
					tokens_[index_ + 1].kind == TokenKind::Number &&
					tokens_[index_ + 2].kind == TokenKind::Name) {
					memberName += Consume().text;
					memberName += Consume().text;
					memberName += Consume().text;
				}
				auto member = MakeNode(SourceExpressionKind::Member, std::move(memberName));
				member->children.push_back(std::move(node));
				node = std::move(member);
				continue;
			}
			break;
		}
		return node;
	}

	bool ParseArgumentList(
		SourceExpressionNode& call,
		SourceExpressionParseResult& result,
		const TokenKind closing)
	{
		if (Match(closing)) {
			return true;
		}
		for (;;) {
			if (Current().kind == TokenKind::Comma) {
				call.children.push_back(MakeNode(SourceExpressionKind::Missing));
				Consume();
				continue;
			}
			else if (Current().kind == closing) {
				call.children.push_back(MakeNode(SourceExpressionKind::Missing));
			}
			else {
				auto argument = ParseExpression(0, result);
				if (!argument) return false;
				call.children.push_back(std::move(argument));
			}
			if (Match(closing)) return true;
			if (!Match(TokenKind::Comma)) {
				Fail(result, "call_argument_separator_missing", Current().position);
				return false;
			}
			if (Current().kind == closing) {
				call.children.push_back(MakeNode(SourceExpressionKind::Missing));
				Consume();
				return true;
			}
		}
	}

	std::unique_ptr<SourceExpressionNode> ParsePrimary(SourceExpressionParseResult& result)
	{
		if (Current().kind == TokenKind::Name) {
			const std::string name = Consume().text;
			if (name == "真" || name == "假") {
				return MakeNode(SourceExpressionKind::LogicalLiteral, name);
			}
			return MakeNode(SourceExpressionKind::Name, name);
		}
		if (Current().kind == TokenKind::Number) {
			return MakeNode(SourceExpressionKind::NumberLiteral, Consume().text);
		}
		if (Current().kind == TokenKind::Text) {
			return MakeNode(SourceExpressionKind::TextLiteral, Consume().text);
		}
		if (Current().kind == TokenKind::DateTime) {
			return MakeNode(SourceExpressionKind::DateTimeLiteral, Consume().text);
		}
		if (Match(TokenKind::LeftParen)) {
			auto child = ParseExpression(0, result);
			if (!child) return nullptr;
			if (!Match(TokenKind::RightParen)) {
				Fail(result, "closing_parenthesis_missing", Current().position);
				return nullptr;
			}
			auto group = MakeNode(SourceExpressionKind::Group);
			group->children.push_back(std::move(child));
			return group;
		}
		if (Match(TokenKind::LeftBrace)) {
			auto byteSet = MakeNode(SourceExpressionKind::ByteSetLiteral);
			if (Match(TokenKind::RightBrace)) return byteSet;
			for (;;) {
				auto item = ParseExpression(0, result);
				if (!item) return nullptr;
				byteSet->children.push_back(std::move(item));
				if (Match(TokenKind::RightBrace)) return byteSet;
				if (!Match(TokenKind::Comma)) {
					Fail(result, "byte_set_separator_missing", Current().position);
					return nullptr;
				}
				if (Current().kind == TokenKind::RightBrace) {
					Fail(result, "byte_set_value_missing", Current().position);
					return nullptr;
				}
			}
		}
		Fail(result, "expression_operand_missing", Current().position);
		return nullptr;
	}

	static int BinaryPrecedence(const std::string& op)
	{
		if (op == "或" || op == "|") return 1;
		if (op == "且" || op == "&") return 2;
		if (op == "＝" || op == "=" || op == "==" || op == "≠" || op == "!=" || op == "<>" ||
			op == "?=" || op == "＜" || op == "<" || op == "＞" || op == ">" || op == "≤" || op == "<=" || op == "≥" || op == ">=") return 3;
		if (op == "＋" || op == "+" || op == "－" || op == "-") return 4;
		if (op == "×" || op == "*" || op == "÷" || op == "/" || op == "\\" || op == "%") return 5;
		return -1;
	}

	std::vector<Token> tokens_;
	std::size_t index_ = 0;
};

bool IsQuoteAt(const std::string& text, const std::size_t pos, const std::string_view left)
{
	return StartsAt(text, pos, left);
}

}  // namespace

bool SourceExpressionParseResult::IsValid() const
{
	return root != nullptr && error.empty();
}

SourceExpressionParseResult ParseSourceExpression(const std::string& text)
{
	std::string error;
	std::size_t offset = 0;
	Lexer lexer(text);
	std::vector<Token> tokens = lexer.Lex(error, offset);
	if (!error.empty()) {
		return SourceExpressionParseResult { .root = nullptr, .errorOffset = offset, .error = std::move(error) };
	}
	Parser parser(std::move(tokens));
	return parser.Parse();
}

bool FindSourceTopLevelAssignment(
	const std::string& text,
	std::size_t& outPosition,
	std::size_t& outLength)
{
	outPosition = 0;
	outLength = 0;
	int parentheses = 0;
	int brackets = 0;
	int braces = 0;
	bool chineseText = false;
	bool asciiText = false;
	for (std::size_t pos = 0; pos < text.size();) {
		if (!asciiText && IsQuoteAt(text, pos, kChineseLeftQuote)) {
			chineseText = !chineseText;
			pos += kChineseLeftQuote.size();
			continue;
		}
		if (!asciiText && IsQuoteAt(text, pos, kChineseRightQuote)) {
			chineseText = !chineseText;
			pos += kChineseRightQuote.size();
			continue;
		}
		if (!chineseText && text[pos] == '"') {
			asciiText = !asciiText;
			++pos;
			continue;
		}
		if (chineseText || asciiText) {
			pos = NextCharacterIndex(text, pos);
			continue;
		}
		if (text[pos] == '(') ++parentheses;
		else if (text[pos] == ')') --parentheses;
		else if (text[pos] == '[') ++brackets;
		else if (text[pos] == ']') --brackets;
		else if (text[pos] == '{') ++braces;
		else if (text[pos] == '}') --braces;
		else if (parentheses == 0 && brackets == 0 && braces == 0) {
			if (IsQuoteAt(text, pos, "＝")) { outPosition = pos; outLength = std::string("＝").size(); return true; }
		}
		pos = NextCharacterIndex(text, pos);
	}
	return false;
}

bool SplitSourceCallArguments(const std::string& text, std::vector<std::string>& outArguments)
{
	outArguments.clear();
	std::size_t begin = 0;
	int parentheses = 0;
	int brackets = 0;
	int braces = 0;
	bool chineseText = false;
	bool asciiText = false;
	for (std::size_t pos = 0; pos < text.size();) {
		if (!asciiText && IsQuoteAt(text, pos, kChineseLeftQuote)) { chineseText = true; pos += kChineseLeftQuote.size(); continue; }
		if (chineseText && IsQuoteAt(text, pos, kChineseRightQuote)) { chineseText = false; pos += kChineseRightQuote.size(); continue; }
		if (!chineseText && text[pos] == '"') { asciiText = !asciiText; ++pos; continue; }
		if (chineseText || asciiText) { pos = NextCharacterIndex(text, pos); continue; }
		if (text[pos] == '(') ++parentheses;
		else if (text[pos] == ')') --parentheses;
		else if (text[pos] == '[') ++brackets;
		else if (text[pos] == ']') --brackets;
		else if (text[pos] == '{') ++braces;
		else if (text[pos] == '}') --braces;
		else if (text[pos] == ',' && parentheses == 0 && brackets == 0 && braces == 0) {
			std::string part = text.substr(begin, pos - begin);
			while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front())) != 0) part.erase(part.begin());
			while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back())) != 0) part.pop_back();
			outArguments.push_back(std::move(part));
			begin = pos + 1;
		}
		pos = NextCharacterIndex(text, pos);
	}
	if (chineseText || asciiText || parentheses != 0 || brackets != 0 || braces != 0) return false;
	std::string part = text.substr(begin);
	while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front())) != 0) part.erase(part.begin());
	while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back())) != 0) part.pop_back();
	if (!part.empty() || !outArguments.empty()) outArguments.push_back(std::move(part));
	return true;
}

}  // namespace e2txt
