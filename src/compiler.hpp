//
// Created by juanb on 30/09/2018.
//

#ifndef CPPLOX_COMPILER_HPP
#define CPPLOX_COMPILER_HPP

#include <string>
#include <memory>
#include <array>

#include "chunk.hpp"
#include "scanner.hpp"
#include "common.hpp"

class Compiler;
class VM;

struct Parser
{
	Token current;
	Token previous;
	bool hadError{false};
	bool panicMode{false};
};

struct Local
{
	Token name;
	int depth{};
};

enum class FunctionType : uint8_t
{
	FUNCTION,
	SCRIPT
};

struct Scope
{
	Scope* enclosing{nullptr};
	ObjFunction* function{nullptr};
	FunctionType type{};

	std::array<Local, MAX_LOCALS> locals;
	int localCount{0};
	int scopeDepth{0};

	Scope(VM* vm, Parser* parser, FunctionType type, Scope* enclosing);
};

enum class Precedence : uint8_t
{
	NONE,
	ASSIGNMENT,    // =
	OR,            // or
	AND,        // and
	EQUALITY,    // == !=
	COMPARISON,    // < > <= >=
	TERM,        // + -
	FACTOR,    // * /
	UNARY,        // ! - +
	CALL,        // . () []
	PRIMARY
};

typedef void (Compiler::*ParseFn)(bool);

struct ParseRule
{
	ParseFn prefix;
	ParseFn infix;
	Precedence precedence;
};

class VM;

class Compiler
{
	friend struct Scope;
public:
	ObjFunction* compile(VM *vm, std::string_view source);
	Chunk *currentChunk();

private:
	void init();
	void advance();
	void consume(TokenType type, const std::string &message);
	bool check(TokenType type);
	bool match(TokenType type);
	bool matchCompound();
	void emitByte(uint8_t byte);
	void emitBytes(uint8_t byte1, uint8_t byte2);
	void emitShort(uint16_t val);
	void emitLong(uint32_t val);
	void emitLoop(uint32_t loopStart);
	uint32_t emitJump(uint8_t instruction);
	void emitReturn();
	uint32_t makeConstant(const Value &value);
	void emitConstant(const Value &value);
	void patchJump(uint32_t offset);
	ObjFunction* endCompiler();
	void beginScope();
	void endScope();
	void binary(bool);
	void call(bool);
	void literal(bool);
	void grouping(bool);
	void number(bool);
	void or_(bool);
	void and_(bool);
	void string(bool);
	void namedVariable(const Token &name, bool canAssign);
	void variable(bool canAssign);
	void unary(bool);
	void parsePrecedence(Precedence precedence);
	uint32_t identifierConstant(const Token &token);
	int32_t resolveLocal(Scope* scope, const Token &name);
	void addLocal(const Token &name);
	void declareVariable();
	uint32_t parseVariable(const std::string &message);
	void markInitialized();
	void defineVariable(uint32_t global);
	uint8_t argumentList();
	static ParseRule *getRule(TokenType type);
	void expression();
	void block();
	void function(FunctionType type);
	void funDeclaration();
	void varDeclaration();
	void expressionStatement();
	void forStatement();
	void ifStatement();
	void printStatement();
	void returnStatement();
	void whileStatement();
	void continueStatement();
	void breakStatement();
	void switchStatement();
	void synchronize();
	void declaration();
	void statement();

	void errorAt(const Token &token, const std::string &message);
	void error(const std::string &message);
	void errorAtCurrent(const std::string &message);

	Scanner m_scanner;
	Parser m_parser;
	Scope *m_current = nullptr;
	VM *m_vm = nullptr;

	int m_innermostBreakJump = -1;
	int m_innermostLoopStart = -1;
	int m_innermostLoopScopeDepth = 0;
	bool m_insideSwitch = false;

	static ParseRule rules[];
};

#endif //CPPLOX_COMPILER_HPP
