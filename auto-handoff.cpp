// auto-handoff: a Claude Code hook that holds auto-compaction until the compacting agent has
// written a handoff, then loads that handoff into its fresh context.
//
// Every hook event runs this one binary (see README.md for the flow). Per-session state lives in
// ~/.local/state/claude-handoffs/<session_id>/: a <agent>.md slot per agent ("main", or a subagent's
// agent_id), state.json, events/<agent> (its mtime is that agent's last activity), types/<agent>
// ("<agent_type>\t<rw|ro>\t<window>"), paths/<agent> (where that subagent's transcript is) and mode
// (the main session's permission mode).
//
// Writing the slot is the signal that the handoff is ready. The slot holds the handoff itself, or the
// path of a handoff file the session already keeps (say a HANDOFF.md in the project), which stays
// where it is. Loaded handoffs, and project handoffs about to be trimmed, are copied to
// archive/<project folder>/, never pruned, unless git has them committed unchanged.
//
// Claude Code's compaction hooks don't say which agent is compacting. PreCompact fires right after
// the compacting agent's own tool call, so the hook takes the agent active a moment ago whose context
// is near its compaction point, else the largest context near its point; when none is, it doesn't
// hold. After compaction each agent picks its own handoff from labelled candidates, because passing
// identity from hook to hook races when agents compact in parallel.
//
// Hooks run on every tool call, so this is a dependency-free native binary that skips the parts of
// its input it doesn't need.

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>
#include <utility>
#include <vector>

extern char** environ;

namespace {

const double kCeiling = 450000;            // past this many tokens, compact without a handoff
const double kDefaultWindow = 200000;      // unknown model: assume a small window so the limit errs early
const double kWindowShare = 0.9;           // never hold past this share of the model's real window
const double kTriggerGap = 33000;          // auto-compact fires about this far below its window
const double kTriggerSlack = 50000;        // an agent further below its trigger than this isn't compacting
const double kMinRoom = 40000;             // hold only where the safety limit is this far above the trigger
const double kTightSlack = 20000;          // a small-window agent this close to its trigger is never blocked
const double kStandInSlack = 10000;        // an agent this close to its trigger may be the one compacting
const double kDeferSeconds = 120;          // a compaction held to check who's compacting goes through next time
const double kAttributeSeconds = 2;        // PreCompact follows the compacting agent's tool call within this
const int kSubagentMaxHolds = 12;          // a subagent held this often compacts without a handoff
const double kReadySeconds = 600;          // a handoff this much older than its compaction is stale
const size_t kInjectCap = 9500;            // Claude Code moves hook context over 10,000 chars to a file
const long kHandoffCap = 9000;             // a longer handoff holds compaction until it's trimmed
const long kReadCap = 1 << 20;             // handoff files are read and archived up to this size
const int kNudgeEvery = 5;                 // tool calls between reminders while a compaction waits
const double kIdleSeconds = 20 * 60;       // a compaction this long after the last activity is idle compaction
const double kClearLinkSeconds = 120;      // SessionEnd(clear) and the new session's start are this close
const double kKeepDays = 14;
const long kTranscriptTail = 4 << 20;      // bytes of transcript searched for the latest usage
const long kTranscriptDeep = 32 << 20;     // searched instead when the tail has none (say, many images)
const int kFactMessages = 3;               // latest messages to an agent kept word for word in its facts file
const size_t kFactMessageCap = 4000;       // characters kept of each
const size_t kFactGitCap = 12000;          // bytes kept of each git listing
const double kGitSeconds = 2;              // a git command taking longer is skipped

// Families with a native 1M window (from 2.1.302's model table); anything else is assumed 200k.
const char* kMillionWindow[] = {"claude-opus-5",   "claude-sonnet-5", "claude-haiku-5-5", "claude-fable-5",
                                "claude-mythos-5", "claude-opus-4-7", "claude-opus-4-8"};
// Built-in agent types that can't write files; custom ones are read from their definitions.
const char* kReadOnlyTypes[] = {"Explore", "Plan"};

// The note opens every message that brings an agent back from a compaction or /clear.
const char* kUserNote = "A note from the user: make no mistakes. I love you.";

const char* kNudge =
    "[auto-handoff] Your context is large ({size}) and compaction is waiting for your handoff. Don't start "
    "anything new: finish the step you're on and bring the work to a clean point, with nothing half-edited. "
    "Then write the handoff.\n{where}\nCompaction runs as soon as {path} is written, and the handoff is "
    "loaded into your fresh context so you can carry on.\n\n"
    "Write it for a reader who has only the handoff, keeping any structure the file already has. Unless it "
    "has its own heading, start with \"Owner: \" and your task in a few words (\"main session\" first if you "
    "are the main conversation). Cover: the goal and what the user asked for; what's in progress and not "
    "started; the next steps in order; key files and what each holds; decisions that still constrain the "
    "work, and why; open questions for the user.\n\n"
    "Housekeeping, so the handoff stays small however long the session runs:\n"
    "- Rewrite it rather than append to it. An earlier handoff in your context is material to prune, not a "
    "base to add to.\n"
    "- Record where things stand now, not the history: earlier versions are kept (in {archive}; a committed "
    "handoff file is left to git history). Collapse finished work into a \"Done so far\" list of at most 10 "
    "one-line entries, merging the oldest as it grows.\n"
    "- Drop what no longer matters: resolved questions, superseded decisions, files no longer involved, "
    "dead ends nobody will retry. Keep a failure only if it would otherwise be repeated.\n"
    "{lasting}\n"
    "- Aim for 3,000 to 6,000 characters. Over 9,000, you'll be asked to trim it before compaction runs.";

// Where lasting knowledge goes: the main session owns the project's docs and memory; a subagent reports.
const char* kLastingMain =
    "- Knowledge that should outlive this session (conventions, lasting decisions, findings) belongs in "
    "the project's own docs or memory, not here. If it isn't recorded there yet, record it there now and "
    "keep at most a one-line pointer.";

const char* kLastingSubagent =
    "- Knowledge that should outlive your task belongs in your final report, or wherever your instructions "
    "say, not here.";

const char* kWhereMain =
    "- If this session already keeps a handoff file (say a HANDOFF.md the user asked you to maintain), "
    "update that file in its usual place and way, then use the Write tool to put its absolute path, alone, "
    "in {path}.\n"
    "- Otherwise, use the Write tool to write the handoff itself to {path}.";

const char* kWhereKnown =
    "This session's handoff file is {file}. Update it there in its usual way, then use the Write tool to "
    "put its absolute path, alone, in {path}.";

const char* kWhereSubagent =
    "Use the Write tool to write your own handoff, the text itself rather than a path, to {path}. That's "
    "separate from any handoff or progress files your instructions have you keep: keep those as they say, "
    "but don't rewrite the main session's handoff for this.";

const char* kAskedMain =
    "[auto-handoff] If you're asked for a handoff: if this session already keeps a handoff file, update it "
    "and put its absolute path, alone, in {path}; otherwise write the handoff itself to {path}.";

const char* kAskedSubagent =
    "[auto-handoff] If the auto-handoff hook asks you for a handoff before compaction, write it, the text "
    "itself, to {path}. Handoffs or progress notes that your instructions or the main session ask for go "
    "where they say, as usual.";

const char* kReminder =
    "[auto-handoff] Reminder: compaction is still waiting for your handoff ({path}). Write it at the next "
    "clean point; past the safety limit the context is compacted without one.";

const char* kDangling =
    "[auto-handoff] {path} holds \"{text}\", which looks like a path, but not one to a handoff file: it "
    "has to be the absolute path of an existing file named like *handoff*.md. Write that path alone to "
    "{path}, or write the handoff itself there.";

const char* kTrimSlot =
    "[auto-handoff] Your handoff at {path} is {chars} characters, over the 9,000 limit. Prune it with the "
    "housekeeping rules (rewrite it, collapse finished work, drop what no longer matters, {move}) and write "
    "it to {path} again. The full version is kept: {copy}. Compaction waits for this until your context reaches about {limit} tokens, then "
    "goes ahead with the handoff as it stands.";

const char* kTrimFile =
    "[auto-handoff] Your handoff at {file} is {chars} characters, over the 9,000 limit. Prune it with the "
    "housekeeping rules (rewrite it, collapse finished work, drop what no longer matters, {move}), making "
    "the edits in its usual way. The full version is kept: {copy}. If moving knowledge out of it is a big "
    "job, note in the handoff what still has to move and do that after compaction. When it's done, write "
    "its path to {path} again. Compaction waits for this until your context reaches about {limit} tokens, "
    "then goes ahead with the handoff as it stands.";

const char* kTrimDone =
    "[auto-handoff] Your handoff at {file} is now {chars} characters. When you've finished pruning it, "
    "write its path to {path} again so compaction can go ahead.";

const char* kTrimReminder =
    "[auto-handoff] Reminder: compaction is waiting for your handoff ({file}, {chars} characters) to be "
    "pruned under 9,000 characters. When it's done, write {what} to {path} again.";

// Settled without its handoff loaded: the release went to another agent, or nothing was released.
const char* kMissed =
    "[auto-handoff] Your context was compacted a moment ago, and the handoff you wrote for it may not have "
    "been loaded. If it isn't in your context already, read {file} before going on.";

// PreCompact's plain stdout is appended to the summary prompt (undocumented; read from 2.1.302's code
// and verified live). With a handoff about to be loaded, the full summary is redundant: keep it short.
// The summarizer checks whose handoff it is, because the hook can't be sure who is compacting. It
// can't look for the handoff being written: that's in the recent stretch Claude Code keeps word for
// word, which the summarizer doesn't see. But each agent is told its own slot when it starts, when
// it's asked for a handoff, and after each compaction, and no other agent is.
const char* kSummaryInstructions =
    "If this conversation names {path} as the agent's own handoff slot, that handoff will be loaded right "
    "after this summary as the main record of the work. In that case keep the summary under 100 words: "
    "the user's latest request in their own words, and anything still open; skip the usual sections. If "
    "the conversation never names that path, ignore this note and write the full summary as usual.";

struct Config {
  std::string root, log, config_dir, settings;
  double ceiling = kCeiling;
} cfg;

// ---- A small JSON value: enough for hook input, transcript lines and our own state file.

struct Json {
  enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> fields;

  static Json object() {
    Json j;
    j.type = Obj;
    return j;
  }
  static Json number(double v) {
    Json j;
    j.type = Num;
    j.n = v;
    return j;
  }
  static Json string(const std::string& v) {
    Json j;
    j.type = Str;
    j.s = v;
    return j;
  }
  const Json* get(const std::string& key) const {
    if (type != Obj) return nullptr;
    for (const auto& f : fields)
      if (f.first == key) return &f.second;
    return nullptr;
  }
  Json* get(const std::string& key) { return const_cast<Json*>(static_cast<const Json*>(this)->get(key)); }
  // The field, created if missing. References to other fields of this object don't survive it.
  Json& at(const std::string& key) {
    if (type != Obj) *this = object();
    if (Json* j = get(key)) return *j;
    fields.emplace_back(key, Json());
    return fields.back().second;
  }
  bool has(const std::string& key) const { return get(key) != nullptr; }
  std::string str(const std::string& key) const {
    const Json* j = get(key);
    return j && j->type == Str ? j->s : "";
  }
  double num(const std::string& key, double def = 0) const {
    const Json* j = get(key);
    return j && j->type == Num ? j->n : def;
  }
  bool truthy(const std::string& key) const {
    const Json* j = get(key);
    return j && j->type == Bool && j->b;
  }
  void erase(const std::string& key) {
    fields.erase(std::remove_if(fields.begin(), fields.end(), [&](const auto& f) { return f.first == key; }),
                 fields.end());
  }
};

class Parser {
 public:
  explicit Parser(const std::string& text) : t_(text) {}
  bool parse(Json& out) {
    ws();
    if (!value(out, 0)) return false;
    ws();
    return i_ == t_.size();
  }

  // A top-level object, keeping only the listed keys; the other values are stepped over unparsed.
  bool parse_top(Json& out, const std::vector<std::string>& keep) {
    ws();
    if (i_ >= t_.size() || t_[i_] != '{') return false;
    out.type = Json::Obj;
    ++i_;
    ws();
    if (i_ < t_.size() && t_[i_] == '}') return ++i_, true;
    while (true) {
      ws();
      std::string key;
      if (i_ >= t_.size() || t_[i_] != '"' || !str(key)) return false;
      ws();
      if (i_ >= t_.size() || t_[i_++] != ':') return false;
      ws();
      if (std::find(keep.begin(), keep.end(), key) != keep.end()) {
        out.fields.emplace_back(std::move(key), Json());
        if (!value(out.fields.back().second, 1)) return false;
      } else if (!skip()) {
        return false;
      }
      ws();
      if (i_ >= t_.size()) return false;
      if (t_[i_] == ',') { ++i_; continue; }
      if (t_[i_] == '}') return ++i_, true;
      return false;
    }
  }

 private:
  const std::string& t_;
  size_t i_ = 0;

  // Jumps between quotes with memchr. After an escaped quote, escapes tend to be dense (JSON inside a
  // string), so it steps byte by byte for a while instead of restarting memchr at every one.
  bool skip_string() {
    size_t from = ++i_, n = t_.size();
    const char* s = t_.data();
    while (true) {
      const void* p = memchr(s + from, '"', n - from);
      if (!p) return false;
      size_t q = size_t(static_cast<const char*>(p) - s), slashes = 0;
      while (s[q - 1 - slashes] == '\\') ++slashes;
      from = q + 1;
      if (slashes % 2 == 0) return i_ = from, true;
      for (size_t stop = std::min(n, from + 4096); from < stop; ++from) {
        if (s[from] == '\\') ++from;
        else if (s[from] == '"') return i_ = from + 1, true;
      }
      if (from > n) return false;
    }
  }
  bool skip() {
    if (i_ >= t_.size()) return false;
    if (t_[i_] == '"') return skip_string();
    if (t_[i_] != '{' && t_[i_] != '[') {  // number or literal
      size_t start = i_;
      while (i_ < t_.size() && !strchr(",}] \t\r\n", t_[i_])) ++i_;
      return i_ > start;
    }
    int depth = 0;
    while (i_ < t_.size()) {
      char c = t_[i_];
      if (c == '"') {
        if (!skip_string()) return false;
        continue;
      }
      ++i_;
      if (c == '{' || c == '[') ++depth;
      else if ((c == '}' || c == ']') && --depth == 0) return true;
    }
    return false;
  }

  void ws() {
    while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\n' || t_[i_] == '\r' || t_[i_] == '\t')) ++i_;
  }
  bool lit(const char* w) {
    size_t n = strlen(w);
    if (t_.compare(i_, n, w) != 0) return false;
    i_ += n;
    return true;
  }
  bool value(Json& j, int depth) {
    if (depth > 256 || i_ >= t_.size()) return false;
    char c = t_[i_];
    if (c == '{') {
      j.type = Json::Obj;
      ++i_;
      ws();
      if (i_ < t_.size() && t_[i_] == '}') return ++i_, true;
      while (true) {
        ws();
        std::string key;
        if (i_ >= t_.size() || t_[i_] != '"' || !str(key)) return false;
        ws();
        if (i_ >= t_.size() || t_[i_++] != ':') return false;
        ws();
        j.fields.emplace_back(std::move(key), Json());
        if (!value(j.fields.back().second, depth + 1)) return false;
        ws();
        if (i_ >= t_.size()) return false;
        if (t_[i_] == ',') { ++i_; continue; }
        if (t_[i_] == '}') return ++i_, true;
        return false;
      }
    }
    if (c == '[') {
      j.type = Json::Arr;
      ++i_;
      ws();
      if (i_ < t_.size() && t_[i_] == ']') return ++i_, true;
      while (true) {
        ws();
        j.items.emplace_back();
        if (!value(j.items.back(), depth + 1)) return false;
        ws();
        if (i_ >= t_.size()) return false;
        if (t_[i_] == ',') { ++i_; continue; }
        if (t_[i_] == ']') return ++i_, true;
        return false;
      }
    }
    if (c == '"') return j.type = Json::Str, str(j.s);
    if (c == 't') return j.type = Json::Bool, j.b = true, lit("true");
    if (c == 'f') return j.type = Json::Bool, lit("false");
    if (c == 'n') return lit("null");
    size_t start = i_;
    while (i_ < t_.size() && strchr("+-0123456789.eE", t_[i_])) ++i_;
    if (i_ == start) return false;
    j.type = Json::Num;
    j.n = strtod(t_.substr(start, i_ - start).c_str(), nullptr);
    return true;
  }
  bool hex4(unsigned& cp) {
    if (i_ + 4 > t_.size()) return false;
    cp = 0;
    for (int k = 0; k < 4; ++k) {
      char h = t_[i_++];
      cp <<= 4;
      if (h >= '0' && h <= '9') cp |= h - '0';
      else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
      else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
      else return false;
    }
    return true;
  }
  static void utf8(unsigned cp, std::string& out) {
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xC0 | (cp >> 6));
      out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += char(0xE0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }
  bool str(std::string& out) {
    ++i_;  // opening quote
    while (i_ < t_.size()) {
      size_t run = i_;
      while (i_ < t_.size() && t_[i_] != '"' && t_[i_] != '\\') ++i_;
      out.append(t_, run, i_ - run);
      if (i_ >= t_.size()) return false;
      if (t_[i_++] == '"') return true;
      if (i_ >= t_.size()) return false;
      char e = t_[i_++];
      switch (e) {
        case '"': case '\\': case '/': out += e; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && t_.compare(i_, 2, "\\u") == 0) {
            i_ += 2;
            unsigned lo;
            if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(cp, out);
          break;
        }
        default: return false;
      }
    }
    return false;
  }
};

std::string quote(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof buf, "\\u%04x", c);
          o += buf;
        } else {
          o += char(c);
        }
    }
  }
  return o + "\"";
}

void dump(const Json& j, std::string& o) {
  switch (j.type) {
    case Json::Null: o += "null"; break;
    case Json::Bool: o += j.b ? "true" : "false"; break;
    case Json::Num: {
      char buf[40];
      snprintf(buf, sizeof buf, j.n == std::floor(j.n) && std::fabs(j.n) < 1e15 ? "%.0f" : "%.6f", j.n);
      o += buf;
      break;
    }
    case Json::Str: o += quote(j.s); break;
    case Json::Arr:
      o += '[';
      for (size_t k = 0; k < j.items.size(); ++k) {
        if (k) o += ',';
        dump(j.items[k], o);
      }
      o += ']';
      break;
    case Json::Obj:
      o += '{';
      for (size_t k = 0; k < j.fields.size(); ++k) {
        if (k) o += ',';
        o += quote(j.fields[k].first) + ':';
        dump(j.fields[k].second, o);
      }
      o += '}';
      break;
  }
}

// ---- Files and time.

double now() {
  timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec + tv.tv_usec / 1e6;
}

long file_size(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 ? long(st.st_size) : -1;
}

double mtime(const std::string& p) {
  struct stat st;
  if (stat(p.c_str(), &st) != 0) return -1;
#ifdef __APPLE__
  return st.st_mtimespec.tv_sec + st.st_mtimespec.tv_nsec / 1e9;
#else
  return st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9;
#endif
}

// Regular files only, opened without blocking (a FIFO in the wrong place must not hang a hook).
bool read_file(const std::string& p, std::string& out, long tail = -1, long cap = -1) {
  int fd = open(p.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return false;
  }
  if (tail >= 0 && st.st_size > tail) lseek(fd, st.st_size - tail, SEEK_SET);
  char buf[65536];
  ssize_t n;
  while ((n = read(fd, buf, sizeof buf)) > 0) {
    out.append(buf, size_t(n));
    if (cap >= 0 && long(out.size()) >= cap) {
      out.resize(size_t(cap));
      break;
    }
  }
  close(fd);
  return true;
}

void mkdirs(const std::string& p) {
  for (size_t k = 1; k <= p.size(); ++k)
    if (k == p.size() || p[k] == '/') mkdir(p.substr(0, k).c_str(), 0700);
}

void write_atomic(const std::string& p, const std::string& data) {
  std::string tmp = p + ".tmp" + std::to_string(getpid());
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return;
  fwrite(data.data(), 1, data.size(), f);
  fclose(f);
  rename(tmp.c_str(), p.c_str());
}

void touch(const std::string& p) {
  int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_NONBLOCK, 0600);
  if (fd >= 0) close(fd);
  utimes(p.c_str(), nullptr);
}

std::vector<std::string> list_dir(const std::string& p) {
  std::vector<std::string> names;
  if (DIR* dir = opendir(p.c_str())) {
    while (dirent* e = readdir(dir))
      if (e->d_name[0] != '.') names.push_back(e->d_name);
    closedir(dir);
  }
  return names;
}

bool is_dir(const std::string& p) {
  struct stat st;
  return lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void remove_tree(const std::string& p) {
  if (DIR* dir = opendir(p.c_str())) {
    while (dirent* e = readdir(dir)) {
      std::string name = e->d_name;
      if (name == "." || name == "..") continue;
      std::string child = p + "/" + name;
      if (is_dir(child)) remove_tree(child);
      else unlink(child.c_str());
    }
    closedir(dir);
  }
  rmdir(p.c_str());
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
  for (size_t k = s.find(from); k != std::string::npos; k = s.find(from, k + to.size())) s.replace(k, from.size(), to);
  return s;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
  for (auto& c : s) c = char(tolower(static_cast<unsigned char>(c)));
  return s;
}

bool ends_with(const std::string& s, const std::string& end) {
  return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
}

bool safe_id(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  for (char c : s)
    if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
  return true;
}

std::string slug(const std::string& cwd) { return cwd.empty() ? "unknown" : replace_all(cwd, "/", "-"); }

std::string stamp(const char* format) {
  time_t t = time(nullptr);
  char buf[32];
  strftime(buf, sizeof buf, format, localtime(&t));
  return buf;
}

void log(const std::string& d, const std::string& msg) {
  if (cfg.log.empty()) return;
  std::string sid = d.substr(d.rfind('/') + 1, 8);
  if (FILE* f = fopen(cfg.log.c_str(), "a")) {
    fprintf(f, "%s %s %s\n", stamp("%H:%M:%S").c_str(), sid.c_str(), msg.c_str());
    fclose(f);
  }
}

void emit(const std::string& s) { fwrite(s.data(), 1, s.size(), stdout); }

void context(const std::string& event, const std::string& text) {
  emit("{\"hookSpecificOutput\":{\"hookEventName\":" + quote(event) + ",\"additionalContext\":" + quote(text) + "}}");
}

void block(const std::string& reason) { emit("{\"decision\":\"block\",\"reason\":" + quote(reason) + "}"); }

// ---- Session state: state.json under a lock, since parallel subagents run hooks concurrently.

class Lock {
 public:
  explicit Lock(const std::string& d) {
    mkdirs(d);
    fd_ = open((d + "/.lock").c_str(), O_RDWR | O_CREAT, 0600);
    if (fd_ >= 0) flock(fd_, LOCK_EX);
  }
  ~Lock() {
    if (fd_ >= 0) close(fd_);
  }

 private:
  int fd_;
};

// Read without the lock too: writes are atomic renames.
Json load_state(const std::string& d) {
  std::string text;
  Json st;
  if (read_file(d + "/state.json", text)) Parser(text).parse(st);
  if (st.type != Json::Obj) st = Json::object();
  if (!st.get("agents") || st.get("agents")->type != Json::Obj) st.at("agents") = Json::object();
  return st;
}

void save_state(const std::string& d, const Json& st) {
  std::string text;
  dump(st, text);
  write_atomic(d + "/state.json", text);
}

void set(Json& a, const std::string& key, double v) { a.at(key) = Json::number(v); }

// Back to "nothing waiting", keeping what's known about the agent's handoff file.
void settle(Json& a) {
  std::string file = a.str("file");
  a = Json::object();
  set(a, "compacted", now());
  if (!file.empty()) a.at("file") = Json::string(file);
}

// ---- Handoffs: the slot, and the file it may point to.

std::string slot_path(const std::string& d, const std::string& agent) { return d + "/" + agent + ".md"; }

// A file the slot may point to: named like a handoff, so the slot can't pull just any file into context.
bool handoff_like(const std::string& p) {
  std::string name = lower(p.substr(p.rfind('/') + 1));
  return name.find("handoff") != std::string::npos &&
         (ends_with(name, ".md") || ends_with(name, ".markdown") || ends_with(name, ".txt"));
}

// The slot's text when it is a path rather than a handoff: one line naming a .md, .markdown or .txt
// file, without the quotes or backticks it may have been written with. Empty otherwise. Only the main
// session's slot can point: a subagent's slot is always its own handoff, so a subagent is never asked
// to prune a project file it may share with others.
std::string pointer(const std::string& d, const std::string& agent) {
  if (agent != "main") return "";
  std::string text;
  read_file(slot_path(d, agent), text, -1, 4096);
  text = trim(text);
  while (text.size() >= 2 && strchr("`\"'", text[0]) && text.back() == text[0])
    text = trim(text.substr(1, text.size() - 2));
  std::string name = lower(text);
  if (text.empty() || text.size() > 1024 || text.find('\n') != std::string::npos ||
      !(ends_with(name, ".md") || ends_with(name, ".markdown") || ends_with(name, ".txt")))
    return "";
  return text;
}

// The handoff itself: the slot, or the existing handoff file whose path the slot holds (absolute, or
// relative to the session's folder).
std::string handoff_file(const std::string& d, const std::string& agent) {
  std::string slot = slot_path(d, agent), text = pointer(d, agent);
  if (text.empty()) return slot;
  if (text.compare(0, 2, "~/") == 0) text = std::string(getenv("HOME") ? getenv("HOME") : "") + text.substr(1);
  if (text[0] != '/') {
    std::string cwd = load_state(d).str("cwd");
    if (cwd.empty()) return slot;
    text = cwd + "/" + (text.compare(0, 2, "./") == 0 ? text.substr(2) : text);
  }
  struct stat st;
  if (text == slot || !handoff_like(text) || stat(text.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return slot;
  return text;
}

// The slot holds a path that doesn't lead to a handoff file, so there is nothing to load yet.
bool dangling(const std::string& d, const std::string& agent) {
  return !pointer(d, agent).empty() && handoff_file(d, agent) == slot_path(d, agent);
}

std::string read_handoff(const std::string& p) {
  std::string text;
  read_file(p, text, -1, kReadCap);
  return trim(text);
}

// Written at or after `since`, and holding a handoff (or the path of one).
bool slot_ready(const std::string& d, const std::string& agent, double since) {
  std::string slot = slot_path(d, agent);
  if (mtime(slot) < since) return false;
  std::string file = handoff_file(d, agent);
  if (file != slot) return file_size(file) > 0;
  return !read_handoff(slot).empty() && pointer(d, agent).empty();
}

int git(const std::string& dir, const std::vector<std::string>& args) {
  std::vector<std::string> all = {"git", "--no-optional-locks", "-C", dir};
  all.insert(all.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& a : all) argv.push_back(&a[0]);
  argv.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  pid_t pid;
  int rc = posix_spawnp(&pid, "git", &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  int status = 0;
  if (rc != 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

// What a git command in `dir` prints, up to `cap` bytes (the rest is read and dropped). Empty if it
// fails or takes longer than kGitSeconds, when it's stopped.
std::string git_text(const std::string& dir, const std::vector<std::string>& args, size_t cap) {
  std::vector<std::string> all = {"git", "--no-optional-locks", "-C", dir};
  all.insert(all.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& a : all) argv.push_back(&a[0]);
  argv.push_back(nullptr);
  int fds[2];
  if (pipe(fds) != 0) return "";
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addclose(&fa, fds[0]);
  posix_spawn_file_actions_addclose(&fa, fds[1]);
  pid_t pid;
  int rc = posix_spawnp(&pid, "git", &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  close(fds[1]);
  std::string out;
  if (rc == 0) {
    double deadline = now() + kGitSeconds;
    char buf[8192];
    while (true) {
      double left = deadline - now();
      if (left <= 0) {
        kill(pid, SIGKILL);
        break;
      }
      pollfd pfd = {fds[0], POLLIN, 0};
      if (poll(&pfd, 1, int(left * 1000) + 1) <= 0) continue;
      ssize_t n = read(fds[0], buf, sizeof buf);
      if (n <= 0) break;
      if (out.size() < cap) out.append(buf, std::min(size_t(n), cap - out.size()));
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) out.clear();
  }
  close(fds[0]);
  return out;
}

// In git and unchanged since the last commit: git history already holds this version.
bool committed_clean(const std::string& path) {
  size_t cut = path.rfind('/');
  std::string dir = path.substr(0, cut), name = path.substr(cut + 1);
  return git(dir, {"ls-files", "--error-unmatch", "--", name}) == 0 &&
         git(dir, {"diff", "--quiet", "HEAD", "--", name}) == 0;
}

std::string archive_dir(const std::string& cwd) { return cfg.root + "/archive/" + slug(cwd); }

// Keep a copy of an agent's handoff, named by time, session and agent. Returns where it went, "git"
// when git history already has it, or "" when there was nothing to keep.
std::string archive(const std::string& d, const std::string& agent, const std::string& cwd, const std::string& tag) {
  std::string slot = slot_path(d, agent), file = handoff_file(d, agent), text;
  if (!read_file(file, text, -1, kReadCap) || trim(text).empty()) return "";
  if (file != slot && committed_clean(file)) return "git";
  std::string dir = archive_dir(cwd);
  mkdirs(dir);
  std::string base = dir + "/" + stamp("%Y-%m-%d_%H%M%S") + "_" + d.substr(d.rfind('/') + 1, 8) + "_" + agent +
                     (tag.empty() ? "" : "_" + tag);
  std::string p = base + ".md";
  for (int n = 2; mtime(p) >= 0; ++n) p = base + "-" + std::to_string(n) + ".md";
  write_atomic(p, trim(text) + "\n");
  return p;
}

// ---- Agents.

void note_event(const std::string& d, const std::string& agent) {
  std::string dir = d + "/events";
  if (mtime(dir) < 0) mkdirs(dir);
  touch(dir + "/" + agent);
}

struct Event {
  std::string agent;
  double t;
};

// Agents by latest activity, newest first.
std::vector<Event> events(const std::string& d) {
  std::vector<Event> evs;
  for (const auto& name : list_dir(d + "/events")) evs.push_back({name, mtime(d + "/events/" + name)});
  std::sort(evs.begin(), evs.end(), [](const Event& a, const Event& b) { return a.t > b.t; });
  return evs;
}

// A subagent's type, whether it can write a handoff, and the auto-compact window its definition sets
// (0 for none), as on_subagent_start recorded them in types/<agent>: "<type>\t<rw|ro>\t<window>".
struct AgentInfo {
  std::string type;
  bool readonly = false;
  double window = 0;
};

AgentInfo agent_info(const std::string& d, const std::string& agent) {
  AgentInfo info;
  std::string text;
  if (agent == "main" || !read_file(d + "/types/" + agent, text)) return info;
  size_t a = text.find('\t'), b = a == std::string::npos ? a : text.find('\t', a + 1);
  info.type = trim(text.substr(0, a));
  info.readonly = a != std::string::npos && trim(text.substr(a + 1, b == std::string::npos ? b : b - a - 1)) == "ro";
  info.window = b == std::string::npos ? 0 : atof(text.c_str() + b + 1);
  return info;
}

bool skip_digits(const std::string& s, size_t& k) {
  size_t from = k;
  while (k < s.size() && isdigit(static_cast<unsigned char>(s[k]))) ++k;
  return k > from;
}

// A number the way 2.1.302 reads the env var and agent files: an integer, scientific notation ("1e6"),
// or digits grouped by one separator ("333,000"); anything else is read up to its first non-digit, so
// "333k" is 333 (only /autocompact understands k and m). -1 when it isn't a positive integer.
double claude_number(const std::string& raw) {
  std::string s = trim(raw);
  if (s.empty() || s[0] == '-') return -1;
  size_t start = s[0] == '+', k = start;
  if (s.size() <= 32) {
    bool whole = skip_digits(s, k), frac = false;
    if (k < s.size() && s[k] == '.') frac = skip_digits(s, ++k);
    if ((whole || frac) && k < s.size() && (s[k] == 'e' || s[k] == 'E')) {
      if (++k < s.size() && (s[k] == '+' || s[k] == '-')) ++k;
      if (skip_digits(s, k) && k == s.size()) {
        double v = strtod(s.c_str() + start, nullptr);
        return v > 0 && v == std::floor(v) ? v : -1;
      }
    }
    k = start;
    skip_digits(s, k);
    if (k - start >= 1 && k - start <= 3 && k < s.size() && strchr("_, ", s[k])) {
      char sep = s[k];
      std::string plain = s.substr(start, k - start);
      bool grouped = true;
      while (grouped && k < s.size()) {
        size_t group = k + 1;
        k = group;
        grouped = s[group - 1] == sep && skip_digits(s, k) && k - group == 3;
        plain += s.substr(group, 3);
      }
      if (grouped) return atof(plain.c_str()) > 0 ? atof(plain.c_str()) : -1;
    }
  }
  k = start;
  if (!skip_digits(s, k)) return -1;
  double v = atof(s.substr(start, k - start).c_str());
  return v > 0 ? v : -1;
}

// A top-level frontmatter value: the rest of its line, or the "- item" lines under it, as one string.
std::string front_value(const std::string& front, const std::string& key) {
  std::vector<std::string> lines;
  for (size_t at = 0;;) {
    size_t eol = front.find('\n', at);
    lines.push_back(front.substr(at, eol == std::string::npos ? eol : eol - at));
    if (eol == std::string::npos) break;
    at = eol + 1;
  }
  for (size_t k = 0; k < lines.size(); ++k) {
    if (lines[k].compare(0, key.size() + 1, key + ":") != 0) continue;
    std::string value = trim(lines[k].substr(key.size() + 1));
    for (size_t j = k + 1; j < lines.size(); ++j) {
      std::string item = trim(lines[j]);
      if (item.empty()) continue;
      if (!strchr(" \t-", lines[j][0])) break;  // the next key
      if (item[0] == '-') value += (value.empty() ? "" : ", ") + trim(item.substr(1));
      else value += " " + item;  // a value continued on the next line, such as a [...] list
    }
    return value;
  }
  return "";
}

// Whether a tool list (comma or space separated, brackets and quotes allowed) names any of `tools`.
bool lists(const std::string& list, std::initializer_list<const char*> tools) {
  std::string word;
  for (size_t k = 0; k <= list.size(); ++k) {
    char c = k < list.size() ? list[k] : ',';
    if (isalnum(static_cast<unsigned char>(c)) || c == '*' || c == '_') {
      word += c;
      continue;
    }
    if (c == '(') {  // Write(src/**) still writes
      size_t close = list.find(')', k);
      k = close == std::string::npos ? list.size() : close;
    }
    for (const char* t : tools)
      if (word == t) return true;
    word.clear();
  }
  return false;
}

// What a subagent's definition says: read-only when its tools leave out both Write and Edit (memory
// adds them back), and the auto-compact window it may set for itself.
AgentInfo definition(const std::string& cwd, const std::string& type) {
  AgentInfo info;
  info.type = type;
  for (const char* t : kReadOnlyTypes)
    if (type == t) info.readonly = true;
  if (info.readonly || type.empty() || type.find('/') != std::string::npos || type.find("..") != std::string::npos)
    return info;
  for (const std::string& dir : {cwd + "/.claude/agents", cfg.config_dir + "/agents"}) {
    std::string text;
    if (!read_file(dir + "/" + type + ".md", text, -1, 16384)) continue;
    text = replace_all(text, "\r\n", "\n");
    if (text.compare(0, 4, "---\n") != 0) return info;
    size_t end = text.find("\n---", 3);
    std::string front = end == std::string::npos ? text.substr(4) : end < 4 ? "" : text.substr(4, end - 4);
    std::string tools = front_value(front, "tools"), denied = front_value(front, "disallowedTools");
    bool writes = tools.empty() || lists(tools, {"Write", "Edit", "*"});
    if (lists(denied, {"Write"}) && lists(denied, {"Edit"})) writes = false;
    info.readonly = !writes && front_value(front, "memory").empty();
    std::string raw = front_value(front, "autoCompactWindow");
    if (raw.size() >= 2 && (raw[0] == '"' || raw[0] == '\'') && raw.back() == raw[0]) raw = raw.substr(1, raw.size() - 2);
    double w = claude_number(raw);
    if (w >= 1e5 && w <= 1e6) info.window = w;  // Claude Code ignores values outside this range
    return info;
  }
  return info;
}

// ---- Transcripts and compaction points.

// Where an agent's transcript is: the main one, or a subagent's, which may sit in a workflow folder.
std::string transcript_of(const std::string& d, const std::string& main, const std::string& agent) {
  if (agent == "main" || main.empty()) return main;
  std::string cached;
  if (read_file(d + "/paths/" + agent, cached) && mtime(trim(cached)) >= 0) return trim(cached);
  std::string base = (ends_with(main, ".jsonl") ? main.substr(0, main.size() - 6) : main) + "/subagents";
  std::string name = "agent-" + agent + ".jsonl", found;
  std::vector<std::pair<std::string, int>> dirs = {{base, 0}};
  while (!dirs.empty() && found.empty()) {
    auto [dir, depth] = dirs.back();
    dirs.pop_back();
    if (mtime(dir + "/" + name) >= 0) found = dir + "/" + name;
    else if (depth < 3)
      for (const auto& child : list_dir(dir))
        if (is_dir(dir + "/" + child)) dirs.push_back({dir + "/" + child, depth + 1});
  }
  if (!found.empty()) {
    mkdirs(d + "/paths");
    write_atomic(d + "/paths/" + agent, found);
  }
  return found;
}

struct Usage {
  double tokens = -1;
  std::string model;
};

bool compact_boundary(const Json& o) { return o.str("type") == "system" && o.str("subtype") == "compact_boundary"; }

// Latest context size and model, from the last real assistant entry in a transcript. API errors and
// other synthetic entries carry zero usage and are skipped. A compaction after that entry means the
// context is small now, whatever the entry said: 0 tokens.
Usage usage(const std::string& transcript, bool main, long tail = kTranscriptTail) {
  Usage u;
  std::string text;
  if (transcript.empty() || !read_file(transcript, text, tail)) return u;
  size_t end = text.size();
  while (end > 0) {
    size_t start = text.rfind('\n', end - 1);
    start = start == std::string::npos ? 0 : start + 1;
    std::string line = text.substr(start, end - start);
    end = start > 0 ? start - 1 : 0;
    bool boundary = line.find("\"compact_boundary\"") != std::string::npos;
    if (!boundary && line.find("\"usage\"") == std::string::npos) continue;
    Json o;
    if (!Parser(line).parse(o) || (main && o.truthy("isSidechain"))) continue;
    if (boundary && compact_boundary(o)) return u.tokens = 0, u;
    const Json* m = o.get("message");
    const Json* use = m ? m->get("usage") : nullptr;
    if (o.truthy("isApiErrorMessage") || !use || use->type != Json::Obj || m->str("model") == "<synthetic>") continue;
    // The next request carries this one's input and its output.
    double tokens = use->num("input_tokens") + use->num("cache_creation_input_tokens") +
                    use->num("cache_read_input_tokens") + use->num("output_tokens");
    if (tokens <= 0) continue;
    u.tokens = tokens;
    u.model = m->str("model");
    return u;
  }
  if (tail < kTranscriptDeep && long(text.size()) >= tail) return usage(transcript, main, kTranscriptDeep);
  return u;
}

// Whether a transcript records a compaction at or after `since` (a Unix time).
bool compacted_since(const std::string& transcript, double since) {
  std::string text;
  if (transcript.empty() || !read_file(transcript, text, kTranscriptTail)) return false;
  for (size_t k = text.find("\"compact_boundary\""); k != std::string::npos; k = text.find("\"compact_boundary\"", k + 1)) {
    size_t start = text.rfind('\n', k), end = text.find('\n', k);
    start = start == std::string::npos ? 0 : start + 1;
    Json o;
    if (!Parser(text.substr(start, end == std::string::npos ? std::string::npos : end - start)).parse(o) ||
        !compact_boundary(o))
      continue;
    std::string ts = o.str("timestamp");  // 2026-10-17T21:39:20.603Z
    tm parts = {};
    if (ts.size() < 19 || !strptime(ts.substr(0, 19).c_str(), "%Y-%m-%dT%H:%M:%S", &parts)) continue;
    if (double(timegm(&parts)) + 1 >= since) return true;
  }
  return false;
}

// The model's canonical name, which modelSettings keys use: without a provider prefix, [1m], a Vertex
// or Bedrock suffix, or a date.
std::string canonical(std::string model) {
  size_t at = model.find("claude-");
  if (at != std::string::npos) model = model.substr(at);
  model = model.substr(0, model.find_first_of("[@:"));
  size_t dash = model.rfind('-');
  if (dash != std::string::npos && model[dash + 1] == 'v' && model.find_first_not_of("0123456789", dash + 2) == std::string::npos)
    model = model.substr(0, dash);  // Bedrock's -v1
  dash = model.rfind('-');
  if (dash != std::string::npos && model.size() - dash - 1 == 8 &&
      model.find_first_not_of("0123456789", dash + 1) == std::string::npos)
    model = model.substr(0, dash);  // -20260815
  return model;
}

double window(const std::string& model) {
  if (ends_with(model, "[1m]")) return 1e6;
  std::string name = canonical(model);
  for (const char* prefix : kMillionWindow)
    if (name.compare(0, strlen(prefix), prefix) == 0) return 1e6;
  return kDefaultWindow;
}

// The auto-compact window set by CLAUDE_CODE_AUTO_COMPACT_WINDOW, held to 100k–1M as Claude Code
// holds it. 0 when it isn't set or isn't a number, which Claude Code ignores too.
double env_window() {
  const char* e = getenv("CLAUDE_CODE_AUTO_COMPACT_WINDOW");
  double v = e && *e ? claude_number(e) : -1;
  return v > 0 ? std::min(std::max(v, 1e5), 1e6) : 0;
}

// The auto-compact window set for a model, read the way Claude Code reads it: the env override, else
// the model's own modelSettings entry, else the top-level setting. "auto", or nothing set, is 0.
double configured_window(const std::string& model) {
  if (double e = env_window()) return e;
  static bool loaded = false;
  static Json settings;
  if (!loaded) {
    loaded = true;
    std::string text;
    if (read_file(cfg.settings, text)) Parser(text).parse(settings);
  }
  const Json* ms = settings.get("modelSettings");
  std::string name = canonical(model);
  const Json* entry = nullptr;
  if (ms)
    for (const auto& f : ms->fields)
      if (f.second.has("autoCompactWindow") && canonical(f.first) == name && (!entry || f.first == name))
        entry = &f.second;
  if (entry) return entry->num("autoCompactWindow");  // "auto" is 0: the model's own window
  return settings.num("autoCompactWindow");
}

// Roughly where auto-compact fires for a model, given the window an agent's definition may set.
double trigger_point(const std::string& model, double agent_window = 0) {
  double w = window(model), c = configured_window(model);
  if (c > 0) w = std::min(w, c);
  if (agent_window > 0 && env_window() == 0) w = std::min(w, agent_window);  // the env override wins
  double t = w - kTriggerGap;
  if (const char* p = getenv("CLAUDE_AUTOCOMPACT_PCT_OVERRIDE")) {
    double pct = atof(p);
    if (pct > 0 && pct <= 100) t = std::min(t, std::floor((w - 20000) * pct / 100));
  }
  return t;
}

bool planning(const std::string& d);

// An agent that may be compacting, and what holding it would mean.
struct Candidate {
  std::string agent;
  Usage u;
  AgentInfo info;
  std::string instead;  // a holdable agent at its own compaction point, which may be the one compacting
  double trigger = 0, limit = 0;
  bool at_limit = false;  // past the safety limit: never block
  bool tight = false;     // its window leaves no room to hold it
  bool holdable = false;  // could be held for a handoff now
};

// Who is compacting: the agents active a moment ago whose context is near their compaction point,
// newest first, else those near it among recently active agents, largest first. Empty when no one is.
// When the likeliest one can't be held (read-only, planning, a small window, already released) but a
// holdable one is right at its own compaction point, `instead` names it: the caller holds the attempt
// once, so the retry shows who it is. Never when blocking might not be safe.
Candidate compacting(const std::string& d, const std::string& transcript, const std::vector<Event>& evs, double t,
                     const Json& st) {
  auto near = [&](const std::string& agent, Candidate& c) {
    c.agent = agent;
    c.u = usage(transcript_of(d, transcript, agent), agent == "main");
    c.info = agent_info(d, agent);
    double trigger = c.trigger = trigger_point(c.u.model, c.info.window);
    if (c.u.tokens <= 0 || c.u.tokens < trigger - kTriggerSlack) return false;
    c.limit = std::min(cfg.ceiling, kWindowShare * window(c.u.model));
    c.at_limit = c.u.tokens >= c.limit;
    c.tight = c.limit - trigger < kMinRoom;
    const Json* a = st.get("agents")->get(agent);
    c.holdable = !c.info.readonly && !c.tight && !(agent == "main" && planning(d)) && !(a && a->has("released"));
    return !c.tight || c.u.tokens >= trigger - kTightSlack;  // a small window is only near when really close
  };
  std::vector<Candidate> found;
  for (const auto& e : evs) {
    if (t - e.t > kAttributeSeconds) break;
    Candidate c;
    if (near(e.agent, c)) found.push_back(c);
  }
  if (found.empty()) {
    std::vector<std::string> pool = {"main"};
    for (const auto& e : evs)
      if (e.agent != "main" && t - e.t <= kIdleSeconds) pool.push_back(e.agent);
    for (const auto& agent : pool) {
      Candidate c;
      if (near(agent, c)) found.push_back(c);
    }
    std::stable_sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) { return a.u.tokens > b.u.tokens; });
  }
  if (found.empty()) return {};
  for (const auto& c : found)
    if (c.at_limit || c.tight) return c;
  Candidate top = found[0];
  if (!top.holdable)
    for (const auto& c : found)
      if (c.holdable && c.u.tokens >= c.trigger - kStandInSlack) {
        top.instead = c.agent;
        break;
      }
  return top;
}

// ---- Messages.

std::string nudge(const std::string& d, const std::string& agent, const Json& a, bool full, const std::string& cwd) {
  std::string p = slot_path(d, agent);
  if (!full) return replace_all(kReminder, "{path}", p);
  std::string size = a.num("tokens") > 0 ? "about " + std::to_string(long(a.num("tokens")) / 1000) + "k tokens"
                                         : "near its limit";
  std::string where = agent != "main"           ? kWhereSubagent
                      : a.str("file").empty()   ? kWhereMain
                                                : replace_all(kWhereKnown, "{file}", a.str("file"));
  std::string text = replace_all(replace_all(kNudge, "{where}", where), "{archive}", archive_dir(cwd));
  text = replace_all(text, "{lasting}", agent == "main" ? kLastingMain : kLastingSubagent);
  return replace_all(replace_all(text, "{size}", size), "{path}", p);
}

std::string thousands(double tokens) { return std::to_string(long(tokens) / 1000) + "k"; }

// A trim was asked for and the slot hasn't been written again since.
bool needs_trim(const std::string& d, const std::string& agent, const Json& a) {
  return a.has("trim") && mtime(slot_path(d, agent)) < a.num("trim");
}

// Where the full version of a handoff being trimmed is kept.
std::string kept(const Json& a) {
  std::string copy = a.str("trim_copy");
  return copy == "git" ? "git history has it" : copy.empty() ? "no copy was made" : "a copy is at " + copy;
}

std::string trim_request(const std::string& d, const std::string& agent, const Json& a, bool reminder) {
  std::string slot = slot_path(d, agent), file = handoff_file(d, agent);
  std::string text = reminder                     ? kTrimReminder
                     : file_size(file) <= kHandoffCap ? kTrimDone
                     : file == slot               ? kTrimSlot
                                                  : kTrimFile;
  text = replace_all(text, "{what}", file == slot ? "the pruned handoff" : "its path");
  text = replace_all(text, "{move}", agent == "main" ? "move lasting knowledge to the project's docs or memory"
                                                     : "move lasting knowledge into your final report");
  text = replace_all(replace_all(text, "{file}", file), "{copy}", kept(a));
  text = replace_all(text, "{limit}", thousands(a.num("limit")));
  return replace_all(replace_all(text, "{chars}", std::to_string(file_size(file))), "{path}", slot);
}

std::string summary_instructions(const std::string& d, const std::string& agent) {
  return replace_all(kSummaryInstructions, "{path}", slot_path(d, agent));
}

std::string session_cwd(const std::string& d, const Json& in) {
  std::string cwd = load_state(d).str("cwd");
  return cwd.empty() ? in.str("cwd") : cwd;
}

// ---- Facts saved at compaction, for the agent to read if it needs them.

// The latest messages an agent was given, newest last: the user's own (origin "human"), or for a
// subagent its task and the coordinator's follow-ups. Summaries, tool results and notices are skipped.
std::vector<std::string> latest_messages(const std::string& transcript, bool main) {
  std::vector<std::string> found;
  std::string text;
  if (transcript.empty() || !read_file(transcript, text, kTranscriptTail)) return found;
  size_t end = text.size();
  while (end > 0 && int(found.size()) < kFactMessages) {
    size_t start = text.rfind('\n', end - 1);
    start = start == std::string::npos ? 0 : start + 1;
    std::string line = text.substr(start, end - start);
    end = start > 0 ? start - 1 : 0;
    if (line.find("\"type\":\"user\"") == std::string::npos && line.find("\"type\": \"user\"") == std::string::npos)
      continue;
    Json o;
    if (!Parser(line).parse(o) || o.str("type") != "user" || o.truthy("isCompactSummary")) continue;
    if (main && o.truthy("isSidechain")) continue;
    const Json* origin = o.get("origin");
    std::string kind = origin ? origin->str("kind") : "";
    if (origin ? kind != "human" && kind != "coordinator" : o.truthy("isMeta")) continue;
    const Json* m = o.get("message");
    const Json* content = m ? m->get("content") : nullptr;
    if (!content) continue;
    std::string said;
    bool tool_result = false;
    if (content->type == Json::Str) said = content->s;
    for (const auto& block : content->items) {
      if (block.str("type") == "tool_result") tool_result = true;
      if (block.str("type") == "text") said += (said.empty() ? "" : "\n") + block.str("text");
    }
    said = trim(said);
    if (tool_result || said.empty() || said.compare(0, 14, "<local-command") == 0) continue;
    if (said.size() > kFactMessageCap) said = said.substr(0, kFactMessageCap) + "\n[... cut here, " + std::to_string(said.size()) + " characters in all]";
    if (std::find(found.begin(), found.end(), said) == found.end()) found.push_back(said);
  }
  std::reverse(found.begin(), found.end());
  return found;
}

std::string fenced(const std::string& text, size_t cap) {
  std::string body = trim(text);
  if (body.empty()) return "(nothing)\n";
  return "```text\n" + body + (text.size() >= cap ? "\n[... cut here]" : "") + "\n```\n";
}

// Writes <agent>.facts.md: the latest messages to the agent word for word, and the git state of the
// session's folder (no diffs, which can be huge). Returns its path, or "" when there was nothing.
std::string save_facts(const std::string& d, const std::string& agent, const std::string& owner,
                       const std::string& transcript, const std::string& cwd) {
  std::vector<std::string> said = latest_messages(transcript_of(d, transcript, agent), agent == "main");
  std::string status = cwd.empty() ? "" : git_text(cwd, {"status", "--short", "--branch"}, kFactGitCap);
  if (said.empty() && status.empty()) return "";
  std::string out = "# Facts saved when " + owner + " was compacted (" + stamp("%Y-%m-%d %H:%M") +
                    ")\n\nGathered by the auto-handoff hook from the transcript and git, not written by an agent.\n";
  if (!said.empty()) {
    out += std::string("\n## ") + (agent == "main" ? "The user's latest messages" : "The latest messages this agent was given") +
           ", word for word, newest last\n";
    for (size_t k = 0; k < said.size(); ++k) out += "\n### " + std::to_string(k + 1) + "\n\n" + said[k] + "\n";
  }
  if (!status.empty()) {
    out += "\n## Git, in " + cwd + "\n\nBranch and changed files (`git status --short --branch`):\n" +
           fenced(status, kFactGitCap);
    std::string stat = git_text(cwd, {"diff", "--stat", "HEAD"}, kFactGitCap);
    if (!trim(stat).empty()) out += "\nChanges against the last commit (`git diff --stat HEAD`):\n" + fenced(stat, kFactGitCap);
    std::string log = git_text(cwd, {"log", "--oneline", "-5"}, kFactGitCap);
    if (!trim(log).empty()) out += "\nRecent commits (`git log --oneline -5`):\n" + fenced(log, kFactGitCap);
  }
  std::string p = d + "/" + agent + ".facts.md";
  write_atomic(p, out);
  return p;
}

// ---- Handlers.

void prune();
std::string pickup(const std::string& cwd, const std::string& d_new);

// Archive the handoff unless this version already was (a release that went elsewhere, then another).
void archive_once(const std::string& d, const std::string& agent, Json& a, const std::string& cwd) {
  if (a.num("archived") >= mtime(handoff_file(d, agent))) return;
  archive(d, agent, cwd, "");
  set(a, "archived", now());
}

void release(const std::string& d, const std::string& agent, Json& a, const std::string& why, const std::string& cwd) {
  set(a, "released", now());
  a.at("why") = Json::string(why);
  if (why != "handoff") return;
  archive_once(d, agent, a, cwd);
  std::string file = handoff_file(d, agent);
  if (file != slot_path(d, agent)) a.at("file") = Json::string(file);
}

std::string label(const std::string& d, const std::string& agent) {
  std::string type = agent_info(d, agent).type, file = handoff_file(d, agent), slot = slot_path(d, agent);
  return file + (file == slot ? "" : " (via " + slot + ")") + ", for " +
         (agent == "main" ? "the main session" : "a subagent" + (type.empty() ? "" : " (" + type + ")"));
}

// Runs inside the context that was just compacted, whichever agent that is.
void after_compact(const Json& in, const std::string& d) {
  struct Handoff {
    std::string label, text, note;
  };
  std::vector<Handoff> handoffs;
  std::vector<std::string> waiting;  // ready but not released: listed in case one of them is compacting
  std::vector<std::pair<std::string, std::string>> released;  // agent, owner: their facts are saved below
  std::string cwd;
  bool forced = false;
  {
    Lock lock(d);
    Json st = load_state(d);
    set(st, "last_compact", now());
    cwd = st.str("cwd").empty() ? in.str("cwd") : st.str("cwd");
    for (const auto& f : st.get("agents")->fields) {
      const Json& a = f.second;
      if (!a.has("released")) {
        if (a.has("pending") && slot_ready(d, f.first, a.num("pending"))) waiting.push_back(label(d, f.first));
        continue;
      }
      std::string type = agent_info(d, f.first).type;
      released.emplace_back(f.first, f.first == "main" ? "the main session" : "a subagent" + (type.empty() ? "" : " (" + type + ")"));
      std::string why = a.str("why");
      if (why == "limit") forced = true;
      if (why != "handoff") continue;
      std::string text = read_handoff(handoff_file(d, f.first)), note;
      if (a.has("unfinished"))
        note = "(This handoff was still being pruned when the context reached its limit, so it may be half-edited. "
               "The full version: " + kept(a) + ". Check it against that, then finish the pruning.)";
      if (!text.empty()) handoffs.push_back({label(d, f.first), text, note});
    }
    save_state(d, st);
  }
  log(d, "compacted: " + std::to_string(handoffs.size()) + " handoff(s) to load" +
             (forced ? ", one at the safety limit" : ""));
  // Kept out of the context on purpose: a git status can be long. One line says where they are.
  std::vector<std::string> facts;
  for (const auto& r : released) {
    std::string p = save_facts(d, r.first, r.second, in.str("transcript_path"), cwd);
    if (!p.empty()) facts.push_back(p + (released.size() > 1 ? " (for " + r.second + ")" : ""));
  }
  std::string text = kUserNote, others;
  if (!waiting.empty()) {
    others = "\n\n[auto-handoff] These handoffs are ready but weren't loaded here. If one of them is yours, read "
             "it before going on:";
    for (const auto& w : waiting) others += "\n- " + w;
  }
  if (!facts.empty()) {
    others += "\n\n[auto-handoff] Saved at compaction, in case you need them: the latest messages to you word for "
              "word, and the git status. Read " + std::string(facts.size() > 1 ? "yours: " : "it at ");
    for (size_t k = 0; k < facts.size(); ++k) others += (k ? "; " : "") + facts[k];
  }
  if (handoffs.empty()) {
    if (forced)
      text += "\n\n[auto-handoff] Your context was compacted before a handoff was written. Continue from the "
              "summary, and tell the user if something important seems to be missing.";
    return context("SessionStart", text + others);
  }
  std::string head =
      handoffs.size() > 1 || forced
          ? "[auto-handoff] Your context was just compacted, at about the same time as other agents'. Continue "
            "from YOUR handoff: the main session's if you are the main conversation, otherwise the one for your "
            "task. The others belong to parallel agents, so ignore them; if none is yours, continue from the "
            "summary."
          : "[auto-handoff] Your context was just compacted. Below is the handoff for " +
                handoffs[0].label.substr(handoffs[0].label.find(", for ") + 6) +
                ". If that's you, continue from it; if it isn't, ignore it and continue from the summary.";
  std::string body;
  for (const auto& h : handoffs) body += "\n\n--- " + h.label + " ---\n" + (h.note.empty() ? "" : h.note + "\n\n") + h.text;
  if (text.size() + 2 + head.size() + body.size() + others.size() <= kInjectCap)
    return context("SessionStart", text + "\n\n" + head + body + others);
  std::string listing;
  for (const auto& h : handoffs)
    listing += "\n- " + h.label + ": " + h.text.substr(0, std::min(h.text.find('\n'), size_t(150))) +
               (h.note.empty() ? "" : "\n  " + h.note);
  context("SessionStart", text + "\n\n" + head + " The handoff is too long to include here: before doing " +
                              "anything else, use the Read tool on yours." + listing + others);
}

void on_session_start(const Json& in, const std::string& d) {
  std::string src = in.str("source");
  if (src == "compact") return after_compact(in, d);
  {
    Lock lock(d);
    Json st = load_state(d);
    st.at("cwd") = Json::string(in.str("cwd"));
    save_state(d, st);
  }
  if (src == "startup") prune();
  std::string text = replace_all(kAskedMain, "{path}", slot_path(d, "main"));
  if (src == "clear") {
    std::string old = pickup(in.str("cwd"), d);
    if (!old.empty()) text = old + "\n\n" + text;
  }
  context("SessionStart", text);
}

// /clear ends the old session first: note which one, so the new session loads only its handoff.
void on_session_end(const Json& in, const std::string& d) {
  if (in.str("reason") != "clear") return;
  mkdirs(cfg.root + "/.cleared");
  write_atomic(cfg.root + "/.cleared/" + slug(in.str("cwd")), d.substr(d.rfind('/') + 1));
}

void on_subagent_start(const Json& in, const std::string& d, const std::string& agent) {
  std::string type = in.str("agent_type");
  if (type.empty()) return;  // Claude Code's own helpers, such as the compaction summarizer
  note_event(d, agent);      // a resumed subagent can compact before its first tool call
  AgentInfo info = definition(session_cwd(d, in), type);
  mkdirs(d + "/types");
  write_atomic(d + "/types/" + agent, type + "\t" + (info.readonly ? "ro" : "rw") + "\t" + std::to_string(long(info.window)));
  log(d, "subagent " + agent + " (" + type + (info.readonly ? ", read-only" : "") + ") started");
  if (!info.readonly) context("SubagentStart", replace_all(kAskedSubagent, "{path}", slot_path(d, agent)));
}

void on_subagent_stop(const Json& in, const std::string& d, const std::string& agent) {
  if (in.str("agent_type").empty()) return;
  for (const char* sub : {"/events/", "/types/", "/paths/"}) unlink((d + sub + agent).c_str());
  if (!load_state(d).get("agents")->has(agent)) return;
  Lock lock(d);
  Json st = load_state(d);
  st.get("agents")->erase(agent);
  save_state(d, st);
}

// The main session's permission mode, kept from its own events since PreCompact doesn't carry it.
void note_mode(const std::string& d, const std::string& mode) {
  std::string old;
  if (mode.empty() || (read_file(d + "/mode", old) && old == mode)) return;
  mkdirs(d);
  write_atomic(d + "/mode", mode);
}

bool planning(const std::string& d) {
  std::string mode;
  return read_file(d + "/mode", mode) && mode == "plan";
}

// After its compaction, or once its transcript shows it was compacted some other way, an agent is no
// longer waiting. Returns true when it settled; `missed` is then the handoff it had ready but didn't
// get back, if any.
bool settle_if_compacted(const std::string& d, const Json& in, const std::string& agent, Json& st, Json& a,
                         std::string& missed) {
  std::string transcript = transcript_of(d, in.str("transcript_path"), agent);
  if (a.has("released")) {  // its first activity since: was it the one compacted?
    if (transcript.empty() || mtime(transcript) < 0 || compacted_since(transcript, a.num("released"))) {
      // Loaded and archived: remove the slot, so the next handoff is a new file. The Write tool won't
      // overwrite a file the agent hasn't read in this context.
      if (a.str("why") == "handoff") unlink(slot_path(d, agent).c_str());
      settle(a);
      return true;
    }
    log(d, agent + " was released but not compacted (another agent was); waiting again");
    for (const char* key : {"released", "why", "unfinished"}) a.erase(key);
    return false;
  }
  if (!a.has("pending") || st.num("last_compact") < a.num("pending") || a.num("checked") >= st.num("last_compact"))
    return false;
  set(a, "checked", st.num("last_compact"));
  if (!compacted_since(transcript, a.num("pending"))) return false;
  log(d, agent + " was compacted without its handoff being released; no longer waiting");
  if (slot_ready(d, agent, a.num("pending"))) {
    missed = handoff_file(d, agent);
    archive_once(d, agent, a, session_cwd(d, in));
  }
  settle(a);
  if (!missed.empty()) a.at("missed") = Json::string(missed);  // said at its next event that can reply
  return true;
}

// Whether an agent has anything going on: waiting, just released, or owed a message.
bool busy(const Json* a) { return a && (a->has("pending") || a->has("released") || a->has("missed")); }

// Tool results, failures, denials and new prompts: note who is active, and remind an agent that is
// holding up its own compaction (only where the event can carry context back to it).
void on_activity(const Json& in, const std::string& d, const std::string& agent, const std::string& ev, bool can_reply) {
  note_event(d, agent);
  if (agent == "main") note_mode(d, in.str("permission_mode"));
  {
    Json st = load_state(d);  // most calls end here, without the lock
    if (!busy(st.get("agents")->get(agent))) return;
  }
  std::string text, kind;
  {
    Lock lock(d);
    Json st = load_state(d);
    Json* cur = st.get("agents")->get(agent);
    if (!cur) return;
    std::string missed;
    settle_if_compacted(d, in, agent, st, *cur, missed);
    if (cur->has("missed") && can_reply) {
      std::string file = cur->str("missed");
      cur->erase("missed");
      save_state(d, st);
      return context(ev, replace_all(kMissed, "{file}", file));
    }
    if (!cur->has("pending") || cur->has("released") || !can_reply || in.str("permission_mode") == "plan")
      return save_state(d, st);
    int n = int(cur->num("nudges")) + 1;
    set(*cur, "nudges", n);
    if (slot_ready(d, agent, cur->num("pending"))) {
      // Ask for the trim, once more when it looks done but the slot hasn't been written again, and
      // remind every few calls in between.
      if (needs_trim(d, agent, *cur)) {
        std::string told = file_size(handoff_file(d, agent)) > kHandoffCap ? "trim_told" : "trim_done_told";
        if (!cur->has(told)) {
          set(*cur, told, now());
          set(*cur, "nudges", 0);
          text = trim_request(d, agent, *cur, false), kind = "trim request";
        } else if (n % kNudgeEvery == 0) {
          text = trim_request(d, agent, *cur, true), kind = "trim reminder";
        }
      }
    } else if (dangling(d, agent)) {
      if (cur->num("dangling_told") < mtime(slot_path(d, agent))) {
        set(*cur, "dangling_told", now());
        text = replace_all(replace_all(kDangling, "{text}", pointer(d, agent)), "{path}", slot_path(d, agent));
        kind = "path problem";
      }
    } else if (n == 1 || ev == "UserPromptSubmit" || n % kNudgeEvery == 0) {
      bool full = n == 1 || ev == "UserPromptSubmit";
      text = nudge(d, agent, *cur, full, session_cwd(d, in)), kind = full ? "nudge" : "reminder";
    }
    save_state(d, st);
  }
  if (text.empty()) return;
  log(d, ev + " " + kind + " for " + agent);
  context(ev, text);
}

// The end of a turn is a clean point: if compaction is waiting, write (or trim) the handoff now.
void on_stop(const Json& in, const std::string& d, const std::string& agent) {
  note_event(d, agent);
  if (agent == "main") note_mode(d, in.str("permission_mode"));
  {
    Json st = load_state(d);
    if (!busy(st.get("agents")->get(agent))) return;
  }
  Json a;
  std::string missed;
  bool quiet = in.truthy("stop_hook_active") || in.str("permission_mode") == "plan";
  {
    Lock lock(d);
    Json st = load_state(d);
    Json* cur = st.get("agents")->get(agent);
    if (!cur) return;
    settle_if_compacted(d, in, agent, st, *cur, missed);
    missed = quiet ? "" : cur->str("missed");
    if (!missed.empty()) cur->erase("missed");
    a = *cur;
    save_state(d, st);
  }
  if (!missed.empty()) return block(replace_all(kMissed, "{file}", missed));
  if (quiet) return;
  if (!a.has("pending") || a.has("released")) return;
  if (slot_ready(d, agent, a.num("pending"))) {
    if (!needs_trim(d, agent, a)) return;
    log(d, "Stop held for " + agent + "'s handoff trim");
    return block(trim_request(d, agent, a, false));
  }
  if (dangling(d, agent)) {
    log(d, "Stop held for " + agent + "'s handoff path");
    return block(replace_all(replace_all(kDangling, "{text}", pointer(d, agent)), "{path}", slot_path(d, agent)));
  }
  log(d, "Stop held for " + agent + "'s handoff");
  block(nudge(d, agent, a, true, session_cwd(d, in)) +
        "\n\nThe end of this turn is a clean point, so write the handoff now, then finish your reply.");
}

void on_pre_compact(const Json& in, const std::string& d) {
  double t = now();
  if (in.str("trigger") == "manual") {  // /compact: always allowed; load a handoff if one was written
    Lock lock(d);
    Json st = load_state(d);
    Json& a = st.get("agents")->at("main");
    std::string cwd = st.str("cwd").empty() ? in.str("cwd") : st.str("cwd");
    double since = a.has("pending") ? a.num("pending") : a.num("compacted");
    release(d, "main", a, slot_ready(d, "main", since) ? "handoff" : "manual", cwd);
    log(d, "/compact: " + a.str("why"));
    if (a.str("why") == "handoff") emit(summary_instructions(d, "main"));
    return save_state(d, st);
  }

  std::vector<Event> evs = events(d);
  bool idle = evs.empty() || t - evs[0].t > kIdleSeconds;
  Candidate c = compacting(d, in.str("transcript_path"), evs, t, load_state(d));
  if (c.agent.empty()) return log(d, "compaction allowed: no agent is near its compaction point");
  std::string who = c.agent;
  double limit = c.limit;
  bool at_limit = c.at_limit;

  Lock lock(d);
  Json st = load_state(d);
  // `who` can't be held, but `instead` may be the one compacting: hold this attempt once. If it's
  // `instead`, its retry follows its own tool call, which makes it the clear pick; if `who` is the
  // likeliest again, it goes through then. Neither agent's state changes, so no hold is counted.
  const Json* deferred = st.get("deferred");
  bool again = deferred && deferred->str("agent") == who && t - deferred->num("at") < kDeferSeconds;
  if (!c.instead.empty() && !again) {
    Json mark = Json::object();
    mark.at("agent") = Json::string(who);
    set(mark, "at", t);
    st.at("deferred") = mark;
    save_state(d, st);
    log(d, "compaction held once: " + who + " can't be held, and " + c.instead + " is at its own compaction point");
    return block("auto-handoff: checking which agent is compacting");
  }
  if (deferred && deferred->str("agent") == who) st.erase("deferred");
  Json& a = st.get("agents")->at(who);
  std::string cwd = st.str("cwd").empty() ? in.str("cwd") : st.str("cwd"), slot = slot_path(d, who);
  if (a.has("released")) {  // another attempt for the same compaction
    log(d, "compaction of " + who + " allowed again: " + a.str("why"));
    if (a.str("why") == "handoff" && !a.has("unfinished")) emit(summary_instructions(d, who));
    return;
  }
  bool ready = a.has("pending") && slot_ready(d, who, a.num("pending"));
  if (ready && !a.has("trim") && t - mtime(slot) > kReadySeconds) {  // written long before this compaction
    log(d, who + "'s handoff is too old for this compaction; asking again");
    set(a, "pending", t);
    set(a, "nudges", 0);
    ready = false;
  }
  if (ready) {
    std::string file = handoff_file(d, who);
    long size = file_size(file);
    bool resaved = a.has("trim") && mtime(slot) >= a.num("trim");
    bool trimming = a.has("trim") ? !resaved : size > kHandoffCap;  // until the slot is written again
    if (trimming && !at_limit) {
      if (!a.has("trim")) {  // keep the long version before it's pruned
        set(a, "trim", t);
        set(a, "limit", limit);
        a.at("trim_copy") = Json::string(archive(d, who, cwd, "before-trim"));
      }
      log(d, "compaction of " + who + " held: handoff is " + std::to_string(size) + " characters");
      save_state(d, st);
      return block("auto-handoff: waiting for the handoff to be trimmed");
    }
    bool unfinished = trimming && a.has("trim");  // over the cap but never asked to trim: complete, just long
    release(d, who, a, "handoff", cwd);
    if (unfinished) set(a, "unfinished", t);
    log(d, "compaction of " + who + " allowed: handoff" + (unfinished ? ", trim unfinished at the safety limit" : ""));
    save_state(d, st);
    if (!unfinished) emit(summary_instructions(d, who));  // otherwise the full summary, as a fallback
    return;
  }
  // Agents that couldn't write a handoff, or have no room to: compact as usual.
  std::string skip = c.info.readonly                  ? "read-only agent"
                     : c.tight                        ? "window too small to hold"
                     : who == "main" && planning(d)   ? "plan mode"
                                                      : "";
  if (!skip.empty()) {
    release(d, who, a, "skipped", cwd);
    log(d, "compaction of " + who + " allowed: " + skip);
    return save_state(d, st);
  }
  set(a, "holds", a.num("holds") + 1);
  if (at_limit || (who != "main" && a.num("holds") > kSubagentMaxHolds)) {
    release(d, who, a, "limit", cwd);
    log(d, "compaction of " + who + " allowed: " + (at_limit ? "safety limit (" + std::to_string(long(c.u.tokens)) +
                                                                  " >= " + std::to_string(long(limit)) + " tokens)"
                                                             : "hold budget used up"));
    return save_state(d, st);
  }
  if (!idle && !a.has("pending")) {
    set(a, "pending", t);
    set(a, "tokens", c.u.tokens);
    set(a, "limit", limit);
    set(a, "nudges", 0);
  }
  log(d, "compaction of " + who + " held (" + std::to_string(long(c.u.tokens)) + " tokens" + (idle ? ", idle" : "") + ")");
  save_state(d, st);
  block("auto-handoff: waiting for a handoff before compacting");
}

// After /clear: the handoff of the session that was just cleared, if it wrote one it hasn't loaded.
std::string pickup(const std::string& cwd, const std::string& d_new) {
  std::string link = cfg.root + "/.cleared/" + slug(cwd), old;
  if (!read_file(link, old) || now() - mtime(link) > kClearLinkSeconds) return "";
  unlink(link.c_str());
  old = trim(old);
  if (!safe_id(old)) return "";
  std::string d = cfg.root + "/" + old;
  if (d == d_new) return "";
  Json st = load_state(d);
  const Json* a = st.get("agents")->get("main");
  double since = a ? (a->has("pending") ? a->num("pending") : a->num("compacted")) : 0;
  if (!slot_ready(d, "main", since)) return "";
  std::string p = handoff_file(d, "main"), text = read_handoff(p);
  {
    Lock lock(d);
    Json cur = load_state(d);
    Json& m = cur.get("agents")->at("main");
    archive_once(d, "main", m, cwd);
    settle(m);
    save_state(d, cur);
  }
  std::string head = std::string(kUserNote) + "\n\n[auto-handoff] ";
  if (text.size() > kInjectCap - 400)
    return head + "Before doing anything else, read the handoff from the session you just cleared: " + p;
  return head + "Handoff from the session you just cleared (" + p + "):\n\n" + text;
}

void prune() {
  double cutoff = now() - kKeepDays * 86400;
  for (const auto& name : list_dir(cfg.root)) {
    if (name == "archive") continue;  // kept for good
    std::string d = cfg.root + "/" + name;
    if (!is_dir(d)) continue;
    double t = mtime(d + "/state.json");
    if ((t >= 0 ? t : mtime(d)) < cutoff) remove_tree(d);
  }
}

void run(const Json& in) {
  std::string ev = in.str("hook_event_name"), sid = in.str("session_id"), agent = in.str("agent_id");
  if (agent.empty()) agent = "main";
  if (!safe_id(sid) || !safe_id(agent)) return;
  std::string d = cfg.root + "/" + sid;
  if (ev == "SessionStart") on_session_start(in, d);
  else if (ev == "SessionEnd") on_session_end(in, d);
  else if (ev == "SubagentStart") on_subagent_start(in, d, agent);
  else if (ev == "SubagentStop") on_subagent_stop(in, d, agent);
  else if (ev == "PostToolUse" || ev == "UserPromptSubmit") on_activity(in, d, agent, ev, true);
  else if (ev == "PostToolUseFailure" || ev == "PermissionDenied") on_activity(in, d, agent, ev, false);
  else if (ev == "Stop") on_stop(in, d, agent);
  else if (ev == "PreCompact") on_pre_compact(in, d);
}

}  // namespace

int main() {
  const char* home = getenv("HOME");
  const char* root = getenv("AUTO_HANDOFF_DIR");
  const char* logp = getenv("AUTO_HANDOFF_LOG");
  const char* ceiling = getenv("AUTO_HANDOFF_CEILING");
  const char* config = getenv("CLAUDE_CONFIG_DIR");
  std::string h = home ? home : "";
  cfg.root = root && *root ? root : h + "/.local/state/claude-handoffs";
  cfg.config_dir = config && *config ? config : h + "/.claude";
  cfg.log = logp ? logp : "";
  const char* settings = getenv("AUTO_HANDOFF_SETTINGS");  // tests: a --settings file Claude Code was given
  cfg.settings = settings && *settings ? settings : cfg.config_dir + "/settings.json";
  if (ceiling && *ceiling) cfg.ceiling = atof(ceiling);

  std::string text;
  char buf[65536];
  ssize_t n;
  while ((n = read(0, buf, sizeof buf)) > 0) text.append(buf, size_t(n));
  // Only these fields are used; the rest (tool input and output can be megabytes) is skipped unparsed.
  Json in;
  if (!Parser(text).parse_top(in, {"hook_event_name", "session_id", "agent_id", "agent_type", "source", "trigger",
                                   "transcript_path", "cwd", "stop_hook_active", "reason", "permission_mode"}))
    return 0;
  try {
    run(in);
  } catch (const std::exception& e) {  // a broken hook must never get in the way of the session
    mkdirs(cfg.root);
    if (FILE* f = fopen((cfg.root + "/errors.log").c_str(), "a")) {
      fprintf(f, "%ld %s: %s\n", long(time(nullptr)), in.str("hook_event_name").c_str(), e.what());
      fclose(f);
    }
  }
  return 0;
}
