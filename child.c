// child.c (part of mintty)
// Copyright 2008-11 Andy Koppe
// Licensed under the terms of the GNU General Public License v3 or later.

#include "child.h"

#include "term.h"
#include "charset.h"

#include <pwd.h>
#include <fcntl.h>
#include <utmp.h>
#include <dirent.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/cygwin.h>

#if CYGWIN_VERSION_API_MINOR >= 93
#include <pty.h>
#else
int forkpty(int *, char *, struct termios *, struct winsize *);
#endif

#include <winbase.h>

#if CYGWIN_VERSION_DLL_MAJOR < 1007
#include <winnls.h>
#include <wincon.h>
#include <wingdi.h>
#include <winuser.h>
#endif

char *home, *cmd;

static pid_t pid;
static bool killed;
static int pty_fd = -1, log_fd = -1, win_fd;

static void
error(char *action)
{
  char *msg;
  int len = asprintf(&msg, "Failed to %s: %s.", action, strerror(errno));
  if (len > 0) {
    term_write(msg, len);
    free(msg);
  }
}

static void
sigexit(int sig)
{
  if (pid)
    kill(-pid, SIGHUP);
  signal(sig, SIG_DFL);
  kill(getpid(), sig);
}

void
child_create(char *argv[], struct winsize *winp)
{
  string lang = cs_lang();

  // xterm and urxvt ignore SIGHUP, so let's do the same.
  signal(SIGHUP, SIG_IGN);
  
  signal(SIGINT, sigexit);
  signal(SIGTERM, sigexit);
  signal(SIGQUIT, sigexit);
  
  // Create the child process and pseudo terminal.
  pid = forkpty(&pty_fd, 0, 0, winp);
  if (pid < 0) {
    pid = 0;
    bool rebase_prompt = (errno == EAGAIN);
    error("fork child process");
    if (rebase_prompt) {
      static const char msg[] =
        "\r\nDLL rebasing may be required. See 'rebaseall --help'.";
      term_write(msg, sizeof msg - 1);
    }
    term_hide_cursor();
  }
  else if (!pid) { // Child process.
#if CYGWIN_VERSION_DLL_MAJOR < 1007
    // Some native console programs require a console to be attached to the
    // process, otherwise they pop one up themselves, which is rather annoying.
    // Cygwin's exec function from 1.5 onwards automatically allocates a console
    // on an invisible window station if necessary. Unfortunately that trick no
    // longer works on Windows 7, which is why Cygwin 1.7 contains a new hack
    // for creating the invisible console.
    // On Cygwin versions before 1.5 and on Cygwin 1.5 running on Windows 7,
    // we need to create the invisible console ourselves. The hack here is not
    // as clever as Cygwin's, with the console briefly flashing up on startup,
    // but it'll do.
#if CYGWIN_VERSION_DLL_MAJOR == 1005
    DWORD win_version = GetVersion();
    win_version = ((win_version & 0xff) << 8) | ((win_version >> 8) & 0xff);
    if (win_version >= 0x0601)  // Windows 7 is NT 6.1.
#endif
      if (AllocConsole()) {
        HMODULE kernel = GetModuleHandle("kernel32");
        HWND (WINAPI *pGetConsoleWindow)(void) =
          (void *)GetProcAddress(kernel, "GetConsoleWindow");
        ShowWindowAsync(pGetConsoleWindow(), SW_HIDE);
      }
#endif

    // Reset signals
    signal(SIGHUP, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);

    // Mimick login's behavior by disabling the job control signals
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    setenv("TERM", cfg.term, true);
    
    if (lang) {
      unsetenv("LC_ALL");
      unsetenv("LC_COLLATE");
      unsetenv("LC_CTYPE");
      unsetenv("LC_MONETARY");
      unsetenv("LC_NUMERIC");
      unsetenv("LC_TIME");
      unsetenv("LC_MESSAGES");
      setenv("LANG", lang, true);
    }

    // Terminal line settings
    struct termios attr;
    tcgetattr(0, &attr);
    attr.c_cc[VERASE] = cfg.backspace_sends_bs ? CTRL('H') : CDEL;
    attr.c_iflag |= IXANY | IMAXBEL;
    attr.c_lflag |= ECHOE | ECHOK | ECHOCTL | ECHOKE;
    tcsetattr(0, TCSANOW, &attr);
    
    // Invoke command
    execvp(cmd, argv);

    // If we get here, exec failed.
    fprintf(stderr, "%s: %s\r\n", cmd, strerror(errno));

#if CYGWIN_VERSION_DLL_MAJOR < 1005
    // Before Cygwin 1.5, the message above doesn't appear if we exit
    // immediately. So have a little nap first.
    usleep(200000);
#endif

    exit(255);
  }
  else { // Parent process.
    fcntl(pty_fd, F_SETFL, O_NONBLOCK);
    
    if (cfg.utmp) {
      char *dev = ptsname(pty_fd);
      if (dev) {
        struct utmp ut;
        memset(&ut, 0, sizeof ut);

        if (!strncmp(dev, "/dev/", 5))
          dev += 5;
        strlcpy(ut.ut_line, dev, sizeof ut.ut_line);

        if (dev[1] == 't' && dev[2] == 'y')
          dev += 3;
        else if (!strncmp(dev, "pts/", 4))
          dev += 4;
        strncpy(ut.ut_id, dev, sizeof ut.ut_id);

        ut.ut_type = USER_PROCESS;
        ut.ut_pid = pid;
        ut.ut_time = time(0);
        strlcpy(ut.ut_user, getlogin() ?: "?", sizeof ut.ut_user);
        gethostname(ut.ut_host, sizeof ut.ut_host);
        login(&ut);
      }
    }
  }

  win_fd = open("/dev/windows", O_RDONLY);

  // Open log file if any
  if (*cfg.log) {
    if (!strcmp(cfg.log, "-"))
      log_fd = fileno(stdout);
    else {
      log_fd = open(cfg.log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (log_fd < 0)
        error("open log file");
    }
  }
}

void
child_proc(void)
{
  for (;;) {
    if (term.paste_buffer)
      term_send_paste();

    struct timeval timeout = {0, 100000}, *timeout_p = 0;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(win_fd, &fds);  
    if (pty_fd >= 0)
      FD_SET(pty_fd, &fds);
    else if (pid) {
      int status;
      if (waitpid(pid, &status, WNOHANG) == pid) {
        pid = 0;
        
        // Decide whether we want to exit now or later
        if (killed || cfg.hold == HOLD_NEVER)
          exit(0);
        else if (cfg.hold == HOLD_START) {
          if (WIFSIGNALED(status) || WEXITSTATUS(status) != 255)
            exit(0);
        }
        else if (cfg.hold == HOLD_ERROR) {
          if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) == 0)
              exit(0);
          }
          else {
            const int error_sigs =
              1<<SIGILL | 1<<SIGTRAP | 1<<SIGABRT | 1<<SIGFPE | 
              1<<SIGBUS | 1<<SIGSEGV | 1<<SIGPIPE | 1<<SIGSYS;
            if (!(error_sigs & 1<<WTERMSIG(status)))
              exit(0);
          }
        }
      
        int l = 0;
        char *s = 0; 
        if (WIFEXITED(status)) {
          int code = WEXITSTATUS(status);
          if (code && cfg.hold != HOLD_START)
            l = asprintf(&s, "%s: Exit %i", cmd, code); 
        }
        else if (WIFSIGNALED(status))
          l = asprintf(&s, "%s: %s", cmd, strsignal(WTERMSIG(status)));

        if (s)
          term_write(s, l);
      }
      else // Pty gone, but process still there: keep checking
        timeout_p = &timeout;
    }
    
    if (select(win_fd + 1, &fds, 0, 0, timeout_p) > 0) {
      if (pty_fd >= 0 && FD_ISSET(pty_fd, &fds)) {
#if CYGWIN_VERSION_DLL_MAJOR >= 1005
        static char buf[4096];
        int len = read(pty_fd, buf, sizeof buf);
#else
        // Pty devices on old Cygwin version deliver only 4 bytes at a time,
        // so call read() repeatedly until we have a worthwhile haul.
        static char buf[512];
        uint len = 0;
        do {
          int ret = read(pty_fd, buf + len, sizeof buf - len);
          if (ret > 0)
            len += ret;
          else
            break;
        } while (len < sizeof buf);
#endif
        if (len > 0) {
          term_write(buf, len);
          if (log_fd >= 0)
            write(log_fd, buf, len);
        }
        else {
          pty_fd = -1;
          term_hide_cursor();
        }
      }
      if (FD_ISSET(win_fd, &fds))
        return;
    }
  }
}

void
child_kill(bool point_blank)
{ 
  if (!pid ||
      kill(-pid, point_blank ? SIGKILL : SIGHUP) < 0 || 
      point_blank)
    exit(0);
  killed = true;
}

bool
child_is_alive(void)
{
    return pid;
}

bool
child_is_parent(void)
{
  if (!pid)
    return false;
  DIR *d = opendir("/proc");
  if (!d)
    return false;
  bool res = false;
  struct dirent *e;
  char fn[18] = "/proc/";
  while ((e = readdir(d))) {
    char *pn = e->d_name;
    if (isdigit((uchar)*pn) && strlen(pn) <= 6) {
      snprintf(fn + 6, 12, "%s/ppid", pn);
      FILE *f = fopen(fn, "r");
      if (!f)
        continue;
      pid_t ppid = 0;
      fscanf(f, "%u", &ppid);
      fclose(f);
      if (ppid == pid) {
        res = true;
        break;
      }
    }
  }
  closedir(d);
  return res;
}

void
child_write(const char *buf, uint len)
{ 
  if (pty_fd >= 0)
    write(pty_fd, buf, len);
}

void
child_printf(const char *fmt, ...)
{
  if (pty_fd >= 0) {
    va_list va;
    va_start(va, fmt);
    char *s;
    int len = vasprintf(&s, fmt, va);
    va_end(va);
    if (len >= 0)
      write(pty_fd, s, len);
    free(s);
  }
}

void
child_send(const char *buf, uint len)
{
  term_reset_screen();
  if (term.echoing)
    term_write(buf, len);
  child_write(buf, len);
}

void
child_sendw(const wchar *ws, uint wlen)
{
  char s[wlen * cs_cur_max];
  int len = cs_wcntombn(s, ws, sizeof s, wlen);
  if (len > 0)
    child_send(s, len);
}

void
child_resize(struct winsize *winp)
{ 
  if (pty_fd >= 0)
    ioctl(pty_fd, TIOCSWINSZ, winp);
}

wstring
child_conv_path(wstring wpath)
{
  int wlen = wcslen(wpath);
  int len = wlen * cs_cur_max;
  char path[len];
  len = cs_wcntombn(path, wpath, len, wlen);
  path[len] = 0;
  
  char *exp_path;  // expanded path
  if (*path == '~') {
    // Tilde expansion
    char *name = path + 1;
    char *rest = strchr(path, '/');
    if (rest)
      *rest++ = 0;
    else
      rest = "";
    char *base;
    if (!*name)
      base = home;
    else {
#if CYGWIN_VERSION_DLL_MAJOR >= 1005
      // Find named user's home directory
      struct passwd *pw = getpwnam(name);
      base = (pw ? pw->pw_dir : 0) ?: "";
#else
      // Pre-1.5 Cygwin simply copies HOME into pw_dir, which is no use here.
      base = "";
#endif
    }
    exp_path = asform("%s/%s", base, rest);
  }
  else if (*path != '/') {
#if CYGWIN_VERSION_DLL_MAJOR >= 1005
    // Handle relative paths. Finding the foreground process working directory
    // requires the /proc filesystem, which isn't available before Cygwin 1.5.
    
    // Find pty's foreground process, if any. Fall back to child process.
    int fg_pid = (pty_fd >= 0) ? tcgetpgrp(pty_fd) : 0;
    if (fg_pid <= 0)
      fg_pid = pid;
    
    char *cwd = 0;
    if (fg_pid > 0) {
      char proc_cwd[32];
      sprintf(proc_cwd, "/proc/%u/cwd", fg_pid);
      cwd = realpath(proc_cwd, 0);
    }
    
    exp_path = asform("%s/%s", cwd ?: home, path);
    free(cwd);
#else
    // If we're lucky, the path is relative to the home directory.
    exp_path = asform("%s/%s", home, path);
#endif
  }
  else
    exp_path = path;
  
#if CYGWIN_VERSION_DLL_MAJOR >= 1007
#if CYGWIN_VERSION_API_MINOR >= 222
  // CW_INT_SETLOCALE was introduced in API 0.222
  cygwin_internal(CW_INT_SETLOCALE);
#endif
  wchar *win_wpath = cygwin_create_path(CCP_POSIX_TO_WIN_W, exp_path);
  
  // Drop long path prefix if possible,
  // because some programs have trouble with them.
  if (win_wpath && wcslen(win_wpath) < MAX_PATH) {
    wchar *old_win_wpath = win_wpath;
    if (wcsncmp(win_wpath, L"\\\\?\\UNC\\", 8) == 0) {
      win_wpath = wcsdup(win_wpath + 6);
      win_wpath[0] = '\\';  // Replace "\\?\UNC\" prefix with "\\"
      free(old_win_wpath);
    }
    else if (wcsncmp(win_wpath, L"\\\\?\\", 4) == 0) {
      win_wpath = wcsdup(win_wpath + 4);  // Drop "\\?\" prefix
      free(old_win_wpath);
    }
  }
#else
  char win_path[MAX_PATH];
  cygwin_conv_to_win32_path(exp_path, win_path);
  wchar *win_wpath = newn(wchar, MAX_PATH);
  MultiByteToWideChar(0, 0, win_path, -1, win_wpath, MAX_PATH);
#endif
  
  if (exp_path != path)
    free(exp_path);
  
  return win_wpath;
}

void
child_fork(char *argv[])
{
  if (fork() == 0) {
    if (pty_fd >= 0)
      close(pty_fd);
    if (log_fd >= 0)
      close(log_fd);
    close(win_fd);

#if CYGWIN_VERSION_DLL_MAJOR >= 1005
    execv("/proc/self/exe", argv);
#else
    // /proc/self/exe isn't available before Cygwin 1.5, so use argv[0] instead.
    // Strip enclosing quotes if present.
    char *path = argv[0];
    int len = strlen(path);
    if (path[0] == '"' && path[len - 1] == '"') {
      path = strdup(path + 1);
      path[len - 2] = 0;
    }
    execvp(path, argv);
#endif
    exit(255);
  }
}
