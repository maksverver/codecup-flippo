#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#define UNLIKELY(c) __builtin_expect((c), 0)
#define CHECK(c) if (!UNLIKELY(c)) CheckFailed(#c, __FILE__, __LINE__);
#define CHECK_EQ(a, b) CHECK((a) == (b))

#define FOR(i, a, b) for (int i = int(a); i < int(b); ++i)
#define REP(i, n) FOR(i, 0, n)

namespace {

const int H = 8;
const int W = 8;
const int MIN_VALUE = -9999;
const int MAX_VALUE = +9999;
const int MAX_MOVES = 60;  // TODO: prove a lower bound?

enum class Player : signed char { NONE = 0, WHITE = 1, BLACK = -1 };

Player Other(Player p) {
    return static_cast<Player>(-static_cast<int>(p));
}

struct Board {
    Player fields[H][W];
    Player next_player;
};

struct Move {
    Move() : row(0), col(0) {}
    constexpr Move(int row, int col) : row(row), col(col) {}

    short row, col;
};

unsigned rng_seed = ((unsigned)getpid() << 16) ^ (unsigned)time(NULL); 

bool ValidCoords(int r, int c) {
    return r >= 0 && r < W && c >= 0 && c < W;
}

bool operator==(const Board &a, const Board &b) {
    REP(r, H) REP(c, W) if (a.fields[r][c] != b.fields[r][c]) return false;
    if (a.next_player != b.next_player) return false;
    return true;
}

bool operator==(const Move &a, const Move &b) {
    return a.row == b.row && a.col == b.col;
}

void CheckFailed(const char *condition, const char *file, int line) {
    std::cerr << '[' << file << ':' << line << "] CHECK failed: " << condition << "!" << std::endl;
    abort();
}

Board InitialBoard() {
    Board board = {{}, Player::WHITE};
    board.fields[H/2 - 1][W/2 - 1] = Player::WHITE;
    board.fields[H/2 - 1][W/2 - 0] = Player::BLACK;
    board.fields[H/2 - 0][W/2 - 1] = Player::BLACK;
    board.fields[H/2 - 0][W/2 - 0] = Player::WHITE;
    return board;
}

bool HasFlips(const Player (&fields)[H][W], Player p, int r, int c) {
    for (int dr = -1; dr <= +1; ++dr) {
        for (int dc = -1; dc <= +1; ++dc) {
            if (dr || dc) {
                for (int n = 1; ; ++n) {
                    int r2 = r + dr*n;
                    int c2 = c + dc*n;
                    if (!ValidCoords(r2, c2)) break;
                    Player o = fields[r2][c2];
                    if (o == Player::NONE) break;
                    if (o == p && n > 1) return true;
                }
            }
        }
    }
    return false;
}

void DoFlips(Player (*fields)[H][W], Player p, int r, int c) {
    for (int dr = -1; dr <= +1; ++dr) {
        for (int dc = -1; dc <= +1; ++dc) {
            if (dr || dc) {
                int last_n = 0;
                for (int n = 1; ; ++n) {
                    int r2 = r + dr*n;
                    int c2 = c + dc*n;
                    if (!ValidCoords(r2, c2)) break;
                    Player o = (*fields)[r2][c2];
                    if (o == Player::NONE) break;
                    if (o == p) last_n = n;
                }
                for (int n = 1; n < last_n; ++n) {
                    Player &o = (*fields)[r + n*dr][c + n*dc];
                    o = Other(o);
                }
            }
        }
    }
}

bool HasOccupiedNeighbor(const Player (&fields)[H][W], int r, int c) {
    CHECK_EQ(fields[r][c], Player::NONE);
    int min_r = r > 0 ? r - 1 : 0;
    int max_r = r < H - 1 ? r + 1 : H - 1;
    int min_c = c > 0 ? c - 1 : 0;
    int max_c = c < W - 1 ? c + 1 : W - 1;
    for (int r2 = min_r; r2 <= max_r; ++r2) {
        for (int c2 = min_c; c2 <= max_c; ++c2) {
            if (fields[r2][c2] != Player::NONE) return true;
        }
    }
    return false;
}

void DoMove(Board *board, Move move) {
    Player (&fields)[H][W] = board->fields;
    Player p = board->next_player;
    CHECK_EQ(fields[move.row][move.col], Player::NONE);
    fields[move.row][move.col] = p;
    DoFlips(&fields, p, move.row, move.col);
    board->next_player = Other(p);
}

void UndoMove(Board *board, Move move) {
    Player (&fields)[H][W] = board->fields;
    Player p = Other(board->next_player);
    CHECK_EQ(fields[move.row][move.col], p);
    fields[move.row][move.col] = Player::NONE;
    DoFlips(&fields, p, move.row, move.col);
    board->next_player = p;
}

int ListMoves(const Board &board, Move (*moves)[MAX_MOVES]) {
    const Player (&fields)[H][W] = board.fields;
    const Player p = board.next_player;
    int total_moves = 0;
    REP(r, H) REP(c, W) {
        if (fields[r][c] == Player::NONE && HasOccupiedNeighbor(fields, r, c)) {
            (*moves)[total_moves++] = Move(r, c);
        }
    }
    int flipping_moves = 0;
    REP(i, total_moves) {
        if (HasFlips(fields, p, (*moves)[i].row, (*moves)[i].col)) {
            (*moves)[flipping_moves++] = (*moves)[i];
        }
    }
    return flipping_moves > 0 ? flipping_moves : total_moves;
}

int Evaluate(const Board &board) {
    const Player (&fields)[H][W] = board.fields;
    const Player p = board.next_player;
    const Player q = Other(p);;
    int score = 0;
    REP(r, H) REP(c, W) {
        if (fields[r][c] == Player::NONE) {
            if (HasOccupiedNeighbor(fields, r, c)) {
                if (HasFlips(fields, p, r, c)) score += 2;
                if (HasFlips(fields, q, r, c)) score -= 2;
            }
        } else {
            score += (fields[r][c] == p) ? +1 : -1;
        }
    }
    return score;
}

int Search(Board *board, int depth) {
    if (depth <= 0) {
        return Evaluate(*board);
    }
    Move moves[MAX_MOVES];
    int num_moves = ListMoves(*board, &moves);
    if (num_moves <= 0) {
        // TODO: special end-game evaluation.
        return Evaluate(*board);
    }
    int best_value = MIN_VALUE - 1;
    REP(i, num_moves) {
        Move move = moves[i];
        DoMove(board, move);
        int value = -Search(board, depth - 1);
        UndoMove(board, move);
        if (value > best_value) best_value = value;
    }
    CHECK(best_value >= MIN_VALUE);
    CHECK(best_value <= MAX_VALUE);
    return best_value;
}

bool SelectMove(const Board &original_board, Move *best_move) {
    Board board = original_board;
    Move moves[MAX_MOVES];
    int num_moves = ListMoves(board, &moves);
    std::random_shuffle(&moves[0], &moves[num_moves]);
    int best_value = MIN_VALUE - 1;
    REP(i, num_moves) {
        Move move = moves[i];
        DoMove(&board, move);
        int value = -Search(&board, 3);
        UndoMove(&board, move);
        if (value > best_value) {
            best_value = value;
            *best_move = move;
        }
    }
    std::cerr << "best_value=" << best_value << '\n';
    CHECK(board == original_board);
    return best_value >= MIN_VALUE;
}

bool MoveIsValid(const Board &board, const Move &move) {
    Move moves[MAX_MOVES];
    int num_moves = ListMoves(board, &moves);
    REP(i, num_moves) {
        if (moves[i] == move) return true;
    }
    return false;
}

std::string FormatMove(const Move &move) {
    std::string s;
    s.reserve(2);
    s += char('A' + move.row);
    s += char('1' + move.col);
    return s;
}

bool ParseMove(const std::string &s, Move *move) {
    if (s.size() != 2) return false;
    int row = s[0] - 'A';
    int col = s[1] - '1';
    if (!ValidCoords(row, col)) return false;
    move->row = row;
    move->col = col;
    return true;
}

}  // namespace

int main(int argc, char *argv[]) {
    std::cerr << "TODO: print player name & version string\n";
    std::cerr << "rng_seed=" << rng_seed << '\n';
    srand(rng_seed);
    Player my_player = Player::NONE;
    Board board = InitialBoard();
    Move move;
    for (;;) {
        if (my_player == board.next_player) {
            if (!SelectMove(board, &move)) {
                std::cerr << "No move possible. Exiting.\n";
                return 0;
            }
            std::cout << FormatMove(move) << std::endl;
        } else {
            std::string line;
            if (!std::getline(std::cin, line)) {
                std::cerr << "Premature end of input.\n";
                return 1;
            }
            if (line == "Quit") {
                std::cerr << "Quit received. Exiting.";
                return 0;
            }
            if (my_player == Player::NONE) {
                if (line == "Start") {
                    my_player = Player::WHITE;
                    continue;
                }
                my_player = Player::BLACK;
            }
            if (!ParseMove(line, &move)) {
                std::cerr << "Invalid move received: [" << line << "]\n";
                return 1;
            }
        }
        CHECK(MoveIsValid(board, move));
        DoMove(&board, move);
    }
}
