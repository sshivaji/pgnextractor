/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <sstream>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#else
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "book.h"
#include "misc.h"
#include "position.h"
#include "uci.h"

namespace {

typedef std::vector<PolyEntry> Keys;

struct Stats {
    int64_t games;
    int64_t moves;
    int64_t fixed;
};

enum Token {
    T_NONE, T_SPACES, T_RESULT, T_MINUS, T_DOT, T_QUOTES, T_DOLLAR,
    T_LEFT_BRACKET, T_RIGHT_BRACKET, T_LEFT_BRACE, T_RIGHT_BRACE,
    T_LEFT_PARENTHESIS, T_RIGHT_PARENTHESIS, T_EVENT, T_ZERO, T_DIGIT,
    T_MOVE_HEAD, TOKEN_NB
};

enum State {
    HEADER, TAG, FEN_TAG, BRACE_COMMENT, VARIATION, NUMERIC_ANNOTATION_GLYPH,
    NEXT_MOVE, MOVE_NUMBER, NEXT_SAN, READ_SAN, RESULT, SKIP_GAME, STATE_NB
};

enum Step : uint8_t {
    FAIL, CONTINUE, GAME_START, OPEN_TAG, OPEN_BRACE_COMMENT, READ_FEN, CLOSE_FEN_TAG,
    OPEN_VARIATION, START_NAG, POP_STATE, START_MOVE_NUMBER, START_NEXT_SAN,
    CASTLE_OR_RESULT, START_READ_SAN, READ_MOVE_CHAR, END_MOVE, START_RESULT,
    END_GAME, TAG_IN_BRACE, MISSING_RESULT
};

enum MetaType {
    MOVE_TOTAL, MOVE_WIN, MOVE_DRAW
};

Token ToToken[256];
Step ToStep[STATE_NB][TOKEN_NB];
Position RootPos;

void map(const char* fname, void** baseAddress, uint64_t* mapping, uint64_t* size) {

#ifndef _WIN32
    struct stat statbuf;
    int fd = ::open(fname, O_RDONLY);
    fstat(fd, &statbuf);
    *mapping = *size = statbuf.st_size;
    *baseAddress = mmap(nullptr, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (*baseAddress == MAP_FAILED)
    {
        std::cerr << "Could not mmap() " << fname << std::endl;
        exit(1);
    }
#else
    HANDLE fd = CreateFile(fname, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD size_high;
    DWORD size_low = GetFileSize(fd, &size_high);
    HANDLE mmap = CreateFileMapping(fd, nullptr, PAGE_READONLY, size_high, size_low, nullptr);
    CloseHandle(fd);
    if (!mmap)
    {
        std::cerr << "CreateFileMapping() failed" << std::endl;
        exit(1);
    }
    *size = ((size_t)size_high << 32) | (size_t)size_low;
    *mapping = (uint64_t)mmap;
    *baseAddress = MapViewOfFile(mmap, FILE_MAP_READ, 0, 0, 0);
    if (!*baseAddress)
    {
        std::cerr << "MapViewOfFile() failed, name = " << fname
                  << ", error = " << GetLastError() << std::endl;
        exit(1);
    }
#endif
}

void unmap(void* baseAddress, uint64_t mapping) {

#ifndef _WIN32
    munmap(baseAddress, mapping);
#else
    UnmapViewOfFile(baseAddress);
    CloseHandle((HANDLE)mapping);
#endif
}

void error(Step* state, const char* data) {

    std::vector<std::string> stateDesc = {
        "HEADER", "TAG", "FEN_TAG", "BRACE_COMMENT", "VARIATION",
        "NUMERIC_ANNOTATION_GLYPH", "NEXT_MOVE", "MOVE_NUMBER",
        "NEXT_SAN", "READ_SAN", "RESULT"
    };

    for (int i = 0; i < STATE_NB; i++)
        if (ToStep[i] == state)
        {
            std::string what = std::string(data, 50);
            std::cout << "Wrong " << stateDesc[i] << ": '"
                      << what << "' " << std::endl;
        }
    //exit(0);
}


/// Convert a number of type T into a sequence of bytes in big-endian format

template<typename T> uint8_t* write(const T& n, uint8_t* data) {

    for (int i =  8 * (sizeof(T) - 1); i >= 0; i -= 8, ++data)
        *data = uint8_t(n >> i);

    return data;
}

//template<bool DryRun = true>
//const char* parse_game(const char* moves, const char* end, Keys& kTable,
//                       const char* fen, const char* fenEnd, size_t& fixed,
//                       uint64_t gameOfs, int result) {
//
//    StateInfo states[1024], *st = states;
//    Position pos = RootPos;
//    const char *cur = moves;
//
//    if (fenEnd != fen)
//        pos.set(fen, false, st++);
//
//    // Use Polyglot 'learn' parameter to store game result in the upper 2 bits,
//    // and game offset in the PGN file. Note that the offset is 8 bytes aligned
//    // and points to "somewhere" in the game. It is up to the look up tool to
//    // find game's boundaries. This allow us to index up to 8GB PGN files.
//    // Result is stored in the upper 2 bits so that sorting by 'learn' allows
//    // easy counting of result statistics.
//
//    // upper 2 bits out of 32 bits store the result
////    const uint32_t learn =  ((uint32_t(result) & 3) << 30)
////                          | ((gameOfs >> 3) & 0x3FFFFFFF);
//    while (cur < end)
//    {
//        Move move = pos.san_to_move(cur, end, fixed);
//        if (move == MOVE_NONE)
//        {
////            if (false)
////            {
////                const char* sep = pos.side_to_move() == WHITE ? "" : "..";
////                std::cerr << "\nWrong move notation: " << sep << cur
////                          << "\n" << pos << std::endl;
////
////            }
//            return cur;
//        }
//        else if (move == MOVE_NULL)
//            pos.do_null_move(*st++);
//        else
//        {
////            if (false)
////                kTable.push_back({pos.key(), to_polyglot(move), 1, learn});
//
//            pos.do_move(move, *st++, pos.gives_check(move));
//        }
//
//        while (*cur++) {} // Go to next move
//    }
//    return end;
//}

int get_result(const char* data) {

    // Result is coded from 0 to 3 as WHITE_WIN, BLACK_WIN, DRAW, RESULT_UNKNOWN.
    // START_RESULT is triggered by '/', '*', '0', '-'.
    switch (*data) {
    case '/':
        return 2;
    case '0':
        return 1;
    case '-':
        if (    *(data-1) == '1'
            || (*(data-1) == ' ' && *(data-2) == '1')) // Like '1 - 0'
            return 0;
        else if (    *(data-1) == '0'
                 || (*(data-1) == ' ' && *(data-2) == '0'))
            return 1;
        break;
    default:
        break;
    }
    return 3;
}

std::string escape_json(const std::string &s) {
    std::ostringstream o;
    for (auto c = s.cbegin(); c != s.cend(); c++) {
        switch (*c) {
        case '\x00': o << "\\u0000"; break;
        case '\x01': o << "\\u0001"; break;
        case '\x0a': o << "\\n"; break;
        case '\x1f': o << "\\u001f"; break;
        case '\x22': o << "\\\""; break;
        case '\x5c': o << "\\\\"; break;
        default: o << *c;
        }
    }
    return o.str();
}

void parse_pgn(void* baseAddress, uint64_t size, Stats& stats, std::ofstream& headerFile) {

    Step* stateStack[16];
    Step**stateSp = stateStack;
    char fen[256], *fenEnd = fen;
    char moves[1024 * 8], *curMove = moves;
    char* end = curMove;
    size_t moveCnt = 0, gameCnt = 0, fixed = 0;
    uint64_t gameOfs = 0;
//    int result = 3;
    char* data = (char*)baseAddress;
    char* eof = data + size;
    int stm = WHITE;
    Step* state = ToStep[HEADER];
    int tag_offset = 1;
    std::string header;
    std::string value;
    headerFile << "{";

    for (  ; data < eof; ++data)
    {
        Token tk = ToToken[*(uint8_t*)data];

        switch (state[tk])
        {
        case FAIL:
            error(state, data);
            break;

        case CONTINUE:
            break;

        case GAME_START:
            if (!strncmp(data-1, "[Event ", 7))
            {
                data -= 2;
                state = ToStep[HEADER];
            }
            break;

        case OPEN_TAG:
            *stateSp++ = state;
            tag_offset = 1;
            // Get the header from the tag
            while (*( data + tag_offset )!=' ') {
                char c = *( data + tag_offset );
                tag_offset+=1;
                header += c;
            }

//                         Move over to the result tag
            tag_offset+=1;
            // Get the result value from the tag
            while (*( data + tag_offset )!='\n')
            {
                char c = *( data + tag_offset );
                tag_offset += 1;
                value += c;
            }
            // Remove characters after the last closing brace
            value.erase(value.rfind(']'));

            // Remove first and last quote
            value = value.substr(1, value.size() - 2);

            // Check for embedded quotes or other chars that cause an invalid JSON to be output
            if (value.find('"')!=std::string::npos || value.find('\\')!=std::string::npos) {
//                std::cout << "value: " << value;
                value = escape_json(value);
            }

            headerFile << "\"" << header << "\": \"" << value << "\", ";
            header = "";
            value = "";

            if (*(data + 1) == 'F' && !strncmp(data+1, "FEN \"", 5))
            {
                data += 5;
                state = ToStep[FEN_TAG];
            }
//            else if (   *(data + 1) == 'V'
//                     && !strncmp(data+1, "Variant ", 8)
//                     &&  strncmp(data+9, "\"Standard\"", 10))
//            {
//                --stateSp; // Pop state, we are inside brackets
//                state = ToStep[SKIP_GAME];
//            }
            else
                state = ToStep[TAG];
            break;

        case OPEN_BRACE_COMMENT:
            *stateSp++ = state;
            state = ToStep[BRACE_COMMENT];
            break;

        case READ_FEN:
            *fenEnd++ = *data;
            break;

        case CLOSE_FEN_TAG:
            *fenEnd++ = 0; // Zero-terminating string
            state = ToStep[TAG];
            if (strstr(fen, " b "))
                stm = BLACK;
            break;

        case OPEN_VARIATION:
            *stateSp++ = state;
            state = ToStep[VARIATION];
            break;

        case START_NAG:
            *stateSp++ = state;
            state = ToStep[NUMERIC_ANNOTATION_GLYPH];
            break;

        case POP_STATE:
            state = *(--stateSp);
            break;

        case START_MOVE_NUMBER:
            state = ToStep[MOVE_NUMBER];
            break;

        case START_NEXT_SAN:
            state = ToStep[NEXT_SAN];
            break;

        case CASTLE_OR_RESULT:
            if (data[2] != '0')
            {
//                assert (result == 3);

//                result = get_result(data);
                state = ToStep[RESULT];
                continue;
            }
            /* Fall through */

        case START_READ_SAN:
            *end++ = *data;
            state = ToStep[READ_SAN];
            break;

        case READ_MOVE_CHAR:
            *end++ = *data;
            break;

        case END_MOVE:
            *end++ = 0; // Zero-terminating string
            curMove = end;
            moveCnt++;
            state = ToStep[stm == WHITE ? NEXT_SAN : NEXT_MOVE];
            stm ^= 1;
            break;

        case START_RESULT:
//            assert (result == 3);

//            result = get_result(data);
            state = ToStep[RESULT];
            break;

        case END_GAME:
            if (*data != '\n') // Handle spaces in result, like 1/2 - 1/2
            {
                state = ToStep[RESULT];
                break;
            }
//            parse_game(moves, end, kTable, fen, fenEnd, fixed, gameOfs, result);

            headerFile << "\"offset\":" << gameOfs << ",\"offset_8\":"<< gameOfs*8;
            // Remove last comma from JSON header
//            headerFile.seekp(-2, headerFile.cur);
            headerFile << "}\n{";

            gameCnt++;
//            result = 3;
            gameOfs = (data - (char*)baseAddress) + 1; // Beginning of next game
            end = curMove = moves;
            fenEnd = fen;
            state = ToStep[HEADER];
            stm = WHITE;
            break;

        case TAG_IN_BRACE:
             // Special case of missed brace close. Detect beginning of next game
             if (strncmp(data, "[Event ", 7))
                 break;

             /* Fall through */

        case MISSING_RESULT: // Missing result, next game already started
//            parse_game(moves, end, kTable, fen, fenEnd, fixed, gameOfs, result);

            headerFile << "\"offset\":" << gameOfs;

            headerFile.seekp(-2, headerFile.cur);
            headerFile << "}\n{";

            gameCnt++;
//            result = 3;
            gameOfs = (data - (char*)baseAddress); // Beginning of next game
            end = curMove = moves;
            fenEnd = fen;
            state = ToStep[HEADER];
            stm = WHITE;

            *stateSp++ = state; // Fast forward into a TAG
            state = ToStep[TAG];
            break;

        default:
            assert(false);
            break;
        }
    }

    // Force accounting of last game if still pending. Many reason for this to
    // trigger: no newline at EOF, missing result, missing closing brace, etc.
    if (state != ToStep[HEADER] && state != ToStep[SKIP_GAME] && end - moves)
    {
//        parse_game(moves, end, kTable, fen, fenEnd, fixed, gameOfs, result);

        headerFile << "\"offset\":" << gameOfs;
        headerFile << "}\n{";

        gameCnt++;
    }

    // Remove the last opening brace as there are no more games
    headerFile.seekp(-2, headerFile.cur);
    headerFile << "  ";

    stats.games = gameCnt;
    stats.moves = moveCnt;
    stats.fixed = fixed;
}

} // namespace

namespace Parser {


void init() {

    static StateInfo st;
    const char* startFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    RootPos.set(startFEN, false, &st);

    ToToken['\n'] = ToToken['\r'] = ToToken[' '] = ToToken['\t'] = T_SPACES;
    ToToken['/'] = ToToken['*'] = T_RESULT;
    ToToken['-'] = T_MINUS;
    ToToken['.'] = T_DOT;
    ToToken['"'] = T_QUOTES;
    ToToken['$'] = T_DOLLAR;
    ToToken['['] = T_LEFT_BRACKET;
    ToToken[']'] = T_RIGHT_BRACKET;
    ToToken['{'] = T_LEFT_BRACE;
    ToToken['}'] = T_RIGHT_BRACE;
    ToToken['('] = T_LEFT_PARENTHESIS;
    ToToken[')'] = T_RIGHT_PARENTHESIS;
    ToToken['E'] = T_EVENT;
    ToToken['0'] = T_ZERO;
    ToToken['1'] = ToToken['2'] = ToToken['3'] =
    ToToken['4'] = ToToken['5'] = ToToken['6'] = ToToken['7'] =
    ToToken['8'] = ToToken['9'] = T_DIGIT;
    ToToken['a'] = ToToken['b'] = ToToken['c'] = ToToken['d'] =
    ToToken['e'] = ToToken['f'] = ToToken['g'] = ToToken['h'] =
    ToToken['N'] = ToToken['B'] = ToToken['R'] = ToToken['Q'] =
    ToToken['K'] = ToToken['O'] = ToToken['o'] = T_MOVE_HEAD;

    // Trailing move notations are ignored because SAN detector
    // does not need them and in some malformed PGN they appear
    // one blank apart from the corresponding move.
    ToToken['!'] = ToToken['?'] = ToToken['+'] = ToToken['#'] = T_SPACES;

    // STATE = HEADER
    //
    // Between tags, before game starts. Accept anything
    for (int i = 0; i < TOKEN_NB; i++)
        ToStep[HEADER][i] = CONTINUE;

    ToStep[HEADER][T_LEFT_BRACKET] = OPEN_TAG;
    ToStep[HEADER][T_LEFT_BRACE  ] = OPEN_BRACE_COMMENT;
    ToStep[HEADER][T_DIGIT       ] = START_MOVE_NUMBER;
    ToStep[HEADER][T_ZERO        ] = START_RESULT;
    ToStep[HEADER][T_RESULT      ] = START_RESULT;

    // STATE = TAG
    //
    // Between brackets in header section, generic tag
    for (int i = 0; i < TOKEN_NB; i++)
        ToStep[TAG][i] = CONTINUE;

    ToStep[TAG][T_RIGHT_BRACKET] = POP_STATE;

    // STATE = FEN_TAG
    //
    // Special tag to set a position from a FEN string
    for (int i = 0; i < TOKEN_NB; i++)
        ToStep[FEN_TAG][i] = READ_FEN;

    ToStep[FEN_TAG][T_QUOTES] = CLOSE_FEN_TAG;

    // STATE = BRACE_COMMENT
    //
    // Comment in braces, can appear almost everywhere. Note that brace comments
    // do not nest according to PGN standard.
    for (int i = 0; i < TOKEN_NB; i++)
        ToStep[BRACE_COMMENT][i] = CONTINUE;

    ToStep[BRACE_COMMENT][T_RIGHT_BRACE ] = POP_STATE;
    ToStep[BRACE_COMMENT][T_LEFT_BRACKET] = TAG_IN_BRACE; // Missed closing brace

    // STATE = VARIATION
    //
    // For the moment variations are ignored
    for (int i = 0; i < TOKEN_NB; i++)
        ToStep[VARIATION][i] = CONTINUE;

    ToStep[VARIATION][T_RIGHT_PARENTHESIS] = POP_STATE;
    ToStep[VARIATION][T_LEFT_PARENTHESIS ] = OPEN_VARIATION; // Nested
    ToStep[VARIATION][T_LEFT_BRACE       ] = OPEN_BRACE_COMMENT;

    // STATE = NUMERIC_ANNOTATION_GLYPH
    //
    // Just read a single number
    for (int i = 0; i < TOKEN_NB; i++)
        ToStep[NUMERIC_ANNOTATION_GLYPH][i] = POP_STATE;

    ToStep[NUMERIC_ANNOTATION_GLYPH][T_ZERO ] = CONTINUE;
    ToStep[NUMERIC_ANNOTATION_GLYPH][T_DIGIT] = CONTINUE;

    // STATE = NEXT_MOVE
    //
    // Check for the beginning of the next move number
    for (int i = 0; i < TOKEN_NB; i++)
        ToStep[NEXT_MOVE][i] = CONTINUE;

    ToStep[NEXT_MOVE][T_LEFT_PARENTHESIS] = OPEN_VARIATION;
    ToStep[NEXT_MOVE][T_LEFT_BRACE      ] = OPEN_BRACE_COMMENT;
    ToStep[NEXT_MOVE][T_LEFT_BRACKET    ] = MISSING_RESULT;
    ToStep[NEXT_MOVE][T_DOLLAR          ] = START_NAG;
    ToStep[NEXT_MOVE][T_RESULT          ] = START_RESULT;
    ToStep[NEXT_MOVE][T_ZERO            ] = START_RESULT;
    ToStep[NEXT_MOVE][T_DOT             ] = FAIL;
    ToStep[NEXT_MOVE][T_MOVE_HEAD       ] = FAIL;
    ToStep[NEXT_MOVE][T_MINUS           ] = FAIL;
    ToStep[NEXT_MOVE][T_DIGIT           ] = START_MOVE_NUMBER;

    // STATE = MOVE_NUMBER
    //
    // Continue until a dot is found, to tolerate missing dots,
    // stop at first space, then start NEXT_SAN that will handle
    // head trailing spaces. We can alias with a result like 1-0 or 1/2-1/2
    ToStep[MOVE_NUMBER][T_ZERO  ] = CONTINUE;
    ToStep[MOVE_NUMBER][T_DIGIT ] = CONTINUE;
    ToStep[MOVE_NUMBER][T_RESULT] = START_RESULT;
    ToStep[MOVE_NUMBER][T_MINUS ] = START_RESULT;
    ToStep[MOVE_NUMBER][T_SPACES] = START_NEXT_SAN;
    ToStep[MOVE_NUMBER][T_DOT   ] = START_NEXT_SAN;

    // STATE = NEXT_SAN
    //
    // Check for the beginning of the next move SAN
    for (int i = 0; i < TOKEN_NB; i++)
        ToStep[NEXT_SAN][i] = CONTINUE;

    ToStep[NEXT_SAN][T_LEFT_PARENTHESIS] = OPEN_VARIATION;
    ToStep[NEXT_SAN][T_LEFT_BRACE      ] = OPEN_BRACE_COMMENT;
    ToStep[NEXT_SAN][T_LEFT_BRACKET    ] = MISSING_RESULT;
    ToStep[NEXT_SAN][T_DOLLAR          ] = START_NAG;
    ToStep[NEXT_SAN][T_RESULT          ] = START_RESULT;
    ToStep[NEXT_SAN][T_ZERO            ] = CASTLE_OR_RESULT;  // 0-0 or 0-1
    ToStep[NEXT_SAN][T_DOT             ] = CONTINUE;          // Like 4... exd5
    ToStep[NEXT_SAN][T_DIGIT           ] = START_MOVE_NUMBER; // Same as above
    ToStep[NEXT_SAN][T_MOVE_HEAD       ] = START_READ_SAN;
    ToStep[NEXT_SAN][T_MINUS           ] = START_READ_SAN;    // Null move "--"

    // STATE = READ_SAN
    //
    // Just read a single move SAN until a space is reached
    for (int i = 0; i < TOKEN_NB; i++)
        ToStep[READ_SAN][i] = READ_MOVE_CHAR;

    ToStep[READ_SAN][T_SPACES    ] = END_MOVE;
    ToStep[READ_SAN][T_LEFT_BRACE] = OPEN_BRACE_COMMENT;

    // STATE = RESULT
    //
    // Ignore anything until a space is reached
    for (int i = 0; i < TOKEN_NB; i++)
        ToStep[RESULT][i] = CONTINUE;

    ToStep[RESULT][T_SPACES] = END_GAME;

    // STATE = SKIP_GAME
    //
    // Ignore anything until start of next game
    for (int i = 0; i < TOKEN_NB; i++)
        ToStep[SKIP_GAME][i] = CONTINUE;

    ToStep[SKIP_GAME][T_EVENT] = GAME_START;
}

void make_book(std::istringstream& is) {

    Keys kTable;
    Stats stats;
    uint64_t mapping, size;
    void* baseAddress;
    std::string pgnFileName, opt, headerFileName;

    is >> pgnFileName;

    if (pgnFileName.empty())
    {
        std::cerr << "Missing PGN file name..." << std::endl;
        exit(0);
    }

    is >> opt;

    map(pgnFileName.c_str(), &baseAddress, &mapping, &size);

    size_t lastdot = pgnFileName.find_last_of(".");
    if (lastdot != std::string::npos)
        headerFileName = pgnFileName.substr(0, lastdot);
    headerFileName += ".headers.json";
    std::ofstream headerFile(headerFileName);

    // Reserve enough capacity according to file size. This is a very crude
    // estimation, mainly we assume key index to be of 2 times the size of
    // the pgn file.
//    kTable.reserve(2 * size / sizeof(PolyEntry));

    std::cout << "\nProcessing...";

    TimePoint elapsed = now();

    parse_pgn(baseAddress, size, stats, headerFile);

    std::cout << "\nWriting headers to " << headerFileName << "\n...";

    elapsed = now() - elapsed + 1; // Ensure positivity to avoid a 'divide by zero'

    unmap(baseAddress, mapping);

    std::cout << "done\n";

//    std::sort(kTable.begin(), kTable.end());
//
//    size_t uniqueKeys = 1, last = 0;
//    for (size_t idx = 1; idx < kTable.size(); ++idx)
//        if (kTable[idx].key != kTable[idx - 1].key)
//        {
//            if (idx - last > 2)
//                idx = sort_by_frequency(kTable, last, idx);
//
//            last = idx;
//            uniqueKeys++;
//        }

//    std::cout << "done\nWriting Polygot book...";

//
//    size_t bookSize = write_poly_file(kTable, pgnFileName, full);

    std::cout << "done\n"
              << "\nGames: " << stats.games
              << "\nMoves: " << stats.moves
              << "\nIncorrect moves: " << stats.fixed
              << "\nGames/second: " << 1000 * stats.games / elapsed
              << "\nMoves/second: " << 1000 * stats.moves / elapsed
              << "\nMBytes/second: " << float(size) / elapsed / 1000
              << "\nBook file: " << pgnFileName
              << "\nProcessing time (ms): " << elapsed << "\n" << std::endl;
}



}
