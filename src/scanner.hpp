//
// Created by juanb on 30/09/2018.
//

#ifndef CPPLOX_SCANNER_HPP
#define CPPLOX_SCANNER_HPP

#include <string>
#include <string_view>

enum class TokenType : uint8_t
{
	// Single-character tokens.
		LEFT_PAREN, RIGHT_PAREN,
	LEFT_BRACE, RIGHT_BRACE,
	COMMA, DOT, MINUS, PLUS,
	SEMICOLON, SLASH, STAR,
	MOD, COLON,

	// One or two character tokens.
		BANG, BANG_EQUAL,
	EQUAL, EQUAL_EQUAL,
	GREATER, GREATER_EQUAL,
	LESS, LESS_EQUAL, MINUS_EQUAL,
	PLUS_EQUAL, SLASH_EQUAL, STAR_EQUAL,
	MOD_EQUAL,
	
	// Literals.
		IDENTIFIER, STRING, NUMBER,

	// Keywords.
		AND, CLASS, ELSE, FALSE,
	FUN, FOR, IF, NIL, OR,
	PRINT, RETURN, SUPER, THIS,
	TRUE, VAR, WHILE, CONTINUE,
	BREAK, CASE, DEFAULT, SWITCH,

	ERROR,
	EOF_
};

struct Token
{
	TokenType type;
	std::string_view lexeme;
	uint32_t line;
};

class Scanner
{
public:
	Scanner() = default;
	explicit Scanner(std::string_view source);
	Token scanToken();
private:
	bool isDigit(char c);
	bool isAlpha(char c);
	bool isAtEnd();
	char advance();
	char peek();
	char peekNext();
	bool match(char expected);
	Token makeToken(TokenType type);
	Token errorToken(const std::string &message);
	void skipWhiteSpace();
	TokenType checkKeyword(int start, int length, const std::string &rest, TokenType type);
	TokenType identifierType();
	Token identifier();
	Token number();
	Token string();

	std::string_view m_source;
	size_t m_start = 0;
	size_t m_current = 0;
	uint32_t m_line = 1;
};

#endif //CPPLOX_SCANNER_HPP
