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
 * @date 2017
 * Indented text writer.
 */

#pragma once

#include <vector>
#include <string>

namespace dev
{

namespace solidity
{

class IndentedWriter
{
public:
	explicit IndentedWriter(): m_lines(std::vector<Line>{{std::string(), 0}}) {}

	std::string format() const;
	void newLine();
	void indent();
	void unindent();
	void add(std::string const& _str);
	void addLine(std::string const& _line);

private:
	struct Line
	{
		std::string contents;
		unsigned indentation;
	};

	std::vector<Line> m_lines;
};

}
}
