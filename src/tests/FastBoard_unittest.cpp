/*
    This file is part of Leela Zero.
    Copyright (C) 2018 Gian-Carlo Pascutto

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

#include <cstddef>
#include <gtest/gtest.h>
#include <limits>
#include <vector>

#include "Utils.h"
#include "FastBoard.h"

using namespace Utils;

TEST(FastBoardTest, Board3x3) {
    FastBoard b;
    b.reset_board(3);
    EXPECT_EQ(
        "\n   a b c \n 3 . . .  3\n 2 . . .  2\n 1 . . .  1\n   a b c \n\n",  
        b.serialize_board()
    );
}
