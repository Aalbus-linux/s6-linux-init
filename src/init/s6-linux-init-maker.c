/* ISC license. */

#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <skalibs/uint64.h>
#include <skalibs/types.h>
#include <skalibs/bytestr.h>
#include <skalibs/allreadwrite.h>
#include <skalibs/buffer.h>
#include <skalibs/strerr2.h>
#include <skalibs/env.h>
#include <skalibs/stralloc.h>
#include <skalibs/djbunix.h>
#include <skalibs/sgetopt.h>
#include <skalibs/skamisc.h>

#include <s6/config.h>

#include <s6-linux-init/config.h>
#include "defaults.h"
#include "initctl.h"

#define USAGE "s6-linux-init-maker [ -c basedir ] [ -b execline_bindir ] [ -u log_uid -g log_gid | -U ] [ -G early_getty_cmd ] [ -r ] [ -L ] [ -p initial_path ] [ -m initial_umask ] [ -t timestamp_style ] [ -d slashdev ] [ -s env_store ] [ -e initial_envvar ... ] [ -q default_grace_time ] dir"
#define dieusage() strerr_dieusage(100, USAGE)
#define dienomem() strerr_diefu1sys(111, "stralloc_catb") ;

#define UNCAUGHT_DIR "uncaught-logs"
#define EXITCODENAME "file\\ created\\ by\\ s6-linux-init,\\ storing\\ a\\ container's\\ exit\\ code"

#define CRASH_SCRIPT \
"redirfd -r 0 /dev/console\n" \
"redirfd -w 1 /dev/console\n" \
"fdmove -c 2 1\n" \
"foreground { s6-echo -- " \
"\"s6-svscan finished. Dropping to an interactive shell.\" }\n" \
"/bin/sh -i\n"

static char const *robase = BASEDIR ;
static char const *bindir = BINDIR ;
static char const *initial_path = INITPATH ;
static char const *env_store = 0 ;
static char const *early_getty = 0 ;
static char const *slashdev = 0 ;
static uid_t uncaught_logs_uid = 0 ;
static gid_t uncaught_logs_gid = 0 ;
static unsigned int initial_umask = 0022 ;
static unsigned int timestamp_style = 1 ;
static unsigned int finalsleep = 3000 ;
static int redirect_stage2 = 0 ;
static int logouthookd = 0 ;

typedef int writetobuf_func_t (buffer *, void *) ;
typedef writetobuf_func_t *writetobuf_func_t_ref ;

#define put_shebang(b) put_shebang_options((b), "-P")

static int put_shebang_options (buffer *b, void *data)
{
  char *options = data ;
  return buffer_puts(b, "#!") >= 0
   && buffer_puts(b, bindir) >= 0
   && buffer_puts(b, "/execlineb") >= 0
   && (!options || !options[0] || buffer_puts(b, " ") >= 0)
   && buffer_puts(b, options) >= 0
   && buffer_puts(b, "\n\n") >= 0 ;
}

static int line_script (buffer *b, void *data)
{
  char *line = data ;
  return put_shebang(b)
   && buffer_puts(b, line) >= 0
   && buffer_put(b, "\n", 1) >= 0 ;
}

static int linewithargs_script (buffer *b, void *data)
{
  char *line = data ;
  return put_shebang_options(b, "-S0")
   && buffer_puts(b, line) >= 0
   && buffer_puts(b, " $@\n") >= 0 ;
}

static int death_script (buffer *b, void *data)
{
  char *s = data ;
  return put_shebang(b)
    && buffer_puts(b,
      "redirfd -r 0 /dev/console\n"
      "redirfd -w 1 /dev/console\n"
      "fdmove -c 2 1\n"
      "foreground { s6-echo -- \"s6-svscan ") >= 0
    && buffer_puts(b, s) >= 0
    && buffer_puts(b,
      ". Dropping to an interactive shell.\" }\n"
      "/bin/sh -i\n") >= 0
}

static int s6_svscan_log_script (buffer *b, void *data)
{
  char fmt[UINT64_FMT] ;
  (void)data ;
  return put_shebang(b)
   && buffer_puts(b,
    "redirfd -w 2 /dev/console\n"
    "redirfd -w 1 /dev/null\n"
    "redirfd -rnb 0 " LOGGER_FIFO "\n"
    "s6-applyuidgid -u ") >= 0
   && buffer_put(b, fmt, uid_fmt(fmt, uncaught_logs_uid)) >= 0
   && buffer_puts(b, " -g ") >= 0
   && buffer_put(b, fmt, gid_fmt(fmt, uncaught_logs_gid)) >= 0
   && buffer_puts(b, " --\ns6-log -bpd3 -- ") >= 0
   && buffer_puts(b, timestamp_style & 1 ? "t " : "") >= 0
   && buffer_puts(b, timestamp_style & 2 ? "T " : "") >= 0
   && buffer_puts(b, S6_LINUX_INIT_TMPFS "/" UNCAUGHT_DIR "\n") >= 0 ;
}

static int logouthookd_script (buffer *b, void *data)
{
  (void)data ;
  return put_shebang(b)
   && buffer_puts(b,
    S6_EXTBINPREFIX "s6-ipcserver -1 -l0 -- " LOGOUTHOOKD_SOCKET "\n"
    S6_LINUX_INIT_BINPREFIX "s6-linux-init-logouthookd\n") >= 0 ;
}

static int shutdownd_script (buffer *b, void *data)
{
  size_t sabase = satmp.len ;
  char fmt[UINT_FMT] ;
  if (!put_shebang(b)
   || buffer_puts(b, S6_LINUX_INIT_BINPREFIX "s6-linux-init-shutdownd -b ") < 0)
   || !string_quote(&satmp, bindir, strlen(bindir))) return 0 ;
  if (buffer_puts(b, satmp.s + sabase) < 0) goto err ;
  satmp.len = sabase ;
  if (buffer_puts(b, " -c ") < 0
   || !string_quote(&satmp, robase, strlen(robase))) return 0 ;
  if (buffer_puts(b, satmp.s + sabase) < 0) goto err ;
  satmp.len = sabase ;
  if (buffer_puts(b, " -g ") < 0
   || buffer_puts(b, fmt, uint_fmt(fmt, finalsleep)) < 0
   || buffer_puts(b, "\n") < 0) return 0 ;
  (void)data ;
  return 1 ;

 err:
  satmp.len = sabase ;
  return 0 ;    
}

static int sig_script (buffer *b, void *data)
{
  char *option = data ;
  return put_shebang(b)
   && buffer_puts(b, S6_LINUX_INIT_BINPREFIX "s6-linux-init-shutdown -a ") >= 0
   && buffer_puts(b, option) >= 0
   && buffer_puts(" -- now\n") >= 0 ;
}

static int stage4_script (buffer *b, void *data)
{
  (void)data ;
}

static inline int stage1_script (buffer *b)
{
  size_t sabase = satmp.len ;
  if (!put_shebang_options(b, "-S0")
   || buffer_puts(b, S6_LINUX_INIT_EXTBINPREFIX "s6-linux-init -c ") < 0
   || !string_quote(&satmp, robase, strlen(robase))) return 0 ;
  if (buffer_puts(b, satmp.s + sabase) < 0) goto err ;
  satmp.len = sabase ;
  {
    char fmt[UINT_OFMT] ;
    if (buffer_puts(b, " -m 00") < 0
     || buffer_put(b, fmt, uint_ofmt(fmt, initial_umask)) < 0) return 0 ;
  }
  if (redirect_stage2)
  {
    if (buffer_puts(b, " -r") < 0) return 0 ;
  }
  if (initial_path)
  {
    if (buffer_puts(b, " -p ") < 0
     || !string_quote(&satmp, initial_path, strlen(initial_path))) return 0 ;
    if (buffer_puts(b, satmp.s + sabase) < 0) goto err ;
    satmp.len = sabase ;
  }
  if (env_store)
  {
    if (buffer_puts(b, " -s ") < 0
     || !string_quote(&satmp, env_store, strlen(env_store))) return 0 ;
    if (buffer_puts(b, satmp.s + sabase) < 0) goto err ;
    satmp.len = sabase ;
  }
  if (slashdev)
  {
    if (buffer_puts(b, " -d ") < 0
     || !string_quote(&satmp, slashdev, strlen(slashdev))) return 0 ;
    if (buffer_puts(b, satmp.s + sabase) < 0) goto err ;
    satmp.len = sabase ;
  }
  if (buffer_puts(b, "\n") < 0) return 0 ;
  return 1 ;

 err:
  satmp.len = sabase ;
  return 0 ;
}

static void cleanup (char const *base)
{
  int e = errno ;
  rm_rf(base) ;
  errno = e ;
}

static void auto_dir (char const *base, char const *dir, uid_t uid, gid_t gid, unsigned int mode)
{
  size_t clen = strlen(base) ;
  size_t dlen = strlen(dir) ;
  char fn[clen + dlen + 2] ;
  memcpy(fn, base, clen) ;
  fn[clen] = dlen ? '/' : 0 ;
  memcpy(fn + clen + 1, dir, dlen + 1) ;
  if (mkdir(fn, mode) < 0
   || ((uid || gid) && (chown(fn, uid, gid) < 0 || chmod(fn, mode) < 0)))
  {
    cleanup(base) ;
    strerr_diefu2sys(111, "mkdir ", fn) ;
  }
}

static void auto_file (char const *base, char const *file, char const *s, unsigned int n, int executable)
{
  size_t clen = strlen(base) ;
  size_t flen = strlen(file) ;
  char fn[clen + flen + 2] ;
  memcpy(fn, base, clen) ;
  fn[clen] = '/' ;
  memcpy(fn + clen + 1, file, flen + 1) ;
  if (!openwritenclose_unsafe(fn, s, n)
   || chmod(fn, executable ? 0755 : 0644) == -1))
  {
    cleanup(base) ;
    strerr_diefu2sys(111, "write to ", fn) ;
  }
}

static void auto_symlink (char const *base, char const *name, char const *target)
{
  size_t clen = strlen(base) ;
  size_t dlen = strlen(name) ;
  char fn[clen + dlen + 2] ;
  memcpy(fn, base, clen) ;
  fn[clen] = '/' ;
  memcpy(fn + clen + 1, name, dlen + 1) ;
  if (symlink(target, fn) == -1)
    strerr_diefu4sys(111, "make a symlink named ", fn, " pointing to ", target) ;
}

static void auto_fifo (char const *base, char const *fifo)
{
  size_t baselen = strlen(base) ;
  size_t fifolen = strlen(fifo) ;
  char fn[baselen + fifolen + 2] ;
  memcpy(fn, base, baselen) ;
  fn[baselen] = '/' ;
  memcpy(fn + baselen + 1, fifo, fifolen + 1) ;
  if (mkfifo(fn, 0600) < 0)
  {
    cleanup(base) ;
    strerr_diefu2sys(111, "mkfifo ", fn) ;
  }
}

static void auto_script (char const *base, char const *file, writetobuf_func_t_ref scriptf, void *data)
{
  char buf[4096] ;
  buffer b ;
  size_t baselen = strlen(base) ;
  size_t filelen = strlen(file) ;
  int fd ;
  char fn[baselen + filelen + 2] ;
  memcpy(fn, base, baselen) ;
  fn[baselen] = '/' ;
  memcpy(fn + baselen + 1, file, filelen + 1) ;
  fd = open_trunc(fn) ;
  if (fd < 0 || ndelay_off(fd) < 0 || fchmod(fd, 0755) < 0)
  {
    cleanup(base) ;
    strerr_diefu3sys(111, "open ", fn, " for script writing") ;
  }
  buffer_init(&b, &fd_writev, fd, buf, 4096) ;
  if (!(*scriptf)(&b, data) || !buffer_flush(&b))
  {
    cleanup(base) ;
    strerr_diefu2sys(111, "write to ", fn) ;
  }
  fd_close(fd) ;
}

static void auto_exec (char const *base, char const *name, char const *target)
{
  if (S6_LINUX_INIT_BINPREFIX[0] == '/')
  {
    size_t len = strlen(target) ;
    char fn[sizeof(S6_LINUX_INIT_BINPREFIX) + len] = S6_LINUX_INIT_BINPREFIX ;
    memcpy(fn + sizeof(S6_LINUX_INIT_BINPREFIX - 1, len + 1, target)) ;
    auto_symlink(base, name, fn) ;
  }
  else
    auto_script(base, name, &linewithargs_script, target) ;
}

static void make_env (char const *base, char const *envname, char const *modif, size_t modiflen)
{
  size_t envnamelen = strlen(envname) ;
  auto_dir(base, envname, 0, 0, 0755) ;
  while (modiflen)
  {
    size_t len = strlen(modif) ;
    size_t pos = byte_chr(modif, len, '=') ;
    char fn[envnamelen + pos + 2] ;
    memcpy(fn, envname, envnamelen) ;
    fn[envnamelen] = '/' ;
    memcpy(fn + envnamelen + 1, modif, pos) ;
    fn[envnamelen + 1 + pos] = 0 ;
    
    if (pos + 1 < len) auto_file(base, fn, modif + pos + 1, len - pos - 1, 0) ;
    else if (pos + 1 == len) auto_file(base, fn, "\n", 1, 0) ;
    else auto_file(base, fn, "", 0, 0) ;
    modif += len+1 ; modiflen -= len+1 ;
  }
}

static inline void make_image (char const *base)
{
  auto_dir(base, "run-image", 0, 0, 0755) ;
  auto_dir(base, "run-image/" UNCAUGHT_DIR, uncaught_logs_uid, uncaught_logs_gid, 02700) ;
  auto_dir(base, "run-image/" SCANDIR, 0, 0, 0755) ;
  auto_dir(base, "run-image/" SCANDIR "/.s6-svscan", 0, 0, 0755) ;
  auto_script(base, "run-image/" SCANDIR "/.s6-svscan/crash", &death_script, "crashed") ;
  auto_script(base, "run-image/" SCANDIR "/.s6-svscan/finish", &death_script, "exited") ;
  auto_script(base, "run-image/" SCANDIR "/.s6-svscan/SIGTERM", &put_shebang_options, 0) ;
  auto_script(base, "run-image/" SCANDIR "/.s6-svscan/SIGHUP", &put_shebang_options, 0) ;
  auto_script(base, "run-image/" SCANDIR "/.s6-svscan/SIGQUIT", &put_shebang_options, 0) ;
  auto_script(base, "run-image/" SCANDIR "/.s6-svscan/SIGINT", &sig_script, "-r") ;
  auto_script(base, "run-image/" SCANDIR "/.s6-svscan/SIGUSR1", &sig_script, "-p") ;
  auto_script(base, "run-image/" SCANDIR "/.s6-svscan/SIGUSR2", &sig_script, "-h") ;
  auto_dir(base, "run-image/" SCANDIR "/" LOGGER_SERVICEDIR, 0, 0, 0755) ;
  auto_fifo(base, "run-image/" SCANDIR "/" LOGGER_SERVICEDIR "/" LOGGER_FIFO) ;
  auto_file(base, "run-image/" SCANDIR "/" LOGGER_SERVICEDIR "/notification-fd", "3\n", 2, 0) ;
  auto_script(base, "run-image/" SCANDIR "/" LOGGER_SERVICEDIR "/run, &s6_svscan_log_script, 0) ;
  auto_dir(base, "run-image/" SCANDIR "/" SHUTDOWND_SERVICEDIR, 0, 0, 0755) ;
  auto_fifo(base, "run-image/" SCANDIR "/" SHUTDOWND_SERVICEDIR "/" SHUTDOWND_FIFO) ;
  auto_script(base, "run-image/" SCANDIR "/" SHUTDOWND_SERVICEDIR "/run", &shutdownd_script, 0) ;
  if (logouthookd)
  {
    auto_dir(base, "run-image/" SCANDIR "/" LOGOUTHOOKD_SERVICEDIR, 0, 0, 0755) ;
    auto_file(base, "run-image/" SCANDIR "/" LOGOUTHOOKD_SERVICEDIR "/notification-fd", "1\n", 2, 0) ;
    auto_script(base, "run-image/" SCANDIR "/" LOGOUTHOOKD_SERVICEDIR "/run", &logouthookd_script, 0) ;
  }
  if (early_getty)
  {
    auto_dir(base, "run-image/" SCANDIR "/" EARLYGETTY_SERVICEDIR, 0, 0, 0755) ;
    auto_script(base, "run-image/" SCANDIR "/" EARLYGETTY_SERVICEDIR "/run", &line_script, early_getty) ;
  }
}

static inline void make_scripts (char const *base)
{
  auto_dir(base, "scripts", 0, 0, 0755) ;
  auto_script(base, "scripts/runlevel", &put_shebang_options, 0) ;
  auto_script(base, "scripts/" STAGE2, &put_shebang_options, 0) ;
  auto_script(base, "scripts/" STAGE3, &put_shebang_options, 0) ;
}

static inline void make_bins (char const *base)
{
  auto_dir(base, "bin", 0, 0, 0755) ;
  auto_exec(base, "bin/halt", "s6-linux-init-halt") ;
  auto_exec(base, "bin/reboot", "s6-linux-init-reboot") ;
  auto_exec(base, "bin/poweroff", "s6-linux-init-poweroff") ;
  auto_exec(base, "bin/shutdown", "s6-linux-init-shutdown") ;
  auto_exec(base, "bin/telinit", "s6-linux-init-telinit") ;
  auto_script(base, "bin/init", &stage1_script, 0) ;
  auto_script(base, "bin/" STAGE4, &stage4_script, 0) ;
}

int main (int argc, char const *const *argv, char const *const *envp)
{
  stralloc saenv1 = STRALLOC_ZERO ;
  stralloc saenv2 = STRALLOC_ZERO ;
  PROG = "s6-linux-init-maker" ;
  {
    subgetopt_t l = SUBGETOPT_ZERO ;
    for (;;)
    {
      int opt = subgetopt_r(argc, argv, "c:b:u:g:UG:rLp:m:t:d:s:e:E:q:", &l) ;
      if (opt == -1) break ;
      switch (opt)
      {
        case 'c' : robase = l.arg ; break ;
        case 'b' : bindir = l.arg ; break ;
        case 'u' : if (!uint0_scan(l.arg, &uncaught_logs_uid)) dieusage() ; break ;
        case 'g' : if (!uint0_scan(l.arg, &uncaught_logs_gid)) dieusage() ; break ;
        case 'U' :
        {
          char const *x = env_get2(envp, "UID") ;
          if (!x) strerr_dienotset(100, "UID") ;
          if (!uint0_scan(x, &uncaught_logs_uid)) strerr_dieinvalid(100, "UID") ;
          x = env_get2(envp, "GID") ;
          if (!x) strerr_dienotset(100, "GID") ;
          if (!uint0_scan(x, &uncaught_logs_gid)) strerr_dieinvalid(100, "GID") ;
        }
        case 'G' : early_getty = l.arg ; break ;
        case 'r' : redirect_stage2 = 1 ; break ;
        case 'L' : logouthookd = 1 ; break ;
        case 'p' : initial_path = l.arg ; break ;
        case 'm' : if (!uint0_oscan(l.arg, &initial_umask)) dieusage() ; break ;
        case 't' : if (!uint0_scan(l.arg, &timestamp_style)) dieusage() ; break ;
        case 'd' : slashdev = l.arg ; break ;
        case 's' : env_store = l.arg ; break ;
        case 'e' : if (!stralloc_catb(&saenv1, l.arg, strlen(l.arg) + 1)) dienomem() ; break ;
        case 'E' : if (!stralloc_catb(&saenv2, l.arg, strlen(l.arg) + 1)) dienomem() ; break ;
        case 'q' : if (!uint0_scan(l.arg, &finalsleep)) dieusage() ; break ;
        default : dieusage() ;
      }
    }
    argc -= l.ind ; argv += l.ind ;
  }
  if (!argc) dieusage() ;

  if (robase[0] != '/')
    strerr_dief3x(100, "base directory ", robase, " is not absolute") ;
  if (bindir[0] != '/')
    strerr_dief3x(100, "initial location for binaries ", bindir, " is not absolute") ;
  if (init_script[0] != '/')
    strerr_dief3x(100, "stage 2 script location ", init_script, " is not absolute") ;
  if (tini_script[0] != '/')
    strerr_dief3x(100, "shutdown script location ", tini_script, " is not absolute") ;
  if (slashdev && slashdev[0] != '/')
    strerr_dief3x(100, "devtmpfs mounting location ", slashdev, " is not absolute") ;
  if (timestamp_style > 3)
    strerr_dief1x(100, "-t timestamp_style must be 0, 1, 2 or 3") ;

  umask(0) ;
  if (mkdir(argv[0], 0755) < 0)
    strerr_diefu2sys(111, "mkdir ", argv[0]) ;

  make_env(argv[0], ENVSTAGE2, saenv2.s, saenv2.len) ;
  stralloc_free(&saenv2) ;
  make_env(argv[0], ENVSTAGE1, saenv1.s, saenv1.len) ;
  saenv1.len = 0 ;
  make_image(argv[0], &saenv1) ;
  make_scripts(argv[0]) ;
  make_bins(argv[0]) ;
  return 0 ;
}
