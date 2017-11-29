/*
    This file is part of Leela Zero.
    Copyright (C) 2017 Gian-Carlo Pascutto

    Leela Zero is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Leela Zero is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Leela Zero.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <string>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <assert.h>
#include <array>
#include <queue>

#include "config.h"

#include "FastBoard.h"
#include "Utils.h"
#include "Random.h"

using namespace Utils;

const int FastBoard::NBR_SHIFT;
const int FastBoard::MAXBOARDSIZE;
const int FastBoard::MAXSQ;
const int FastBoard::BIG;
const int FastBoard::PASS;
const int FastBoard::RESIGN;

const std::array<int, 2> FastBoard::s_eyemask = {
    4 * (1 << (NBR_SHIFT * BLACK)),
    4 * (1 << (NBR_SHIFT * WHITE))
};

const std::array<FastBoard::square_t, 4> FastBoard::s_cinvert = {
    WHITE, BLACK, EMPTY, INVAL
};

int FastBoard::get_boardsize(void) const {
    return m_boardsize;
}

int FastBoard::get_vertex(int x, int y) const {
    assert(x >= 0 && x < MAXBOARDSIZE);
    assert(y >= 0 && y < MAXBOARDSIZE);
    assert(x >= 0 && x < m_boardsize);
    assert(y >= 0 && y < m_boardsize);

    int vertex = ((y + 1) * (get_boardsize() + 2)) + (x + 1);

    assert(vertex >= 0 && vertex < m_maxsq);

    return vertex;
}

std::pair<int, int> FastBoard::get_xy(int vertex) const {
    std::pair<int, int> xy;

    //int vertex = ((y + 1) * (get_boardsize() + 2)) + (x + 1);
    int x = (vertex % (get_boardsize() + 2)) - 1;
    int y = (vertex / (get_boardsize() + 2)) - 1;

    assert(x >= 0 && x < get_boardsize());
    assert(y >= 0 && y < get_boardsize());

    xy.first  = x;
    xy.second = y;

    assert(get_vertex(x, y) == vertex);

    return xy;
}

FastBoard::square_t FastBoard::get_square(int vertex) const {
    assert(vertex >= 0 && vertex < MAXSQ);
    assert(vertex >= 0 && vertex < m_maxsq);

    return m_square[vertex];
}

void FastBoard::set_square(int vertex, FastBoard::square_t content) {
    assert(vertex >= 0 && vertex < MAXSQ);
    assert(vertex >= 0 && vertex < m_maxsq);
    assert(content >= BLACK && content <= INVAL);

    m_square[vertex] = content;
}

FastBoard::square_t FastBoard::get_square(int x, int y) const {
    return get_square(get_vertex(x,y));
}

void FastBoard::set_square(int x, int y, FastBoard::square_t content) {
    set_square(get_vertex(x, y), content);
}

void FastBoard::reset_board(int size) {
    m_boardsize = size;
    m_maxsq = (size + 2) * (size + 2);
    m_tomove = BLACK;
    m_prisoners[BLACK] = 0;
    m_prisoners[WHITE] = 0;
    m_totalstones[BLACK] = 0;
    m_totalstones[WHITE] = 0;
    m_empty_cnt = 0;

    m_dirs[0] = -size-2;
    m_dirs[1] = +1;
    m_dirs[2] = +size+2;
    m_dirs[3] = -1;

    m_extradirs[0] = -size-2-1;
    m_extradirs[1] = -size-2;
    m_extradirs[2] = -size-2+1;
    m_extradirs[3] = -1;
    m_extradirs[4] = +1;
    m_extradirs[5] = +size+2-1;
    m_extradirs[6] = +size+2;
    m_extradirs[7] = +size+2+1;

    for (int i = 0; i < m_maxsq; i++) {
        m_square[i]     = INVAL;
        m_neighbours[i] = 0;
        m_parent[i]     = MAXSQ;
    }

    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            int vertex = get_vertex(i, j);

            m_square[vertex]          = EMPTY;
            m_empty_idx[vertex]       = m_empty_cnt;
            m_empty[m_empty_cnt++]    = vertex;

            if (i == 0 || i == size - 1) {
                m_neighbours[vertex] += (1 << (NBR_SHIFT * BLACK))
                                      | (1 << (NBR_SHIFT * WHITE));
                m_neighbours[vertex] +=  1 << (NBR_SHIFT * EMPTY);
            } else {
                m_neighbours[vertex] +=  2 << (NBR_SHIFT * EMPTY);
            }

            if (j == 0 || j == size - 1) {
                m_neighbours[vertex] += (1 << (NBR_SHIFT * BLACK))
                                      | (1 << (NBR_SHIFT * WHITE));
                m_neighbours[vertex] +=  1 << (NBR_SHIFT * EMPTY);
            } else {
                m_neighbours[vertex] +=  2 << (NBR_SHIFT * EMPTY);
            }
        }
    }

    m_parent[MAXSQ] = MAXSQ;
    m_libs[MAXSQ]   = 16384;    /* we will subtract from this */
    m_next[MAXSQ]   = MAXSQ;
}

bool FastBoard::is_suicide(int i, int color) {
    if (count_pliberties(i)) {
        return false;
    }

    bool connecting = false;

    for (int k = 0; k < 4; k++) {
        int ai = i + m_dirs[k];

        int libs = m_libs[m_parent[ai]];
        if (get_square(ai) == color) {
            if (libs > 1) {
                // connecting to live group = never suicide
                return false;
            }
            connecting = true;
        } else {
            if (libs <= 1) {
                // killing neighbor = never suicide
                return false;
            }
        }
    }

    add_neighbour(i, color);

    bool opps_live = true;
    bool ours_die = true;

    for (int k = 0; k < 4; k++) {
        int ai = i + m_dirs[k];

        int libs = m_libs[m_parent[ai]];

        if (libs == 0 && get_square(ai) != color) {
            opps_live = false;
        } else if (libs != 0 && get_square(ai) == color) {
            ours_die = false;
        }
    }

    remove_neighbour(i, color);

    if (!connecting) {
        return opps_live;
    } else {
        return opps_live && ours_die;
    }
}

int FastBoard::count_pliberties(const int i) {
    return count_neighbours(EMPTY, i);
}

// count neighbours of color c at vertex v
// the border of the board has fake neighours of both colors
int FastBoard::count_neighbours(const int c, const int v) {
    assert(c == WHITE || c == BLACK || c == EMPTY);
    return (m_neighbours[v] >> (NBR_SHIFT * c)) & 7;
}

void FastBoard::add_neighbour(const int i, const int color) {
    assert(color == WHITE || color == BLACK || color == EMPTY);

    std::array<int, 4> nbr_pars;
    int nbr_par_cnt = 0;

    for (int k = 0; k < 4; k++) {
        int ai = i + m_dirs[k];

        m_neighbours[ai] += (1 << (NBR_SHIFT * color)) - (1 << (NBR_SHIFT * EMPTY));

        bool found = false;
        for (int i = 0; i < nbr_par_cnt; i++) {
            if (nbr_pars[i] == m_parent[ai]) {
                found = true;
                break;
            }
        }
        if (!found) {
            m_libs[m_parent[ai]]--;
            nbr_pars[nbr_par_cnt++] = m_parent[ai];
        }
    }
}

void FastBoard::remove_neighbour(const int i, const int color) {
    assert(color == WHITE || color == BLACK || color == EMPTY);

    std::array<int, 4> nbr_pars;
    int nbr_par_cnt = 0;

    for (int k = 0; k < 4; k++) {
        int ai = i + m_dirs[k];

        m_neighbours[ai] += (1 << (NBR_SHIFT * EMPTY))
                          - (1 << (NBR_SHIFT * color));

        bool found = false;
        for (int i = 0; i < nbr_par_cnt; i++) {
            if (nbr_pars[i] == m_parent[ai]) {
                found = true;
                break;
            }
        }
        if (!found) {
            m_libs[m_parent[ai]]++;
            nbr_pars[nbr_par_cnt++] = m_parent[ai];
        }
    }
}

int FastBoard::remove_string_fast(int i) {
    int pos = i;
    int removed = 0;
    int color = m_square[i];

    assert(color == WHITE || color == BLACK || color == EMPTY);

    do {
        assert(m_square[pos] == color);

        m_square[pos]  = EMPTY;
        m_parent[pos]  = MAXSQ;
        m_totalstones[color]--;

        remove_neighbour(pos, color);

        m_empty_idx[pos]     = m_empty_cnt;
        m_empty[m_empty_cnt] = pos;
        m_empty_cnt++;

        removed++;
        pos = m_next[pos];
    } while (pos != i);

    return removed;
}

std::vector<bool> FastBoard::calc_reach_color(int col) {
    auto bd = std::vector<bool>(m_maxsq, false);
    auto open = std::queue<int>();
    for (auto i = 0; i < m_boardsize; i++) {
        for (auto j = 0; j < m_boardsize; j++) {
            auto vertex = get_vertex(i, j);
            if (m_square[vertex] == col) {
                bd[vertex] = true;
                open.push(vertex);
            }
        }
    }
    while (!open.empty()) {
        /* colored field, spread */
        auto vertex = open.front();
        open.pop();

        for (auto k = 0; k < 4; k++) {
            auto neighbor = vertex + m_dirs[k];
            if (!bd[neighbor] && m_square[neighbor] == EMPTY) {
                bd[neighbor] = true;
                open.push(neighbor);
            }
        }
    }
    return bd;
}

float FastBoard::area_score(float komi) {
    auto white = calc_reach_color(WHITE);
    auto black = calc_reach_color(BLACK);

    auto score = -komi;

    for (int i = 0; i < m_boardsize; i++) {
        for (int j = 0; j < m_boardsize; j++) {
            auto vertex = get_vertex(i, j);

            if (white[vertex] && !black[vertex]) {
                score -= 1.0f;
            } else if (black[vertex] && !white[vertex]) {
                score += 1.0f;
            }
        }
    }

    return score;
}

int FastBoard::estimate_mc_score(float komi) {
    int wsc, bsc;

    bsc = m_totalstones[BLACK];
    wsc = m_totalstones[WHITE];

    return bsc-wsc-((int)komi)+1;
}

float FastBoard::final_mc_score(float komi) {
    int wsc, bsc;
    int maxempty = m_empty_cnt;

    bsc = m_totalstones[BLACK];
    wsc = m_totalstones[WHITE];

    for (int v = 0; v < maxempty; v++) {
        int i = m_empty[v];

        assert(m_square[i] == EMPTY);

        int allblack = ((m_neighbours[i] >> (NBR_SHIFT * BLACK)) & 7) == 4;
        int allwhite = ((m_neighbours[i] >> (NBR_SHIFT * WHITE)) & 7) == 4;

        if (allwhite) {
            wsc++;
        } else if (allblack) {
            bsc++;
        }
    }

    return (float)(bsc)-((float)(wsc)+komi);
}

void FastBoard::display_board(int lastmove) {
    int boardsize = get_boardsize();

    myprintf("\n   ");
    for (int i = 0; i < boardsize; i++) {
        if (i < 25) {
            myprintf("%c ", (('a' + i < 'i') ? 'a' + i : 'a' + i + 1));
        } else {
            myprintf("%c ", (('A' + (i-25) < 'I') ? 'A' + (i-25) : 'A' + (i-25) + 1));
        }
    }
    myprintf("\n");
    for (int j = boardsize-1; j >= 0; j--) {
        myprintf("%2d", j+1);
        if (lastmove == get_vertex(0, j))
            myprintf("(");
        else
            myprintf(" ");
        for (int i = 0; i < boardsize; i++) {
            if (get_square(i,j) == WHITE) {
                myprintf("O");
            } else if (get_square(i,j) == BLACK)  {
                myprintf("X");
            } else if (starpoint(boardsize, i, j)) {
                myprintf("+");
            } else {
                myprintf(".");
            }
            if (lastmove == get_vertex(i, j)) myprintf(")");
            else if (i != boardsize-1 && lastmove == get_vertex(i, j)+1) myprintf("(");
            else myprintf(" ");
        }
        myprintf("%2d\n", j+1);
    }
    myprintf("   ");
    for (int i = 0; i < boardsize; i++) {
         if (i < 25) {
            myprintf("%c ", (('a' + i < 'i') ? 'a' + i : 'a' + i + 1));
        } else {
            myprintf("%c ", (('A' + (i-25) < 'I') ? 'A' + (i-25) : 'A' + (i-25) + 1));
        }
    }
    myprintf("\n\n");
}

void FastBoard::merge_strings(const int ip, const int aip) {
    assert(ip != MAXSQ && aip != MAXSQ);

    /* merge stones */
    m_stones[ip] += m_stones[aip];

    /* loop over stones, update parents */
    int newpos = aip;

    do {
        // check if this stone has a liberty
        for (int k = 0; k < 4; k++) {
            int ai = newpos + m_dirs[k];
            // for each liberty, check if it is not shared
            if (m_square[ai] == EMPTY) {
                // find liberty neighbors
                bool found = false;
                for (int kk = 0; kk < 4; kk++) {
                    int aai = ai + m_dirs[kk];
                    // friendly string shouldn't be ip
                    // ip can also be an aip that has been marked
                    if (m_parent[aai] == ip) {
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    m_libs[ip]++;
                }
            }
        }

        m_parent[newpos] = ip;
        newpos = m_next[newpos];
    } while (newpos != aip);

    /* merge stings */
    int tmp = m_next[aip];
    m_next[aip] = m_next[ip];
    m_next[ip] = tmp;
}

int FastBoard::update_board_eye(const int color, const int i) {
    m_square[i]  = (square_t)color;
    m_next[i]    = i;
    m_parent[i]  = i;
    m_libs[i]    = 0;
    m_stones[i]  = 1;
    m_totalstones[color]++;

    add_neighbour(i, color);

    int captured_sq;
    int captured_stones = 0;

    for (int k = 0; k < 4; k++) {
        int ai = i + m_dirs[k];

        assert(ai >= 0 && ai <= m_maxsq);

        if (m_libs[m_parent[ai]] <= 0) {
            int this_captured    = remove_string_fast(ai);
            captured_sq          = ai;
            captured_stones     += this_captured;
        }
    }

    /* move last vertex in list to our position */
    int lastvertex               = m_empty[--m_empty_cnt];
    m_empty_idx[lastvertex]      = m_empty_idx[i];
    m_empty[m_empty_idx[i]]      = lastvertex;

    m_prisoners[color] += captured_stones;

    // possibility of ko
    if (captured_stones == 1) {
        return captured_sq;
    }

    return -1;
}

/*
    returns ko square or suicide tag
    does not update side to move
*/
int FastBoard::update_board_fast(const int color, const int i, bool & capture) {
    assert(m_square[i] == EMPTY);
    assert(color == WHITE || color == BLACK);

    /* did we play into an opponent eye? */
    int eyeplay = (m_neighbours[i] & s_eyemask[!color]);

    // because we check for single stone suicide, we know
    // its a capture, and it might be a ko capture
    if (eyeplay) {
        capture = true;
        return update_board_eye(color, i);
    }

    m_square[i]  = (square_t)color;
    m_next[i]    = i;
    m_parent[i]  = i;
    m_libs[i]    = count_pliberties(i);
    m_stones[i]  = 1;
    m_totalstones[color]++;

    add_neighbour(i, color);

    for (int k = 0; k < 4; k++) {
        int ai = i + m_dirs[k];

        if (m_square[ai] > WHITE) continue;

        assert(ai >= 0 && ai <= m_maxsq);

        if (m_square[ai] == !color) {
            if (m_libs[m_parent[ai]] <= 0) {
                capture = true;
                m_prisoners[color] += remove_string_fast(ai);
            }
        } else if (m_square[ai] == color) {
            int ip  = m_parent[i];
            int aip = m_parent[ai];

            if (ip != aip) {
                if (m_stones[ip] >= m_stones[aip]) {
                    merge_strings(ip, aip);
                } else {
                    merge_strings(aip, ip);
                }
            }
        }
    }

    /* move last vertex in list to our position */
    int lastvertex               = m_empty[--m_empty_cnt];
    m_empty_idx[lastvertex]      = m_empty_idx[i];
    m_empty[m_empty_idx[i]]      = lastvertex;

    assert(m_libs[m_parent[i]] < m_boardsize*m_boardsize);

    /* check whether we still live (i.e. detect suicide) */
    if (m_libs[m_parent[i]] == 0) {
        remove_string_fast(i);
    }

    return -1;
}

bool FastBoard::is_eye(const int color, const int i) {
    /* check for 4 neighbors of the same color */
    int ownsurrounded = (m_neighbours[i] & s_eyemask[color]);

    // if not, it can't be an eye
    // this takes advantage of borders being colored
    // both ways
    if (!ownsurrounded) {
        return false;
    }

    // 2 or more diagonals taken
    // 1 for side groups
    int colorcount[4];

    colorcount[BLACK] = 0;
    colorcount[WHITE] = 0;
    colorcount[INVAL] = 0;

    colorcount[m_square[i - 1 - m_boardsize - 2]]++;
    colorcount[m_square[i + 1 - m_boardsize - 2]]++;
    colorcount[m_square[i - 1 + m_boardsize + 2]]++;
    colorcount[m_square[i + 1 + m_boardsize + 2]]++;

    if (colorcount[INVAL] == 0) {
        if (colorcount[!color] > 1) {
            return false;
        }
    } else {
        if (colorcount[!color]) {
            return false;
        }
    }

    return true;
}

std::string FastBoard::move_to_text(int move) {
    std::ostringstream result;

    int column = move % (m_boardsize + 2);
    int row = move / (m_boardsize + 2);

    column--;
    row--;

    assert(move == FastBoard::PASS || move == FastBoard::RESIGN || (row >= 0 && row < m_boardsize));
    assert(move == FastBoard::PASS || move == FastBoard::RESIGN || (column >= 0 && column < m_boardsize));

    if (move >= 0 && move <= m_maxsq) {
        result << static_cast<char>(column < 8 ? 'A' + column : 'A' + column + 1);
        result << (row + 1);
    } else if (move == FastBoard::PASS) {
        result << "pass";
    } else if (move == FastBoard::RESIGN) {
        result << "resign";
    } else {
        result << "error";
    }

    return result.str();
}

std::string FastBoard::move_to_text_sgf(int move) {
    std::ostringstream result;

    int column = move % (m_boardsize + 2);
    int row = move / (m_boardsize + 2);

    column--;
    row--;

    assert(move == FastBoard::PASS || move == FastBoard::RESIGN || (row >= 0 && row < m_boardsize));
    assert(move == FastBoard::PASS || move == FastBoard::RESIGN || (column >= 0 && column < m_boardsize));

    // SGF inverts rows
    row = m_boardsize - row - 1;

    if (move >= 0 && move <= m_maxsq) {
        if (column <= 25) {
            result << static_cast<char>('a' + column);
        } else {
            result << static_cast<char>('A' + column - 26);
        }
        if (row <= 25) {
            result << static_cast<char>('a' + row);
        } else {
            result << static_cast<char>('A' + row - 26);
        }
    } else if (move == FastBoard::PASS) {
        result << "tt";
    } else if (move == FastBoard::RESIGN) {
	result << "tt";
    } else {
	result << "error";
    }

    return result.str();
}

bool FastBoard::starpoint(int size, int point) {
    int stars[3];
    int points[2];
    int hits = 0;

    if (size % 2 == 0 || size < 9) {
        return false;
    }

    stars[0] = size >= 13 ? 3 : 2;
    stars[1] = size / 2;
    stars[2] = size - 1 - stars[0];

    points[0] = point / size;
    points[1] = point % size;

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 3; j++) {
            if (points[i] == stars[j]) {
                hits++;
            }
        }
    }

    return hits >= 2;
}

bool FastBoard::starpoint(int size, int x, int y) {
    return starpoint(size, y * size + x);
}

int FastBoard::get_prisoners(int side) {
    assert(side == WHITE || side == BLACK);

    return m_prisoners[side];
}

int FastBoard::get_to_move() {
    return m_tomove;
}

void FastBoard::set_to_move(int tomove) {
    m_tomove = tomove;
}

std::string FastBoard::get_string(int vertex) {
    std::string result;

    int start = m_parent[vertex];
    int newpos = start;

    do {
        result += move_to_text(newpos) + " ";
        newpos = m_next[newpos];
    } while (newpos != start);

    // eat last space
    result.resize(result.size() - 1);

    return result;
}

std::string FastBoard::get_stone_list() {
    std::string res;

    for (int i = 0; i < m_boardsize; i++) {
        for (int j = 0; j < m_boardsize; j++) {
            int vertex = get_vertex(i, j);

            if (get_square(vertex) != EMPTY) {
                res += move_to_text(vertex) + " ";
            }
        }
    }

    // eat final space
    res.resize(res.size() - 1);

    return res;
}
