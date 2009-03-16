// child.c (part of MinTTY)
// Copyright 2008-09 Andy Koppe
// Licensed under the terms of the GNU General Public License v3 or later.

#include "child.h"

#include "term.h"
#include "config.h"

#include <pwd.h>
#include <pty.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <argz.h>
#include <utmp.h>
#include <dirent.h>
#include <pthread.h>

#include <winbase.h>

HANDLE child_event;
static HANDLE proc_event;
static pid_t pid;
static int status;
static int fd = -1;
static int read_len;
static char read_buf[4096];
static struct utmp ut;
static char *name;
static bool killed;

static sigset_t term_sigs;

static void *
signal_thread(void *unused(arg))
{
  int sig;
  sigwait(&term_sigs, &sig);
  if (pid)
    kill(pid, SIGHUP);
  exit(0);
}

static void *
read_thread(void *unused(arg))
{
  while ((read_len = read(fd, read_buf, sizeof read_buf)) > 0) {
    SetEvent(child_event);
    WaitForSingleObject(proc_event, INFINITE);
  };
  close(fd);
  fd = -1;
  return 0;
}

static void *
wait_thread(void *unused(arg))
{
  while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
    ;
  Sleep(100); // Give any ongoing output some time to finish.
  pid = 0;
  SetEvent(child_event);
  return 0;
}

bool
child_proc(void)
{
  if (read_len > 0) {
    term_write(read_buf, read_len);
    read_len = 0;
    SetEvent(proc_event);
  }
  if (pid == 0) {
    logout(ut.ut_line);
    
    // No point hanging around if the user wants the terminal shut.
    if (killed)
      exit(0);
    
    // Otherwise, display a message if the child process died with an error. 
    int l = -1;
    char *s; 
    if (WIFEXITED(status)) {
      status = WEXITSTATUS(status);
      if (status == 0)
        exit(0);
      else
        l = asprintf(&s, "\r\n%s exited with status %i\r\n", name, status); 
    }
    else if (WIFSIGNALED(status)) {
      int error_sigs =
        1<<SIGILL | 1<<SIGTRAP | 1<<SIGABRT | 1<<SIGFPE | 
        1<<SIGBUS | 1<<SIGSEGV | 1<<SIGPIPE | 1<<SIGSYS;
      int sig = WTERMSIG(status);
      if ((error_sigs & 1<<sig) == 0)
        exit(0);
      l = asprintf(&s, "\r\n%s terminated: %s\r\n", name, strsignal(sig));
    }
    if (l != -1) {
      term_write(s, l);
      free(s);
    }
    return true;
  }
  return false;
}

char *
child_create(char *argv[], struct winsize *winp)
{
  struct passwd *pw = getpwuid(getuid());
  
  char *cmd = (pw ? pw->pw_shell : 0) ?: "/bin/sh";  
  if (!*argv)
    argv = (char *[]){cmd, 0};
  else if (argv[1] || strcmp(*argv, "-"))
    cmd = *argv;
  else {  
    char *slash_p = strrchr(cmd, '/');
    asprintf(argv, "-%s", slash_p ? slash_p + 1 : cmd);
  }
  
  // Create the child process and pseudo terminal.
  pid = forkpty(&fd, 0, 0, winp);
  if (pid == -1) { // Fork failed.
    char *msg = strdup(strerror(errno));
    term_write("forkpty: ", 8);
    term_write(msg, strlen(msg));
    free(msg);
    pid = 0;
  }
  else if (pid == 0) { // Child process.
    struct termios attr;
    tcgetattr(0, &attr);
    attr.c_cc[VERASE] = cfg.backspace_sends_del ? 0x7F : '\b';
    attr.c_cc[VDISCARD] = 0;
    tcsetattr(0, TCSANOW, &attr);
    setenv("TERM", "xterm", 1);
    execvp(cmd, argv);

    // If we get here, exec failed.
    char *msg = strdup(strerror(errno));
    write(STDERR_FILENO, "exec: ", 6);
    write(STDERR_FILENO, *argv, strlen(*argv));
    write(STDERR_FILENO, ": ", 2);
    write(STDERR_FILENO, msg, strlen(msg));
    free(msg);
    exit(1);
  }
  else { // Parent process.
    name = *argv;
    
    child_event = CreateEvent(null, false, false, null);
    proc_event = CreateEvent(null, false, false, null);
    
    sigemptyset(&term_sigs);
    sigaddset(&term_sigs, SIGHUP);
    sigaddset(&term_sigs, SIGINT);
    sigaddset(&term_sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &term_sigs, null);
    
    pthread_t thread;
    pthread_create(&thread, 0, signal_thread, 0);
    pthread_create(&thread, 0, wait_thread, 0);
    pthread_create(&thread, 0, read_thread, 0);

    ut.ut_type = USER_PROCESS;
    ut.ut_pid = pid;
    ut.ut_time = time(0);
    char *dev = ptsname(fd);
    if (dev) {
      if (strncmp(dev, "/dev/", 5) == 0)
        dev += 5;
      strncpy(ut.ut_line, dev ?: "?", sizeof ut.ut_line);
      if (strncmp(dev, "pty", 3) == 0 || strncmp(dev, "tty", 3) == 0)
        dev += 3;
      strncpy(ut.ut_id, dev ?: "?", sizeof ut.ut_id);      
    }
    strncpy(ut.ut_user, (pw ? pw->pw_name : 0) ?: "?", sizeof ut.ut_user);
    login(&ut);
  }
  
  // Return child command line for window title.
  char *argz;
  size_t argz_len;
  argz_create(argv, &argz, &argz_len);
  argz_stringify(argz, argz_len, ' ');
  return argz;
}

void
child_kill(void)
{ 
  if (!killed) {
    // Tell the child nicely.
    kill(pid, SIGHUP);
    killed = true;
  }
  else {
    // Use brute force and head for the exit.
    kill(pid, SIGKILL);
    exit(0);
  }
}

bool
child_is_parent(void)
{
  DIR *d = opendir("/proc");
  if (!d)
    return false;
  bool res = false;
  struct dirent *e;
  char fn[18] = "/proc/";
  while ((e = readdir(d))) {
    char *pn = e->d_name;
    if (isdigit(*pn) && strlen(pn) <= 6) {
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
child_write(const char *buf, int len)
{ 
  if (fd >= 0)
    write(fd, buf, len); 
  else if (!pid)
    exit(0);
}

void
child_resize(struct winsize *winp)
{ 
  if (fd >= 0)
    ioctl(fd, TIOCSWINSZ, winp);
}

