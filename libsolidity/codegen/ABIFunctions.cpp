/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * @author Christian <chris@ethereum.org>
 * @date 2017
 * Routines that generate JULIA code related to ABI encoding, decoding and type conversions.
 */

#include <libsolidity/codegen/ABIFunctions.h>

#include <libdevcore/Whiskers.h>

#include <libsolidity/ast/AST.h>

using namespace std;
using namespace dev;
using namespace dev::solidity;

ABIFunctions::~ABIFunctions()
{
	// This throws an exception and thus might cause immediate termination, but hey,
	// it's a failed assertion anyway :-)
//TODO	solAssert(m_requestedFunctions.empty(), "Forgot to call ``requestedFunctions()``.");
}

string ABIFunctions::tupleEncoder(
	TypePointers const& _givenTypes,
	TypePointers const& _tos,
	bool _encodeAsLibraryTypes
)
{
	// stack: <$value0> <$value1> ... <$value(n-1)> <$headStart>

	string encoder = R"(
		let dynFree := add($headStart, <headSize>)
		<#values>
			dynFree := <abiEncode>(
				$value<i>,
				$headStart,
				add($headStart, <headPos>),
				dynFree
			)
		</values>
		$value0 := dynFree
	)";
	solAssert(!_givenTypes.empty(), "");
	size_t headSize = 0;
	for (auto const& t: _tos)
	{
		solAssert(t->calldataEncodedSize() > 0, "");
		headSize += t->calldataEncodedSize();
	}
	Whiskers templ(encoder);
	templ("headSize", to_string(headSize));
	vector<Whiskers::StringMap> values(_givenTypes.size());
	map<string, pair<TypePointer, TypePointer>> requestedEncodingFunctions;
	size_t headPos = 0;
	for (size_t i = 0; i < _givenTypes.size(); ++i)
	{
		solUnimplementedAssert(_givenTypes[i]->sizeOnStack() == 1, "");
		solAssert(_givenTypes[i], "");
		solAssert(_tos[i], "");
		values[i]["fromTypeID"] = _givenTypes[i]->identifier();
		values[i]["toTypeID"] = _tos[i]->identifier();
		values[i]["i"] = to_string(i);
		values[i]["headPos"] = to_string(headPos);
		values[i]["abiEncode"] =
			abiEncodingFunction(*_givenTypes[i], *_tos[i], _encodeAsLibraryTypes);
		headPos += _tos[i]->calldataEncodedSize();
	}
	solAssert(headPos == headSize, "");
	templ("values", values);

	return templ.render();
}

string ABIFunctions::requestedFunctions()
{
	string result;
	for (auto const& f: m_requestedFunctions)
		result += f.second;
	m_requestedFunctions.clear();
	return result;
}

string ABIFunctions::cleanupFunction(Type const& _type, bool _revertOnFailure)
{
	string functionName = string("cleanup_") + (_revertOnFailure ? "revert_" : "assert_") + _type.identifier();
	return createFunction(functionName, [&]() {
		Whiskers templ(R"(
			function <functionName>(value) -> cleaned {
				<body>
			}
		)");
		templ("functionName", functionName);
		switch (_type.category())
		{
		case Type::Category::Integer:
		{
			IntegerType const& type = dynamic_cast<IntegerType const&>(_type);
			if (type.numBits() == 256)
				templ("body", "cleaned := value");
			else if (type.isSigned())
				templ("body", "cleaned := signextend(" + to_string(type.numBits() / 8 - 1) + ", value)");
			else
				templ("body", "cleaned := and(value, " + toCompactHexWithPrefix((u256(1) << type.numBits()) - 1) + ")");
			break;
		}
		case Type::Category::RationalNumber:
			templ("body", "cleaned := value");
			break;
		case Type::Category::Bool:
			templ("body", "cleaned := iszero(iszero(value))");
			break;
		case Type::Category::FixedPoint:
			solUnimplemented("Fixed point types not implemented.");
			break;
		case Type::Category::Array:
			solAssert(false, "Array cleanup requested.");
			break;
		case Type::Category::Struct:
			solAssert(false, "Struct cleanup requested.");
			break;
		case Type::Category::FixedBytes:
		{
			FixedBytesType const& type = dynamic_cast<FixedBytesType const&>(_type);
			if (type.numBytes() == 32)
				templ("body", "cleaned := value");
			else if (type.numBytes() == 0)
				templ("body", "cleaned := 0");
			else
			{
				size_t numBits = type.numBytes() * 8;
				u256 mask = ((u256(1) << numBits) - 1) << (256 - numBits);
				templ("body", "cleaned := and(value, " + toCompactHexWithPrefix(mask) + ")");
			}
			break;
		}
		case Type::Category::Contract:
			templ("body", "cleaned := " + cleanupFunction(IntegerType(120, IntegerType::Modifier::Address)) + "(value)");
			break;
		case Type::Category::Enum:
		{
			size_t members = dynamic_cast<EnumType const&>(_type).numberOfMembers();
			solAssert(members > 0, "empty enum should have caused a parser error.");
			Whiskers w("switch lt(value, <members>) case 0 { <failure> }");
			w("members", to_string(members));
			if (_revertOnFailure)
				w("failure", "revert(0, 0)");
			else
				w("failure", "invalid()");
			templ("body", w.render());
			break;
		}
		default:
			solAssert(false, "Cleanup of type " + _type.identifier() + " requested.");
		}

		return templ.render();
	});
}

string ABIFunctions::conversionFunction(Type const& _from, Type const& _to)
{
	string functionName =
		"convert_" +
		_from.identifier() +
		"_to_" +
		_to.identifier();
	return createFunction(functionName, [&]() {
		Whiskers templ(R"(
			function <functionName>(value) -> converted {
				<body>
			}
		)");
		templ("functionName", functionName);
		string body;
		auto toCategory = _to.category();
		auto fromCategory = _from.category();
		switch (fromCategory)
		{
		case Type::Category::Integer:
		case Type::Category::RationalNumber:
		case Type::Category::Contract:
		{
			if (RationalNumberType const* rational = dynamic_cast<RationalNumberType const*>(&_from))
				solUnimplementedAssert(!rational->isFractional(), "Not yet implemented - FixedPointType.");
			if (toCategory == Type::Category::FixedBytes)
			{
				solAssert(
					fromCategory == Type::Category::Integer || fromCategory == Type::Category::RationalNumber,
					"Invalid conversion to FixedBytesType requested."
				);
				FixedBytesType const& toBytesType = dynamic_cast<FixedBytesType const&>(_to);
				body =
					Whiskers("converted := <shiftLeft>(<clean>(value)")
					("shiftLeft", shiftLeftFunction(256 - toBytesType.numBytes() * 8))
					("clean", cleanupFunction(_from))
					.render();
			}
			else if (toCategory == Type::Category::Enum)
			{
				solAssert(_from.mobileType(), "");
				body =
					Whiskers("converted := <cleanEnum>(<cleanInt>(value))")
					("cleanEnum", cleanupFunction(_to, false))
					// "mobileType()" returns integer type for rational
					("cleanInt", cleanupFunction(*_from.mobileType()))
					.render();
			}
			else if (toCategory == Type::Category::FixedPoint)
			{
				solUnimplemented("Not yet implemented - FixedPointType.");
			}
			else
			{
				solAssert(
					toCategory == Type::Category::Integer ||
					toCategory == Type::Category::Contract,
				"");
				IntegerType const addressType(0, IntegerType::Modifier::Address);
				IntegerType const& to =
					toCategory == Type::Category::Integer ?
					dynamic_cast<IntegerType const&>(_to) :
					addressType;

				// Clean according to the "to" type, except if this is
				// a widening conversion.
				IntegerType const* cleanupType = &to;
				if (fromCategory != Type::Category::RationalNumber)
				{
					IntegerType const& from =
						fromCategory == Type::Category::Integer ?
						dynamic_cast<IntegerType const&>(_from) :
						addressType;
					if (to.numBits() > from.numBits())
						cleanupType = &from;
				}
				body =
					Whiskers("converted := <cleanInt>(value)")
					("cleanInt", cleanupFunction(*cleanupType))
					.render();
			}
			break;
		}
		case Type::Category::Bool:
		{
			solAssert(_from == _to, "Invalid conversion for bool.");
			body =
				Whiskers("converted := <clean>(value)")
				("clean", cleanupFunction(_from))
				.render();
			break;
		}
		case Type::Category::FixedPoint:
			solUnimplemented("Fixed point types not implemented.");
			break;
		case Type::Category::Array:
			solUnimplementedAssert(false, "Array conversion not implemented.");
			break;
		case Type::Category::Struct:
			solUnimplementedAssert(false, "Struct conversion not implemented.");
			break;
		case Type::Category::FixedBytes:
		{
			FixedBytesType const& from = dynamic_cast<FixedBytesType const&>(_from);
			if (toCategory == Type::Category::Integer)
				body =
					Whiskers("converted := <convert>(<shift>(value))")
					("shift", shiftRightFunction(256 - from.numBytes() * 8, false))
					("convert", conversionFunction(IntegerType(from.numBytes() * 8), _to))
					.render();
			else
			{
				// clear for conversion to longer bytes
				solAssert(toCategory == Type::Category::FixedBytes, "Invalid type conversion requested.");
				body =
					Whiskers("converted := <clean>(value)")
					("clean", cleanupFunction(from))
					.render();
			}
			break;
		}
		case Type::Category::Function:
		{
			solUnimplementedAssert(false, "Function conversion not implemented.");
			break;
		}
		case Type::Category::Enum:
		{
			solAssert(toCategory == Type::Category::Integer || _from == _to, "");
			EnumType const& enumType = dynamic_cast<decltype(enumType)>(_from);
			body =
				Whiskers("converted := <clean>(value)")
				("clean", cleanupFunction(enumType))
				.render();
		}
		case Type::Category::Tuple:
		{
			solUnimplementedAssert(false, "Tuple conversion not implemented.");
			break;
		}
		default:
			solAssert(false, "");
		}

		solAssert(!body.empty(), "");
		templ("body", body);
		return templ.render();
	});
}

string ABIFunctions::abiEncodingFunction(
	Type const& _givenType,
	Type const& _to,
	bool _encodeAsLibraryTypes
)
{
	string functionName =
		"abi_encode_" +
		_givenType.identifier() +
		"_to_" +
		_to.identifier() +
		(_encodeAsLibraryTypes ? "_lib" : "");
	return createFunction(functionName, [&]() {
		Whiskers templ(R"(
			function <functionName>(value, headStart, headPos, dyn) -> newDyn {
				<body>
			}
		)");
		templ("functionName", functionName);

		string body;
		if (_to.isDynamicallySized())
		{
			solUnimplementedAssert(false, "");
		}
		else
		{
			body = "newDyn := dyn\n";
			solUnimplementedAssert(_givenType.sizeOnStack() == 1, "");
			if (_givenType.dataStoredIn(DataLocation::Storage) && _to.isValueType())
			{
				// special case: convert storage reference type to value type - this is only
				// possible for library calls where we just forward the storage reference
				solAssert(_encodeAsLibraryTypes, "");
				solAssert(_givenType.sizeOnStack() == 1, "");
				solAssert(_to == IntegerType(256), "");
				body += "mstore(headPos, value)";
			}
			else if (
				_givenType.dataStoredIn(DataLocation::Storage) ||
				_givenType.dataStoredIn(DataLocation::CallData) ||
				_givenType.category() == Type::Category::StringLiteral ||
				_givenType.category() == Type::Category::Function
			)
			{
				// This used to delay conversion
				solUnimplemented("");
			}
			else if (dynamic_cast<ArrayType const*>(&_to))
			{
				// This used to perform a conversion first and then call
				// ArrayUtils(m_context).copyArrayToMemory(*arrayType, _padToWordBoundaries);
				solUnimplemented("");
			}
			else if (dynamic_cast<StructType const*>(&_to))
			{
				solUnimplemented("");
			}
			else
			{
				solAssert(_to.isValueType(), "");
				solAssert(_to.calldataEncodedSize() == 32, "");
				if (_givenType == _to)
					body += "mstore(headPos, " + cleanupFunction(_givenType) + "(value))\n";
				else
					body += "mstore(headPos, " + conversionFunction(_givenType, _to) + "(value))\n";
			}
		}
		templ("body", body);
		return templ.render();
	});
}

string ABIFunctions::shiftLeftFunction(size_t _numBits)
{
	string functionName = "shift_left_" + to_string(_numBits);
	return createFunction(functionName, [&]() {
		solAssert(_numBits < 256, "");
		return
			Whiskers(R"(function <functionName>(value) -> newValue {
					newValue := mul(value, <multiplier>)
			})")
			("functionName", functionName)
			("multiplier", (u256(1) << _numBits).str())
			.render();
	});
}

string ABIFunctions::shiftRightFunction(size_t _numBits, bool _signed)
{
	string functionName = "shift_right_" + to_string(_numBits) + (_signed ? "_signed" : "_unsigned");
	return createFunction(functionName, [&]() {
		solAssert(_numBits < 256, "");
		return
			Whiskers(R"(function <functionName>(value) -> newValue {
					newValue := <div>(value, <multiplier>)
			})")
			("functionName", functionName)
			("div", _signed ? "sdiv" : "div")
			("multiplier", (u256(1) << _numBits).str())
			.render();
	});
}

string ABIFunctions::createFunction(string const& _name, function<string ()> const& _creator)
{
	if (!m_requestedFunctions.count(_name))
	{
		auto fun = _creator();
		solAssert(!fun.empty(), "");
		m_requestedFunctions[_name] = fun;
	}
	return _name;
}
