/*
 * ExpressionParser.cpp
 *
 *  Created on: 6 Mar 2020
 *      Author: David
 */

#include "ExpressionParser.h"

#include "GCodeBuffer.h"
#include <Platform/RepRap.h>
#include <Platform/Platform.h>
#include <General/NamedEnum.h>
#include <General/NumericConverter.h>
#include <Hardware/ExceptionHandlers.h>

#include <limits>

#ifdef exists
# undef exists
#endif

constexpr size_t MaxStringExpressionLength = StringLength100;

namespace StackUsage
{
	// The following values are the number of bytes of stack space needed by the corresponding functions and functions they call,
	// not counting other called functions that call CheckStack. They are obtained from file ExpressionParser.su generated by the compiler.
	constexpr uint32_t ParseInternal = 80;
	constexpr uint32_t ParseIdentifierExpression = 240;
	constexpr uint32_t GetObjectValueUsingTableNumber = 48;
}

// These can't be declared locally inside ParseIdentifierExpression because NamedEnum includes static data
NamedEnum(NamedConstant, unsigned int, _false, iterations, line, _null, pi, _result, _true);
NamedEnum(Function, unsigned int, abs, acos, asin, atan, atan2, cos, datetime, degrees, exists, floor, isnan, max, min, mod, radians, random, sin, sqrt, tan);

const char * const InvalidExistsMessage = "invalid 'exists' expression";

ExpressionParser::ExpressionParser(const GCodeBuffer& p_gb, const char *text, const char *textLimit, int p_column) noexcept
	: currentp(text), startp(text), endp(textLimit), gb(p_gb), column(p_column)
{
}

// Evaluate a bracketed expression
void ExpressionParser::ParseExpectKet(ExpressionValue& rslt, bool evaluate, char closingBracket) THROWS(GCodeException)
{
	CheckStack(StackUsage::ParseInternal);
	ParseInternal(rslt, evaluate, 0);
	if (CurrentCharacter() != closingBracket)
	{
		ThrowParseException("expected '%c'", (uint32_t)closingBracket);
	}
	AdvancePointer();
}

// Evaluate an expression. Do not call this one recursively!
ExpressionValue ExpressionParser::Parse(bool evaluate) THROWS(GCodeException)
{
	obsoleteField.Clear();
	ExpressionValue result;
	ParseInternal(result, evaluate, 0);
	if (!obsoleteField.IsEmpty())
	{
		reprap.GetPlatform().MessageF(WarningMessage, "obsolete object model field %s queried\n", obsoleteField.c_str());
	}
	return result;
}

// Evaluate an expression internally, stopping before any binary operators with priority 'priority' or lower
// This is recursive, so avoid allocating large amounts of data on the stack
void ExpressionParser::ParseInternal(ExpressionValue& val, bool evaluate, uint8_t priority) THROWS(GCodeException)
{
	// Lists of binary operators and their priorities
	static constexpr const char *operators = "?^&|!=<>+-*/";				// for multi-character operators <= and >= and != this is the first character
	static constexpr uint8_t priorities[] = { 1, 2, 3, 3, 4, 4, 4, 4, 5, 5, 6, 6 };
	constexpr uint8_t UnaryPriority = 10;									// must be higher than any binary operator priority
	static_assert(ARRAY_SIZE(priorities) == strlen(operators));

	// Start by looking for a unary operator or opening bracket
	SkipWhiteSpace();
	const char c = CurrentCharacter();
	switch (c)
	{
	case '"':
		ParseQuotedString(val);
		break;

	case '-':
		AdvancePointer();
		CheckStack(StackUsage::ParseInternal);
		ParseInternal(val, evaluate, UnaryPriority);
		switch (val.GetType())
		{
		case TypeCode::Int32:
			val.iVal = -val.iVal;		//TODO overflow check
			break;

		case TypeCode::Float:
			val.fVal = -val.fVal;
			break;

		default:
			ThrowParseException("expected numeric value after '-'");
		}
		break;

	case '+':
		AdvancePointer();
		CheckStack(StackUsage::ParseInternal);
		ParseInternal(val, evaluate, UnaryPriority);
		switch (val.GetType())
		{
		case TypeCode::Uint32:
			// Convert enumeration to integer
			val.iVal = (int32_t)val.uVal;
			val.SetType(TypeCode::Int32);
			break;

		case TypeCode::Int32:
		case TypeCode::Float:
			break;

		case TypeCode::DateTime_tc:					// unary + converts a DateTime to a seconds count
			val.iVal = (uint32_t)val.Get56BitValue();
			val.SetType(TypeCode::Int32);
			break;

		default:
			ThrowParseException("expected numeric or enumeration value after '+'");
		}
		break;

	case '#':
		AdvancePointer();
		SkipWhiteSpace();
		if (isalpha(CurrentCharacter()))
		{
			// Probably applying # to an object model array, so optimise by asking the OM for just the length
			CheckStack(StackUsage::ParseIdentifierExpression);
			ParseIdentifierExpression(val, evaluate, true, false);
		}
		else
		{
			CheckStack(StackUsage::ParseInternal);
			ParseInternal(val, evaluate, UnaryPriority);
			if (val.GetType() == TypeCode::CString)
			{
				val.Set((int32_t)strlen(val.sVal));
			}
			else if (val.GetType() == TypeCode::HeapString)
			{
				val.Set((int32_t)val.shVal.GetLength());
			}
			else
			{
				ThrowParseException("expected object model value or string after '#");
			}
		}
		break;

	case '{':
		AdvancePointer();
		ParseExpectKet(val, evaluate, '}');
		break;

	case '(':
		AdvancePointer();
		ParseExpectKet(val, evaluate, ')');
		break;

	case '!':
		AdvancePointer();
		CheckStack(StackUsage::ParseInternal);
		ParseInternal(val, evaluate, UnaryPriority);
		ConvertToBool(val, evaluate);
		val.bVal = !val.bVal;
		break;

	default:
		if (isdigit(c))						// looks like a number
		{
			ParseNumber(val);
		}
		else if (isalpha(c))				// looks like a variable name
		{
			CheckStack(StackUsage::ParseIdentifierExpression);
			ParseIdentifierExpression(val, evaluate, false, false);
		}
		else
		{
			ThrowParseException("expected an expression");
		}
		break;
	}

	// See if it is followed by a binary operator
	do
	{
		SkipWhiteSpace();
		char opChar = CurrentCharacter();
		if (opChar == 0)	// don't pass null to strchr
		{
			return;
		}

		const char * const q = strchr(operators, opChar);
		if (q == nullptr)
		{
			return;
		}
		const size_t index = q - operators;
		const uint8_t opPrio = priorities[index];
		if (opPrio <= priority)
		{
			return;
		}

		AdvancePointer();								// skip the [first] operator character

		// Handle >= and <= and !=
		bool invert = false;
		if (opChar == '!')
		{
			if (CurrentCharacter() != '=')
			{
				ThrowParseException("expected '='");
			}
			invert = true;
			AdvancePointer();
			opChar = '=';
		}
		else if ((opChar == '>' || opChar == '<') && CurrentCharacter() == '=')
		{
			invert = true;
			AdvancePointer();
			opChar ^= ('>' ^ '<');			// change < to > or vice versa
		}

		// Allow == && || as alternatives to = & |
		if ((opChar == '=' || opChar == '&' || opChar == '|') && CurrentCharacter() == opChar)
		{
			AdvancePointer();
		}

		// Handle operators that do not always evaluate their second operand
		switch (opChar)
		{
		case '&':
			ConvertToBool(val, evaluate);
			{
				ExpressionValue val2;
				CheckStack(StackUsage::ParseInternal);
				ParseInternal(val2, evaluate && val.bVal, opPrio);		// get the next operand
				if (val.bVal)
				{
					ConvertToBool(val2, evaluate);
					val.bVal = val2.bVal;
				}
			}
			break;

		case '|':
			ConvertToBool(val, evaluate);
			{
				ExpressionValue val2;
				CheckStack(StackUsage::ParseInternal);
				ParseInternal(val2, evaluate && !val.bVal, opPrio);		// get the next operand
				if (!val.bVal)
				{
					ConvertToBool(val2, evaluate);
					val.bVal = val2.bVal;
				}
			}
			break;

		case '?':
			ConvertToBool(val, evaluate);
			{
				const bool b = val.bVal;
				ExpressionValue val2;
				CheckStack(StackUsage::ParseInternal);
				ParseInternal(((b) ? val : val2), evaluate && b, opPrio);		// get the second operand
				if (CurrentCharacter() != ':')
				{
					ThrowParseException("expected ':'");
				}
				AdvancePointer();
				// We recently checked the stack for a call to ParseInternal, no need to do it again
				ParseInternal(((b) ? val2 : val), evaluate && !b, opPrio - 1);	// get the third operand, which may be a further conditional expression
				return;
			}

		default:
			// Handle binary operators that always evaluate both operands
			{
				ExpressionValue val2;
				CheckStack(StackUsage::ParseInternal);
				ParseInternal(val2, evaluate, opPrio);	// get the next operand
				switch(opChar)
				{
				case '+':
					if (val.GetType() == TypeCode::DateTime_tc)
					{
						if (val2.GetType() == TypeCode::Uint32)
						{
							val.Set56BitValue(val.Get56BitValue() + val2.uVal);
						}
						else if (val2.GetType() == TypeCode::Int32)
						{
							val.Set56BitValue((int64_t)val.Get56BitValue() + val2.iVal);
						}
						else if (evaluate)
						{
							ThrowParseException("invalid operand types");
						}
					}
					else
					{
						BalanceNumericTypes(val, val2, evaluate);
						if (val.GetType() == TypeCode::Float)
						{
							val.fVal += val2.fVal;
							val.param = max(val.param, val2.param);
						}
						else
						{
							val.iVal += val2.iVal;
						}
					}
					break;

				case '-':
					if (val.GetType() == TypeCode::DateTime_tc)
					{
						if (val2.GetType() == TypeCode::DateTime_tc)
						{
							// Difference of two data/times
							val.SetType(TypeCode::Int32);
							val.iVal = (int32_t)(val.Get56BitValue() - val2.Get56BitValue());
						}
						else if (val2.GetType() == TypeCode::Uint32)
						{
							val.Set56BitValue(val.Get56BitValue() - val2.uVal);
						}
						else if (val2.GetType() == TypeCode::Int32)
						{
							val.Set56BitValue((int64_t)val.Get56BitValue() - val2.iVal);
						}
						else if (evaluate)
						{
							ThrowParseException("invalid operand types");
						}
					}
					else
					{
						BalanceNumericTypes(val, val2, evaluate);
						if (val.GetType() == TypeCode::Float)
						{
							val.fVal -= val2.fVal;
							val.param = max(val.param, val2.param);
						}
						else
						{
							val.iVal -= val2.iVal;
						}
					}
					break;

				case '*':
					BalanceNumericTypes(val, val2, evaluate);
					if (val.GetType() == TypeCode::Float)
					{
						val.fVal *= val2.fVal;
						val.param = max(val.param, val2.param);
					}
					else
					{
						val.iVal *= val2.iVal;
					}
					break;

				case '/':
					ConvertToFloat(val, evaluate);
					ConvertToFloat(val2, evaluate);
					val.fVal /= val2.fVal;
					val.param = MaxFloatDigitsDisplayedAfterPoint;
					break;

				case '>':
					BalanceTypes(val, val2, evaluate);
					switch (val.GetType())
					{
					case TypeCode::Int32:
						val.bVal = (val.iVal > val2.iVal);
						break;

					case TypeCode::Float:
						val.bVal = (val.fVal > val2.fVal);
						break;

					case TypeCode::DateTime_tc:
						val.bVal = val.Get56BitValue() > val2.Get56BitValue();
						break;

					case TypeCode::Bool:
						val.bVal = (val.bVal && !val2.bVal);
						break;

					default:
						if (evaluate)
						{
							ThrowParseException("expected numeric or Boolean operands to comparison operator");
						}
						val.bVal = false;
						break;
					}
					val.SetType(TypeCode::Bool);
					if (invert)
					{
						val.bVal = !val.bVal;
					}
					break;

				case '<':
					BalanceTypes(val, val2, evaluate);
					switch (val.GetType())
					{
					case TypeCode::Int32:
						val.bVal = (val.iVal < val2.iVal);
						break;

					case TypeCode::Float:
						val.bVal = (val.fVal < val2.fVal);
						break;

					case TypeCode::DateTime_tc:
						val.bVal = val.Get56BitValue() < val2.Get56BitValue();
						break;

					case TypeCode::Bool:
						val.bVal = (!val.bVal && val2.bVal);
						break;

					default:
						if (evaluate)
						{
							ThrowParseException("expected numeric or Boolean operands to comparison operator");
						}
						val.bVal = false;
						break;
					}
					val.SetType(TypeCode::Bool);
					if (invert)
					{
						val.bVal = !val.bVal;
					}
					break;

				case '=':
					// Before balancing, handle comparisons with null
					if (val.GetType() == TypeCode::None)
					{
						val.bVal = (val2.GetType() == TypeCode::None);
					}
					else if (val2.GetType() == TypeCode::None)
					{
						val.bVal = false;
					}
					else
					{
						BalanceTypes(val, val2, evaluate);
						switch (val.GetType())
						{
						case TypeCode::ObjectModel_tc:
							ThrowParseException("cannot compare objects");

						case TypeCode::Int32:
							val.bVal = (val.iVal == val2.iVal);
							break;

						case TypeCode::Uint32:
							val.bVal = (val.uVal == val2.uVal);
							break;

						case TypeCode::Float:
							val.bVal = (val.fVal == val2.fVal);
							break;

						case TypeCode::DateTime_tc:
							val.bVal = val.Get56BitValue() == val2.Get56BitValue();
							break;

						case TypeCode::Bool:
							val.bVal = (val.bVal == val2.bVal);
							break;

						case TypeCode::CString:
							val.bVal = (strcmp(val.sVal, (val2.GetType() == TypeCode::HeapString) ? val2.shVal.Get().Ptr() : val2.sVal) == 0);
							break;

						case TypeCode::HeapString:
							val.bVal = (strcmp(val.shVal.Get().Ptr(), (val2.GetType() == TypeCode::HeapString) ? val2.shVal.Get().Ptr() : val2.sVal) == 0);
							break;

						default:
							if (evaluate)
							{
								ThrowParseException("unexpected operand type to equality operator");
							}
							val.bVal = false;
							break;
						}
					}
					val.SetType(TypeCode::Bool);
					if (invert)
					{
						val.bVal = !val.bVal;
					}
					break;

				case '^':
					StringConcat(val, val2);
					break;
				}
			}
		}
	} while (true);
}

// Concatenate val1 and val2 and assign the result to val1
// This is written as a separate function because it needs a temporary string buffer, and its caller is recursive. Its declaration must be declared 'noinline'.
/*static*/ void  ExpressionParser::StringConcat(ExpressionValue &val, ExpressionValue &val2) noexcept
{
    String<MaxStringExpressionLength> str;
    val.AppendAsString(str.GetRef());
    val2.AppendAsString(str.GetRef());
    StringHandle sh(str.c_str());
    val.Set(sh);
}

bool ExpressionParser::ParseBoolean() THROWS(GCodeException)
{
	ExpressionValue val = Parse();
	ConvertToBool(val, true);
	return val.bVal;
}

float ExpressionParser::ParseFloat() THROWS(GCodeException)
{
	ExpressionValue val = Parse();
	ConvertToFloat(val, true);
	return val.fVal;
}

int32_t ExpressionParser::ParseInteger() THROWS(GCodeException)
{
	ExpressionValue val = Parse();
	switch (val.GetType())
	{
	case TypeCode::Int32:
		return val.iVal;

	case TypeCode::Uint32:
		if (val.uVal > (uint32_t)std::numeric_limits<int32_t>::max())
		{
			ThrowParseException("unsigned integer too large");
		}
		return (int32_t)val.uVal;

	default:
		ThrowParseException("expected integer value");
	}
}

uint32_t ExpressionParser::ParseUnsigned() THROWS(GCodeException)
{
	ExpressionValue val = Parse();
	switch (val.GetType())
	{
	case TypeCode::Uint32:
		return val.uVal;

	case TypeCode::Int32:
		if (val.iVal >= 0)
		{
			return (uint32_t)val.iVal;
		}
		ThrowParseException("value must be non-negative");

	default:
		ThrowParseException("expected non-negative integer value");
	}
}

DriverId ExpressionParser::ParseDriverId() THROWS(GCodeException)
{
	ExpressionValue val = Parse();
	ConvertToDriverId(val, true);
	return val.GetDriverIdValue();
}

void ExpressionParser::ParseArray(size_t& length, function_ref<void(size_t index) THROWS(GCodeException)> processElement) THROWS(GCodeException)
{
	size_t numElements = 0;
	AdvancePointer();					// skip the '{'
	while (numElements < length)
	{
		processElement(numElements);
		++numElements;
		if (CurrentCharacter() != EXPRESSION_LIST_SEPARATOR)
		{
			break;
		}
		if (numElements == length)
		{
			ThrowParseException("Array too long");
		}
		AdvancePointer();				// skip the ','
	}
	if (CurrentCharacter() != '}')
	{
		ThrowParseException("Expected '}'");
	}
	AdvancePointer();					// skip the '{'
	length = numElements;
}

// This is called when we expect a non-empty float array parameter and we have encountered (but not skipped) '{'
void ExpressionParser::ParseFloatArray(float arr[], size_t& length) THROWS(GCodeException)
{
	ParseArray(length, [this, &arr](size_t index) { arr[index] = ParseFloat(); });
}

void ExpressionParser::ParseIntArray(int32_t arr[], size_t& length) THROWS(GCodeException)
{
	ParseArray(length, [this, &arr](size_t index) { arr[index] = ParseInteger(); });
}

void ExpressionParser::ParseUnsignedArray(uint32_t arr[], size_t& length) THROWS(GCodeException)
{
	ParseArray(length, [this, &arr](size_t index) { arr[index] = ParseUnsigned(); });
}

void ExpressionParser::ParseDriverIdArray(DriverId arr[], size_t& length) THROWS(GCodeException)
{
	ParseArray(length, [this, &arr](size_t index) { arr[index] = ParseDriverId(); });
}

void ExpressionParser::BalanceNumericTypes(ExpressionValue& val1, ExpressionValue& val2, bool evaluate) const THROWS(GCodeException)
{
	// First convert any Uint64 or Uint32 operands to float
	if (val1.GetType() == TypeCode::Uint64 || val1.GetType() == TypeCode::Uint32)
	{
		ConvertToFloat(val1, evaluate);
	}
	if (val2.GetType() == TypeCode::Uint64 || val2.GetType() == TypeCode::Uint32)
	{
		ConvertToFloat(val2, evaluate);
	}

	if (val1.GetType() == TypeCode::Float)
	{
		ConvertToFloat(val2, evaluate);						// both are now float
	}
	else if (val2.GetType() == TypeCode::Float)
	{
		ConvertToFloat(val1, evaluate);						// both are now float
	}
	else if (val1.GetType() != TypeCode::Int32 || val2.GetType() != TypeCode::Int32)
	{
		if (evaluate)
		{
			ThrowParseException("expected numeric operands");
		}
		val1.Set((int32_t)0);
		val2.Set((int32_t)0);
	}
}

// Return true if the specified type has no literals and should therefore be converted to string when comparing with another value that is not of the same type.
// We don't need to handle Port and UniqueId types here because we convent them to string before calling this.
/*static*/ bool ExpressionParser::TypeHasNoLiterals(TypeCode t) noexcept
{
	return t == TypeCode::Char || t == TypeCode::DateTime_tc || t == TypeCode::IPAddress_tc || t == TypeCode::MacAddress_tc || t == TypeCode::DriverId_tc;
}

// Balance types for a comparison operator
void ExpressionParser::BalanceTypes(ExpressionValue& val1, ExpressionValue& val2, bool evaluate) THROWS(GCodeException)
{
	// First convert any Uint64 or Uint32 operands to float
	if (val1.GetType() == TypeCode::Uint64 || val1.GetType() == TypeCode::Uint32)
	{
		ConvertToFloat(val1, evaluate);
	}
	if (val2.GetType() == TypeCode::Uint64 || val2.GetType() == TypeCode::Uint32)
	{
		ConvertToFloat(val2, evaluate);
	}

	// Convert any port or unique ID values to string
	if (val1.GetType() == TypeCode::Port || val1.GetType() == TypeCode::UniqueId_tc)
	{
		ConvertToString(val1, evaluate);
	}
	if (val2.GetType() == TypeCode::Port || val2.GetType() == TypeCode::UniqueId_tc)
	{
		ConvertToString(val2, evaluate);
	}

	if ((val1.GetType() == val2.GetType()) || (val1.IsStringType() && val2.IsStringType()))			// handle the common case first
	{
		// nothing to do
	}
	else if (val1.GetType() == TypeCode::Float)
	{
		ConvertToFloat(val2, evaluate);
	}
	else if (val2.GetType() == TypeCode::Float)
	{
		ConvertToFloat(val1, evaluate);
	}
	else if (val2.IsStringType() && TypeHasNoLiterals(val1.GetType()))
	{
		ConvertToString(val1, evaluate);
	}
	else if (val1.IsStringType() && TypeHasNoLiterals(val2.GetType()))
	{
		ConvertToString(val2, evaluate);
	}
	else
	{
		if (evaluate)
		{
			ThrowParseException("cannot convert operands to same type");
		}
		val1.Set((int32_t)0);
		val2.Set((int32_t)0);
	}
}

void ExpressionParser::ConvertToFloat(ExpressionValue& val, bool evaluate) const THROWS(GCodeException)
{
	switch (val.GetType())
	{
	case TypeCode::Uint32:
		val.SetType(TypeCode::Float);
		val.fVal = (float)val.uVal;
		val.param = 1;
		break;

	case TypeCode::Uint64:
		val.SetType(TypeCode::Float);
		val.fVal = (float)val.Get56BitValue();
		val.param = 1;
		break;

	case TypeCode::Int32:
		val.fVal = (float)val.iVal;
		val.SetType(TypeCode::Float);
		val.param = 1;
		break;

	case TypeCode::Float:
		break;

	default:
		if (evaluate)
		{
			ThrowParseException("expected numeric operand");
		}
		val.Set(0.0f, 1);
	}
}

void ExpressionParser::ConvertToBool(ExpressionValue& val, bool evaluate) const THROWS(GCodeException)
{
	if (val.GetType() != TypeCode::Bool)
	{
		if (evaluate)
		{
			ThrowParseException("expected Boolean operand");
		}
		val.Set(false);
	}
}

void ExpressionParser::ConvertToString(ExpressionValue& val, bool evaluate) noexcept
{
	if (!val.IsStringType())
	{
		if (evaluate)
		{
			String<MaxStringExpressionLength> str;
			val.AppendAsString(str.GetRef());
			StringHandle sh(str.c_str());
			val.Set(sh);
		}
		else
		{
			val.Set("");
		}
	}
}

void ExpressionParser::ConvertToDriverId(ExpressionValue& val, bool evaluate) const THROWS(GCodeException)
{
	switch (val.GetType())
	{
	case TypeCode::DriverId_tc:
		break;

	case TypeCode::Int32:
#if SUPPORT_CAN_EXPANSION
		val.Set(DriverId(0, val.uVal));
#else
		val.Set(DriverId(val.uVal));
#endif
		break;

	case TypeCode::Float:
		{
			const float f10val = 10.0 * val.fVal;
			const int32_t ival = lrintf(f10val);
#if SUPPORT_CAN_EXPANSION
			if (ival >= 0 && fabsf(f10val - (float)ival) <= 0.002)
			{
				val.Set(DriverId(ival/10, ival % 10));
			}
#else
			if (ival >= 0 && ival < 10 && fabsf(f10val - (float)ival) <= 0.002)
			{
				val.Set(DriverId(ival % 10));
			}
#endif
			else
			{
				ThrowParseException("invalid driver ID");
			}
		}
		break;

	default:
		if (evaluate)
		{
			ThrowParseException("expected driver ID");
		}
	}
}

void ExpressionParser::SkipWhiteSpace() noexcept
{
	char c;
	while ((c = CurrentCharacter()) == ' ' || c == '\t')
	{
		AdvancePointer();
	}
}

void ExpressionParser::CheckForExtraCharacters() THROWS(GCodeException)
{
	SkipWhiteSpace();
	if (CurrentCharacter() != 0)
	{
		ThrowParseException("Unexpected characters after expression");
	}
}

// Parse a number. The initial character of the string is a decimal digit.
void ExpressionParser::ParseNumber(ExpressionValue& rslt) noexcept
{
	NumericConverter conv;
	conv.Accumulate(CurrentCharacter(), NumericConverter::AcceptSignedFloat | NumericConverter::AcceptHex, [this]()->char { AdvancePointer(); return CurrentCharacter(); });	// must succeed because CurrentCharacter is a decimal digit

	if (conv.FitsInInt32())
	{
		rslt.Set(conv.GetInt32());
	}
	else
	{
		rslt.Set(conv.GetFloat(), constrain<unsigned int>(conv.GetDigitsAfterPoint(), 1, MaxFloatDigitsDisplayedAfterPoint));
	}
}

// Parse an identifier expression
// If 'evaluate' is false then the object model path may not exist, in which case we must ignore error that and parse it all anyway
// This means we can use expressions such as: if {a.b == null || a.b.c == 1}
// *** This function is recursive, so keep its stack usage low!
void ExpressionParser::ParseIdentifierExpression(ExpressionValue& rslt, bool evaluate, bool applyLengthOperator, bool applyExists) THROWS(GCodeException)
{
	if (!isalpha(CurrentCharacter()))
	{
		ThrowParseException("expected an identifier");
	}

	String<MaxVariableNameLength> id;
	ObjectExplorationContext context(&gb, applyLengthOperator, applyExists, gb.GetLineNumber(), GetColumn());

	// Loop parsing identifiers and index expressions
	// When we come across an index expression, evaluate it, add it to the context, and place a marker in the identifier string.
	char c;
	while (isalpha((c = CurrentCharacter())) || isdigit(c) || c == '_' || c == '.' || c == '[')
	{
		AdvancePointer();
		if (c == '[')
		{
			ExpressionValue index;
			CheckStack(StackUsage::ParseInternal);
			ParseInternal(index, evaluate, 0);
			if (CurrentCharacter() != ']')
			{
				ThrowParseException("expected ']'");
			}
			if (index.GetType() != TypeCode::Int32)
			{
				if (evaluate)
				{
					ThrowParseException("expected integer expression");
				}
				index.Set((int32_t)0);
			}
			AdvancePointer();										// skip the ']'
			context.ProvideIndex(index.iVal);
			c = '^';												// add the marker
		}
		if (id.cat(c))
		{
			ThrowParseException("variable name too long");;
		}
	}

	// Check for the names of constants
	NamedConstant whichConstant(id.c_str());
	if (whichConstant.IsValid())
	{
		if (context.WantExists())
		{
			ThrowParseException(InvalidExistsMessage);
		}

		switch (whichConstant.RawValue())
		{
		case NamedConstant::_true:
			rslt.Set(true);
			return;

		case NamedConstant::_false:
			rslt.Set(false);
			return;

		case NamedConstant::_null:
			rslt.Set(nullptr);
			return;

		case NamedConstant::pi:
			rslt.Set(Pi);
			return;

		case NamedConstant::iterations:
			{
				const int32_t v = gb.CurrentFileMachineState().GetIterations();
				if (v < 0)
				{
					ThrowParseException("'iterations' used when not inside a loop");
				}
				rslt.Set(v);
			}
			return;

		case NamedConstant::_result:
			{
				int32_t res;
				switch (gb.GetLastResult())
				{
				case GCodeResult::ok:
					res = 0;
					break;

				case GCodeResult::warning:
				case GCodeResult::warningNotSupported:
					res = 1;
					break;

				default:
					res = 2;
					break;
				}
				rslt.Set(res);
			}
			return;

		case NamedConstant::line:
			rslt.Set((int32_t)gb.GetLineNumber());
			return;

		default:
			THROW_INTERNAL_ERROR;
		}
	}

	// Check whether it is a function call
	SkipWhiteSpace();
	if (CurrentCharacter() == '(')
	{
		// It's a function call
		if (context.WantExists())
		{
			ThrowParseException(InvalidExistsMessage);
		}

		const Function func(id.c_str());
		if (!func.IsValid())
		{
			ThrowParseException("unknown function");
		}

		AdvancePointer();
		if (func == Function::exists)
		{
			CheckStack(StackUsage::ParseIdentifierExpression);
			ParseIdentifierExpression(rslt, evaluate, false, true);
		}
		else
		{
			CheckStack(StackUsage::ParseInternal);
			ParseInternal(rslt, evaluate, 0);					// evaluate the first operand

			switch (func.RawValue())
			{
			case Function::abs:
				switch (rslt.GetType())
				{
				case TypeCode::Int32:
					rslt.iVal = labs(rslt.iVal);
					break;

				case TypeCode::Float:
					rslt.fVal = fabsf(rslt.fVal);
					break;

				default:
					if (evaluate)
					{
						ThrowParseException("expected numeric operand");
					}
					rslt.Set((int32_t)0);
				}
				break;

			case Function::sin:
				ConvertToFloat(rslt, evaluate);
				rslt.fVal = sinf(rslt.fVal);
				rslt.param = MaxFloatDigitsDisplayedAfterPoint;
				break;

			case Function::cos:
				ConvertToFloat(rslt, evaluate);
				rslt.fVal = cosf(rslt.fVal);
				rslt.param = MaxFloatDigitsDisplayedAfterPoint;
				break;

			case Function::tan:
				ConvertToFloat(rslt, evaluate);
				rslt.fVal = tanf(rslt.fVal);
				rslt.param = MaxFloatDigitsDisplayedAfterPoint;
				break;

			case Function::asin:
				ConvertToFloat(rslt, evaluate);
				rslt.fVal = asinf(rslt.fVal);
				rslt.param = MaxFloatDigitsDisplayedAfterPoint;
				break;

			case Function::acos:
				ConvertToFloat(rslt, evaluate);
				rslt.fVal = acosf(rslt.fVal);
				rslt.param = MaxFloatDigitsDisplayedAfterPoint;
				break;

			case Function::atan:
				ConvertToFloat(rslt, evaluate);
				rslt.fVal = atanf(rslt.fVal);
				rslt.param = MaxFloatDigitsDisplayedAfterPoint;
				break;

			case Function::atan2:
				{
					ConvertToFloat(rslt, evaluate);
					SkipWhiteSpace();
					if (CurrentCharacter() != ',')
					{
						ThrowParseException("expected ','");
					}
					AdvancePointer();
					SkipWhiteSpace();
					ExpressionValue nextOperand;
					// We recently checked the stack for a call to ParseInternal, no need to do it again
					ParseInternal(nextOperand, evaluate, 0);
					ConvertToFloat(nextOperand, evaluate);
					rslt.fVal = atan2f(rslt.fVal, nextOperand.fVal);
					rslt.param = MaxFloatDigitsDisplayedAfterPoint;
				}
				break;

			case Function::degrees:
				ConvertToFloat(rslt, evaluate);
				rslt.fVal = rslt.fVal * RadiansToDegrees;
				rslt.param = MaxFloatDigitsDisplayedAfterPoint;
				break;

			case Function::radians:
				ConvertToFloat(rslt, evaluate);
				rslt.fVal = rslt.fVal * DegreesToRadians;
				rslt.param = MaxFloatDigitsDisplayedAfterPoint;
				break;

			case Function::sqrt:
				ConvertToFloat(rslt, evaluate);
				rslt.fVal = fastSqrtf(rslt.fVal);
				rslt.param = MaxFloatDigitsDisplayedAfterPoint;
				break;

			case Function::isnan:
				ConvertToFloat(rslt, evaluate);
				rslt.SetType(TypeCode::Bool);
				rslt.bVal = (std::isnan(rslt.fVal) != 0);
				break;

			case Function::floor:
				{
					ConvertToFloat(rslt, evaluate);
					const float f = floorf(rslt.fVal);
					if (f <= (float)std::numeric_limits<int32_t>::max() && f >= (float)std::numeric_limits<int32_t>::min())
					{
						rslt.SetType(TypeCode::Int32);
						rslt.iVal = (int32_t)f;
					}
					else
					{
						rslt.fVal = f;
					}
				}
				break;

			case Function::mod:
				{
					SkipWhiteSpace();
					if (CurrentCharacter() != ',')
					{
						ThrowParseException("expected ','");
					}
					AdvancePointer();
					SkipWhiteSpace();
					ExpressionValue nextOperand;
					// We recently checked the stack for a call to ParseInternal, no need to do it again
					ParseInternal(nextOperand, evaluate, 0);
					BalanceNumericTypes(rslt, nextOperand, evaluate);
					if (rslt.GetType() == TypeCode::Float)
					{
						rslt.fVal = fmod(rslt.fVal, nextOperand.fVal);
					}
					else if (nextOperand.iVal == 0)
					{
						rslt.iVal = 0;
					}
					else
					{
						rslt.iVal %= nextOperand.iVal;
					}
				}
				break;

			case Function::max:
				for (;;)
				{
					SkipWhiteSpace();
					if (CurrentCharacter() != ',')
					{
						break;
					}
					AdvancePointer();
					SkipWhiteSpace();
					ExpressionValue nextOperand;
					// We recently checked the stack for a call to ParseInternal, no need to do it again
					ParseInternal(nextOperand, evaluate, 0);
					BalanceNumericTypes(rslt, nextOperand, evaluate);
					if (rslt.GetType() == TypeCode::Float)
					{
						rslt.fVal = max<float>(rslt.fVal, nextOperand.fVal);
						rslt.param = max(rslt.param, nextOperand.param);
					}
					else
					{
						rslt.iVal = max<int32_t>(rslt.iVal, nextOperand.iVal);
					}
				}
				break;

			case Function::min:
				for (;;)
				{
					SkipWhiteSpace();
					if (CurrentCharacter() != ',')
					{
						break;
					}
					AdvancePointer();
					SkipWhiteSpace();
					ExpressionValue nextOperand;
					// We recently checked the stack for a call to ParseInternal, no need to do it again
					ParseInternal(nextOperand, evaluate, 0);
					BalanceNumericTypes(rslt, nextOperand, evaluate);
					if (rslt.GetType() == TypeCode::Float)
					{
						rslt.fVal = min<float>(rslt.fVal, nextOperand.fVal);
						rslt.param = max(rslt.param, nextOperand.param);
					}
					else
					{
						rslt.iVal = min<int32_t>(rslt.iVal, nextOperand.iVal);
					}
				}
				break;

			case Function::random:
				{
					uint32_t limit;
					if (rslt.GetType() == TypeCode::Uint32)
					{
						limit = rslt.uVal;
					}
					else if (rslt.GetType() == TypeCode::Int32 && rslt.iVal > 0)
					{
						limit = rslt.iVal;
					}
					else
					{
						ThrowParseException("expected positive integer");
					}
					rslt.Set((int32_t)random(limit));
				}
				break;

			case Function::datetime:
				{
					uint64_t val;
					switch (rslt.GetType())
					{
					case TypeCode::Int32:
						val = (uint64_t)max<uint32_t>(rslt.iVal, 0);
						break;

					case TypeCode::Uint32:
						val = (uint64_t)rslt.uVal;
						break;

					case TypeCode::Uint64:
					case TypeCode::DateTime_tc:
						val = rslt.Get56BitValue();
						break;

					case TypeCode::CString:
						val = ParseDateTime(rslt.sVal);
						break;

					case TypeCode::HeapString:
						val = ParseDateTime(rslt.shVal.Get().Ptr());
						break;

					default:
						ThrowParseException("can't convert value to DateTime");
					}
					rslt.SetType(TypeCode::DateTime_tc);
					rslt.Set56BitValue(val);
				}
				break;

			default:
				THROW_INTERNAL_ERROR;
			}
		}

		SkipWhiteSpace();
		if (CurrentCharacter() != ')')
		{
			ThrowParseException("expected ')'");
		}
		AdvancePointer();
		return;
	}

	// If we are not evaluating then the object expression doesn't have to exist, so don't retrieve it because that might throw an error
	if (evaluate)
	{
		// Check for a parameter, local or global variable
		if (StringStartsWith(id.c_str(), "param."))
		{
			GetVariableValue(rslt, &gb.GetVariables(), id.c_str() + strlen("param."), true, applyExists);
			return;
		}

		if (StringStartsWith(id.c_str(), "global."))
		{
			auto vars = reprap.GetGlobalVariablesForReading();
			GetVariableValue(rslt, vars.Ptr(), id.c_str() + strlen("global."), false, applyExists);
			return;
		}

		if (StringStartsWith(id.c_str(), "var."))
		{
			GetVariableValue(rslt, &gb.GetVariables(), id.c_str() + strlen("var."), false, applyExists);
			return;
		}

		// "exists(var)", "exists(param)" and "exists(global)" should return true.
		// "exists(global)" will anyway because "global" is a root key in the object model. Handle the other two here.
		if (applyExists && (strcmp(id.c_str(), "param") == 0 || strcmp(id.c_str(), "var") == 0))
		{
			rslt.Set(true);
			return;
		}

		// Else assume an object model value
		CheckStack(StackUsage::GetObjectValueUsingTableNumber);
		rslt = reprap.GetObjectValueUsingTableNumber(context, nullptr, id.c_str(), 0);
		if (context.ObsoleteFieldQueried() && obsoleteField.IsEmpty())
		{
			obsoleteField.copy(id.c_str());
		}
		return;
	}
	rslt.Set(nullptr);
}

// Parse a string to a DateTime
time_t ExpressionParser::ParseDateTime(const char *s) const THROWS(GCodeException)
{
	tm timeInfo;
	if (SafeStrptime(s, "%Y-%m-%dT%H:%M:%S", &timeInfo) == nullptr)
	{
		ThrowParseException("string is not a valid date and time");
	}
	return mktime(&timeInfo);
}

// Get the value of a variable
void ExpressionParser::GetVariableValue(ExpressionValue& rslt, const VariableSet *vars, const char *name, bool parameter, bool wantExists) THROWS(GCodeException)
{
	const Variable* var = vars->Lookup(name);
	if (wantExists)
	{
		rslt.Set(var != nullptr);
		return;
	}

	if (var != nullptr && (!parameter || var->GetScope() < 0))
	{
		rslt = var->GetValue();
		return;
	}

	ThrowParseException((parameter) ? "unknown parameter '%s'" : "unknown variable '%s'", name);
}

// Parse a quoted string, given that the current character is double-quote
// This is almost a copy of InternalGetQuotedString in class StringParser
void ExpressionParser::ParseQuotedString(ExpressionValue& rslt) THROWS(GCodeException)
{
	String<MaxStringExpressionLength> str;
	AdvancePointer();
	while (true)
	{
		char c = CurrentCharacter();
		AdvancePointer();
		if (c < ' ')
		{
			ThrowParseException("control character in string");
		}
		if (c == '"')
		{
			if (CurrentCharacter() != c)
			{
				StringHandle sh(str.c_str());
				rslt.Set(sh);
				return;
			}
			AdvancePointer();
		}
		else if (c == '\'')
		{
			if (isalpha(CurrentCharacter()))
			{
				// Single quote before an alphabetic character forces that character to lower case
				c = tolower(CurrentCharacter());
				AdvancePointer();
			}
			else if (CurrentCharacter() == c)
			{
				// Two quotes are used to represent one
				AdvancePointer();
			}
		}
		if (str.cat(c))
		{
			ThrowParseException("string too long");
		}
	}
}

// Return the current character, or 0 if we have run out of string
char ExpressionParser::CurrentCharacter() const noexcept
{
	return (currentp < endp) ? *currentp : 0;
}

int ExpressionParser::GetColumn() const noexcept
{
	return (column < 0) ? column : (currentp - startp) + column;
}

void ExpressionParser::ThrowParseException(const char *str) const THROWS(GCodeException)
{
	throw GCodeException(gb.GetLineNumber(), GetColumn(), str);
}

void ExpressionParser::ThrowParseException(const char *str, const char *param) const THROWS(GCodeException)
{
	throw GCodeException(gb.GetLineNumber(), GetColumn(), str, param);
}

void ExpressionParser::ThrowParseException(const char *str, uint32_t param) const THROWS(GCodeException)
{
	throw GCodeException(gb.GetLineNumber(), GetColumn(), str, param);
}

// Call this before making a recursive call, or before calling a function that needs a lot of stack from a recursive function
void ExpressionParser::CheckStack(uint32_t calledFunctionStackUsage) const THROWS(GCodeException)
{
	const char *_ecv_array stackPtr = (const char *_ecv_array)GetStackPointer();
	const char *_ecv_array stackLimit = (const char *_ecv_array)TaskBase::GetCurrentTaskStackBase();	// get the base (lowest available address) of the stack for this task

	//debugPrintf("Margin: %u\n", stackPtr - stackLimit);
	if (stackLimit + calledFunctionStackUsage + (StackUsage::Throw + StackUsage::Margin) <= stackPtr)
	{
		return;				// we have enough stack
	}

	// The stack is in danger of overflowing. Throw an exception if we have enough stack to do so (ideally, this should always be the case)
	if (stackLimit + StackUsage::Throw <= stackPtr)
	{
		throw GCodeException(gb.GetLineNumber(), GetColumn(), "Expression nesting too deep");
	}

	// Not enough stack left to throw an exception
	SoftwareReset(SoftwareResetReason::stackOverflow, (const uint32_t *)stackPtr);
}

// End
