// SPDX-License-Identifier: GPL-2.0-or-later
// rsh: bindings. Rule for every function the interpreter calls: no C++ object with a destructor
// may be alive when the interpreter raises (it unwinds with longjmp). Work happens in an inner
// scope; the raise comes after it.
#include "rt.hpp"

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/compile.h>
#include <mruby/data.h>
#include <mruby/dump.h>
#include <mruby/error.h>
#include <mruby/hash.h>
#include <mruby/irep.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/io.h>
#include <pcre2.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "exec.hpp"
#include "glob.hpp"

extern char** environ;
extern "C" const uint8_t rsh_prelude[];
extern "C" const uint8_t rsh_prelude_regexp[];

namespace rsh {

std::string read_file(const std::string& path, bool& ok) {
    std::string s;
    FILE* f = path == "-" ? stdin : std::fopen(path.c_str(), "rb");
    ok = f != nullptr;
    if (!f) return s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    if (f != stdin) std::fclose(f);
    return s;
}

namespace {

struct RtHost : Host {
    mrb_state* mrb = nullptr;
    mrb_value vals = mrb_nil_value();  // the Ruby values of the running line (procs inside)
    mrb_value exc = mrb_nil_value();
    std::vector<std::string> argv;
    std::vector<std::string> files;  // being loaded, innermost last (require_relative)
    std::vector<std::string> hist;

    bool stage(int val, const std::string& line, std::string& out, bool& keep) override;
    int source(const std::vector<std::string>& a) override;
    bool raised() override { return !mrb_nil_p(exc); }
    const std::vector<std::string>& args() override { return argv; }
    void history(int fd) override {
        for (size_t k = 0; k < hist.size(); ++k) {
            std::string l = std::to_string(k + 1) + "  " + hist[k] + "\n";
            if (write(fd, l.data(), l.size()) < 0) return;
        }
    }
};
RtHost host;

struct StageCall {
    mrb_value proc, arg;
};
mrb_value stage_body(mrb_state* mrb, void* ud) {
    auto* c = static_cast<StageCall*>(ud);
    return mrb_yield(mrb, c->proc, c->arg);
}

bool RtHost::stage(int val, const std::string& line, std::string& out, bool& keep) {
    int ai = mrb_gc_arena_save(mrb);
    StageCall c{mrb_ary_ref(mrb, vals, val), mrb_str_new(mrb, line.data(), line.size())};
    mrb_bool err = FALSE;
    mrb_value r = mrb_protect_error(mrb, stage_body, &c, &err);
    if (err) {
        exc = r;
        mrb_gc_register(mrb, exc);
        mrb_gc_arena_restore(mrb, ai);
        return false;
    }
    if (mrb_string_p(r)) {
        keep = true;
        out.assign(RSTRING_PTR(r), size_t(RSTRING_LEN(r)));
    } else if (mrb_test(r)) {
        keep = true;
        out = line;
    } else keep = false;
    mrb_gc_arena_restore(mrb, ai);
    return true;
}

RClass* rsh_mod(mrb_state* mrb) { return mrb_module_get(mrb, "Rsh"); }

mrb_value str(mrb_state* mrb, const std::string& s) { return mrb_str_new(mrb, s.data(), s.size()); }
std::string cxx(mrb_value v) { return std::string(RSTRING_PTR(v), size_t(RSTRING_LEN(v))); }

void raise_with(mrb_state* mrb, RClass* cls, mrb_value msg) { mrb_exc_raise(mrb, mrb_exc_new_str(mrb, cls, msg)); }

// [code, statuses, signal, raise?, argv, tail, interrupted?, output]
mrb_value outcome(mrb_state* mrb, const Outcome& oc) {
    mrb_value r = mrb_ary_new_capa(mrb, 8);
    mrb_ary_push(mrb, r, mrb_fixnum_value(oc.code));
    mrb_value st = mrb_ary_new(mrb);
    for (int s : oc.statuses) mrb_ary_push(mrb, st, mrb_fixnum_value(s));
    mrb_ary_push(mrb, r, st);
    mrb_ary_push(mrb, r, oc.sig ? mrb_fixnum_value(oc.sig) : mrb_nil_value());
    mrb_ary_push(mrb, r, mrb_bool_value(oc.raise));
    mrb_value av = mrb_ary_new(mrb);
    for (auto& a : oc.failed_argv) mrb_ary_push(mrb, av, str(mrb, a));
    mrb_ary_push(mrb, r, av);
    mrb_ary_push(mrb, r, str(mrb, oc.tail));
    mrb_ary_push(mrb, r, mrb_bool_value(oc.interrupted));
    mrb_ary_push(mrb, r, str(mrb, oc.out));
    mrb_ary_push(mrb, r, oc.failed_msg.empty() ? mrb_nil_value() : str(mrb, oc.failed_msg));
    return r;
}

// Rsh.__exec(line, values, flags): flags 1 condition, 2 capture, 4 relaxed.
extern "C" struct mrb_data_type mrb_io_type;

// `answer = gets`, then a command that reads standard input too (`rsh job.rsh < list.txt`): the
// interpreter has read ahead up to 4 KB into $stdin's buffer, and the command started after the
// buffered lines (0.9.0). When standard input is a file the read-ahead is given back (lseek), so
// the command and a later gets see the same rest. A pipe cannot be given back (docs/rsh.md).
void stdin_give_back(mrb_state* mrb) {
    mrb_value in = mrb_gv_get(mrb, mrb_intern_lit(mrb, "$stdin"));
    if (!mrb_data_p(in) || DATA_TYPE(in) != &mrb_io_type) return;
    mrb_io* f = static_cast<mrb_io*>(DATA_PTR(in));
    if (!f || f->fd != 0 || !f->buf || f->buf->len <= 0) return;
    if (lseek(0, -off_t(f->buf->len), SEEK_CUR) < 0) return;
    f->buf->start = 0;
    f->buf->len = 0;
}

mrb_value m_exec(mrb_state* mrb, mrb_value) {
    mrb_int idx, flags;
    mrb_value vals;
    mrb_get_args(mrb, "iAi", &idx, &vals, &flags);
    // A `source` or `Rsh.eval` run by this line reuses this call's argument slots; the values (and
    // the Ruby stages in them) must stay alive for the rest of the line.
    mrb_gc_protect(mrb, vals);
    mrb_value result = mrb_nil_value(), errmsg = mrb_nil_value(), exc = mrb_nil_value();
    if (idx < 0 || size_t(idx) >= table().size()) mrb_raise(mrb, E_ARGUMENT_ERROR, "no such command line");
    mrb_value saved = host.vals;
    stdin_give_back(mrb);
    {
        std::vector<Value> vs;
        for (mrb_int k = 0; k < RARRAY_LEN(vals); ++k) {
            mrb_value x = mrb_ary_ref(mrb, vals, k);
            Value v;
            if (mrb_string_p(x)) v.items.push_back(cxx(x));
            else if (mrb_array_p(x)) {
                v.array = true;
                for (mrb_int j = 0; j < RARRAY_LEN(x); ++j) {
                    mrb_value e = mrb_ary_ref(mrb, x, j);
                    if (mrb_string_p(e)) v.items.push_back(cxx(e));
                }
            } else v.proc = true;
            vs.push_back(std::move(v));
        }
        Options o;
        o.cond = flags & 1;
        o.capture = flags & 2;
        o.relaxed = flags & 4;
        host.vals = vals;
        host.exc = mrb_nil_value();
        Outcome oc = run(int(idx), vs, host, o);
        host.vals = saved;
        if (oc.ruby_error) exc = host.exc;
        else if (!oc.error.empty()) errmsg = str(mrb, oc.error);
        else result = outcome(mrb, oc);
    }
    if (!mrb_nil_p(exc)) {
        mrb_gc_unregister(mrb, exc);
        mrb_exc_raise(mrb, exc);
    }
    if (!mrb_nil_p(errmsg)) raise_with(mrb, mrb_class_get_under(mrb, rsh_mod(mrb), "Error"), errmsg);
    return result;
}

// Rsh.__argv(argv, flags): one program, these arguments.
mrb_value m_argv(mrb_state* mrb, mrb_value) {
    mrb_value av;
    mrb_int flags;
    mrb_get_args(mrb, "Ai", &av, &flags);
    mrb_value result = mrb_nil_value(), errmsg = mrb_nil_value();
    if (RARRAY_LEN(av) == 0) mrb_raise(mrb, E_ARGUMENT_ERROR, "no program given");
    stdin_give_back(mrb);
    {
        std::vector<std::string> a;
        for (mrb_int k = 0; k < RARRAY_LEN(av); ++k) {
            mrb_value x = mrb_ary_ref(mrb, av, k);
            if (mrb_string_p(x)) a.push_back(cxx(x));
        }
        Options o;
        o.cond = flags & 1;
        Outcome oc = run_argv(a, host, o);
        if (!oc.error.empty()) errmsg = str(mrb, oc.error);
        else result = outcome(mrb, oc);
    }
    if (!mrb_nil_p(errmsg)) raise_with(mrb, mrb_class_get_under(mrb, rsh_mod(mrb), "Error"), errmsg);
    return result;
}

mrb_value m_set_status(mrb_state* mrb, mrb_value) {
    mrb_value st;
    mrb_get_args(mrb, "o", &st);
    mrb_gv_set(mrb, mrb_intern_lit(mrb, "$?"), st);
    return st;
}

mrb_value m_set_match(mrb_state* mrb, mrb_value) {
    mrb_value md, caps;
    mrb_get_args(mrb, "oA", &md, &caps);
    mrb_gv_set(mrb, mrb_intern_lit(mrb, "$~"), md);
    char name[3] = {'$', '1', 0};
    for (int k = 0; k < 9; ++k) {
        name[1] = char('1' + k);
        mrb_gv_set(mrb, mrb_intern_cstr(mrb, name), k < RARRAY_LEN(caps) ? mrb_ary_ref(mrb, caps, k) : mrb_nil_value());
    }
    return md;
}

mrb_value m_chdir(mrb_state* mrb, mrb_value) {
    const char* d;
    mrb_get_args(mrb, "z", &d);
    if (chdir(d) != 0) mrb_raisef(mrb, E_RUNTIME_ERROR, "cd %s: %s", d, std::strerror(errno));
    {
        std::string now = current_dir();
        if (!now.empty()) setenv("PWD", now.c_str(), 1);
    }
    return mrb_nil_value();
}

mrb_value m_nullglob(mrb_state* mrb, mrb_value) {
    mrb_bool on;
    mrb_get_args(mrb, "b", &on);
    shell().nullglob = on;
    return mrb_bool_value(on);
}

mrb_value m_getenv(mrb_state* mrb, mrb_value) {
    const char* k;
    mrb_get_args(mrb, "z", &k);
    const char* v = getenv(k);
    return v ? mrb_str_new_cstr(mrb, v) : mrb_nil_value();
}

mrb_value m_setenv(mrb_state* mrb, mrb_value) {
    const char* k;
    mrb_value v;
    mrb_get_args(mrb, "zo", &k, &v);
    if (!*k || std::strchr(k, '=')) mrb_raise(mrb, E_ARGUMENT_ERROR, "not an environment variable name");
    if (mrb_nil_p(v)) unsetenv(k);
    else {
        const char* s = mrb_string_value_cstr(mrb, &v);
        setenv(k, s, 1);
    }
    return v;
}

mrb_value m_env_hash(mrb_state* mrb, mrb_value) {
    mrb_value h = mrb_hash_new(mrb);
    for (char** p = environ; *p; ++p) {
        const char* eq = std::strchr(*p, '=');
        if (!eq) continue;
        mrb_hash_set(mrb, h, mrb_str_new(mrb, *p, eq - *p), mrb_str_new_cstr(mrb, eq + 1));
    }
    return h;
}

mrb_value m_pid(mrb_state*, mrb_value) { return mrb_fixnum_value(getpid()); }
mrb_value m_nproc(mrb_state*, mrb_value) { return mrb_fixnum_value(sysconf(_SC_NPROCESSORS_ONLN)); }

mrb_value m_kill(mrb_state* mrb, mrb_value) {
    const char* s;
    mrb_int pid;
    mrb_get_args(mrb, "zi", &s, &pid);
    int sig = std::atoi(s);
    struct {
        const char* n;
        int v;
    } names[] = {{"TERM", SIGTERM}, {"KILL", SIGKILL}, {"INT", SIGINT}, {"HUP", SIGHUP}, {"USR1", SIGUSR1},
                 {"USR2", SIGUSR2}, {"STOP", SIGSTOP}, {"CONT", SIGCONT}, {"QUIT", SIGQUIT}, {"0", 0}};
    const char* n = std::strncmp(s, "SIG", 3) == 0 ? s + 3 : s;
    for (auto& x : names)
        if (!std::strcmp(n, x.n)) sig = x.v;
    if (kill(pid_t(pid), sig) != 0) mrb_raisef(mrb, E_RUNTIME_ERROR, "kill %d: %s", (int)pid, std::strerror(errno));
    return mrb_true_value();
}

// File.mtime(path): the time of the last change to the file's contents (Errno::ENOENT and the
// other Errno errors as in Ruby).
mrb_value m_file_mtime(mrb_state* mrb, mrb_value) {
    mrb_value path;
    mrb_get_args(mrb, "S", &path);
    struct stat st;
    if (stat(mrb_string_cstr(mrb, path), &st) != 0) mrb_sys_fail(mrb, mrb_string_cstr(mrb, path));
    mrb_value t = mrb_obj_value(mrb_class_get(mrb, "Time"));
    return mrb_funcall(mrb, t, "at", 2, mrb_int_value(mrb, (mrb_int)st.st_mtim.tv_sec),
                       mrb_int_value(mrb, (mrb_int)(st.st_mtim.tv_nsec / 1000)));
}

// 0.9.0: what a script asks about its files and its user, without a program (`test -x`, `id -u`).
// File.readable?/writable?/executable?/owned? use the effective ids, as Ruby does (eaccess).
template <int MODE>
mrb_value m_file_access(mrb_state* mrb, mrb_value) {
    mrb_value path;
    mrb_get_args(mrb, "S", &path);
    return mrb_bool_value(faccessat(AT_FDCWD, mrb_string_cstr(mrb, path), MODE, AT_EACCESS) == 0);
}
mrb_value m_file_owned(mrb_state* mrb, mrb_value) {
    mrb_value path;
    mrb_get_args(mrb, "S", &path);
    struct stat st;
    return mrb_bool_value(stat(mrb_string_cstr(mrb, path), &st) == 0 && st.st_uid == geteuid());
}
// Rsh.__stat(path, follow): [mode, size, uid, gid, nlink, ino, dev, atime, mtime, ctime (each
// seconds, microseconds), rdev, blksize, blocks] for File::Stat (src/prelude_more.rb); Errno as Ruby.
mrb_value m_stat(mrb_state* mrb, mrb_value) {
    mrb_value path;
    mrb_bool follow;
    mrb_get_args(mrb, "Sb", &path, &follow);
    struct stat st;
    const char* p = mrb_string_cstr(mrb, path);
    if ((follow ? stat(p, &st) : lstat(p, &st)) != 0) mrb_sys_fail(mrb, p);
    mrb_value a = mrb_ary_new_capa(mrb, 16);
    auto put = [&](long long v) { mrb_ary_push(mrb, a, mrb_int_value(mrb, (mrb_int)v)); };
    put(st.st_mode), put(st.st_size), put(st.st_uid), put(st.st_gid), put(st.st_nlink), put(st.st_ino), put(st.st_dev);
    put(st.st_atim.tv_sec), put(st.st_atim.tv_nsec / 1000), put(st.st_mtim.tv_sec), put(st.st_mtim.tv_nsec / 1000);
    put(st.st_ctim.tv_sec), put(st.st_ctim.tv_nsec / 1000), put(st.st_rdev), put(st.st_blksize), put(st.st_blocks);
    return a;
}
// Rsh.__touch(path) (0.9.0, Sh.touch / FileUtils.touch): makes the file, or sets its times to now.
mrb_value m_touch(mrb_state* mrb, mrb_value) {
    mrb_value path;
    mrb_get_args(mrb, "S", &path);
    const char* p = mrb_string_cstr(mrb, path);
    int fd = open(p, O_WRONLY | O_CREAT | O_NOCTTY | O_CLOEXEC, 0666);
    if (fd < 0 && errno != EISDIR) mrb_sys_fail(mrb, p);
    if (fd >= 0) close(fd);
    if (utimensat(AT_FDCWD, p, nullptr, 0) != 0) mrb_sys_fail(mrb, p);
    return mrb_nil_value();
}

// Rsh.__copy_file(src, tmp) (0.9.1, audit 2 M6, M7, L16; Sh.cp): copies a plain file in 64 KB
// pieces into a new file `tmp` (O_EXCL, never through a link, 0600 until done, then src's mode).
// A source that is not a plain file (a FIFO, a device, a folder) is ArgumentError; an error is the
// system's (Errno), and tmp is removed.
mrb_value m_copy_file(mrb_state* mrb, mrb_value) {
    // no C++ object lives across a raise here (it is a longjmp): plain C strings only
    mrb_value srcv, tmpv;
    mrb_get_args(mrb, "SS", &srcv, &tmpv);
    const char* src = mrb_string_cstr(mrb, srcv);
    const char* tmp = mrb_string_cstr(mrb, tmpv);
    int in = open(src, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (in < 0) mrb_sys_fail(mrb, src);
    struct stat st;
    if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(in);
        mrb_raisef(mrb, E_ARGUMENT_ERROR, "cp: %s is not a plain file (a folder, FIFO or device); cp copies files", src);
    }
    int out = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out < 0) {
        int e = errno;
        close(in);
        errno = e;
        mrb_sys_fail(mrb, tmp);
    }
    static char buf[65536];
    int err = 0;
    const char* where = src;
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            err = errno;
            break;
        }
        if (n == 0) break;
        for (ssize_t o = 0; o < n && !err;) {
            ssize_t w = write(out, buf + o, size_t(n - o));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) err = w < 0 ? errno : EIO, where = tmp;
            else o += w;
        }
        if (err) break;
    }
    if (!err && fchmod(out, st.st_mode & 07777) != 0) err = errno, where = tmp;
    if (close(out) != 0 && !err) err = errno, where = tmp;
    close(in);
    if (err) {
        unlink(tmp);
        errno = err;
        mrb_sys_fail(mrb, where);
    }
    return mrb_nil_value();
}

// Rsh.__rm_tree(path) (0.9.1, audit 2 M9; Sh.rm_rf): removes a file, a link or a folder and all
// below it, walking with openat(O_NOFOLLOW | O_DIRECTORY) and unlinkat from the folder's own
// descriptor, so a folder swapped for a link while the walk runs is never followed. A missing path
// is fine. Errors: the first one, as Errno, after removing what can be removed.
static int rm_at(int at, const char* name, std::string& bad) {
    struct stat st;
    if (fstatat(at, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT ? 0 : (bad = name, errno);
    if (!S_ISDIR(st.st_mode)) return unlinkat(at, name, 0) == 0 || errno == ENOENT ? 0 : (bad = name, errno);
    int fd = openat(at, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return bad = name, errno;
    DIR* d = fdopendir(fd);
    if (!d) {
        int e = errno;
        close(fd);
        return bad = name, e;
    }
    int first = 0;
    std::vector<std::string> names;
    while (dirent* e = readdir(d))
        if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) names.push_back(e->d_name);
    for (auto& c : names) {
        std::string inner;
        int r = rm_at(dirfd(d), c.c_str(), inner);
        if (r && !first) first = r, bad = std::string(name) + "/" + inner;
    }
    closedir(d);
    if (unlinkat(at, name, AT_REMOVEDIR) != 0 && errno != ENOENT && !first) first = errno, bad = name;
    return first;
}
mrb_value m_rm_tree(mrb_state* mrb, mrb_value) {
    mrb_value pv;
    mrb_get_args(mrb, "S", &pv);
    const char* p = mrb_string_cstr(mrb, pv);
    int r;
    char where[4096];
    {  // the C++ objects end here, before any raise (a longjmp)
        std::string bad;
        r = rm_at(AT_FDCWD, p, bad);
        std::string full = bad.compare(0, std::strlen(p), p) == 0 ? bad : std::string(p) + "/" + bad;
        std::snprintf(where, sizeof where, "%s", full.c_str());
    }
    if (r) {
        errno = r;
        mrb_sys_fail(mrb, where);
    }
    return mrb_nil_value();
}

// Rsh.last_pid (0.9.0, D2): the process id of the last `cmd &`, nil before the first (Ruby's $! is
// its exception variable; `$!` in a command line is the same number).
mrb_value m_last_pid(mrb_state* mrb, mrb_value) {
    pid_t p = shell().last_bg;
    return p > 0 ? mrb_int_value(mrb, (mrb_int)p) : mrb_nil_value();
}
mrb_value m_uid(mrb_state* mrb, mrb_value) { return mrb_int_value(mrb, (mrb_int)getuid()); }
mrb_value m_euid(mrb_state* mrb, mrb_value) { return mrb_int_value(mrb, (mrb_int)geteuid()); }
mrb_value m_gid(mrb_state* mrb, mrb_value) { return mrb_int_value(mrb, (mrb_int)getgid()); }
mrb_value m_egid(mrb_state* mrb, mrb_value) { return mrb_int_value(mrb, (mrb_int)getegid()); }

// IO.popen and IO._popen: the interpreter's version runs its text with /bin/sh. rsh never hands
// text to a shell (audit C2), so these only say what to write instead. The interpreter is also
// built without popen (build_config.rb), so no code that could do it is in the program.
mrb_value m_no_shell(mrb_state* mrb, mrb_value) {
    raise_with(mrb, mrb_class_get_under(mrb, rsh_mod(mrb), "Error"),
               mrb_str_new_lit(mrb, "IO.popen would run its text with /bin/sh, which rsh never does; write the command line itself, "
                                    "`cmd args` for its output, or system(\"prog\", \"arg\")"));
    return mrb_nil_value();
}

// Rsh.__home(user): the home folder of `user` from /etc/passwd, or nil (File.expand_path("~user")
// without the account database of the C library, audit L6).
mrb_value m_home(mrb_state* mrb, mrb_value) {
    const char* u;
    mrb_get_args(mrb, "z", &u);
    mrb_value r = mrb_nil_value();
    {
        std::string h = *u ? home_of(u) : "";
        if (!h.empty()) r = str(mrb, h);
    }
    return r;
}

mrb_value m_glob(mrb_state* mrb, mrb_value) {
    const char* p;
    mrb_get_args(mrb, "z", &p);
    mrb_value r = mrb_ary_new(mrb);
    bool too_many = false;
    {
        std::vector<std::string> all;
        std::vector<std::string> alts = brace_expand(p, &too_many);
        if (!too_many)
            for (auto& b : alts)
                for (auto& m : glob(b)) all.push_back(m);
        for (auto& m : all) mrb_ary_push(mrb, r, str(mrb, m));
    }
    if (too_many)
        mrb_raisef(mrb, mrb_class_get_under(mrb, rsh_mod(mrb), "Error"), "a {a,b} pattern gives more than %d words",
                   (int)BRACE_LIMIT);
    return r;
}

// ---- require_relative / load / source ---------------------------------------------------------

std::string dir_of(const std::string& f) {
    size_t s = f.rfind('/');
    return s == std::string::npos ? "." : s == 0 ? "/" : f.substr(0, s);
}

// The folder of a script for require_relative and a written-out source path: that of its real
// path, links resolved, as Ruby does (0.9.1, audit 2 M5: `ln -s sub/main.rsh ~/bin/tool` looked
// next to the link). A name that is not a file (-c, -, (rsh eval)) is the current folder.
}  // namespace
std::string script_dir(const std::string& f) {
    if (f.empty() || f == "-" || f == "-c" || f[0] == '(') return ".";
    char* r = realpath(f.c_str(), nullptr);
    if (!r) return dir_of(f);
    std::string d = dir_of(r);
    free(r);
    return d;
}
namespace {

bool ends_with(const std::string& s, const char* t) {
    size_t n = std::strlen(t);
    return s.size() >= n && s.compare(s.size() - n, n, t) == 0;
}

// Methods the shell layer reads as Ruby in an Rsh.eval text: those of every file read so far
// (def, define_method :x, attr_*, rsh_methods :x, ...) and those named by rsh_methods at run time.
std::set<std::string>& script_methods() {
    static std::set<std::string> m;
    return m;
}

// Runs compiled text under a protect: inside a running script (require_relative, source, Rsh.eval)
// an exception would otherwise jump past the caller's clean-up, which leaked the compile context
// (audit M8) and left the file list wrong. The exception, if any, is left in mrb->exc.
struct LoadCall {
    mrb_value code;
    mrb_ccontext* c;
};
mrb_value load_body(mrb_state* mrb, void* ud) {
    auto* l = static_cast<LoadCall*>(ud);
    return mrb_load_nstring_cxt(mrb, RSTRING_PTR(l->code), RSTRING_LEN(l->code), l->c);
}
mrb_value load_protected(mrb_state* mrb, mrb_value code, mrb_ccontext* c) {
    LoadCall l{code, c};
    mrb_bool err = FALSE;
    mrb_value v = mrb_protect_error(mrb, load_body, &l, &err);
    if (err && mrb_obj_ptr(v)) mrb->exc = mrb_obj_ptr(v);
    return err ? mrb_nil_value() : v;
}

// Compiles and runs `src` as file `file` at the top level; leaves an exception in mrb->exc.
void load_text(mrb_state* mrb, const std::string& src, const std::string& file, bool rsh_syntax, bool& syntax_error) {
    syntax_error = false;
    mrb_value code = mrb_nil_value();
    int ai = mrb_gc_arena_save(mrb);  // before the code string (0.9.0: each source kept its text)
    {
        Known k;
        if (rsh_syntax) {
            collect_defs(src, script_dir(file), k.methods);
            Result r = transpile(src, file, k);
            script_methods().insert(k.methods.begin(), k.methods.end());
            if (!r.ok()) {
                for (auto& d : r.diags)
                    if (!d.warning) std::fprintf(stderr, "rsh: %s:%d: %s\n", file.c_str(), d.line, d.msg.c_str());
                syntax_error = true;
                mrb_gc_arena_restore(mrb, ai);
                return;
            }
            code = str(mrb, r.ruby);
        } else code = str(mrb, src);
    }
    mrb_ccontext* c = mrb_ccontext_new(mrb);
    mrb_ccontext_filename(mrb, c, file.c_str());
    c->capture_errors = TRUE;
    host.files.push_back(file);
    load_protected(mrb, code, c);
    host.files.pop_back();
    mrb_gc_arena_restore(mrb, ai);
    mrb_ccontext_free(mrb, c);
}

mrb_value m_require(mrb_state* mrb, mrb_value) {
    const char* rel;
    mrb_bool relative;
    mrb_get_args(mrb, "zb", &rel, &relative);
    mrb_value path = mrb_nil_value(), src = mrb_nil_value();
    bool once = relative;
    {
        std::string base = relative && !host.files.empty() ? script_dir(host.files.back()) : ".";
        std::string r = rel;
        std::string found;
        for (const char* ext : {"", ".rsh", ".rb"}) {
            std::string p = (r[0] == '/' ? "" : base + "/") + r + ext;
            if (access(p.c_str(), R_OK) == 0 && !ends_with(p, "/")) {
                found = p;
                break;
            }
        }
        if (!found.empty()) {
            char real[4096];
            if (realpath(found.c_str(), real)) found = real;
            bool ok;
            std::string body = read_file(found, ok);
            path = str(mrb, found);
            if (ok) src = str(mrb, body);
        }
    }
    if (mrb_nil_p(path)) mrb_raisef(mrb, mrb_class_get(mrb, "LoadError"), "cannot load such file -- %s", rel);
    if (mrb_nil_p(src)) mrb_raisef(mrb, mrb_class_get(mrb, "LoadError"), "cannot read %v", path);
    mrb_sym loaded = mrb_intern_lit(mrb, "$__rsh_loaded");
    mrb_value list = mrb_gv_get(mrb, loaded);
    if (!mrb_hash_p(list)) {
        list = mrb_hash_new(mrb);
        mrb_gv_set(mrb, loaded, list);
    }
    if (once && mrb_test(mrb_hash_get(mrb, list, path))) return mrb_false_value();
    mrb_hash_set(mrb, list, path, mrb_true_value());
    bool syn;
    std::string p = cxx(path);
    std::string s = cxx(src);
    bool rsh_syntax = wants_shell_layer(p, s);
    load_text(mrb, s, p, rsh_syntax, syn);
    std::string().swap(p);
    std::string().swap(s);
    if (syn) mrb_raisef(mrb, mrb_class_get(mrb, "ScriptError"), "%v has mistakes (see above)", path);
    if (mrb->exc) {
        mrb_value e = mrb_obj_value(mrb->exc);
        mrb->exc = nullptr;
        mrb_exc_raise(mrb, e);
    }
    return mrb_true_value();
}

// Rsh.__translate(text, file, line, locals): the text read with the shell layer, as Ruby. Bare
// words that are `locals` (the binding's local variables) or script_methods() stay Ruby.
mrb_value m_translate(mrb_state* mrb, mrb_value) {
    mrb_value text, file, locals;
    mrb_int line;
    mrb_get_args(mrb, "SSiA", &text, &file, &line, &locals);
    mrb_value out = mrb_nil_value(), err = mrb_nil_value();
    {
        Known k;
        k.methods = script_methods();
        for (mrb_int n = 0; n < RARRAY_LEN(locals); ++n) {
            mrb_value v = mrb_ary_ref(mrb, locals, n);
            if (mrb_symbol_p(v)) k.locals.insert(mrb_sym_name(mrb, mrb_symbol(v)));
        }
        std::string src = cxx(text), f = cxx(file);
        collect_defs(src, "", k.methods);
        Result r = transpile(src, f, k, int(line));
        if (r.ok()) out = str(mrb, r.ruby);
        else {
            std::string m;
            for (auto& d : r.diags)
                if (!d.warning) m += (m.empty() ? "" : "; ") + f + ":" + std::to_string(d.line) + ": " + d.msg;
            err = str(mrb, m);
        }
    }
    if (!mrb_nil_p(err)) raise_with(mrb, mrb_class_get(mrb, "SyntaxError"), err);
    return out;
}

// Rsh.__load(ruby, file, line): runs Ruby text at the top level (self is main, no caller locals).
mrb_value m_load(mrb_state* mrb, mrb_value) {
    mrb_value code;
    const char* file;
    mrb_int line;
    mrb_get_args(mrb, "Szi", &code, &file, &line);
    mrb_ccontext* c = mrb_ccontext_new(mrb);
    mrb_ccontext_filename(mrb, c, file);
    c->lineno = uint16_t(line);
    c->capture_errors = TRUE;
    mrb_value v = load_protected(mrb, code, c);
    mrb_ccontext_free(mrb, c);
    if (mrb->exc) {
        mrb_value e = mrb_obj_value(mrb->exc);
        mrb->exc = nullptr;
        // The compiler says "line N: ..." for the text's own line; name the text, not the caller's
        // file (Rsh.eval("x = (") reported `FILE:1:` against the script's name, audit L-9).
        if (!std::strcmp(mrb_obj_classname(mrb, e), "SyntaxError")) {
            mrb_value named = mrb_nil_value();
            {  // no C++ object alive across the raise (it does not unwind)
                mrb_value msg = mrb_funcall(mrb, e, "message", 0);
                std::string m = mrb_string_p(msg) ? cxx(msg) : "";
                if (m.compare(0, 5, "line ") == 0) named = str(mrb, std::string(file) + ":" + m.substr(5));
            }
            if (!mrb_nil_p(named)) raise_with(mrb, mrb_class_get(mrb, "SyntaxError"), named);
        }
        mrb_exc_raise(mrb, e);
    }
    return v;
}

// Rsh.__declare(names): rsh_methods at run time (for Rsh.eval texts).
mrb_value m_declare(mrb_state* mrb, mrb_value) {
    mrb_value names;
    mrb_get_args(mrb, "A", &names);
    for (mrb_int n = 0; n < RARRAY_LEN(names); ++n) {
        mrb_value v = mrb_ary_ref(mrb, names, n);
        if (mrb_string_p(v)) script_methods().insert(cxx(v));
    }
    return mrb_nil_value();
}

int report(mrb_state* mrb, mrb_value exc);

// A syntax error on a line that starts with a Ruby word that is also a common program (`printf "%s\n"
// x | sort | head`): the line is Ruby by rule 4, and the compiler's message alone does not say why
// (0.9.0).
std::string ruby_word_hint(const std::string& src, int line) {
    size_t b = 0;
    for (int n = 1; n < line && b != std::string::npos; ++n) {
        b = src.find('\n', b);
        if (b != std::string::npos) ++b;
    }
    if (b == std::string::npos || b >= src.size()) return "";
    while (b < src.size() && (src[b] == ' ' || src[b] == '\t')) ++b;
    size_t e = b;
    while (e < src.size() && (std::isalnum((unsigned char)src[e]) || src[e] == '_')) ++e;
    std::string w = src.substr(b, e - b);
    static const char* const programs[] = {"printf", "open", "format", "test", "exec", "eval", "send", "method", "trap", "p"};
    bool hit = false;
    for (const char* x : programs) hit = hit || w == x;
    if (!hit || !is_ruby_word(w) || e >= src.size() || src[e] != ' ') return "";
    return " (`" + w + "` at the start of a line is Ruby's " + w + "; for the program write $ " + w + " ...)";
}

int RtHost::source(const std::vector<std::string>& a) {
    if (a.size() < 2) {
        std::fprintf(stderr, "rsh: source needs a file\n");
        return 2;
    }
    // only a plain file, as the read-ahead (0.9.1, audit 2 M4): a folder, /dev/null or a FIFO is
    // a mistake, not an empty script
    struct stat sb;
    if (stat(a[1].c_str(), &sb) == 0 && !S_ISREG(sb.st_mode)) {
        std::fprintf(stderr, "rsh: source: %s: %s\n", a[1].c_str(), S_ISDIR(sb.st_mode) ? "Is a directory" : "not a plain file");
        return 1;
    }
    bool ok;
    std::string body = read_file(a[1], ok);
    if (!ok) {
        std::fprintf(stderr, "rsh: source: %s: %s\n", a[1].c_str(), std::strerror(errno));
        return 1;
    }
    // `source lib.rsh x y`: ARGV is [x, y] while the file runs, as $1 $2 in sh (0.9.0; the words
    // were dropped); without words the file sees the script's ARGV.
    mrb_value argv = mrb_const_get(mrb, mrb_obj_value(mrb->object_class), mrb_intern_lit(mrb, "ARGV"));
    mrb_value kept = mrb_nil_value();
    if (a.size() > 2 && mrb_array_p(argv)) {
        kept = mrb_ary_new_from_values(mrb, RARRAY_LEN(argv), RARRAY_PTR(argv));
        mrb_gc_protect(mrb, kept);
        mrb_ary_clear(mrb, argv);
        for (size_t k = 2; k < a.size(); ++k) mrb_ary_push(mrb, argv, str(mrb, a[k]));
    }
    bool syn;
    load_text(mrb, body, a[1], wants_shell_layer(a[1], body), syn);
    if (mrb_array_p(kept)) mrb_ary_replace(mrb, argv, kept);
    if (syn) return 2;
    if (mrb->exc) {  // reported here, as `rsh: FILE:LINE: message (Class)`; the command line only fails
        mrb_value e = mrb_obj_value(mrb->exc);
        mrb->exc = nullptr;
        if (!std::strcmp(mrb_obj_classname(mrb, e), "SyntaxError")) {
            // the compiler says "line N: ...": the sourced file's name goes in front (0.9.0: the
            // message was `rsh: line 1: ...`, with no file)
            mrb_value msg = mrb_funcall(mrb, e, "message", 0);
            if (mrb->exc) mrb->exc = nullptr;
            std::string m = mrb_string_p(msg) ? cxx(msg) : "syntax error";
            if (m.compare(0, 5, "line ") == 0) {
                std::fprintf(stderr, "rsh: %s:%s%s\n", a[1].c_str(), m.c_str() + 5, ruby_word_hint(body, std::atoi(m.c_str() + 5)).c_str());
                return 2;
            }
        }
        // `exit` or a signal inside the file ends the script, as in sh: the line raises it again
        // (1.0, audit 2 L5; it was reported as a failed command, "source exited N")
        if (MRB_EXC_EXIT_P(mrb_obj_ptr(e)) || mrb_obj_is_kind_of(mrb, e, mrb_class_get(mrb, "SignalException"))) {
            exc = e;
            mrb_gc_register(mrb, exc);
            return 0;
        }
        int st = report(mrb, e);
        return st ? st : 1;
    }
    return 0;
}

// ---- Regexp over the system's regex library ------------------------------------------------------
// Two compiled codes per pattern (audit H-1). The engine checks a UTF-8 subject on every call
// unless told it was checked, and with PCRE2_MATCH_INVALID_UTF it checks always (from the start
// offset to the end), which made every loop over a long text quadratic. So: `code` is compiled
// with PCRE2_UTF only and runs with PCRE2_NO_UTF_CHECK on a subject this file checked once
// (utf8_kind); `inv` adds PCRE2_MATCH_INVALID_UTF, is made on the first subject with an invalid
// sequence, and keeps the behaviour of 0.1.3 there (an invalid byte matches no pattern item and
// is kept: "caf\xC3".scan(/./).size == 3). The loops without a block (scan, split, sub/gsub with
// a String) run here, the subject checked once per call; the Ruby loops (a block, a Hash) match on
// a frozen copy, whose check is remembered (Re::subj, kept alive in @__subject). One match data
// for all patterns (match_data).

struct Re {
    pcre2_code* code = nullptr;
    pcre2_code* inv = nullptr;  // only for a pattern that cannot take frag_match
    uint32_t opts = 0, groups = 0, lookbehind = 0;
    bool frag = false;               // an invalid subject goes through frag_match (no \A or \G, newline LF)
    const RString* subj = nullptr;  // the last frozen subject checked
    const char* sptr = nullptr;
    mrb_int slen = 0;
    int kind = 0;
    size_t *bad = nullptr, nbad = 0, badcap = 0;  // utf8_bad of the subject checked last
};
void re_free(mrb_state* mrb, void* p) {
    auto* r = static_cast<Re*>(p);
    if (!r) return;
    if (r->code) pcre2_code_free(r->code);
    if (r->inv) pcre2_code_free(r->inv);
    if (r->bad) mrb_free(mrb, r->bad);
    mrb_free(mrb, r);
}
const mrb_data_type re_type = {"Regexp", re_free};

// The names the match functions use, looked up once per interpreter (a symbol lookup costs about
// half a microsecond here, ten of them per match were most of the time of =~). rt_open resets it.
struct ReNames {
    mrb_state* owner = nullptr;
    mrb_sym last, nth[10], regexp, string, offsets, source, options, subject, loaded;
    RClass* match_data;
    RClass* error;
};
ReNames re_names;
const ReNames& names(mrb_state* mrb) {
    ReNames& n = re_names;
    if (n.owner == mrb) return n;
    n.last = mrb_intern_lit(mrb, "$~");
    char name[3] = {'$', '0', 0};
    for (int k = 1; k <= 9; ++k) {
        name[1] = char('0' + k);
        n.nth[k] = mrb_intern(mrb, name, 2);
    }
    n.regexp = mrb_intern_lit(mrb, "@regexp");
    n.string = mrb_intern_lit(mrb, "@string");
    n.offsets = mrb_intern_lit(mrb, "@offsets");
    n.source = mrb_intern_lit(mrb, "@source");
    n.options = mrb_intern_lit(mrb, "@options");
    n.subject = mrb_intern_lit(mrb, "@__subject");
    n.loaded = mrb_intern_lit(mrb, "__rsh_loaded");
    n.match_data = mrb_class_get(mrb, "MatchData");
    n.error = mrb_class_get(mrb, "RegexpError");
    n.owner = mrb;
    return n;
}

// The few places where Ruby's pattern language and the engine's differ.
std::string translate(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[i + 1];
            if (c == 'h') o += "[0-9a-fA-F]";
            else if (c == 'H') o += "[^0-9a-fA-F]";
            else {
                o += s[i];
                o += c;
            }
            ++i;
            continue;
        }
        o += s[i];
    }
    return o;
}

}  // namespace

// What a subject is, for the engine: 2 ASCII only, 1 valid UTF-8, -1 not valid UTF-8. The rules are
// the engine's (RFC 3629: no overlong forms, no surrogates, nothing over U+10FFFF), so a subject
// called valid here is valid for PCRE2_NO_UTF_CHECK. Eight bytes at a time while they are ASCII.
int utf8_kind(const char* s, size_t n) {
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    size_t i = 0;
    bool ascii = true;
    while (i < n) {
        if (i + 8 <= n) {
            uint64_t w;
            std::memcpy(&w, p + i, 8);
            if (!(w & 0x8080808080808080ULL)) {
                i += 8;
                continue;
            }
        }
        unsigned c = p[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        ascii = false;
        auto cont = [&](size_t k) { return i + k < n && (p[i + k] & 0xC0) == 0x80; };
        if (c < 0xC2) return -1;
        if (c < 0xE0) {
            if (!cont(1)) return -1;
            i += 2;
        } else if (c < 0xF0) {
            if (!cont(1) || !cont(2)) return -1;
            if ((c == 0xE0 && p[i + 1] < 0xA0) || (c == 0xED && p[i + 1] >= 0xA0)) return -1;
            i += 3;
        } else if (c < 0xF5) {
            if (!cont(1) || !cont(2) || !cont(3)) return -1;
            if ((c == 0xF0 && p[i + 1] < 0x90) || (c == 0xF4 && p[i + 1] >= 0x90)) return -1;
            i += 4;
        } else return -1;
    }
    return ascii ? 2 : 1;
}

namespace {
bool cont_byte(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }
// The length of the valid character at p[i], or 0 (utf8_kind's rules).
size_t char_len(const unsigned char* p, size_t i, size_t n) {
    unsigned c = p[i];
    auto cont = [&](size_t k) { return i + k < n && (p[i + k] & 0xC0) == 0x80; };
    if (c < 0x80) return 1;
    if (c < 0xC2) return 0;
    if (c < 0xE0) return cont(1) ? 2 : 0;
    if (c < 0xF0) {
        if (!cont(1) || !cont(2) || (c == 0xE0 && p[i + 1] < 0xA0) || (c == 0xED && p[i + 1] >= 0xA0)) return 0;
        return 3;
    }
    if (c < 0xF5) {
        if (!cont(1) || !cont(2) || !cont(3) || (c == 0xF0 && p[i + 1] < 0x90) || (c == 0xF4 && p[i + 1] >= 0x90)) return 0;
        return 4;
    }
    return 0;
}
}  // namespace

// Where the engine finds invalid UTF-8 in s[0, n): the offset of each bad character, as its own
// check reports it (pcre2_get_startchar after a UTF error), going on one byte later and past
// continuation bytes, as PCRE2_MATCH_INVALID_UTF does. Writes up to cap offsets; returns how many.
size_t utf8_bad(const char* s, size_t n, size_t* out, size_t cap) {
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    size_t i = 0, k = 0;
    while (i < n) {
        if (i + 8 <= n) {
            uint64_t w;
            std::memcpy(&w, p + i, 8);
            if (!(w & 0x8080808080808080ULL)) {
                i += 8;
                continue;
            }
        }
        size_t l = char_len(p, i, n);
        if (l) {
            i += l;
            continue;
        }
        if (k < cap) out[k] = i;
        ++k;
        ++i;
        while (i < n && cont_byte(s[i])) ++i;
    }
    return k;
}

// One match as the engine makes it with PCRE2_MATCH_INVALID_UTF (pcre2_match.c, "fragments"),
// but on the valid pieces only, with PCRE2_NO_UTF_CHECK and `code` compiled without that option:
// the engine's own way re-checks the subject from the start offset to the next invalid byte on
// every call, quadratic in a scan of a long text with one bad byte at its end. The steps are the
// engine's: skip continuation bytes at the start; look behind at most `lookbehind` characters and
// not over a bad one; the first piece ends at the next bad character (no end of line there); if it
// does not match, each next piece (no start or end of line at its edges). A piece is passed as its
// own subject, so \A and \G would match at its start, and \z and \Z at its end (PCRE2 10.45 and
// later match them at the end of the whole subject only): a pattern with them does not come here
// (Re::frag).
// `bad` is utf8_bad of the subject. The offsets in md are made absolute.
int frag_match(const pcre2_code* code, const char* p, size_t len, size_t start, const size_t* bad, size_t nbad,
               uint32_t lookbehind, pcre2_match_data* md, pcre2_match_context* cx) {
    auto next_bad = [&](size_t from) {
        size_t lo = 0, hi = nbad;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (bad[mid] < from) lo = mid + 1;
            else hi = mid;
        }
        return lo < nbad ? bad[lo] : len;
    };
    size_t s0 = start;
    bool skipped = false;
    while (s0 < len && cont_byte(p[s0])) {
        ++s0;
        skipped = true;
    }
    size_t cs = s0;
    if (!skipped)
        for (uint32_t i = lookbehind; i > 0 && cs > 0; --i) {
            --cs;
            while (cs > 0 && cont_byte(p[cs])) --cs;
        }
    size_t x;
    for (;;) {
        x = next_bad(cs);
        if (x >= s0) break;
        cs = x + 1;
        while (cs < s0 && cont_byte(p[cs])) ++cs;
    }
    uint32_t opt = PCRE2_NO_UTF_CHECK | (x < len ? PCRE2_NOTEOL : 0);
    if (cs > 0 && (p[cs - 1] != '\n' || cs == x)) opt |= PCRE2_NOTBOL;  // ^ at cs: only after a newline
    int rc = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(p + cs), x - cs, s0 - cs, opt, md, cx);
    while (rc == PCRE2_ERROR_NOMATCH && x < len) {
        size_t st = x + 1;
        while (st < len && cont_byte(p[st])) ++st;
        if (st >= len) break;
        x = next_bad(st);
        if (x == st) continue;
        cs = st;
        rc = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(p + cs), x - cs, 0,
                         PCRE2_NO_UTF_CHECK | PCRE2_NOTBOL | (x < len ? PCRE2_NOTEOL : 0), md, cx);
    }
    if (rc > 0) {
        PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
        for (int g = 0; g < rc; ++g)
            if (ov[2 * g] != PCRE2_UNSET) ov[2 * g] += cs, ov[2 * g + 1] += cs;
    }
    return rc;
}

namespace {

extern "C" {
mrb_value mrb_str_byte_subseq(mrb_state* mrb, mrb_value str, mrb_int beg, mrb_int len);
mrb_int mrb_utf8len(const char* str, const char* end);
}

// The number of characters in the first n bytes of a subject of this kind (String#size's count:
// an invalid byte is one character).
mrb_int char_count(const char* p, mrb_int n, int kind) {
    if (kind == 2) return n;
    mrb_int c = 0;
    if (kind == 1) {
        for (mrb_int i = 0; i < n; ++i) c += (static_cast<unsigned char>(p[i]) & 0xC0) != 0x80;
        return c;
    }
    for (const char *q = p, *e = p + n; q < e; ++c) q += static_cast<unsigned char>(*q) < 0x80 ? 1 : mrb_utf8len(q, e);
    return c;
}

// Rsh.__regexp: loads the Ruby side of Regexp (src/prelude_regexp.rb) once per interpreter; true
// if it was loaded by this call. Called by the first Regexp made and by the stand-ins in
// src/prelude.rb, so start-up does not read it.
mrb_value m_regexp(mrb_state* mrb, mrb_value) {
    mrb_value re = mrb_obj_value(mrb_class_get(mrb, "Regexp"));
    mrb_sym done = names(mrb).loaded;
    if (mrb_iv_defined(mrb, re, done)) return mrb_false_value();
    mrb_iv_set(mrb, re, done, mrb_true_value());
    // At the top level, as src/prelude.rb is: `class MatchData` names ::MatchData even though the
    // caller is a method (mrb_load_irep would take the caller's class as the place for definitions).
    mrb_irep* irep = mrb_read_irep(mrb, rsh_prelude_regexp);
    if (!irep) mrb_raise(mrb, E_SCRIPT_ERROR, "rsh: the Regexp side did not load");
    RProc* proc = mrb_proc_new(mrb, irep);
    mrb_irep_decref(mrb, irep);
    MRB_PROC_SET_TARGET_CLASS(proc, mrb->object_class);
    mrb_top_run(mrb, proc, mrb_top_self(mrb), 0);
    if (mrb->exc) {
        mrb_value e = mrb_obj_value(mrb->exc);
        mrb->exc = nullptr;
        mrb_exc_raise(mrb, e);
    }
    return mrb_true_value();
}

// Compiles `src` (Ruby's pattern language) with `o`; nil, or the error message.
// The last patterns compiled, so a pattern literal in a loop (made again on every pass: the
// interpreter compiles /re/ to Regexp.compile) is a copy, not a compile. Owned here; each Regexp
// gets its own copy (pcre2_code_copy).
struct CompiledPattern {
    std::string src;
    uint32_t opts = 0;
    pcre2_code* code = nullptr;
};
CompiledPattern compiled[16];
unsigned compiled_next = 0;

mrb_value re_compile(mrb_state* mrb, mrb_value src, uint32_t o, pcre2_code** out) {
    const char* sp = RSTRING_PTR(src);
    size_t sn = size_t(RSTRING_LEN(src));
    for (CompiledPattern& c : compiled)
        if (c.code && c.opts == o && c.src.size() == sn && !std::memcmp(c.src.data(), sp, sn)) {
            *out = pcre2_code_copy(c.code);
            if (*out) return mrb_nil_value();
        }
    std::string pat = translate(cxx(src));
    int ec;
    PCRE2_SIZE eo;
    *out = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pat.data()), pat.size(), o, &ec, &eo, nullptr);
    if (*out) {
        CompiledPattern& c = compiled[compiled_next++ % 16];
        if (c.code) pcre2_code_free(c.code);
        c.code = pcre2_code_copy(*out);
        c.src.assign(sp, sn);
        c.opts = o;
        return mrb_nil_value();
    }
    PCRE2_UCHAR buf[256];
    pcre2_get_error_message(ec, buf, sizeof buf);
    return str(mrb, std::string(reinterpret_cast<char*>(buf)) + " at " + std::to_string(eo) + ": /" + cxx(src) + "/");
}

mrb_value m_re_init(mrb_state* mrb, mrb_value self) {
    // The arguments first: running the Regexp side's code from here reuses this call's argument
    // slots (the interpreter's rule for code run from a C function), so they are read before.
    mrb_value src, opt = mrb_nil_value();
    mrb_get_args(mrb, "o|o", &src, &opt);
    mrb_gc_protect(mrb, src);
    mrb_gc_protect(mrb, opt);
    m_regexp(mrb, self);
    if (mrb_obj_is_kind_of(mrb, src, mrb_class_get(mrb, "Regexp"))) {
        opt = mrb_iv_get(mrb, src, names(mrb).options);
        src = mrb_iv_get(mrb, src, names(mrb).source);
    }
    if (!mrb_string_p(src)) mrb_raise(mrb, E_TYPE_ERROR, "a Regexp needs a String");
    mrb_int flags = 0;
    if (mrb_integer_p(opt)) flags = mrb_integer(opt);
    else if (mrb_string_p(opt)) {
        for (char c : cxx(opt)) flags |= c == 'i' ? 1 : c == 'x' ? 2 : c == 'm' ? 4 : 0;
    } else if (mrb_test(opt)) flags = 1;
    Re* re = static_cast<Re*>(mrb_malloc(mrb, sizeof(Re)));
    *re = Re{};
    if (DATA_PTR(self)) re_free(mrb, DATA_PTR(self));
    DATA_PTR(self) = re;
    DATA_TYPE(self) = &re_type;
    re->opts = PCRE2_UTF | PCRE2_MULTILINE;
    if (flags & 1) re->opts |= PCRE2_CASELESS;
    if (flags & 2) re->opts |= PCRE2_EXTENDED;
    if (flags & 4) re->opts |= PCRE2_DOTALL;
    mrb_value err = re_compile(mrb, src, re->opts, &re->code);
    if (re->code) {
        pcre2_pattern_info(re->code, PCRE2_INFO_CAPTURECOUNT, &re->groups);
        pcre2_pattern_info(re->code, PCRE2_INFO_MAXLOOKBEHIND, &re->lookbehind);
        uint32_t nl = 0;
        pcre2_pattern_info(re->code, PCRE2_INFO_NEWLINE, &nl);
        // \A, \G: frag_match passes a piece as the subject, so they would match at its start. \z, \Z:
        // since PCRE2 10.45 ("\z was misbehaving when matching fragments inside invalid UTF strings")
        // the engine matches them only at the end of the whole subject, not at the end of a piece
        // before a bad byte, which a piece passed as the subject cannot tell. All four take the
        // engine's own invalid-UTF path (Re::inv).
        bool anchor = false;
        const char* sp = RSTRING_PTR(src);
        for (mrb_int i = 0; i + 1 < RSTRING_LEN(src); ++i)
            if (sp[i] == '\\') {
                anchor |= sp[i + 1] == 'A' || sp[i + 1] == 'G' || sp[i + 1] == 'z' || sp[i + 1] == 'Z';
                ++i;
            }
        re->frag = nl == PCRE2_NEWLINE_LF && !anchor;
    }
    if (!mrb_nil_p(err)) raise_with(mrb, names(mrb).error, err);
    mrb_iv_set(mrb, self, names(mrb).source, mrb_str_dup(mrb, src));
    mrb_iv_set(mrb, self, names(mrb).options, mrb_fixnum_value(flags));
    return self;
}

// One match data for every match (the offsets of the last one): a match data per Regexp kept the
// engine's working memory (20 KB from the first match) per pattern object, so a pattern literal in
// a loop over 100,000 lines held 140 MB until the collector ran. Nothing runs between a match
// and the reading of its offsets.
pcre2_match_data* the_md = nullptr;
uint32_t the_md_pairs = 0;
pcre2_match_data* match_data(mrb_state* mrb, uint32_t pairs) {
    if (!the_md || the_md_pairs < pairs) {
        if (the_md) pcre2_match_data_free(the_md);
        the_md_pairs = pairs < 10 ? 10 : pairs;
        the_md = pcre2_match_data_create(the_md_pairs, nullptr);
        if (!the_md) {
            the_md_pairs = 0;
            mrb_raise(mrb, E_RUNTIME_ERROR, "pattern match failed: out of memory");
        }
    }
    return the_md;
}
PCRE2_SIZE* ovector() { return pcre2_get_ovector_pointer(the_md); }

// One match context for every match: a pattern from input (Regexp.new(text)) gets at most this
// much work per call, then RegexpError (audit M7). The engine's own default match limit is 10 times
// more (about 0.1 s per call); its heap limit 20 GB. No JIT.
constexpr uint32_t MATCH_LIMIT = 1000000, DEPTH_LIMIT = 1000000, HEAP_LIMIT_KIB = 262144;  // 256 MB of backtracking
pcre2_match_context* match_context() {
    static pcre2_match_context* cx = nullptr;
    if (!cx) {
        cx = pcre2_match_context_create(nullptr);
        if (cx) {
            pcre2_set_match_limit(cx, MATCH_LIMIT);
            pcre2_set_depth_limit(cx, DEPTH_LIMIT);
            pcre2_set_heap_limit(cx, HEAP_LIMIT_KIB);
        }
    }
    return cx;
}

Re* get_re(mrb_state* mrb, mrb_value self) {
    Re* re = static_cast<Re*>(mrb_data_get_ptr(mrb, self, &re_type));
    if (!re || !re->code) mrb_raise(mrb, E_TYPE_ERROR, "uninitialized Regexp");
    return re;
}

// The kind (utf8_kind) of subject `s`: checked now, or remembered when `s` is the frozen subject
// checked last (a frozen String cannot change; @__subject keeps it, so its address is not reused).
// For an invalid subject, re->bad then holds its utf8_bad (for frag_match).
int subject_kind(mrb_state* mrb, mrb_value self, Re* re, mrb_value s) {
    const RString* rs = mrb_str_ptr(s);
    bool frozen = mrb_frozen_p(rs);
    if (frozen && re->subj == rs && re->sptr == RSTRING_PTR(s) && re->slen == RSTRING_LEN(s)) return re->kind;
    re->subj = nullptr;
    int k = utf8_kind(RSTRING_PTR(s), size_t(RSTRING_LEN(s)));
    if (k < 0 && re->frag) {
        re->nbad = utf8_bad(RSTRING_PTR(s), size_t(RSTRING_LEN(s)), re->bad, re->badcap);
        if (re->nbad > re->badcap) {
            re->bad = static_cast<size_t*>(mrb_realloc(mrb, re->bad, re->nbad * sizeof(size_t)));
            re->badcap = re->nbad;
            utf8_bad(RSTRING_PTR(s), size_t(RSTRING_LEN(s)), re->bad, re->badcap);
        }
    }
    if (frozen && RSTRING_LEN(s) >= 256) {
        mrb_iv_set(mrb, self, names(mrb).subject, s);
        re->subj = rs;
        re->sptr = RSTRING_PTR(s);
        re->slen = RSTRING_LEN(s);
        re->kind = k;
    }
    return k;
}

// One match at byte `pos`: the number of groups set plus one, or 0 for no match. The engine's
// offsets are in ovector(). A subject with an invalid sequence, or a start inside a character,
// matches as with PCRE2_MATCH_INVALID_UTF (what 0.1.3 did on every call): through frag_match, or
// for a pattern with \A or \G through the engine's own checking code.
int re_exec(mrb_state* mrb, mrb_value self, Re* re, mrb_value s, int kind, mrb_int pos) {
    const char* p = RSTRING_PTR(s);
    mrb_int len = RSTRING_LEN(s);
    pcre2_code* code = re->code;
    uint32_t opt = PCRE2_NO_UTF_CHECK;
    bool odd = kind < 0 || (pos < len && (static_cast<unsigned char>(p[pos]) & 0xC0) == 0x80);
    int rc;
    if (odd && re->frag)
        rc = frag_match(re->code, p, size_t(len), size_t(pos), kind < 0 ? re->bad : nullptr, kind < 0 ? re->nbad : 0,
                        re->lookbehind, match_data(mrb, re->groups + 1), match_context());
    else {
        if (odd) {
            if (!re->inv) {
                mrb_value err = re_compile(mrb, mrb_iv_get(mrb, self, names(mrb).source),
                                           re->opts | PCRE2_MATCH_INVALID_UTF, &re->inv);
                if (!re->inv) raise_with(mrb, names(mrb).error, err);
            }
            code = re->inv;
            opt = 0;
        }
        rc = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(p), size_t(len), size_t(pos), opt, match_data(mrb, re->groups + 1),
                         match_context());
    }
    if (rc == PCRE2_ERROR_NOMATCH) return 0;
    if (rc < 0) {
        if (rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT || rc == PCRE2_ERROR_HEAPLIMIT)
            mrb_raise(mrb, names(mrb).error,
                      "the pattern is too complex for this text (it needs more than the match limit); make it simpler");
        PCRE2_UCHAR buf[256];
        pcre2_get_error_message(rc, buf, sizeof buf);
        mrb_raisef(mrb, E_RUNTIME_ERROR, "pattern match failed: %s", reinterpret_cast<char*>(buf));
    }
    return rc == 0 ? int(re->groups) + 1 : rc;
}

// Byte offsets of group g of the last match, or false when the group took no part.
bool group(int rc, uint32_t g, mrb_int& b, mrb_int& e) {
    PCRE2_SIZE* ov = ovector();
    if (int(g) >= rc || ov[2 * g] == PCRE2_UNSET) return false;
    b = static_cast<mrb_int>(ov[2 * g]);
    e = static_cast<mrb_int>(ov[2 * g + 1]);
    return true;
}

// [b0, e0, b1, e1, ...] of the last match (-1 for groups that took no part).
mrb_value offsets(mrb_state* mrb, Re* re, int rc) {
    mrb_value r = mrb_ary_new_capa(mrb, 2 * (re->groups + 1));
    for (uint32_t g = 0; g <= re->groups; ++g) {
        mrb_int b = -1, e = -1;
        group(rc, g, b, e);
        mrb_ary_push(mrb, r, mrb_fixnum_value(b));
        mrb_ary_push(mrb, r, mrb_fixnum_value(e));
    }
    return r;
}

// The text of group g of the last match, or nil.
mrb_value group_str(mrb_state* mrb, int rc, mrb_value s, uint32_t g) {
    mrb_int b, e;
    if (!group(rc, g, b, e)) return mrb_nil_value();
    return mrb_str_byte_subseq(mrb, s, b, e - b);
}

// $~ and $1..$9 from a MatchData (nil: all nil), as Rsh.__set_match does from Ruby.
void set_last(mrb_state* mrb, mrb_value md) {
    const ReNames& n = names(mrb);
    mrb_gv_set(mrb, n.last, md);
    mrb_value offs = mrb_nil_value(), s = mrb_nil_value();
    if (!mrb_nil_p(md)) {
        offs = mrb_iv_get(mrb, md, n.offsets);
        s = mrb_iv_get(mrb, md, n.string);
    }
    for (mrb_int k = 1; k <= 9; ++k) {
        mrb_value v = mrb_nil_value();
        if (mrb_array_p(offs) && mrb_string_p(s) && 2 * k + 1 < RARRAY_LEN(offs)) {
            mrb_value vb = mrb_ary_ref(mrb, offs, 2 * k), ve = mrb_ary_ref(mrb, offs, 2 * k + 1);
            mrb_int b = mrb_integer_p(vb) ? mrb_integer(vb) : -1, e = mrb_integer_p(ve) ? mrb_integer(ve) : -1;
            if (b >= 0 && e >= b && e <= RSTRING_LEN(s)) v = mrb_str_byte_subseq(mrb, s, b, e - b);
        }
        mrb_gv_set(mrb, n.nth[k], v);
    }
}

// A MatchData for the last match, built without running Ruby code.
mrb_value make_md(mrb_state* mrb, mrb_value self, Re* re, int rc, mrb_value s) {
    const ReNames& n = names(mrb);
    RObject* o = reinterpret_cast<RObject*>(mrb_obj_alloc(mrb, MRB_TT_OBJECT, n.match_data));
    mrb_value md = mrb_obj_value(o);
    mrb_iv_set(mrb, md, n.regexp, self);
    mrb_iv_set(mrb, md, n.string, s);
    mrb_iv_set(mrb, md, n.offsets, offsets(mrb, re, rc));
    return md;
}

mrb_int check_pos(mrb_value s, mrb_int pos) { return pos < 0 || pos > RSTRING_LEN(s) ? -1 : pos; }

// __match(str, byte_pos) -> [b0, e0, b1, e1, ...] (-1 for groups that took no part) or nil
mrb_value m_re_match(mrb_state* mrb, mrb_value self) {
    mrb_value s;
    mrb_int pos;
    mrb_get_args(mrb, "Si", &s, &pos);
    Re* re = get_re(mrb, self);
    if (check_pos(s, pos) < 0) return mrb_nil_value();
    int rc = re_exec(mrb, self, re, s, subject_kind(mrb, self, re, s), pos);
    return rc ? offsets(mrb, re, rc) : mrb_nil_value();
}

// __test(str, byte_pos) -> true or false (match?: no MatchData, $~ untouched)
mrb_value m_re_test(mrb_state* mrb, mrb_value self) {
    mrb_value s;
    mrb_int pos;
    mrb_get_args(mrb, "Si", &s, &pos);
    Re* re = get_re(mrb, self);
    if (check_pos(s, pos) < 0) return mrb_false_value();
    return mrb_bool_value(re_exec(mrb, self, re, s, subject_kind(mrb, self, re, s), pos) != 0);
}

// __md(str, byte_pos, reset) -> MatchData or nil; sets $~ and $1..$9 on a match, and to nil on
// no match when `reset` (match); a loop passes false and keeps the last match.
mrb_value m_re_md(mrb_state* mrb, mrb_value self) {
    mrb_value s;
    mrb_int pos;
    mrb_bool reset;
    mrb_get_args(mrb, "Sib", &s, &pos, &reset);
    Re* re = get_re(mrb, self);
    int rc = check_pos(s, pos) < 0 ? 0 : re_exec(mrb, self, re, s, subject_kind(mrb, self, re, s), pos);
    mrb_value md = rc ? make_md(mrb, self, re, rc, s) : mrb_nil_value();
    if (rc || reset) set_last(mrb, md);
    return md;
}

// __eqtilde(str) -> the character index of the match, or nil; sets $~ and $1..$9 (=~)
mrb_value m_re_eqtilde(mrb_state* mrb, mrb_value self) {
    mrb_value s;
    mrb_get_args(mrb, "S", &s);
    Re* re = get_re(mrb, self);
    int kind = subject_kind(mrb, self, re, s);
    int rc = re_exec(mrb, self, re, s, kind, 0);
    if (!rc) {
        set_last(mrb, mrb_nil_value());
        return mrb_nil_value();
    }
    mrb_int b = static_cast<mrb_int>(ovector()[0]);
    set_last(mrb, make_md(mrb, self, re, rc, s));
    return mrb_fixnum_value(char_count(RSTRING_PTR(s), b, kind));
}

// Regexp.__chars(str, byte_pos) -> the character index of byte_pos (MatchData#begin and #end)
mrb_value m_re_chars(mrb_state* mrb, mrb_value) {
    mrb_value s;
    mrb_int b;
    mrb_get_args(mrb, "Si", &s, &b);
    if (b < 0 || b > RSTRING_LEN(s)) return mrb_nil_value();
    return mrb_fixnum_value(char_count(RSTRING_PTR(s), b, utf8_kind(RSTRING_PTR(s), size_t(b))));
}

// Regexp.__set_last(md): $~ and $1..$9 from md (or nil)
mrb_value m_re_set_last(mrb_state* mrb, mrb_value) {
    mrb_value md;
    mrb_get_args(mrb, "o", &md);
    set_last(mrb, md);
    return md;
}

// __scan(str) -> the matches (a String each, or an Array of the groups); $~ the last match (left
// as it was when there is none). The loop of String#scan without a block.
mrb_value m_re_scan(mrb_state* mrb, mrb_value self) {
    mrb_value s;
    mrb_get_args(mrb, "S", &s);
    Re* re = get_re(mrb, self);
    int kind = subject_kind(mrb, self, re, s);
    mrb_value found = mrb_ary_new(mrb);
    mrb_int pos = 0, bytes = RSTRING_LEN(s);
    int ai = mrb_gc_arena_save(mrb);
    int last = 0;
    mrb_int lastpos = 0;
    while (pos <= bytes) {
        int rc = re_exec(mrb, self, re, s, kind, pos);
        if (!rc) break;
        last = rc;
        lastpos = pos;
        if (re->groups == 0) mrb_ary_push(mrb, found, group_str(mrb, rc, s, 0));
        else {
            mrb_value caps = mrb_ary_new_capa(mrb, re->groups);
            for (uint32_t g = 1; g <= re->groups; ++g) mrb_ary_push(mrb, caps, group_str(mrb, rc, s, g));
            mrb_ary_push(mrb, found, caps);
        }
        mrb_gc_arena_restore(mrb, ai);
        PCRE2_SIZE* ov = ovector();
        pos = ov[1] == ov[0] ? static_cast<mrb_int>(ov[1]) + 1 : static_cast<mrb_int>(ov[1]);
    }
    if (last) {  // the last match again: its offsets were overwritten by the failed try
        int rc = re_exec(mrb, self, re, s, kind, lastpos);
        set_last(mrb, make_md(mrb, self, re, rc, s));
    }
    return found;
}

// __split(str, limit) -> the parts (String#split with a pattern; $~ untouched)
mrb_value m_re_split(mrb_state* mrb, mrb_value self) {
    mrb_value s;
    mrb_int limit;
    mrb_get_args(mrb, "Si", &s, &limit);
    Re* re = get_re(mrb, self);
    int kind = subject_kind(mrb, self, re, s);
    mrb_value parts = mrb_ary_new(mrb);
    mrb_int pos = 0, search = 0, bytes = RSTRING_LEN(s);
    int ai = mrb_gc_arena_save(mrb);
    while (search <= bytes && (limit <= 0 || RARRAY_LEN(parts) < limit - 1)) {
        int rc = re_exec(mrb, self, re, s, kind, search);
        if (!rc) break;
        mrb_int b, e;
        group(rc, 0, b, e);
        if (e == b) {
            if (b >= bytes) break;
            if (b == pos) {
                search = b + 1;
                continue;
            }
        }
        mrb_ary_push(mrb, parts, mrb_str_byte_subseq(mrb, s, pos, b - pos));
        for (uint32_t g = 1; g <= re->groups; ++g) {
            mrb_int gb, ge;
            if (group(rc, g, gb, ge)) mrb_ary_push(mrb, parts, mrb_str_byte_subseq(mrb, s, gb, ge - gb));
        }
        mrb_gc_arena_restore(mrb, ai);
        pos = e;
        search = e == b ? e + 1 : e;
    }
    mrb_ary_push(mrb, parts, mrb_str_byte_subseq(mrb, s, pos, bytes - pos));
    if (limit == 0) {
        mrb_int n = RARRAY_LEN(parts);
        while (n > 0 && RSTRING_LEN(mrb_ary_ref(mrb, parts, n - 1)) == 0) --n;
        mrb_ary_resize(mrb, parts, n);
    }
    return parts;
}

// The number of the group called name[0, n) (the last one, as MatchData#[] finds it), or -1.
int name_number(Re* re, const char* name, size_t n) {
    uint32_t count, size;
    PCRE2_SPTR table;
    pcre2_pattern_info(re->code, PCRE2_INFO_NAMECOUNT, &count);
    pcre2_pattern_info(re->code, PCRE2_INFO_NAMEENTRYSIZE, &size);
    pcre2_pattern_info(re->code, PCRE2_INFO_NAMETABLE, &table);
    int found = -1;
    for (uint32_t k = 0; k < count; ++k) {
        PCRE2_SPTR e = table + k * size;
        const char* en = reinterpret_cast<const char*>(e + 2);
        if (std::strlen(en) == n && !std::memcmp(en, name, n)) found = (e[0] << 8) | e[1];
    }
    return found;
}

// Appends group g of the last match to out (nothing when it took no part).
void cat_group(mrb_state* mrb, Re* re, int rc, mrb_value s, mrb_int g, mrb_value out) {
    mrb_int b, e;
    if (g >= 0 && g <= static_cast<mrb_int>(re->groups) && group(rc, uint32_t(g), b, e)) mrb_str_cat(mrb, out, RSTRING_PTR(s) + b, size_t(e - b));
}

// The replacement text of sub/gsub for the last match: \0..\9, \&, \\ and \k<name>; any other
// byte as written (String#__rsh_expand in 0.1.3, the same rules).
void expand(mrb_state* mrb, Re* re, int rc, mrb_value s, mrb_value rep, mrb_value out) {
    const char* r = RSTRING_PTR(rep);
    mrb_int n = RSTRING_LEN(rep), i = 0, lit = 0;
    while (i < n) {
        if (r[i] != '\\' || i + 1 >= n) {
            ++i;
            continue;
        }
        char d = r[i + 1];
        mrb_int skip = 0;
        if (d >= '0' && d <= '9') {
            mrb_str_cat(mrb, out, r + lit, size_t(i - lit));
            cat_group(mrb, re, rc, s, d - '0', out);
            skip = 2;
        } else if (d == '&') {
            mrb_str_cat(mrb, out, r + lit, size_t(i - lit));
            cat_group(mrb, re, rc, s, 0, out);
            skip = 2;
        } else if (d == '\\') {
            mrb_str_cat(mrb, out, r + lit, size_t(i - lit));
            mrb_str_cat_lit(mrb, out, "\\");
            skip = 2;
        } else if (d == 'k' && i + 2 < n && r[i + 2] == '<') {
            const void* gt = std::memchr(r + i + 3, '>', size_t(n - i - 3));
            if (gt) {
                mrb_int e = static_cast<const char*>(gt) - r;
                int g = name_number(re, r + i + 3, size_t(e - i - 3));
                if (g < 0) {
                    mrb_value m = mrb_str_new_lit(mrb, "undefined group name reference: ");
                    mrb_str_cat(mrb, m, r + i + 3, size_t(e - i - 3));
                    raise_with(mrb, E_INDEX_ERROR, m);
                }
                mrb_str_cat(mrb, out, r + lit, size_t(i - lit));
                cat_group(mrb, re, rc, s, g, out);
                skip = e + 1 - i;
            }
        }
        if (skip) {
            i += skip;
            lit = i;
        } else ++i;
    }
    mrb_str_cat(mrb, out, r + lit, size_t(n - lit));
}

// __sub(str, rep, global) -> the new String (sub/gsub with a String replacement); $~ the last
// match, nil when there is none.
mrb_value m_re_sub(mrb_state* mrb, mrb_value self) {
    mrb_value s, rep;
    mrb_bool global;
    mrb_get_args(mrb, "SSb", &s, &rep, &global);
    Re* re = get_re(mrb, self);
    int kind = subject_kind(mrb, self, re, s);
    const char* p = RSTRING_PTR(s);
    mrb_int pos = 0, bytes = RSTRING_LEN(s), lastpos = -1;
    mrb_value out = mrb_str_new_capa(mrb, size_t(bytes));
    bool plain = !std::memchr(RSTRING_PTR(rep), '\\', size_t(RSTRING_LEN(rep)));
    while (pos <= bytes) {
        int rc = re_exec(mrb, self, re, s, kind, pos);
        if (!rc) break;
        lastpos = pos;
        mrb_int b, e;
        group(rc, 0, b, e);
        mrb_str_cat(mrb, out, p + pos, size_t(b - pos));
        if (plain) mrb_str_cat_str(mrb, out, rep);
        else expand(mrb, re, rc, s, rep, out);
        if (e == b) {
            if (e < bytes) mrb_str_cat(mrb, out, p + e, 1);
            pos = e + 1;
        } else pos = e;
        if (!global) break;
    }
    if (pos < bytes) mrb_str_cat(mrb, out, p + pos, size_t(bytes - pos));
    mrb_value md = mrb_nil_value();
    if (lastpos >= 0) md = make_md(mrb, self, re, re_exec(mrb, self, re, s, kind, lastpos), s);
    set_last(mrb, md);
    return out;
}

mrb_value names_of(mrb_state* mrb, Re* re, const char* want, mrb_int* index) {
    uint32_t count, size;
    PCRE2_SPTR table;
    pcre2_pattern_info(re->code, PCRE2_INFO_NAMECOUNT, &count);
    pcre2_pattern_info(re->code, PCRE2_INFO_NAMEENTRYSIZE, &size);
    pcre2_pattern_info(re->code, PCRE2_INFO_NAMETABLE, &table);
    mrb_value r = mrb_ary_new(mrb);
    for (uint32_t k = 0; k < count; ++k) {
        PCRE2_SPTR e = table + k * size;
        int n = (e[0] << 8) | e[1];
        const char* name = reinterpret_cast<const char*>(e + 2);
        if (want && !std::strcmp(want, name)) *index = n;
        mrb_ary_push(mrb, r, mrb_str_new_cstr(mrb, name));
    }
    return r;
}

mrb_value m_re_names(mrb_state* mrb, mrb_value self) { return names_of(mrb, get_re(mrb, self), nullptr, nullptr); }

mrb_value m_re_name_index(mrb_state* mrb, mrb_value self) {
    const char* n;
    mrb_get_args(mrb, "z", &n);
    mrb_int idx = -1;
    names_of(mrb, get_re(mrb, self), n, &idx);
    return idx < 0 ? mrb_nil_value() : mrb_fixnum_value(idx);
}

mrb_value m_re_source(mrb_state* mrb, mrb_value self) { return mrb_iv_get(mrb, self, names(mrb).source); }
mrb_value m_re_options(mrb_state* mrb, mrb_value self) { return mrb_iv_get(mrb, self, names(mrb).options); }

// ---- JSON ---------------------------------------------------------------------------------------

struct JsonReader {
    mrb_state* mrb;
    const char* p;
    const char* end;
    bool sym;
    std::string err;
    int depth = 0;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool fail(const char* m) {
        if (err.empty()) err = m;
        return false;
    }
    static void utf8(std::string& o, uint32_t c) {
        if (c < 0x80) o += char(c);
        else if (c < 0x800) o += char(0xC0 | (c >> 6)), o += char(0x80 | (c & 0x3F));
        else if (c < 0x10000) o += char(0xE0 | (c >> 12)), o += char(0x80 | ((c >> 6) & 0x3F)), o += char(0x80 | (c & 0x3F));
        else {
            o += char(0xF0 | (c >> 18));
            o += char(0x80 | ((c >> 12) & 0x3F));
            o += char(0x80 | ((c >> 6) & 0x3F));
            o += char(0x80 | (c & 0x3F));
        }
    }
    bool hex4(uint32_t& v) {
        if (end - p < 4) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& o) {
        ++p;
        while (p < end && *p != '"') {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c < 0x20) return fail("control character in a string");
            if (c != '\\') {
                o += char(c);
                ++p;
                continue;
            }
            if (++p >= end) return fail("unfinished escape");
            char e = *p++;
            switch (e) {
                case '"': o += '"'; break;
                case '\\': o += '\\'; break;
                case '/': o += '/'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'u': {
                    uint32_t v;
                    if (!hex4(v)) return fail("bad \\u escape");
                    if (v >= 0xD800 && v < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        uint32_t lo;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return fail("bad surrogate pair");
                        v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(o, v);
                    break;
                }
                default: return fail("unknown escape");
            }
        }
        if (p >= end) return fail("unfinished string");
        ++p;
        return true;
    }
    bool value(mrb_value& out) {
        ws();
        if (p >= end) return fail("unexpected end of text");
        if (++depth > 512) return fail("nested too deep");
        bool ok = true;
        char c = *p;
        if (c == '{') {
            ++p;
            out = mrb_hash_new(mrb);
            ws();
            if (p < end && *p == '}') ++p;
            else
                for (;;) {
                    ws();
                    if (p >= end || *p != '"') {
                        ok = fail("expected a key");
                        break;
                    }
                    std::string k;
                    if (!string(k)) {
                        ok = false;
                        break;
                    }
                    ws();
                    if (p >= end || *p != ':') {
                        ok = fail("expected :");
                        break;
                    }
                    ++p;
                    mrb_value v;
                    if (!value(v)) {
                        ok = false;
                        break;
                    }
                    mrb_value key = sym ? mrb_symbol_value(mrb_intern(mrb, k.data(), k.size())) : str(mrb, k);
                    mrb_hash_set(mrb, out, key, v);
                    ws();
                    if (p < end && *p == ',') {
                        ++p;
                        continue;
                    }
                    if (p < end && *p == '}') {
                        ++p;
                        break;
                    }
                    ok = fail("expected , or }");
                    break;
                }
        } else if (c == '[') {
            ++p;
            out = mrb_ary_new(mrb);
            ws();
            if (p < end && *p == ']') ++p;
            else
                for (;;) {
                    mrb_value v;
                    if (!value(v)) {
                        ok = false;
                        break;
                    }
                    mrb_ary_push(mrb, out, v);
                    ws();
                    if (p < end && *p == ',') {
                        ++p;
                        continue;
                    }
                    if (p < end && *p == ']') {
                        ++p;
                        break;
                    }
                    ok = fail("expected , or ]");
                    break;
                }
        } else if (c == '"') {
            std::string s;
            ok = string(s);
            if (ok) out = str(mrb, s);
        } else if (end - p >= 4 && !std::strncmp(p, "true", 4)) out = mrb_true_value(), p += 4;
        else if (end - p >= 5 && !std::strncmp(p, "false", 5)) out = mrb_false_value(), p += 5;
        else if (end - p >= 4 && !std::strncmp(p, "null", 4)) out = mrb_nil_value(), p += 4;
        else if (c == '-' || (c >= '0' && c <= '9')) {
            const char* b = p;
            bool real = false;
            if (*p == '-') ++p;
            while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) {
                if (*p == '.' || *p == 'e' || *p == 'E') real = true;
                ++p;
            }
            std::string num(b, size_t(p - b));
            if (real) out = mrb_float_value(mrb, std::strtod(num.c_str(), nullptr));
            else {
                errno = 0;
                long long v = std::strtoll(num.c_str(), nullptr, 10);
                out = errno ? mrb_float_value(mrb, std::strtod(num.c_str(), nullptr)) : mrb_int_value(mrb, (mrb_int)v);
            }
        } else ok = fail("unexpected character");
        --depth;
        return ok;
    }
};

mrb_value m_json_parse(mrb_state* mrb, mrb_value) {
    mrb_value s;
    mrb_bool sym;
    mrb_get_args(mrb, "Sb", &s, &sym);
    mrb_value out = mrb_nil_value(), err = mrb_nil_value();
    {
        JsonReader r{mrb, RSTRING_PTR(s), RSTRING_PTR(s) + RSTRING_LEN(s), bool(sym), {}};
        if (r.value(out)) {
            r.ws();
            if (r.p != r.end) r.fail("text after the value");
        }
        if (!r.err.empty())
            err = str(mrb, r.err + " at byte " + std::to_string(r.p - RSTRING_PTR(s)));
    }
    if (!mrb_nil_p(err)) {
        RClass* json = mrb_module_get(mrb, "JSON");
        raise_with(mrb, mrb_class_get_under(mrb, json, "ParserError"), err);
    }
    return out;
}

mrb_value m_json_str(mrb_state* mrb, mrb_value) {
    mrb_value s;
    mrb_get_args(mrb, "S", &s);
    mrb_value r;
    {
        std::string o = "\"";
        const char* p = RSTRING_PTR(s);
        for (mrb_int k = 0; k < RSTRING_LEN(s); ++k) {
            unsigned char c = static_cast<unsigned char>(p[k]);
            switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                case '\b': o += "\\b"; break;
                case '\f': o += "\\f"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof buf, "\\u%04x", c);
                        o += buf;
                    } else o += char(c);
            }
        }
        o += '"';
        r = str(mrb, o);
    }
    return r;
}

// ---- errors -------------------------------------------------------------------------------------

// "file:line" of the first place in the person's code (the built-in part has no file name).
std::string where(mrb_state* mrb, mrb_value exc) {
    mrb_value bt = mrb_funcall(mrb, exc, "backtrace", 0);
    if (mrb->exc) mrb->exc = nullptr;
    if (!mrb_array_p(bt)) return "";
    for (mrb_int k = 0; k < RARRAY_LEN(bt); ++k) {
        mrb_value e = mrb_ary_ref(mrb, bt, k);
        if (!mrb_string_p(e)) continue;
        std::string s = cxx(e);
        if (s.empty() || s[0] == '(') continue;  // the built-in part; a script read from standard input is "-" (0.9.0)
        size_t in = s.find(":in ");
        if (in != std::string::npos) s = s.substr(0, in);
        return s;
    }
    return "";
}

// Prints an exception the script did not handle; returns the exit status for it.
int report(mrb_state* mrb, mrb_value exc) {
    RObject* o = mrb_obj_ptr(exc);
    if (MRB_EXC_EXIT_P(o)) {
        mrb_value st = mrb_iv_get(mrb, exc, mrb_intern_lit(mrb, "status"));
        return mrb_integer_p(st) ? int(mrb_integer(st)) : 0;
    }
    if (mrb_obj_is_kind_of(mrb, exc, mrb_class_get(mrb, "Interrupt"))) return 130;
    mrb_value msg = mrb_funcall(mrb, exc, "message", 0);
    if (mrb->exc) mrb->exc = nullptr;
    std::string m = mrb_string_p(msg) ? cxx(msg) : "error";
    std::string at = where(mrb, exc);
    RClass* failed = mrb_class_get_under(mrb, rsh_mod(mrb), "Failed");
    if (mrb_obj_is_kind_of(mrb, exc, failed)) {
        mrb_value st = mrb_funcall(mrb, exc, "status", 0);
        mrb_value code = mrb_funcall(mrb, st, "exitstatus", 0);
        std::fprintf(stderr, "rsh: %s%s%s\n", at.c_str(), at.empty() ? "" : ": ", m.c_str());
        int c = mrb_integer_p(code) ? int(mrb_integer(code)) : 1;
        return c ? c : 1;
    }
    const char* cls = mrb_obj_classname(mrb, exc);
    if (m.compare(0, 5, "line ") == 0 && !std::strcmp(cls, "SyntaxError")) at.clear();
    std::fprintf(stderr, "rsh: %s%s%s (%s)\n", at.c_str(), at.empty() ? "" : ": ", m.c_str(), cls);
    return 1;
}

mrb_ccontext* repl_cxt = nullptr;

}  // namespace

// Does this file get the shell layer? Every file does (`.rsh`, `.rb`, no ending, -c, standard
// input) unless its first line, or the line after a `#!` line, is the comment `# rsh: ruby`: then
// its text goes to the interpreter unchanged. `# rsh: shell` there says the default out loud.
bool wants_shell_layer(const std::string&, const std::string& src) {
    size_t b = 0;
    for (int n = 0; n < 2 && b < src.size(); ++n) {
        size_t e = src.find('\n', b);
        if (e == std::string::npos) e = src.size();
        std::string line = src.substr(b, e - b);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (n == 0 && line.compare(0, 2, "#!") == 0) {
            b = e + 1;
            continue;
        }
        if (line.compare(0, 1, "#") != 0) return true;
        size_t k = 1;
        while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
        if (line.compare(k, 4, "rsh:") != 0) return true;
        k += 4;
        while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
        return line.compare(k, std::string::npos, "ruby") != 0;
    }
    return true;
}

mrb_state* rt_open(const std::string& script, const std::vector<std::string>& args) {
    mrb_state* mrb = mrb_open();
    if (!mrb) return nullptr;
    host.mrb = mrb;
    host.argv = args;
    if (shell().start_dir.empty()) shell().start_dir = current_dir();
    RClass* r = mrb_define_module(mrb, "Rsh");
    mrb_define_module_function(mrb, r, "__exec", m_exec, MRB_ARGS_REQ(3));
    mrb_define_module_function(mrb, r, "__argv", m_argv, MRB_ARGS_REQ(2));
    mrb_define_module_function(mrb, r, "__set_status", m_set_status, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, r, "__set_match", m_set_match, MRB_ARGS_REQ(2));
    mrb_define_module_function(mrb, r, "__chdir", m_chdir, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, r, "__nullglob", m_nullglob, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, r, "__getenv", m_getenv, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, r, "__setenv", m_setenv, MRB_ARGS_REQ(2));
    mrb_define_module_function(mrb, r, "__env_hash", m_env_hash, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, r, "__pid", m_pid, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, r, "__nproc", m_nproc, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, r, "__kill", m_kill, MRB_ARGS_REQ(2));
    mrb_define_module_function(mrb, r, "__glob", m_glob, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, r, "__require", m_require, MRB_ARGS_REQ(2));
    mrb_define_module_function(mrb, r, "__json_parse", m_json_parse, MRB_ARGS_REQ(2));
    mrb_define_module_function(mrb, r, "__json_str", m_json_str, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, r, "__translate", m_translate, MRB_ARGS_REQ(4));
    mrb_define_module_function(mrb, r, "__load", m_load, MRB_ARGS_REQ(3));
    mrb_define_module_function(mrb, r, "__declare", m_declare, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, r, "__regexp", m_regexp, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, r, "__home", m_home, MRB_ARGS_REQ(1));
    RClass* file = mrb_class_get(mrb, "File");
    mrb_define_class_method(mrb, file, "mtime", m_file_mtime, MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, file, "readable?", m_file_access<R_OK>, MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, file, "writable?", m_file_access<W_OK>, MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, file, "executable?", m_file_access<X_OK>, MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, file, "owned?", m_file_owned, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, r, "__stat", m_stat, MRB_ARGS_REQ(2));
    mrb_define_module_function(mrb, r, "last_pid", m_last_pid, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, r, "__touch", m_touch, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, r, "__copy_file", m_copy_file, MRB_ARGS_REQ(2));
    mrb_define_module_function(mrb, r, "__rm_tree", m_rm_tree, MRB_ARGS_REQ(1));
    RClass* proc_mod = mrb_define_module(mrb, "Process");
    mrb_define_module_function(mrb, proc_mod, "uid", m_uid, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, proc_mod, "euid", m_euid, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, proc_mod, "gid", m_gid, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, proc_mod, "egid", m_egid, MRB_ARGS_NONE());
    re_names.owner = nullptr;  // a new interpreter: its own symbols
    RClass* re = mrb_define_class(mrb, "Regexp", mrb->object_class);
    MRB_SET_INSTANCE_TT(re, MRB_TT_CDATA);
    mrb_define_method(mrb, re, "initialize", m_re_init, MRB_ARGS_ARG(1, 1));
    mrb_define_method(mrb, re, "__match", m_re_match, MRB_ARGS_REQ(2));
    mrb_define_method(mrb, re, "__test", m_re_test, MRB_ARGS_REQ(2));
    mrb_define_method(mrb, re, "__md", m_re_md, MRB_ARGS_REQ(3));
    mrb_define_method(mrb, re, "__eqtilde", m_re_eqtilde, MRB_ARGS_REQ(1));
    mrb_define_method(mrb, re, "__scan", m_re_scan, MRB_ARGS_REQ(1));
    mrb_define_method(mrb, re, "__split", m_re_split, MRB_ARGS_REQ(2));
    mrb_define_method(mrb, re, "__sub", m_re_sub, MRB_ARGS_REQ(3));
    mrb_define_class_method(mrb, re, "__set_last", m_re_set_last, MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, re, "__chars", m_re_chars, MRB_ARGS_REQ(2));
    mrb_define_method(mrb, re, "names", m_re_names, MRB_ARGS_NONE());
    mrb_define_method(mrb, re, "__name_index", m_re_name_index, MRB_ARGS_REQ(1));
    mrb_define_method(mrb, re, "source", m_re_source, MRB_ARGS_NONE());
    mrb_define_method(mrb, re, "options", m_re_options, MRB_ARGS_NONE());
    mrb_load_irep(mrb, rsh_prelude);
    if (mrb->exc) {
        mrb_print_error(mrb);
        return nullptr;
    }
    RClass* io = mrb_class_get(mrb, "IO");
    mrb_define_class_method(mrb, io, "popen", m_no_shell, MRB_ARGS_ANY());
    mrb_define_class_method(mrb, io, "_popen", m_no_shell, MRB_ARGS_ANY());
    mrb_value av = mrb_ary_new(mrb);
    for (auto& a : args) mrb_ary_push(mrb, av, str(mrb, a));
    mrb_define_global_const(mrb, "ARGV", av);
    mrb_gv_set(mrb, mrb_intern_lit(mrb, "$0"), str(mrb, script));
    mrb_gv_set(mrb, mrb_intern_lit(mrb, "$PROGRAM_NAME"), str(mrb, script));  // L-5
    mrb_gv_set(mrb, mrb_intern_lit(mrb, "$$"), mrb_int_value(mrb, (mrb_int)getpid()));  // 1.0, audit 2 L6: rsh's pid, as sh's $$ (a forked stage keeps it; Process.pid is the live one)
    mrb_gv_set(mrb, mrb_intern_lit(mrb, "$*"), av);
    return mrb;
}

void rt_close(mrb_state* mrb) {
    if (repl_cxt) mrb_ccontext_free(mrb, repl_cxt);
    repl_cxt = nullptr;
    mrb_close(mrb);
}

int rt_run(mrb_state* mrb, const std::string& src, const std::string& file, bool rsh_syntax) {
    bool syn;
    load_text(mrb, src, file, rsh_syntax, syn);
    if (syn) return 2;
    if (mrb->exc) {
        mrb_value e = mrb_obj_value(mrb->exc);
        mrb->exc = nullptr;
        if (!std::strcmp(mrb_obj_classname(mrb, e), "SyntaxError")) {
            // the compiler says "line N: ...": put the file in front, like every other message
            mrb_value msg = mrb_funcall(mrb, e, "message", 0);
            std::string m = mrb_string_p(msg) ? cxx(msg) : "syntax error";
            // Rsh.eval's errors name their own text, "(rsh eval):1: ..." (audit L-9)
            if (m.compare(0, 5, "line ") == 0)
                std::fprintf(stderr, "rsh: %s:%s%s\n", file.c_str(), m.c_str() + 5, ruby_word_hint(src, std::atoi(m.c_str() + 5)).c_str());
            else std::fprintf(stderr, "rsh: %s\n", m.c_str());
            return 2;
        }
        return report(mrb, e);
    }
    return 0;
}

int rt_eval(mrb_state* mrb, const std::string& src, Known& known, bool& exited, bool& incomplete) {
    exited = false;
    incomplete = false;
    mrb_value code = mrb_nil_value();
    bool commands = false;
    std::string ruby_text;
    size_t first = table().size(), after = 0;
    {
        Known trial = known;
        size_t before = table().size();
        Result r = transpile(src, "(rsh)", trial);
        if (!r.ok()) {
            bool open = false;
            for (auto& d : r.diags)
                if (d.msg.find("not closed") != std::string::npos || d.msg.find("without its end word") != std::string::npos ||
                    d.msg.find("without a closing") != std::string::npos)
                    open = true;
            table().resize(before);
            if (open) {
                incomplete = true;
                return 0;
            }
            for (auto& d : r.diags)
                if (!d.warning) std::fprintf(stderr, "rsh: %s\n", d.msg.c_str());
            return 2;
        }
        // Does the Ruby parse? An unfinished block or string asks for another line.
        mrb_ccontext* c = mrb_ccontext_new(mrb);
        c->capture_errors = TRUE;
        c->no_exec = TRUE;
        mrb_parser_state* p = mrb_parse_nstring(mrb, r.ruby.data(), r.ruby.size(), c);
        if (p && p->nerr) {
            std::string m = p->error_buffer[0].message ? p->error_buffer[0].message : "";
            if (m.find("end of file") != std::string::npos || m.find("$end") != std::string::npos ||
                m.find("unterminated") != std::string::npos)
                incomplete = true;
        }
        if (p) mrb_parser_free(p);
        mrb_ccontext_free(mrb, c);
        if (incomplete) {
            table().resize(before);
            return 0;
        }
        known = trial;
        script_methods().insert(known.methods.begin(), known.methods.end());
        ruby_text = std::move(r.ruby);
        after = table().size();
        commands = !r.marks.empty();
    }
    // The arena is saved before the code string is made (0.9.0): made after the save, each input
    // left its string and an arena slot behind, about 250 bytes a line in a long session.
    int ai = mrb_gc_arena_save(mrb);
    code = str(mrb, ruby_text);
    if (!repl_cxt) {
        repl_cxt = mrb_ccontext_new(mrb);
        mrb_ccontext_filename(mrb, repl_cxt, "(rsh)");
        repl_cxt->capture_errors = TRUE;
    }
    mrb_value v = mrb_load_nstring_cxt(mrb, RSTRING_PTR(code), RSTRING_LEN(code), repl_cxt);
    int status = 0;
    if (mrb->exc) {
        mrb_value e = mrb_obj_value(mrb->exc);
        mrb->exc = nullptr;
        if (MRB_EXC_EXIT_P(mrb_obj_ptr(e))) exited = true;
        status = report(mrb, e);
    } else if (!commands && !mrb_nil_p(v) && !mrb_undef_p(v)) {
        mrb_value s = mrb_inspect(mrb, v);
        if (mrb->exc) mrb->exc = nullptr;
        else if (mrb_string_p(s)) std::printf("=> %s\n", cxx(s).c_str());
        std::fflush(stdout);
    } else if (commands) status = shell().last_status;
    mrb_gc_arena_restore(mrb, ai);
    // The command lines of this input are not needed once it ran, unless code that outlives it
    // can hold them (a method, a block, a proc) or the run read more lines (source, Rsh.eval). They
    // were kept for ever: about 600 bytes per command typed (0.9.0, the 10,000-command check).
    bool may_keep = false;
    for (const char* w : {"def", "do", "->", "proc", "lambda", "method", "class", "module", "Struct", "Data"})
        may_keep = may_keep || ruby_text.find(w) != std::string::npos;
    for (size_t k = ruby_text.find('{'); !may_keep && k != std::string::npos; k = ruby_text.find('{', k + 1))
        may_keep = k == 0 || ruby_text[k - 1] != '#';  // a block or a Hash; #{...} is a value
    if (!may_keep && table().size() == after) table().resize(first);
    return status;
}

int rt_check(mrb_state* mrb, const std::string& src, const std::string& file, bool lint, bool rsh_syntax) {
    int bad = 0;
    size_t before = table().size();
    Known k;
    Result r;
    if (rsh_syntax) {
        collect_defs(src, script_dir(file), k.methods);
        r = transpile(src, file, k);
    } else r.ruby = src;
    for (auto& d : r.diags) {
        std::fprintf(stderr, "%s:%d: %s%s\n", file.c_str(), d.line, d.warning ? "warning: " : "", d.msg.c_str());
        if (!d.warning) bad = 2;
    }
    if (bad) return bad;
    mrb_ccontext* c = mrb_ccontext_new(mrb);
    mrb_ccontext_filename(mrb, c, file.c_str());
    c->capture_errors = TRUE;
    c->no_exec = TRUE;
    mrb_parser_state* p = mrb_parse_nstring(mrb, r.ruby.data(), r.ruby.size(), c);
    if (!p) bad = 2;
    else {
        for (size_t e = 0; e < p->nerr && e < 10; ++e) {
            std::fprintf(stderr, "%s:%d:%d: %s%s\n", file.c_str(), p->error_buffer[e].lineno, p->error_buffer[e].column,
                         p->error_buffer[e].message ? p->error_buffer[e].message : "syntax error",
                         ruby_word_hint(src, p->error_buffer[e].lineno).c_str());
            bad = 2;
        }
        mrb_parser_free(p);
    }
    mrb_ccontext_free(mrb, c);
    if (bad || !lint) return bad;
    // Warnings: words taken as commands that are not on PATH, and risky forms.
    int warn = 0;
    for (auto& d : r.diags) warn += d.warning;  // the reader's own (printed above), 0.1.4
    for (size_t li = before; li < table().size(); ++li) {
        const Line& L = table()[li];
        for (auto& it : L.items)
            for (auto& cmd : it.pipeline) {
                if (cmd.words.empty()) continue;
                const Word& w0 = cmd.words[0];
                std::string name;
                bool plain = true;
                for (auto& part : w0.parts) {
                    if (part.k != Part::Pat && part.k != Part::Lit) plain = false;
                    else name += part.s;
                }
                if (plain && !name.empty() && !is_builtin(name) && find_program(name).empty() && name[0] != '.' &&
                    name[0] != '/' && name[0] != '~') {
                    std::fprintf(stderr, "%s:%d: warning: `%s` is taken as a command and is not on PATH; if it is a Ruby method, write %s(...) or declare it with rsh_methods :%s\n",
                                 file.c_str(), L.line, name.c_str(), name.c_str(), name.c_str());
                    ++warn;
                }
                // A value as the first argument of a program: "-rf" in it is an option (audit M2).
                if (plain && !name.empty() && !is_builtin(name) && cmd.words.size() > 1 && !cmd.words[1].parts.empty() &&
                    (cmd.words[1].parts[0].k == Part::Val || cmd.words[1].parts[0].k == Part::Env)) {
                    std::fprintf(stderr, "%s:%d: warning: a value is the first argument of %s; if it starts with - it is read as an option: write %s -- #{...}\n",
                                 file.c_str(), L.line, name.c_str(), name.c_str());
                    ++warn;
                }
                // A fixed name under /tmp as the file a redirect writes (echo > /tmp/x, audit L-15).
                for (auto& rd : cmd.redirs) {
                    if (rd.k != Redir::Out && rd.k != Redir::Append && rd.k != Redir::Both && rd.k != Redir::BothAppend) continue;
                    std::string lit;
                    for (auto& part : rd.target.parts)
                        if (part.k == Part::Lit || part.k == Part::Pat) lit += part.s;
                    if (lit.compare(0, 5, "/tmp/") == 0 && lit.size() > 5) {
                        std::fprintf(stderr, "%s:%d: warning: a fixed name under /tmp; use mktemp\n", file.c_str(), L.line);
                        ++warn;
                    }
                }
                bool shell_c = (name == "sh" || name == "bash" || name == "zsh") && cmd.words.size() > 1;
                for (size_t k2 = 1; k2 < cmd.words.size(); ++k2) {
                    bool has_val = false;
                    std::string lit;
                    for (auto& part : cmd.words[k2].parts) {
                        if (part.k == Part::Val || part.k == Part::Env) has_val = true;
                        else lit += part.s;
                    }
                    if (shell_c && has_val) {
                        std::fprintf(stderr, "%s:%d: warning: a Ruby value inside %s -c text is parsed again by that shell; pass it as an argument\n",
                                     file.c_str(), L.line, name.c_str());
                        ++warn;
                    }
                    if (name == "chmod" && (lit == "777" || lit == "666" || lit.find("o+w") != std::string::npos ||
                                            lit.find("a+w") != std::string::npos)) {
                        std::fprintf(stderr, "%s:%d: warning: chmod %s lets everyone write\n", file.c_str(), L.line, lit.c_str());
                        ++warn;
                    }
                    if (lit.compare(0, 5, "/tmp/") == 0 && lit.size() > 5) {
                        std::fprintf(stderr, "%s:%d: warning: a fixed name under /tmp; use mktemp\n", file.c_str(), L.line);
                        ++warn;
                    }
                }
            }
    }
    return warn ? 1 : 0;
}

bool rt_prompt(mrb_state* mrb, std::string& out) {
    mrb_value self = mrb_top_self(mrb);
    if (!mrb_respond_to(mrb, self, mrb_intern_lit(mrb, "prompt"))) return false;
    mrb_value v = mrb_funcall(mrb, self, "prompt", 0);
    if (mrb->exc) {
        mrb->exc = nullptr;
        return false;
    }
    if (!mrb_string_p(v)) return false;
    out = cxx(v);
    return true;
}

void rt_add_history(const std::string& line) {
    host.hist.push_back(line);
    if (host.hist.size() > 6000) host.hist.erase(host.hist.begin(), host.hist.begin() + 1000);  // as Editor::add
}

}  // namespace rsh
