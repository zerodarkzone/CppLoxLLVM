//
// Created by juanb on 30/09/2018.
//

#include "scanner.hpp"

Scanner::Scanner(std::string_view source) : m_source(source)
{}

Token Scanner::scanToken()
{
	skipWhiteSpace();

	m_start = m_current;

	if (isAtEnd())
		return makeToken(TokenType::EOF_);

	auto c = advance();
	if (isAlpha(c))
		return identifier();
	if (isdigit(c))
		return number();

	switch (c)
	{
		case '(':
			return makeToken(TokenType::LEFT_PAREN);
		case ')':
			return makeToken(TokenType::RIGHT_PAREN);
		case '{':
			return makeToken(TokenType::LEFT_BRACE);
		case '}':
			return makeToken(TokenType::RIGHT_BRACE);
		case ';':
			return makeToken(TokenType::SEMICOLON);
		case ':':
			return makeToken(TokenType::COLON);
		case ',':
			return makeToken(TokenType::COMMA);
		case '.':
			return makeToken(TokenType::DOT);
		case '-':
			return makeToken(match('=') ? TokenType::MINUS_EQUAL : TokenType::MINUS);
		case '+':
			return makeToken(match('=') ? TokenType::PLUS_EQUAL : TokenType::PLUS);
		case '/':
			return makeToken(match('=') ? TokenType::SLASH_EQUAL : TokenType::SLASH);
		case '*':
			return makeToken(match('=') ? TokenType::STAR_EQUAL : TokenType::STAR);
		case '%':
			return makeToken(match('=') ? TokenType::MOD_EQUAL : TokenType::MOD);
		case '!':
			return makeToken(match('=') ? TokenType::BANG_EQUAL : TokenType::BANG);
		case '=':
			return makeToken(match('=') ? TokenType::EQUAL_EQUAL : TokenType::EQUAL);
		case '<':
			return makeToken(match('=') ? TokenType::LESS_EQUAL : TokenType::LESS);
		case '>':
			return makeToken(match('=') ? TokenType::GREATER_EQUAL: TokenType::GREATER);
		case '"':
			return string();
		default:
			break;
	}

	return errorToken("Unexpected character.");
}

bool Scanner::isDigit(char c)
{
	return isdigit(c);
}

bool Scanner::isAlpha(char c)
{
	return isalpha(c) || c == '_';
}

bool Scanner::isAtEnd()
{
	return m_current >= m_source.length();
}

char Scanner::advance()
{
	return m_source[m_current++];
}

char Scanner::peek()
{
	return m_source[m_current];
}

char Scanner::peekNext()
{
	if (isAtEnd())
		return '\0';
	return m_source[m_current + 1];
}

bool Scanner::match(char expected)
{
	if (isAtEnd())
		return false;
	if (m_source[m_current] != expected)
		return false;
	m_current ++;
	return true;
}

Token Scanner::makeToken(TokenType type)
{
	Token token;
	token.type = type;
	token.lexeme = m_source.substr(m_start, m_current - m_start);
	token.line = m_line;

	return token;
}

Token Scanner::errorToken(const std::string &message)
{
	Token token;
	token.type = TokenType::ERROR;
	token.lexeme = message;
	token.line = m_line;
	return token;
}

void Scanner::skipWhiteSpace()
{
	for (;;)
	{
		if (isAtEnd())
			return;
		auto c = peek();
		switch (c)
		{
			case ' ':
			case '\r':
			case '\t':
				advance();
				break;

			case '\n':
				m_line++;
				advance();
				break;

			case '/':
				if (peekNext() == '/')
				{
					// A comment goes until the end of the line.
					while (peek() != '\n' && !isAtEnd()) advance();
				}
				else
					return;
				break;
			default:
				return;
		}
	}
}

TokenType Scanner::checkKeyword(int start, int length, const std::string &rest, TokenType type)
{
	if (m_current - m_start == start + length &&
		m_source.substr(m_start + start, static_cast<std::string_view::size_type>(length)) == rest)
		return type;
	return TokenType::IDENTIFIER;
}

TokenType Scanner::identifierType()
{
	switch (m_source[m_start])
	{
		case 'a': return checkKeyword(1, 2, "nd", TokenType::AND);
		case 'b': return checkKeyword(1, 4, "reak", TokenType::BREAK);
		case 'c':
			if (m_current - m_start > 1)
			{
				switch (m_source[m_start + 1])
				{
					case 'a': return checkKeyword(2, 2, "se", TokenType::CASE);
					case 'l': return checkKeyword(2, 3, "ass", TokenType::CLASS);
					case 'o': return checkKeyword(2, 6, "ntinue", TokenType::CONTINUE);

					default:break;
				}
			}
			break;
		case 'd': return checkKeyword(1, 6, "efault", TokenType::DEFAULT);
		case 'e': return checkKeyword(1, 3, "lse", TokenType::ELSE);
		case 'f':
			if (m_current - m_start > 1)
			{
				switch (m_source[m_start + 1])
				{
					case 'a': return checkKeyword(2, 3, "lse", TokenType::FALSE);
					case 'o': return checkKeyword(2, 1, "r", TokenType::FOR);
					case 'u': return checkKeyword(2, 1, "n", TokenType::FUN);

					default:break;
				}
			}
			break;
		case 'i': return checkKeyword(1, 1, "f", TokenType::IF);
		case 'n': return checkKeyword(1, 2, "il", TokenType::NIL);
		case 'o': return checkKeyword(1, 1, "r", TokenType::OR);
		case 'p': return checkKeyword(1, 4, "rint", TokenType::PRINT);
		case 'r': return checkKeyword(1, 5, "eturn", TokenType::RETURN);
		case 's':
			if (m_current - m_start > 1)
			{
				switch (m_source[m_start + 1])
				{
					case 'u': return checkKeyword(2, 3, "per", TokenType::SUPER);
					case 'w': return checkKeyword(2, 4, "itch", TokenType::SWITCH);

					default:break;
				}
			}
			break;
		case 't':
			if (m_current - m_start > 1)
			{
				switch (m_source[m_start + 1])
				{
					case 'h': return checkKeyword(2, 2, "is", TokenType::THIS);
					case 'r': return checkKeyword(2, 2, "ue", TokenType::TRUE);

					default:break;
				}
			}
			break;
		case 'v': return checkKeyword(1, 2, "ar", TokenType::VAR);
		case 'w': return checkKeyword(1, 4, "hile", TokenType::WHILE);
		default:
			break;
	}
	return TokenType::IDENTIFIER;
}

Token Scanner::identifier()
{
	while (isAlpha(peek()) || isDigit(peek()))
		advance();
	return makeToken(identifierType());
}

Token Scanner::number()
{
	while (isDigit(peek()))
		advance();

	// Look for a fractional part.
	if (peek() == '.' && isDigit(peekNext()))
	{
		// Consume the ".".
		advance();
		while (isDigit(peek()))
			advance();
	}

	return makeToken(TokenType::NUMBER);
}

Token Scanner::string()
{
	while (peek() != '"' && !isAtEnd())
	{
		if (peek() == '\n')
			m_line++;
		advance();
	}

	if (isAtEnd())
		return errorToken("Unterminated string.");

	// The closing ".
	advance();
	return makeToken(TokenType::STRING);
}


