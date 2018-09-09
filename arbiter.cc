#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <sstream>
#include <utility>
#include <vector>

namespace {

const int SIZE = 8;
const int MAX_MOVES = 60;

struct State {
  char fields[SIZE][SIZE];
  int moves_played;
};

struct Move {
  int row;
  int col;
};

bool operator==(const Move &a, const Move &b) {
  return a.row == b.row && a.col == b.col;
}

State InitialState() {
  State state = {{}, 0};
  state.fields[SIZE/2 - 1][SIZE/2 - 1] = 1;
  state.fields[SIZE/2 - 1][SIZE/2 - 0] = 2;
  state.fields[SIZE/2 - 0][SIZE/2 - 1] = 2;
  state.fields[SIZE/2 - 0][SIZE/2 - 0] = 1;
  return state;
}

bool ValidCoords(int r, int c) {
  return 0 <= r && r < SIZE && 0 <= c && c < SIZE;
}

int CalculateScore(const State &state) {
  int scores[3] = {0, 0, 0};
  for (int r = 0; r < SIZE; ++r) {
    for (int c = 0; c < SIZE; ++c) {
      ++scores[int{state.fields[r][c]}];
    }
  }
  return scores[1] - scores[2];
}

// End of game logic. Arbiter logic follows.

struct Player {
  const int fd_in;
  const int fd_out;
  const pid_t pid;
};

std::string ReadLine(Player &player) {
  // In theory, the player is allowed to write less or more than one line at a
  // time. However, we currently don't support that. We expect to read exactly
  // one line at a time; nothing more, nothing less.
  char buf[1024];
  ssize_t n = read(player.fd_out, buf, sizeof(buf));
  std::string line;
  if (n < 0) {
    perror("read()");
    return line;
  }
  if (n == 0) {
    fprintf(stderr, "End of file reached!\n");
    return line;
  }
  char *p = reinterpret_cast<char*>(memchr(buf, '\n', n));
  if (p == NULL) {
    fprintf(stderr, "End of line not found!\n");
    return line;
  }
  if (p != buf + n - 1) {
    fprintf(stderr, "Extra data after end of line found!\n");
    return line;
  }
  line.assign(buf, n - 1);
  return line;
}

// Returns a copy of `s` with all non-ASCII characters escaped, using C-style
// escapes (e.g. "\x09" == tab).
std::string EscapeString(const std::string &s) {
  std::string t;
  t.reserve(s.size() + 2);
  t += '"';
  static const char hexdigits[] = "0123456789abcdef";
  for (char ch : s) {
    if (ch >= 32 && ch <= 126) {
      if (ch == '\\' || ch == '"') {
        t += '\\';
      }
      t += ch;
    } else {
      t += "\\x";
      t += hexdigits[(ch & 0xf0) >> 4];
      t += hexdigits[(ch & 0x0f) >> 0];
    }
  }
  t += '"';
  return t;
}

bool Write(Player &player, const std::string &s) {
  // Temporarily ignore SIGPIPE to avoid aborting the process if the write
  // fails. This makes this function non-thread-safe!
  void (*old_handler)(int) = signal(SIGPIPE, SIG_IGN);
  ssize_t size_written = write(player.fd_in, s.data(), s.size());
  signal(SIGPIPE, old_handler);
  return size_written == static_cast<ssize_t>(s.size());
}

void Quit(Player &player) {
  Write(player, "Quit\n");  // may fail if player has already exited
  close(player.fd_in);
  int status = 0;
  if (waitpid(player.pid, &status, 0) != player.pid) {
    perror("waitpid");
  } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "Player did not exit normally! status=%d\n", status);
  }
  close(player.fd_out);
}

Player SpawnPlayer(const char *command, const char *log_filename) {
  int pipe_in[2];
  int pipe_out[2];
  if (pipe(pipe_in) != 0 || pipe(pipe_out) != 0) {
    perror("pipe()");
    exit(1);
  }
  pid_t pid = fork();
  if (pid == -1) {
    perror("fork()");
    exit(1);
  }
  if (pid == 0) {
    // Child process.
    if (dup2(pipe_in[0], 0) != 0 || dup2(pipe_out[1], 1) != 1) {
      perror("dup2()");
      exit(1);
    }
    int fd_err = open(log_filename, O_CREAT | O_TRUNC | O_RDWR, 0666);
    if (fd_err < 0) {
      fprintf(stderr, "Cannot open logfile [%s]!\n", log_filename);
      exit(1);
    } else {
      dup2(fd_err, 2);
      close(fd_err);
    }

    if (close(pipe_in[0]) != 0 || close(pipe_in[1]) != 0 ||
        close(pipe_out[0]) != 0 || close(pipe_out[1]) != 0) {
      perror("close()");
      exit(1);
    }
    execl("/bin/sh", "/bin/sh", "-c", command, NULL);
    perror("exec");
    exit(1);
  } else {
    // Parent process.
    if (close(pipe_in[0]) != 0 || close(pipe_out[1]) != 0) {
      perror("close()");
      exit(1);
    }
    Player player = { pipe_in[1], pipe_out[0], pid };
    return player;
  }
}

struct PlayerQuitter {
  PlayerQuitter(Player &player) : player(player) {}
  ~PlayerQuitter() { Quit(player); }

private:
  PlayerQuitter(const PlayerQuitter&) = delete;
  void operator=(const PlayerQuitter&) = delete;

  Player &player;
};

bool IsGameOver(const State &state) {
  return state.moves_played >= MAX_MOVES;
}

int GetNextPlayer(const State &state) {
  return state.moves_played & 1;
}

std::string FormatMove(const Move &move) {
  std::string s(2, '\0');
  s[0] = 'A' + move.row;
  s[1] = '1' + move.col;
  return s;
}

bool ParseMove(const std::string &s, Move *move) {
  if (s.size() != 2) {
    return false;
  }
  int row = s[0] - 'A';
  int col = s[1] - '1';
  if (!ValidCoords(row, col)) {
    return false;
  }
  move->row = row;
  move->col = col;
  return true;
}

bool HasOccupiedNeighbor(const State &state, int r, int c) {
  for (int r2 = r - 1; r2 <= r + 1; ++r2) {
    for (int c2 = c - 1; c2 <= c + 1; ++c2) {
      if ((r2 != r || c2 != c) && ValidCoords(r2, c2) && state.fields[r2][c2] != 0) {
        return true;
      }
    }
  }
  return false;
}

// Finds coordinates of pieces to be flipped.
//
// Callback should act like a bool(*)(int, int), and return `true` to continue
// finding flipped pieces or `false` to abort the search.
//
// FindFlips() returns false if the search was aborted, true otherwise (i.e.,
// no flips were found, or all callback invocations returned true.)
template<class Callback>
bool FindFlips(const State &state, int player, int r, int c, Callback callback) {
  for (int dr = -1; dr <= 1; ++dr) {
    for (int dc = -1; dc <=1; ++dc) {
      if (dr != 0 || dc != 0) {
        int last_n = 0;
        for (int n = 1; ; ++n) {
          const int r2 = r + n*dr;
          const int c2 = c + n*dc;
          if (!ValidCoords(r2, c2) || state.fields[r2][c2] == 0) {
            break;
          }
          if (state.fields[r2][c2] == player) {
            last_n = n;
          }
        }
        for (int n = 1; n < last_n; ++n) {
          if (!callback(r + n*dr, c + n *dc)) {
            return false;
          }
        }
      }
    }
  }
  return true;
}

std::vector<Move> ListValidMoves(const State &state) {
  std::vector<Move> all_moves;
  const int player = (state.moves_played & 1) + 1;
  for (int r = 0; r < SIZE; ++r) {
    for (int c = 0; c < SIZE; ++c) {
      if (state.fields[r][c] == 0 && HasOccupiedNeighbor(state, r, c)) {
        all_moves.push_back(Move{r, c});
      }
    }
  }
  std::vector<Move> flipping_moves;
  for (const Move &move : all_moves) {
    if (!FindFlips(state, player, move.row, move.col, [](int, int) { return false; })) {
      flipping_moves.push_back(move);
    }
  }
  if (flipping_moves.empty()) {
    return all_moves;
  } else {
    return flipping_moves;
  }
}

bool ValidateMove(const State &state, const Move &move, std::string *reason = nullptr) {
  std::vector<Move> valid_moves = ListValidMoves(state);
  if (std::find(valid_moves.begin(), valid_moves.end(), move) == valid_moves.end()) {
    if (reason != nullptr) {
      *reason = "Valid moves:";
      for (const Move &move : valid_moves) {
        *reason += ' ';
        *reason += FormatMove(move);
      }
    }
    return false;
  } else {
    return true;
  }
}

void ExecuteMove(State &state, const Move &move) {
  const int player = (state.moves_played & 1) + 1;
  FindFlips(state, player, move.row, move.col, [&state](int r, int c) {
    state.fields[r][c] ^= 3;
    return true;
  });
  state.fields[move.row][move.col] = player;
  state.moves_played += 1;
}

std::string EncodeHistory(const std::vector<Move> &moves) {
  std::string s;
  s.reserve(moves.size()*2);
  for (const Move &move : moves) {
    s += FormatMove(move);
  }
  return s;
}

/*
double GetWallTime() {
  struct timeval tv;
  int res = gettimeofday(&tv, NULL);
  assert(res == 0);
  return tv.tv_sec + tv.tv_usec*1e-6;
}
*/

double GetMonotonicTime() {
  struct timespec ts;
  int res = clock_gettime(CLOCK_MONOTONIC, &ts);
  assert(res == 0);
  return ts.tv_sec + ts.tv_nsec*1e-9;
}

struct GameResult {
  std::string transcript;
  int score;
  double walltime_used[2];
};

GameResult RunGame(const char *command_player1, const char *command_player2,
    const char *log_filename1, const char *log_filename2) {
  Player players[2] = {
    SpawnPlayer(command_player1, log_filename1),
    SpawnPlayer(command_player2, log_filename2)};
  PlayerQuitter quit1(players[0]);
  PlayerQuitter quit2(players[1]);

  State state = InitialState();
  std::vector<Move> history;
  double time_used[2] = {0.0, 0.0};
  double time_start = GetMonotonicTime();
  bool started = false;
  while (!IsGameOver(state)) {
    const int next_player = GetNextPlayer(state);
    assert(next_player == 0 || next_player == 1);
    if (!started) {
      if (!Write(players[0], "Start\n")) {
        fprintf(stderr, "Could not send 'Start' to player %d!\n", next_player);
        break;
      }
      started = true;
    }
    std::string line = ReadLine(players[next_player]);
    time_used[next_player] += GetMonotonicTime() - time_start;
    Move move;
    if (!ParseMove(line, &move)) {
      fprintf(stderr, "Could not parse move from player %d %s!\n",
          next_player, EscapeString(line).c_str());
      break;
    }
    std::string reason;
    if (!ValidateMove(state, move, &reason)) {
      fprintf(stderr, "Invalid move from player %d %s: (%s)!\n",
          next_player, EscapeString(line).c_str(), reason.c_str());
      break;
    }
    ExecuteMove(state, move);
    history.push_back(move);
    if (!IsGameOver(state)) {
      // Send player's move to other player.
      std::string s = FormatMove(move);
      time_start = GetMonotonicTime();
      if (!Write(players[1 - next_player], s + '\n')) {
        fprintf(stderr, "Could not send '%s' to player %d!\n", s.c_str(), next_player);
        break;
      }
    }
  }
  int score = 0;
  if (IsGameOver(state)) {
    // Regular game end.
    score = CalculateScore(state);
  } else {
    const int failing_player = GetNextPlayer(state);
    if (failing_player == 0) {
      // White made an illegal move. Black wins.
      score = -99;
    } else if (failing_player == 1) {
      // Black made an illegal move. White wins.
      score = +99;
    } else {
      assert(false);
    }
  }
  return {EncodeHistory(history), score, {time_used[0], time_used[1]}};
}

// Maybe: support competition mode with random number of players?
void Main(const char *player1_command, const char *player2_command, int rounds,
    const char *logs_prefix) {
  int wins[2] = {0, 0};
  int ties[2] = {0, 0};
  int losses[2] = {0, 0};
  int failures[2] = {0, 0};
  int score_by_color[2][2] = {{0, 0}, {0, 0}};
  int score[2] = {0, 0};
  double total_time[2] = {0.0, 0.0};
  double max_time[2] = {0.0, 0.0};

  char filename_buf[2][1024];

  const char *player_commands[2] = {player1_command, player2_command};
  const char *program_names[2] = {"p1", "p2"};
  const char *role_names[2] = {"white", "black"};
  int games = rounds <= 0 ? 1 : 2*rounds;
  for (int game = 0; game < games; ++game) {
    int p = game & 1;
    int q = 1 - p;

    if (logs_prefix == nullptr) {
      snprintf(filename_buf[0], sizeof(filename_buf[0]), "/dev/null");
      snprintf(filename_buf[1], sizeof(filename_buf[1]), "/dev/null");
    } else if (strcmp(logs_prefix, "-") == 0) {
      snprintf(filename_buf[0], sizeof(filename_buf[0]), "/dev/stderr");
      snprintf(filename_buf[1], sizeof(filename_buf[1]), "/dev/stderr");
    } else {
      snprintf(filename_buf[0], sizeof(filename_buf[0]), "%s%04d_%s_%s",
          logs_prefix, game, program_names[p], role_names[0]);
      snprintf(filename_buf[1], sizeof(filename_buf[1]), "%s%04d_%s_%s",
          logs_prefix, game, program_names[q], role_names[1]);
    }
    GameResult result = RunGame(player_commands[p], player_commands[q],
        filename_buf[0], filename_buf[1]);
    printf("%4d: %s %s%d\n", game, result.transcript.c_str(),
        (result.score > 0 ? "+" : ""), result.score);
    score[p] += result.score;
    score[q] -= result.score;
    score_by_color[p][0] += result.score;
    score_by_color[q][1] += -result.score;
    wins[p] += result.score > 0;
    wins[q] += result.score < 0;
    ties[p] += result.score == 0;
    ties[q] += result.score == 0;
    losses[p] += result.score < 0;
    losses[q] += result.score > 0;
    failures[p] += result.score == -99;
    failures[q] += result.score == +99;
    total_time[p] += result.walltime_used[0];
    total_time[q] += result.walltime_used[1];
    max_time[p] = std::max(max_time[p], result.walltime_used[0]);
    max_time[q] = std::max(max_time[q], result.walltime_used[1]);
  }
  if (games > 1) {
    printf("\n");
    printf("Player               AvgTm MaxTm Wins Ties Loss Fail RedPts BluePt Total\n");
    printf("-------------------- ----- ----- ---- ---- ---- ---- ------ ------ ------\n");
    for (int i = 0; i < 2; ++i) {
      const char *command = player_commands[i];
      while (strlen(command) > 20 && strchr(command, '/')) {
        command = strchr(command, '/') + 1;
      }
      printf("%-20s %.3f %.3f %4d %4d %4d %4d %+6d %+6d %+6d\n",
          command, total_time[i]/games, max_time[i],
          wins[i], ties[i], losses[i], failures[i],
          score_by_color[i][0], score_by_color[i][1], score[i]);
    }
  }
}

}  // namespace

int main(int argc, char *argv[]) {
  int opt_rounds = 0;
  const char *opt_logs_prefix = nullptr;
  // Parse option arguments.
  int j = 1;
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (*arg != '-') {
      argv[j++] = argv[i];
      continue;
    }
    int value = 0;
    if (sscanf(argv[i], "--rounds=%d", &value) == 1) {
      opt_rounds = value;
    } else if (strncmp(argv[i], "--logs=", strlen("--logs=")) == 0) {
      opt_logs_prefix = arg + strlen("--logs=");
    } else {
      fprintf(stderr, "Unrecognized option argument: '%s'!\n", argv[i]);
    }
  }
  argc = j;
  if (argc != 3) {
    printf("Usage: arbiter [--rounds=<N>] [--logs=<filename-prefix>] <player1> <player2>\n");
    return 1;
  }
  Main(argv[1], argv[2], opt_rounds, opt_logs_prefix);
  return 0;
}
