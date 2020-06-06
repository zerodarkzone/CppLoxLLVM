#include "compiler.hpp"


ParseRule Compiler::rules[] = {
	{ &Compiler::grouping,	&Compiler::call,	Precedence::CALL },       // TOKEN_LEFT_PAREN
	{ nullptr, 				nullptr,			Precedence::NONE },       // TOKEN_RIGHT_PAREN
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_LEFT_BRACE
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_RIGHT_BRACE
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_COMMA
	{ nullptr,     			nullptr,    		Precedence::CALL },       // TOKEN_DOT
	{ &Compiler::unary,    	&Compiler::binary,  Precedence::TERM },       // TOKEN_MINUS
	{ nullptr,     			&Compiler::binary,  Precedence::TERM },       // TOKEN_PLUS
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_SEMICOLON
	{ nullptr,     			&Compiler::binary,  Precedence::FACTOR },     // TOKEN_SLASH
	{ nullptr,     			&Compiler::binary,  Precedence::FACTOR },     // TOKEN_STAR
	{ nullptr,     			&Compiler::binary,  Precedence::FACTOR },     // TOKEN_MOD
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_COLON
	{ &Compiler::unary,     nullptr,    		Precedence::NONE },       // TOKEN_BANG
	{ nullptr,     			&Compiler::binary,  Precedence::EQUALITY },   // TOKEN_BANG_EQUAL
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_EQUAL
	{ nullptr,     			&Compiler::binary,  Precedence::EQUALITY },   // TOKEN_EQUAL_EQUAL
	{ nullptr,     			&Compiler::binary,  Precedence::COMPARISON }, // TOKEN_GREATER
	{ nullptr,     			&Compiler::binary,  Precedence::COMPARISON }, // TOKEN_GREATER_EQUAL
	{ nullptr,     			&Compiler::binary,  Precedence::COMPARISON }, // TOKEN_LESS
	{ nullptr,     			&Compiler::binary,  Precedence::COMPARISON }, // TOKEN_LESS_EQUAL
	{ nullptr, 				nullptr,  			Precedence::NONE }, 	  // TOKEN_MINUS_EQUAL
	{ nullptr, 				nullptr,  			Precedence::NONE }, 	  // TOKEN_PLUS_EQUAL
	{ nullptr, 				nullptr,  			Precedence::NONE }, 	  // TOKEN_SLASH_EQUAL
	{ nullptr, 				nullptr,  			Precedence::NONE }, 	  // TOKEN_STAR_EQUAL
	{ nullptr, 				nullptr,  			Precedence::NONE }, 	  // TOKEN_MOD_EQUAL
	{ &Compiler::variable, 	nullptr,    		Precedence::NONE },       // TOKEN_IDENTIFIER
	{ &Compiler::string,    nullptr,    		Precedence::NONE },       // TOKEN_STRING
	{ &Compiler::number,   	nullptr,    		Precedence::NONE },       // TOKEN_NUMBER
	{ nullptr,     			&Compiler::and_,    Precedence::AND },        // TOKEN_AND
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_CLASS
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_ELSE
	{ &Compiler::literal,   nullptr,    		Precedence::NONE },       // TOKEN_FALSE
	{ nullptr,     			nullptr,			Precedence::NONE },       // TOKEN_FUN
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_FOR
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_IF
	{ &Compiler::literal,   nullptr,    		Precedence::NONE },       // TOKEN_NIL
	{ nullptr,     			&Compiler::or_,    	Precedence::OR },         // TOKEN_OR
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_PRINT
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_RETURN
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_SUPER
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_THIS
	{ &Compiler::literal,   nullptr,    		Precedence::NONE },       // TOKEN_TRUE
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_VAR
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_WHILE
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_CONTINUE
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_BREAK
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_CASE
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_DEFAULT
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_SWITCH
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_ERROR
	{ nullptr,     			nullptr,    		Precedence::NONE },       // TOKEN_EOF
};