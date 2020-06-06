//
// Created by juanb on 30/09/2018.
//

#include <iostream>
#include <iomanip>
#include "compiler.hpp"
#include "vm.hpp"
#include "scanner.hpp"
#include "common.hpp"
#include "memory.hpp"

#ifdef DEBUG_PRINT_CODE
#include "debug.hpp"
#endif

#define MAX_CONSTANTS_BEFORE_LONG 256
#define MAX_CASES 256

Scope::Scope(VM* vm, Parser* parser, FunctionType type, Scope* enclosing) :
	enclosing(enclosing), type(type)
{
	function = Memory::createFunction(vm);
	if (type != FunctionType::SCRIPT)
	{
		function->name = Memory::createString(vm, parser->previous.lexeme);
	}

	auto local = &locals[localCount++];
	local->depth = 0;
	local->name.lexeme = "";
}

ObjFunction* Compiler::compile(VM *vm, std::string_view source)
{
	init();
	auto scope = Scope{vm, &m_parser, FunctionType::SCRIPT, nullptr};
	m_scanner = Scanner(source);
	m_current = &scope;
	m_vm = vm;
	m_parser.hadError = false;
	m_parser.panicMode = false;

	advance();
	while (!match(TokenType::EOF_))
	{
		declaration();
	}
	auto function = endCompiler();
	return m_parser.hadError ? nullptr : function;
}

Chunk *Compiler::currentChunk()
{
	return &m_current->function->chunk;
}

void Compiler::init()
{
	m_current = nullptr;
	m_vm = nullptr;

	m_innermostBreakJump = -1;
	m_innermostLoopStart = -1;
	m_innermostLoopScopeDepth = 0;
	m_insideSwitch = false;
}

void Compiler::advance()
{
	m_parser.previous = m_parser.current;

	for (;;)
	{
		m_parser.current = m_scanner.scanToken();
		if (m_parser.current.type != TokenType::ERROR)
			break;

		errorAtCurrent(std::string(m_parser.current.lexeme));
	}
}

void Compiler::consume(TokenType type, const std::string &message)
{
	if (m_parser.current.type == type)
	{
		advance();
		return;
	}

	errorAtCurrent(message);
}

bool Compiler::check(TokenType type)
{
	return m_parser.current.type == type;
}

bool Compiler::match(TokenType type)
{
	if (!check(type))
		return false;
	advance();
	return true;
}

bool Compiler::matchCompound()
{
	return match(TokenType::MINUS_EQUAL) || match(TokenType::PLUS_EQUAL) ||
		match(TokenType::SLASH_EQUAL) || match(TokenType::STAR_EQUAL) || match(TokenType::MOD_EQUAL);
}

void Compiler::emitByte(uint8_t byte)
{
	currentChunk()->write(byte, m_parser.previous.line);
}

void Compiler::emitBytes(uint8_t byte1, uint8_t byte2)
{
	emitByte(byte1);
	emitByte(byte2);
}

void Compiler::emitShort(uint16_t val)
{
	emitByte(static_cast<uint8_t>(val & 0xffu));
	emitByte(static_cast<uint8_t>(val >> 8u) & 0xffu);
}

void Compiler::emitLong(uint32_t val)
{
	emitByte(static_cast<uint8_t>(val & 0xffu));
	emitByte(static_cast<uint8_t>((val >> 8u) & 0xffu));
	emitByte(static_cast<uint8_t>((val >> 16u) & 0xffu));
}

void Compiler::emitLoop(uint32_t loopStart)
{
	emitByte(OpCode::JUMP_BACK);

	auto offset = currentChunk()->size() - loopStart + 2;
	if (offset > std::numeric_limits<uint16_t>::max())
	{
		error("Loop body too large.");
	}

	emitShort(offset);
}

uint32_t Compiler::emitJump(uint8_t instruction)
{
	emitByte(instruction);
	emitShort(0xffffu);
	return currentChunk()->size() - 2;
}

void Compiler::emitReturn()
{
	emitByte(OpCode::NIL);
	emitByte(OpCode::RETURN);
}

uint32_t Compiler::makeConstant(const Value &value)
{
	return currentChunk()->addConstant(value);
}

void Compiler::emitConstant(const Value &value)
{
	auto index = makeConstant(value);
	if (index < MAX_CONSTANTS_BEFORE_LONG)
	{
		emitBytes(OpCode::CONSTANT, static_cast<uint8_t>(index));
	}
	else
	{
		emitByte(OpCode::CONSTANT_LONG);
		emitLong(index);
	}
}

void Compiler::patchJump(uint32_t offset)
{
	// -2 to adjust for the bytecode for the jump offset itself.
	auto jump = currentChunk()->size() - offset - 2u;

	if (jump > std::numeric_limits<uint16_t>::max())
	{
		error("Too much code to jump over.");
	}

	currentChunk()->get(offset) = jump & 0xffu;
	currentChunk()->get(offset + 1u) = (jump >> 8u) & 0xffu;
}

ObjFunction* Compiler::endCompiler()
{
	emitReturn();
	auto function = m_current->function;
#ifdef DEBUG_PRINT_CODE
	if (!m_parser.hadError)
	{
		disassembleChunk(*currentChunk(), function->name != nullptr ? function->name->value : "<script>");
	}
#endif
	m_current = m_current->enclosing;
	return function;
}

void Compiler::beginScope()
{
	m_current->scopeDepth++;
}

void Compiler::endScope()
{
	m_current->scopeDepth--;

	while (m_current->localCount > 0 &&
	       m_current->locals[m_current->localCount - 1].depth > m_current->scopeDepth)
	{
		emitByte(OpCode::POP);
		m_current->localCount--;
	}
}

void Compiler::binary(bool)
{
	// Remember the operator.
	auto operatorType = m_parser.previous.type;

	// Compile the right operand.
	ParseRule *rule = getRule(operatorType);
	parsePrecedence(static_cast<Precedence>(static_cast<uint8_t>(rule->precedence) + 1u));

	// Emit the operator instruction.
	switch (operatorType)
	{
		case TokenType::BANG_EQUAL:
			emitBytes(OpCode::EQUAL, OpCode::NOT);
			break;
		case TokenType::EQUAL_EQUAL:
			emitByte(OpCode::EQUAL);
			break;
		case TokenType::GREATER:
			emitByte(OpCode::GREATER);
			break;
		case TokenType::GREATER_EQUAL:
			emitBytes(OpCode::LESS, OpCode::NOT);
			break;
		case TokenType::LESS:
			emitByte(OpCode::LESS);
			break;
		case TokenType::LESS_EQUAL:
			emitBytes(OpCode::GREATER, OpCode::NOT);
			break;
		case TokenType::PLUS:
			emitByte(OpCode::ADD);
			break;
		case TokenType::MINUS:
			emitByte(OpCode::SUBTRACT);
			break;
		case TokenType::STAR:
			emitByte(OpCode::MULTIPLY);
			break;
		case TokenType::SLASH:
			emitByte(OpCode::DIVIDE);
			break;
		case TokenType::MOD:
			emitByte(OpCode::MODULO);
			break;
		default:
			return; // Unreachable.
	}
}

void Compiler::call(bool)
{
	auto argCount = argumentList();
	emitBytes(OpCode::CALL, argCount);
}

void Compiler::literal(bool)
{
	switch (m_parser.previous.type)
	{
		case TokenType::FALSE:
			emitByte(OpCode::FALSE);
			break;
		case TokenType::NIL:
			emitByte(OpCode::NIL);
			break;
		case TokenType::TRUE:
			emitByte(OpCode::TRUE);
			break;
		default:
			return; // Unreachable.
	}
}

void Compiler::grouping(bool)
{
	expression();
	consume(TokenType::RIGHT_PAREN, "Expect ')' after expression.");
}

void Compiler::number(bool)
{
	auto value = std::stod(m_parser.previous.lexeme.data());
	emitConstant(Value::Number(value));
}

void Compiler::or_(bool)
{
	auto endJump = emitJump(OpCode::JUMP_IF_TRUE);

	emitByte(OpCode::POP);
	parsePrecedence(Precedence::OR);

	patchJump(endJump);
}

void Compiler::and_(bool)
{
	auto endJump = emitJump(OpCode::JUMP_IF_FALSE);

	emitByte(OpCode::POP);
	parsePrecedence(Precedence::AND);

	patchJump(endJump);
}

void Compiler::string(bool)
{
	auto length = m_parser.previous.lexeme.length() - 2;
	emitConstant(Value::Object(Mem::createString(m_vm, m_parser.previous.lexeme.substr(1, length))));
}

void Compiler::namedVariable(const Token &name, bool canAssign)
{
	auto arg = resolveLocal(m_current, name);
	if (arg != -1)
	{
		if (canAssign && matchCompound())
		{
			auto type = m_parser.previous.type;
			if (arg < MAX_CONSTANTS_BEFORE_LONG)
			{
				emitBytes(OpCode::GET_LOCAL, arg);
			}
			else
			{
				emitByte(OpCode::GET_LOCAL_SHORT);
				emitShort(arg);
			}
			expression();
			switch (type)
			{
				case TokenType::MINUS_EQUAL:
					emitByte(OpCode::SUBTRACT);
					break;
				case TokenType::PLUS_EQUAL:
					emitByte(OpCode::ADD);
					break;
				case TokenType::SLASH_EQUAL:
					emitByte(OpCode::DIVIDE);
					break;
				case TokenType::STAR_EQUAL:
					emitByte(OpCode::MULTIPLY);
					break;
				case TokenType::MOD_EQUAL:
					emitByte(OpCode::MODULO);
					break;
				default:
					break;
			}
			if (arg < MAX_CONSTANTS_BEFORE_LONG)
			{
				emitBytes(OpCode::SET_LOCAL, arg);
			}
			else
			{
				emitByte(OpCode::SET_LOCAL_SHORT);
				emitShort(arg);
			}
		}
		else if (canAssign && match(TokenType::EQUAL))
		{
			expression();
			if (arg < MAX_CONSTANTS_BEFORE_LONG)
			{
				emitBytes(OpCode::SET_LOCAL, arg);
			}
			else
			{
				emitByte(OpCode::SET_LOCAL_SHORT);
				emitShort(arg);
			}
		}
		else
		{
			if (arg < MAX_CONSTANTS_BEFORE_LONG) {
				emitBytes(OpCode::GET_LOCAL, arg);
			}
			else {
				emitByte(OpCode::GET_LOCAL_SHORT);
				emitShort(arg);
			}
		}

	}
	else
	{
		arg = identifierConstant(name);
		if (canAssign && matchCompound())
		{
			auto type = m_parser.previous.type;
			if (arg < MAX_CONSTANTS_BEFORE_LONG) {
				emitBytes(OpCode::GET_GLOBAL, static_cast<uint8_t>(arg));
			}
			else {
				emitByte(OpCode::GET_GLOBAL_LONG);
				emitLong(arg);
			}
			expression();
			switch (type)
			{
				case TokenType::MINUS_EQUAL:
					emitByte(OpCode::SUBTRACT);
					break;
				case TokenType::PLUS_EQUAL:
					emitByte(OpCode::ADD);
					break;
				case TokenType::SLASH_EQUAL:
					emitByte(OpCode::DIVIDE);
					break;
				case TokenType::STAR_EQUAL:
					emitByte(OpCode::MULTIPLY);
					break;
				case TokenType::MOD_EQUAL:
					emitByte(OpCode::MODULO);
					break;
				default:
					break;
			}
			if (arg < MAX_CONSTANTS_BEFORE_LONG) {
				emitBytes(OpCode::SET_GLOBAL, static_cast<uint8_t>(arg));
			}
			else {
				emitByte(OpCode::SET_GLOBAL_LONG);
				emitLong(arg);
			}
		}
		else if (canAssign && match(TokenType::EQUAL))
		{
			expression();
			if (arg < MAX_CONSTANTS_BEFORE_LONG) {
				emitBytes(OpCode::SET_GLOBAL, static_cast<uint8_t>(arg));
			}
			else {
				emitByte(OpCode::SET_GLOBAL_LONG);
				emitLong(arg);
			}
		}
		else
		{
			if (arg < MAX_CONSTANTS_BEFORE_LONG) {
				emitBytes(OpCode::GET_GLOBAL, static_cast<uint8_t>(arg));
			}
			else {
				emitByte(OpCode::GET_GLOBAL_LONG);
				emitLong(arg);
			}
		}
	}
}

void Compiler::variable(bool canAssign)
{
	namedVariable(m_parser.previous, canAssign);
}

void Compiler::unary(bool)
{
	auto operatorType = m_parser.previous.type;

	// Compile the operand.
	parsePrecedence(Precedence::UNARY);

	// Emit the operator instruction.
	switch (operatorType)
	{
		case TokenType::BANG:
			emitByte(OpCode::NOT);
			break;
		case TokenType::MINUS:
			emitByte(OpCode::NEGATE);
			break;
		default:
			return; // Unreachable.
	}
}

void Compiler::parsePrecedence(Precedence precedence)
{
	advance();
	auto prefixRule = getRule(m_parser.previous.type)->prefix;
	if (!prefixRule)
	{
		error("Expect expression.");
		return;
	}

	auto canAssign = precedence <= Precedence::ASSIGNMENT;
	(this->*prefixRule)(canAssign);

	while (precedence <= getRule(m_parser.current.type)->precedence)
	{
		advance();
		auto infixRule = getRule(m_parser.previous.type)->infix;
		(this->*infixRule)(canAssign);
	}

	if (canAssign && match(TokenType::EQUAL))
	{
		error("Invalid assigment target.");
		expression();
	}
}

uint32_t Compiler::identifierConstant(const Token &token)
{
	auto identifier = std::string(token.lexeme);
	auto index = m_vm->globalsMap()[identifier];
	if (!index.isUndefined())
	{
		return static_cast<uint32_t>(index.asNumber());
	}

	m_vm->globalValues().emplace_back();
	m_vm->globalNames().push_back(identifier);
	uint32_t newIndex = m_vm->globalValues().size() - 1;

	m_vm->globalsMap()[identifier] = Value::Number(newIndex);
	return newIndex;
}

bool identifiersEqual(const Token &a, const Token &b)
{
	return a.lexeme == b.lexeme;
}

int32_t Compiler::resolveLocal(Scope* scope, const Token &name)
{
	for (auto i = scope->localCount -1; i >= 0; --i)
	{
		auto &local = scope->locals[i];
		if (identifiersEqual(name, local.name))
		{
			if (local.depth == -1)
			{
				error("Cannot read local variable in its own initializer.");
			}
			return i;
		}
	}

	return -1;
}

void Compiler::addLocal(const Token &name)
{
	if (m_current->localCount >= MAX_LOCALS)
	{
		error("Too many local variables in scope.");
		return;
	}

	auto &local = m_current->locals[m_current->localCount++];
	local.name = name;
	local.depth = -1;
}

void Compiler::declareVariable()
{
	// Global variables are implicitly declared.
	if (m_current->scopeDepth == 0)
		return;

	const auto &name = m_parser.previous;

	for (auto i = m_current->localCount - 1; i >= 0; --i)
	{
		auto &local = m_current->locals[i];
		if (local.depth != -1 && local.depth < m_current->scopeDepth)
			break;
		if (identifiersEqual(name, local.name))
		{
			error("Variable with this name already declared in this scope.");
		}
	}

	addLocal(name);
}

uint32_t Compiler::parseVariable(const std::string &message)
{
	consume(TokenType::IDENTIFIER, message);

	declareVariable();
	if (m_current->scopeDepth > 0) return 0;

	return identifierConstant(m_parser.previous);
}

void Compiler::markInitialized()
{
	if (m_current->scopeDepth == 0)
		return;
	m_current->locals[m_current->localCount - 1].depth = m_current->scopeDepth;
}

void Compiler::defineVariable(uint32_t global)
{
	if (m_current->scopeDepth > 0)
	{
		markInitialized();
		return;
	}

	if (global < MAX_CONSTANTS_BEFORE_LONG)
	{
		emitBytes(OpCode::DEFINE_GLOBAL, static_cast<uint8_t>(global));
	}
	else
	{
		emitByte(OpCode::DEFINE_GLOBAL_LONG);
		emitLong(global);
	}
}

uint8_t Compiler::argumentList()
{
	uint8_t argCount = 0;
	if (!check(TokenType::RIGHT_PAREN))
	{
		do {
			expression();
			if (argCount >= 255)
			{
				error("Cannot have more than 255 arguments.");
			}
			argCount++;
		} while (match(TokenType::COMMA));
	}
	consume(TokenType::RIGHT_PAREN, "Expect ')' after function arguments.");
	return argCount;
}

ParseRule *Compiler::getRule(TokenType type)
{
	return &rules[static_cast<uint8_t>(type)];
}

void Compiler::expression()
{
	parsePrecedence(Precedence::ASSIGNMENT);
}

void Compiler::block()
{
	while (!check(TokenType::RIGHT_BRACE) && !check(TokenType::EOF_))
	{
		declaration();
	}

	consume(TokenType::RIGHT_BRACE, "Expect '}' after block.");
}

void Compiler::function(FunctionType type)
{
	auto scope = Scope{m_vm, &m_parser, type, m_current};
	m_current = &scope;
	beginScope();

	// Compile the parameter list.
	consume(TokenType::LEFT_PAREN, "Expect '(' after function name.");
	if (!check(TokenType::RIGHT_PAREN))
	{
		do
		{
			auto paramConstant = parseVariable("Expect parameter name.");
			defineVariable(paramConstant);

			m_current->function->arity++;
			if (m_current->function->arity > 255)
			{
				error("Cannot have more than 255 parameters.");
			}
		}
		while (match(TokenType::COMMA));
	}
	consume(TokenType::RIGHT_PAREN, "Expect ')' after parameter list.");

	// The body.
	consume(TokenType::LEFT_BRACE, "Expect '{' before function body.");
	block();

	// Create the function object.
	auto function = endCompiler();
	emitConstant(Value::Object(function));
}

void Compiler::funDeclaration()
{
	uint32_t global = parseVariable("Expect function name.");
	markInitialized();
	function(FunctionType::FUNCTION);
	defineVariable(global);
}

void Compiler::varDeclaration()
{
	uint32_t global = parseVariable("Expect variable name.");

	if (match(TokenType::EQUAL)) {
		expression();
	} else {
		emitByte(OpCode::NIL);
	}
	consume(TokenType::SEMICOLON, "Expect ';' after variable declaration.");

	defineVariable(global);
}

void Compiler::expressionStatement()
{
	expression();
	consume(TokenType::SEMICOLON, "Expect ';' after expression.");
	emitByte(OpCode::POP);
}

void Compiler::forStatement()
{
	beginScope();

	consume(TokenType::LEFT_PAREN, "Expect '(' after 'for'.");
	// Initializer clause.
	if (match(TokenType::VAR))
	{
		varDeclaration();
	}
	else if (match(TokenType::SEMICOLON))
	{
		// No initializer.
	}
	else
	{
		expressionStatement();
	}

	auto currentBreakJump = m_innermostBreakJump;
	auto surroundingLoopStart = m_innermostLoopStart;
	auto surroundingLoopScopeDepth = m_innermostLoopScopeDepth;

	m_innermostLoopStart = currentChunk()->size();
	m_innermostLoopScopeDepth = m_current->scopeDepth;

	int32_t exitJump = -1;
	if (!match(TokenType::SEMICOLON))
	{
		expression();
		consume(TokenType::SEMICOLON, "Expect ';' after loop condition.");

		// Jump out of the loop if the condition is false.
		exitJump = emitJump(OpCode::JUMP_IF_FALSE);
		emitByte(OpCode::POP); // Condition
	}

	if (!match(TokenType::RIGHT_PAREN))
	{
		auto bodyJump = emitJump(OpCode::JUMP);

		auto incrementStart = currentChunk()->size();
		expression();
		emitByte(OpCode::POP);
		consume(TokenType::RIGHT_PAREN, "Expect ')' after for clauses.");

		emitLoop(m_innermostLoopStart);
		m_innermostLoopStart = incrementStart;
		patchJump(bodyJump);
	}

	statement();

	emitLoop(m_innermostLoopStart);

	if (exitJump != -1)
	{
		patchJump(exitJump);
		emitByte(OpCode::POP);
	}

	// Patch break jump if found.
	if (m_innermostBreakJump != -1)
	{
		patchJump(m_innermostBreakJump);
	}

	m_innermostBreakJump = currentBreakJump;
	m_innermostLoopStart = surroundingLoopStart;
	m_innermostLoopScopeDepth = surroundingLoopScopeDepth;

	endScope();
}

void Compiler::ifStatement()
{
	consume(TokenType::LEFT_PAREN, "Expect '(' after 'if'.");
	expression();
	consume(TokenType::RIGHT_PAREN, "Expect ')' after condition.");

	auto thenJump = emitJump(OpCode::JUMP_IF_FALSE);
	emitByte(OpCode::POP);
	statement();

	auto elseJump = emitJump(OpCode::JUMP);

	patchJump(thenJump);
	emitByte(OpCode::POP);

	if (match(TokenType::ELSE))
		statement();
	patchJump(elseJump);
}

void Compiler::printStatement()
{
	expression();
	consume(TokenType::SEMICOLON, "Expect ';' after value.");
	emitByte(OpCode::PRINT);
}

void Compiler::returnStatement()
{
	if (m_current->type == FunctionType::SCRIPT)
	{
		error("Cannot return from top-level code.");
	}
	if (match(TokenType::SEMICOLON))
	{
		emitReturn();
	}
	else
	{
		expression();
		consume(TokenType::SEMICOLON, "Expect ';' after return value.");
		emitByte(OpCode::RETURN);
	}
}

void Compiler::whileStatement()
{
	auto currentBreakJump = m_innermostBreakJump;
	auto surroundingLoopStart = m_innermostLoopStart;
	auto surroundingLoopScopeDepth = m_innermostLoopScopeDepth;

	m_innermostLoopStart = currentChunk()->size();
	m_innermostLoopScopeDepth = m_current->scopeDepth;

	consume(TokenType::LEFT_PAREN, "Expect '(' after a 'while'.");
	expression();
	consume(TokenType::RIGHT_PAREN, "Expect ')' after condition.");

	auto exitJump = emitJump(OpCode::JUMP_IF_FALSE);

	emitByte(OpCode::POP);
	statement();

	emitLoop(m_innermostLoopStart);

	patchJump(exitJump);
	emitByte(OpCode::POP);

	// Patch break jump if found.
	if (m_innermostBreakJump != -1)
	{
		patchJump(m_innermostBreakJump);
	}

	m_innermostBreakJump = currentBreakJump;
	m_innermostLoopStart = surroundingLoopStart;
	m_innermostLoopScopeDepth = surroundingLoopScopeDepth;
}

void Compiler::continueStatement()
{
	if (m_innermostLoopStart == -1)
	{
		error("Cannot use 'continue' outside of a loop.");
	}

	consume(TokenType::SEMICOLON, "Expect ';' after 'continue'");

	// Discard any locals created inside the loop.
	for (auto i = m_current->localCount -1; i >= 0 &&
	                                        m_current->locals[i].depth > m_innermostLoopScopeDepth; --i)
	{
		emitByte(OpCode::POP);
	}

	// Jump to top of current innermost loop.
	emitLoop(m_innermostLoopStart);
}

void Compiler::breakStatement()
{
	if (m_innermostLoopStart == -1 && !m_insideSwitch)
	{
		error("Cannot use 'break' outside of a loop or a 'switch' statement.");
	}

	consume(TokenType::SEMICOLON, "Expect ';' after 'break'");
	// Discard any locals created inside the loop.
	for (auto i = m_current->localCount -1; i >= 0 &&
	                                        m_current->locals[i].depth > m_innermostLoopScopeDepth; --i)
	{
		emitByte(OpCode::POP);
	}

	// Jump to end of current innermost loop.
	m_innermostBreakJump = emitJump(OpCode::JUMP);
}

void Compiler::switchStatement()
{
	beginScope();

	std::vector<int> breakJumps;
	auto currentBreakJump = m_innermostBreakJump;
	auto surroundingLoopScopeDepth = m_innermostLoopScopeDepth;
	auto insideSwitch = m_insideSwitch;

	m_innermostLoopScopeDepth = m_current->scopeDepth;
	m_insideSwitch = true;

	consume(TokenType::LEFT_PAREN, "Expect '(' after 'switch'.");
	expression();
	consume(TokenType::RIGHT_PAREN, "Expect ')' after value.");
	consume(TokenType::LEFT_BRACE, "Expect '{' before switch cases.");

	// Add a dummy local variale pointing to the switch expression.
	Token dummy;
	dummy.type = TokenType::IDENTIFIER;
	dummy.lexeme = "__switch__";
	dummy.line = m_parser.previous.line;
	addLocal(dummy);
	markInitialized();

	// Compile body of switch.
	int state = 0; // 0: before all cases, 1: before default, 2: after default.
	int previousCaseSkip = -1;
	int nextCaseSkip = -1;

	while (!match(TokenType::RIGHT_BRACE) && !check(TokenType::EOF_))
	{
		if (match(TokenType::CASE) || match(TokenType::DEFAULT))
		{
			auto caseType = m_parser.previous.type;

			if (state == 2)
			{
				error("Cannot have another case or default after the default case.");
			}

			if (state == 1)
			{
				// Fallthrough
				nextCaseSkip = emitJump(OpCode::JUMP);

				// Patch its condition to jump to the next case (this one).
				patchJump(previousCaseSkip);
				emitByte(OpCode::POP);
			}

			if (caseType == TokenType::CASE)
			{
				state = 1;

				// See if the case is equal to the value;
				emitByte(OpCode::DUP);
				expression();

				consume(TokenType::COLON, "Expect ':' after case value.");

				emitByte(OpCode::EQUAL);
				previousCaseSkip = emitJump(OpCode::JUMP_IF_FALSE);

				// Pop the comparison result.
				emitByte(OpCode::POP);

				// Patch the jump to the start of the case.
				if (nextCaseSkip != -1)
					patchJump(nextCaseSkip);
			}
			else
			{
				state = 2;
				consume(TokenType::COLON, "Expect ':' after default.");
				previousCaseSkip = -1;
				if (nextCaseSkip != -1)
					patchJump(nextCaseSkip);

				// The default clause must have an statement after it.
				statement();

				// Look for a break statement;
				if (m_innermostBreakJump != -1)
				{
					breakJumps.push_back(m_innermostBreakJump);
				}
			}
		}
		else
		{
			// Otherwise, it's a statement inside the current case.
			if (state == 0) {
				error("Cannot have statements before any case.");
			}

			statement();

			// Look for a break statement;
			if (m_innermostBreakJump != -1)
			{
				breakJumps.push_back(m_innermostBreakJump);
			}
		}
	}

	// If we ended without a default case, patch its condition jump.
	if (state == 1)
	{
		patchJump(previousCaseSkip);
	}

	// Patch break jump if found.
	if (m_innermostBreakJump != -1)
	{
		for (auto breakJump: breakJumps)
		{
			patchJump(breakJump);
		}
	}

	m_insideSwitch = insideSwitch;
	m_innermostLoopScopeDepth = surroundingLoopScopeDepth;
	m_innermostBreakJump = currentBreakJump;

	endScope();
}

void Compiler::synchronize()
{
	m_parser.panicMode = false;

	while (m_parser.current.type != TokenType::EOF_)
	{
		if (m_parser.previous.type == TokenType::SEMICOLON)
			return;

		switch (m_parser.current.type)
		{
			case TokenType::CLASS:
			case TokenType::FUN:
			case TokenType::VAR:
			case TokenType::FOR:
			case TokenType::IF:
			case TokenType::WHILE:
			case TokenType::SWITCH:
			case TokenType::PRINT:
			case TokenType::RETURN:
				return;

			default:
				// do nothing.
				break;
		}

		advance();
	}
}

void Compiler::declaration()
{
	if (match(TokenType::FUN))
	{
		funDeclaration();
	}
	else if (match(TokenType::VAR))
	{
		varDeclaration();
	}
	else
	{
		statement();
	}

	if (m_parser.panicMode)
		synchronize();
}

void Compiler::statement()
{
	if (match(TokenType::PRINT))
	{
		printStatement();
	}
	else if (match(TokenType::FOR))
	{
		forStatement();
	}
	else if (match(TokenType::IF))
	{
		ifStatement();
	}
	else if (match(TokenType::RETURN))
	{
		returnStatement();
	}
	else if (match(TokenType::WHILE))
	{
		whileStatement();
	}
	else if (match(TokenType::CONTINUE))
	{
		continueStatement();
	}
	else if (match(TokenType::BREAK))
	{
		breakStatement();
	}
	else if (match(TokenType::SWITCH))
	{
		switchStatement();
	}
	else if (match(TokenType::LEFT_BRACE))
	{
		beginScope();
		block();
		endScope();
	}
	else
	{
		expressionStatement();
	}
}

void Compiler::errorAt(const Token &token, const std::string &message)
{
	if (m_parser.panicMode)
		return;
	m_parser.panicMode = true;

	std::cerr << "[line " << token.line << "] Error";
	if (token.type == TokenType::EOF_)
	{
		std::cerr << " at end.";
	}
	else if (token.type == TokenType::ERROR)
	{
		// Nothing
	}
	else
	{
		std::cerr << " at '" << token.lexeme << "'";
	}
	std::cerr << ": " << message << std::endl;
	m_parser.hadError = true;
}

void Compiler::error(const std::string &message)
{
	errorAt(m_parser.previous, message);
}

void Compiler::errorAtCurrent(const std::string &message)
{
	errorAt(m_parser.current, message);
}

#include "compiler.cxx"
