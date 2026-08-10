#include "SimpleXmlDocument.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace e2txt {
namespace {

bool StartsWith(const std::string_view text, const std::string_view prefix)
{
	return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string DecodeXmlEntities(const std::string& text)
{
	std::string out;
	out.reserve(text.size());
	for (std::size_t index = 0; index < text.size(); ++index) {
		if (text[index] != '&') {
			out.push_back(text[index]);
			continue;
		}
		const std::string_view remaining(text.data() + index, text.size() - index);
		if (StartsWith(remaining, "&amp;")) { out.push_back('&'); index += 4; }
		else if (StartsWith(remaining, "&lt;")) { out.push_back('<'); index += 3; }
		else if (StartsWith(remaining, "&gt;")) { out.push_back('>'); index += 3; }
		else if (StartsWith(remaining, "&quot;")) { out.push_back('"'); index += 5; }
		else if (StartsWith(remaining, "&apos;")) { out.push_back('\''); index += 5; }
		else out.push_back(text[index]);
	}
	return out;
}

class Parser {
public:
	explicit Parser(const std::string& text) : text_(text) {}

	bool Parse(SimpleXmlNode& outRoot, SimpleXmlParseError* outError)
	{
		SkipWhitespace();
		if (StartsWith(Remaining(), "<?xml")) {
			const std::size_t end = text_.find("?>", position_);
			if (end == std::string::npos) return Fail("xml_declaration_invalid", outError);
			position_ = end + 2;
		}
		SkipWhitespace();
		if (!ParseNode(outRoot, outError)) return false;
		SkipWhitespace();
		if (position_ != text_.size()) return Fail("xml_trailing_content", outError);
		return true;
	}

private:
	std::string_view Remaining() const
	{
		return std::string_view(text_).substr((std::min)(position_, text_.size()));
	}

	void SkipWhitespace()
	{
		while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_])) != 0) ++position_;
	}

	bool Fail(const std::string_view code, SimpleXmlParseError* outError)
	{
		if (outError != nullptr) {
			outError->code = std::string(code);
			outError->lineIndex = static_cast<std::size_t>(std::count(text_.begin(), text_.begin() + (std::min)(position_, text_.size()), '\n'));
		}
		return false;
	}

	bool ParseName(std::string& outName)
	{
		const std::size_t start = position_;
		while (position_ < text_.size()) {
			const unsigned char ch = static_cast<unsigned char>(text_[position_]);
			if (std::isspace(ch) != 0 || ch == '/' || ch == '>' || ch == '=' || ch == '?') break;
			++position_;
		}
		if (position_ == start) return false;
		outName = text_.substr(start, position_ - start);
		return true;
	}

	bool ParseQuotedValue(std::string& outValue)
	{
		if (position_ >= text_.size() || text_[position_] != '"') return false;
		++position_;
		const std::size_t start = position_;
		while (position_ < text_.size() && text_[position_] != '"') ++position_;
		if (position_ >= text_.size()) return false;
		outValue = DecodeXmlEntities(text_.substr(start, position_ - start));
		++position_;
		return true;
	}

	bool ParseAttributes(SimpleXmlNode& node, bool& outSelfClosing, SimpleXmlParseError* outError)
	{
		outSelfClosing = false;
		while (position_ < text_.size()) {
			SkipWhitespace();
			if (position_ >= text_.size()) break;
			if (text_[position_] == '/') {
				++position_;
				if (position_ >= text_.size() || text_[position_] != '>') return Fail("xml_self_closing_invalid", outError);
				++position_;
				outSelfClosing = true;
				return true;
			}
			if (text_[position_] == '>') {
				++position_;
				return true;
			}

			std::string key;
			if (!ParseName(key)) return Fail("xml_attr_name_invalid", outError);
			SkipWhitespace();
			if (position_ >= text_.size() || text_[position_] != '=') return Fail("xml_attr_assign_missing", outError);
			++position_;
			SkipWhitespace();
			std::string value;
			if (!ParseQuotedValue(value)) return Fail("xml_attr_value_invalid", outError);
			if (!node.attributes.emplace(std::move(key), std::move(value)).second) return Fail("xml_attr_duplicate", outError);
		}
		return Fail("xml_attr_eof", outError);
	}

	bool ParseNode(SimpleXmlNode& outNode, SimpleXmlParseError* outError)
	{
		if (position_ >= text_.size() || text_[position_] != '<') return Fail("xml_tag_missing", outError);
		++position_;
		if (!ParseName(outNode.name)) return Fail("xml_tag_name_invalid", outError);

		bool selfClosing = false;
		if (!ParseAttributes(outNode, selfClosing, outError)) return false;
		if (selfClosing) return true;

		while (position_ < text_.size()) {
			SkipWhitespace();
			if (StartsWith(Remaining(), "</")) {
				position_ += 2;
				std::string closeName;
				if (!ParseName(closeName) || closeName != outNode.name) return Fail("xml_close_tag_invalid", outError);
				SkipWhitespace();
				if (position_ >= text_.size() || text_[position_] != '>') return Fail("xml_close_tag_end_missing", outError);
				++position_;
				return true;
			}
			if (position_ < text_.size() && text_[position_] == '<') {
				SimpleXmlNode child;
				if (!ParseNode(child, outError)) return false;
				outNode.children.push_back(std::move(child));
				continue;
			}
			while (position_ < text_.size() && text_[position_] != '<') {
				if (std::isspace(static_cast<unsigned char>(text_[position_])) == 0) return Fail("xml_text_content_unsupported", outError);
				++position_;
			}
		}
		return Fail("xml_close_tag_missing", outError);
	}

	const std::string& text_;
	std::size_t position_ = 0;
};

}  // namespace

bool ParseSimpleXmlDocument(
	const std::string& text,
	SimpleXmlNode& outRoot,
	SimpleXmlParseError* outError)
{
	outRoot = {};
	if (outError != nullptr) *outError = {};
	Parser parser(text);
	return parser.Parse(outRoot, outError);
}

}  // namespace e2txt
