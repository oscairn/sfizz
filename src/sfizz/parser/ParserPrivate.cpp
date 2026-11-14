// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "ParserPrivate.h"
#include <cstring>

namespace sfz {

thread_local size_t Reader::_locationSteps = 0;

Reader::Reader(const fs::path& filePath)
{
    _buf.reserve(256);
    _loc.filePath = std::make_shared<fs::path>(filePath);
    _lineNumColumns.reserve(256);
}

// Makes the character at `index` available in the buffer, if the stream has one
bool Reader::fill(size_t index)
{
    while (index >= _buf.size()) {
        int byte = getNextStreamByte();
        if (byte == kEof)
            return false;
        _buf.push_back(static_cast<char>(byte));
    }

    return true;
}

// Forgets the characters read so far, when none is unread
void Reader::compact()
{
    if (_pos >= 4096 && _pos == _buf.size()) {
        _buf.clear();
        _pos = 0;
        _eolScanned = false;
    }
}

int Reader::getChar()
{
    compact();
    if (!fill(_pos))
        return kEof;

    int byte = static_cast<unsigned char>(_buf[_pos++]);
    updateSourceLocationAdding(byte);
    return byte;
}

int Reader::peekChar()
{
    compact();
    return fill(_pos) ? static_cast<unsigned char>(_buf[_pos]) : kEof;
}

size_t Reader::extractToEol(std::string* dst)
{
    compact();
    size_t end;

    // a line with many opcodes asks again from each one: the end found last time still holds
    if (_eolScanned && _eolFrom <= _pos && _pos <= _eolEnd)
        end = _eolEnd;
    else {
        for (end = _pos; fill(end); ++end) {
            char c = _buf[end];
            if (c == '\r' || c == '\n')
                break;
            if (c == '/' && fill(end + 1) && (_buf[end + 1] == '/' || _buf[end + 1] == '*'))
                break;
        }
        _eolScanned = true;
        _eolFrom = _pos;
        _eolEnd = end;
    }

    const size_t count = end - _pos;
    if (dst)
        dst->append(_buf, _pos, count);
    _pos = end;
    _loc.columnNumber += count; // the stop characters are not in it
    return count;
}

bool Reader::extractExactChar(char c)
{
    int next = peekChar();

    if (next == kEof || next != static_cast<unsigned char>(c))
        return false;

    getChar();
    return true;
}

void Reader::putBackChar(int c)
{
    if (c == kEof)
        return;

    char c8bit = static_cast<unsigned char>(c);
    putBackChars(absl::string_view(&c8bit, 1));
}

void Reader::putBackChars(absl::string_view characters)
{
    const size_t count = characters.size();

    // the usual case: these are the characters just read, so only the position moves
    if (_pos >= count && _buf.compare(_pos - count, count, characters.data(), count) == 0)
        _pos -= count;
    else {
        _buf.insert(_pos, characters.data(), count);
        _eolScanned = false;
    }

    if (!std::memchr(characters.data(), '\n', count))
        _loc.columnNumber -= count;
    else {
        for (size_t i = count; i-- > 0;)
            updateSourceLocationRemoving(static_cast<unsigned char>(characters[i]));
    }
}

size_t Reader::skipChars(absl::string_view chars)
{
    size_t count = 0;

    while (chars.find(static_cast<unsigned char>(peekChar())) != chars.npos) {
        getChar();
        ++count;
    }

    return count;
}

bool Reader::hasEof()
{
    return peekChar() == kEof;
}

bool Reader::hasOneOfChars(absl::string_view chars)
{
    int c = peekChar();
    if (c == kEof)
        return false;

    return chars.find(static_cast<unsigned char>(c)) != chars.npos;
}

void Reader::updateSourceLocationAdding(int byte)
{
    ++_locationSteps;
    if (byte != '\n')
        _loc.columnNumber += 1;
    else {
        _lineNumColumns.push_back(_loc.columnNumber);
        _loc.lineNumber += 1;
        _loc.columnNumber = 0;
    }
}

void Reader::updateSourceLocationRemoving(int byte)
{
    ++_locationSteps;
    if (byte != '\n')
        _loc.columnNumber -= 1;
    else {
        _loc.lineNumber -= 1;
        _loc.columnNumber = _lineNumColumns[_loc.lineNumber];
        _lineNumColumns.pop_back();
    }
}

//------------------------------------------------------------------------------

FileReader::FileReader(const fs::path& filePath)
    : Reader(filePath), _fileStream(filePath)
{
}

bool FileReader::hasError()
{
    return !_fileStream.is_open() || _fileStream.bad();
}

int FileReader::getNextStreamByte()
{
    return _fileStream.get();
}

StringViewReader::StringViewReader(const fs::path& filePath, absl::string_view sfzView)
    : Reader(filePath), _sfzView(sfzView)
{
}

int StringViewReader::getNextStreamByte()
{
    if (position < _sfzView.length())
        return _sfzView[position++];

    return kEof;
}

}  // namespace sfz
